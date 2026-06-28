// CamC2Config.cpp — see CamC2Config.h for design notes.
//
// Clones the LoRaWANConfig storage pattern: a bounded table in NVS namespace
// "c2auth", per-identity counters in small dedicated keys, block-reserved and
// fail-closed. The ONE deliberate inversion vs LoRaWANConfig is the inbound
// counter: there we track the highest seq SEEN from a peer and reject <= (RX
// anti-replay), instead of reserving ahead for our own TX.

#include "CamC2Config.h"
#if defined(BRIDGE_ROLE_CAMERA) || defined(BRIDGE_CAM_COMMANDER)

#include "SerialLog.h"   // serialized console (anti-garble); C2 runs in radioTask
#include <Preferences.h>
#include <string.h>

namespace CamC2Config {

static const char *NVS_NS     = "c2auth";
static const char *KEY_TABLE  = "peers";
static const char *KEY_TXSEQ  = "txseq";

static Peer     s_peer[MAX_PEERS];
static uint32_t s_txSeq      = 0;   // next outbound seq to issue (RAM)
static uint32_t s_txReserved = 0;   // persisted outbound high-water (== confirmed NVS)

// rxHighWater is persisted per-peer keyed by peerId (NOT slot index) so a peer
// keeps its anti-replay floor across a portal row move / clear+re-add. Key fits
// NVS's 15-char cap: "rx_" + 8 hex = 11 chars.
static void rxKeyForPeer(uint32_t peerId, char *out, size_t cap) {
    snprintf(out, cap, "rx_%08lx", (unsigned long)peerId);
}

static uint32_t loadRxForPeer(uint32_t peerId) {
    if (peerId == 0) return 0;
    Preferences p;
    if (!p.begin(NVS_NS, /*readOnly=*/true)) return 0;
    char k[16]; rxKeyForPeer(peerId, k, sizeof(k));
    uint32_t v = p.getUInt(k, 0);
    p.end();
    return v;
}

// Durably persist a peer's rx high-water. True only on a CONFIRMED 4-byte write.
static bool persistRxForPeer(uint32_t peerId, uint32_t val) {
    if (peerId == 0) return false;
    Preferences p;
    if (!p.begin(NVS_NS, /*readOnly=*/false)) return false;
    char k[16]; rxKeyForPeer(peerId, k, sizeof(k));
    size_t wrote = p.putUInt(k, val);
    p.end();
    return wrote == sizeof(uint32_t);
}

static bool persistTxSeq(uint32_t val) {
    Preferences p;
    if (!p.begin(NVS_NS, /*readOnly=*/false)) return false;
    size_t wrote = p.putUInt(KEY_TXSEQ, val);
    p.end();
    return wrote == sizeof(uint32_t);
}

#if defined(BRIDGE_C2_PEER_ID) && defined(BRIDGE_C2_PEER_KEY)
// Parse exactly 32 hex chars into a 16-byte key. False on bad length/char.
static int hexNibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
static bool parseHex16(const char *hex, uint8_t out[16]) {
    if (!hex) return false;
    for (int n = 0; n < 16; n++) {
        int hi = hexNibble(hex[2 * n]);     if (hi < 0) return false;
        int lo = hexNibble(hex[2 * n + 1]); if (lo < 0) return false;
        out[n] = (uint8_t)((hi << 4) | lo);
    }
    return hex[32] == '\0';
}
#endif

void begin() {
    memset(s_peer, 0, sizeof(s_peer));
    // seq is 1-BASED (0 is reserved): peers start with rxHighWater=0 and accept
    // only seq > rxHighWater, so the first outbound seq must be >= 1 or the very
    // first frame would be rejected as a replay. Default holds for a fresh device
    // (no NVS namespace yet → early return below).
    s_txReserved = 0;
    s_txSeq      = 1;

    Preferences p;
    bool haveNs = p.begin(NVS_NS, /*readOnly=*/true);
    if (!haveNs)
        Serial.println("[CamC2Config] no NVS namespace yet (no C2 peers configured)");
    // NB: do NOT early-return here — the build-flag peer seed below must still run on
    // a fresh/erased board (where the namespace doesn't exist yet).
    size_t got = haveNs ? p.getBytesLength(KEY_TABLE) : 0;
    if (got == sizeof(s_peer)) {
        p.getBytes(KEY_TABLE, s_peer, sizeof(s_peer));
    } else if (got != 0) {
        Serial.printf("[CamC2Config] peer table size mismatch (%u B); ignoring\n",
                      (unsigned)got);
        memset(s_peer, 0, sizeof(s_peer));
    }
    // Resume each peer's anti-replay floor from its persisted (peerId-keyed) value
    // so a command after a reboot can never replay a seq already accepted.
    char k[16];
    if (haveNs) for (size_t i = 0; i < MAX_PEERS; i++) {
        if (s_peer[i].peerId == 0) continue;
        rxKeyForPeer(s_peer[i].peerId, k, sizeof(k));
        s_peer[i].rxHighWater = p.getUInt(k, 0);
    }
    // Resume our own outbound seq at the persisted reservation (skipping any
    // unused values in the last reserved block — monotonic across reboot).
    uint32_t txr = haveNs ? p.getUInt(KEY_TXSEQ, 0) : 0;
    s_txReserved = txr;
    s_txSeq      = (txr == 0) ? 1u : txr;   // resume at the reservation; 1-based on a fresh table
    if (haveNs) p.end();
#if defined(BRIDGE_C2_PEER_ID) && defined(BRIDGE_C2_PEER_KEY)
    // Bench: if the NVS table is empty, seed ONE peer from build flags (RAM only,
    // not persisted) — the build-flag equivalent of provisioning a commander, so two
    // boards whitelist each other with -DBRIDGE_C2_PEER_ID/_KEY. The anti-replay
    // floor still loads from NVS (rx_<id>), so replay protection survives reboot.
    if (!anyConfigured()) {
        uint8_t key[16];
        if (parseHex16(BRIDGE_C2_PEER_KEY, key)) {
            Peer &pr = s_peer[0];
            memset(&pr, 0, sizeof(pr));
            pr.inUse  = 1;
            pr.peerId = (uint32_t)(BRIDGE_C2_PEER_ID);
            memcpy(pr.psk, key, 16);
  #ifdef BRIDGE_C2_PEER_PRIMARY
            pr.flags |= FLAG_PRIMARY_MASTER;
  #endif
            pr.rxHighWater = loadRxForPeer(pr.peerId);
            Serial.printf("[CamC2Config] bench-seeded peer 0x%08lx (rxHW=%lu) from build flags\n",
                          (unsigned long)pr.peerId, (unsigned long)pr.rxHighWater);
        } else {
            Serial.println("[CamC2Config] BRIDGE_C2_PEER_KEY must be 32 hex chars — peer NOT seeded");
        }
    }
#endif
    Serial.printf("[CamC2Config] loaded %u peer slots (anyConfigured=%d) txSeq=%lu\n",
                  (unsigned)MAX_PEERS, anyConfigured() ? 1 : 0, (unsigned long)s_txSeq);
    debugDump();
}

void saveTable() {
    Preferences p;
    if (!p.begin(NVS_NS, /*readOnly=*/false)) {
        Serial.println("[CamC2Config] saveTable: prefs.begin RW failed");
        return;
    }
    size_t wrote = p.putBytes(KEY_TABLE, s_peer, sizeof(s_peer));
    p.end();
    Serial.printf("[CamC2Config] peer table saved (%u B)\n", (unsigned)wrote);
}

bool anyConfigured() {
    for (size_t i = 0; i < MAX_PEERS; i++)
        if (s_peer[i].inUse && s_peer[i].peerId != 0) return true;
    return false;
}

const Peer *resolve(uint32_t peerId, int &outIndex) {
    if (peerId != 0) {
        for (size_t i = 0; i < MAX_PEERS; i++) {
            if (s_peer[i].inUse && s_peer[i].peerId == peerId) {
                outIndex = (int)i;
                return &s_peer[i];
            }
        }
    }
    outIndex = -1;
    return nullptr;
}

bool acceptInboundSeq(int i, uint32_t seq) {
    if (i < 0 || i >= (int)MAX_PEERS) return false;
    Peer &pr = s_peer[i];
    if (!pr.inUse || pr.peerId == 0) return false;
    if (seq <= pr.rxHighWater) {                       // replay / stale → reject
        return false;
    }
    // Persist the new floor BEFORE accepting — fail-closed: if we can't durably
    // record it, drop the command rather than actuate on a seq a reboot could
    // forget (and thus replay). Only authenticated frames reach here, so this
    // write rate is bounded by the legitimate command rate.
    if (!persistRxForPeer(pr.peerId, seq)) {
        SerialLog::logf("[CamC2Config] RX_PERSIST_FAIL peer=0x%08lx — dropping cmd\n",
                        (unsigned long)pr.peerId);
        return false;
    }
    pr.rxHighWater = seq;
    return true;
}

uint32_t nextTxSeq() {
    uint32_t v = s_txSeq;
    if (v >= s_txReserved) {                           // v not yet durably reserved
        if (v > UINT32_MAX - 1 - SEQ_RESERVE) {        // would wrap → space exhausted
            SerialLog::logf("[CamC2Config] TXSEQ_EXHAUSTED — rekey required\n");
            return SEQ_INVALID;
        }
        uint32_t nr = v + 1 + SEQ_RESERVE;
        if (!persistTxSeq(nr)) {
            SerialLog::logf("[CamC2Config] TXSEQ_PERSIST_FAIL — dropping outbound\n");
            return SEQ_INVALID;                         // fail closed: caller drops
        }
        s_txReserved = nr;
    }
    s_txSeq = v + 1;
    return v;
}

size_t peerCount() { return MAX_PEERS; }

const Peer &peer(int i) {
    static Peer empty = {};
    if (i < 0 || i >= (int)MAX_PEERS) return empty;
    return s_peer[i];
}

void setPeer(int i, const Peer &p) {
    if (i < 0 || i >= (int)MAX_PEERS) return;
    // If this slot's identity changes, re-seed the RAM anti-replay floor from the
    // NEW peerId's persisted high-water so a moved/re-added peer never inherits the
    // previous identity's floor (which could let an old seq replay).
    if (p.peerId != s_peer[i].peerId) {
        s_peer[i] = p;
        s_peer[i].rxHighWater = loadRxForPeer(p.peerId);
        return;
    }
    s_peer[i] = p;
}

void clearPeer(int i) {
    if (i < 0 || i >= (int)MAX_PEERS) return;
    memset(&s_peer[i], 0, sizeof(Peer));
}

uint32_t currentTxSeq() { return s_txSeq; }

void debugDump() {
    for (size_t i = 0; i < MAX_PEERS; i++) {
        const Peer &pr = s_peer[i];
        if (!pr.inUse) continue;
        Serial.printf("[CamC2Config] peer%u id=0x%08lx flags=0x%02x rxHW=%lu\n",
                      (unsigned)i, (unsigned long)pr.peerId, (unsigned)pr.flags,
                      (unsigned long)pr.rxHighWater);
    }
}

}  // namespace CamC2Config

#endif  // BRIDGE_ROLE_CAMERA || BRIDGE_CAM_COMMANDER
