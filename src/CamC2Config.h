// CamC2Config.h
// ---------------------------------------------------------------------------
// LoRaCam C2 peer table: the whitelist of trusted command-and-control peers
// (per-peer shared PSK + inbound anti-replay state) plus this device's own
// reboot-safe outbound sequence counter. Mirrors the LoRaWANConfig pattern — a
// bounded table in its OWN NVS namespace ("c2auth") with a reboot-safe,
// fail-closed, block-reserved counter — so there is NO BridgeConfig schema bump.
//
// A "peer" is the OTHER end of a C2 link. On the camera (BRIDGE_ROLE_CAMERA) a
// peer is a whitelisted COMMANDER (e.g. a paired repeater). On a repeater
// (BRIDGE_CAM_COMMANDER) a peer is a CAM it commands. The link is symmetric (the
// same 16-byte PSK on both ends), so one module serves both roles. The local
// device's own C2 id is its BridgeConfig::mtNodeId() (MAC-derived) — no new
// identity system.
//
// Auth / replay model (see LORACAM-SPEC §4.3):
//   - A peer is matched by peerId (the far end's C2 id).
//   - INBOUND anti-replay: accept a frame only if seq > the peer's rxHighWater,
//     then persist the new high-water — persist-on-accept, FAIL-CLOSED (a persist
//     failure DROPS the frame rather than risk a post-reboot replay window). Only
//     authenticated frames (valid CMAC, in-whitelist) ever reach acceptInboundSeq(),
//     so a non-key-holder cannot churn flash. This is LoRaWANConfig's TX counter
//     logic INVERTED for RX (store highest-SEEN, reject <=).
//   - OUTBOUND: nextTxSeq() is one reboot-safe monotonic counter (block-reserved,
//     fail-closed) for this device's own ACK/beacon frames.
// ---------------------------------------------------------------------------

#pragma once
#if defined(BRIDGE_ROLE_CAMERA) || defined(BRIDGE_CAM_COMMANDER)

#include <Arduino.h>
#include <stdint.h>

namespace CamC2Config {

constexpr size_t   MAX_PEERS    = 4;            // bounded whitelist
constexpr uint32_t SEQ_RESERVE  = 16;          // TX seq values reserved per NVS write
constexpr uint32_t SEQ_INVALID  = 0xFFFFFFFFu; // nextTxSeq() persist-fail sentinel → drop

constexpr uint8_t  FLAG_PRIMARY_MASTER = 0x01; // Q6: unsolicited-telemetry target

struct Peer {
    uint8_t  inUse;        // 0 = empty slot
    uint8_t  flags;        // bit0 = FLAG_PRIMARY_MASTER
    uint8_t  reserved[2];  // pad / future use (keep struct 4-byte aligned)
    uint32_t peerId;       // far-end C2 id (its BridgeConfig node id)
    uint8_t  psk[16];      // shared 16-byte C2 key (AES-128)
    uint32_t rxHighWater;  // highest accepted inbound seq from this peer (anti-replay)
};

void begin();        // load the peer table + per-peer rx high-water + tx seq from NVS
void saveTable();    // persist the peer table (NOT the rx/tx counter keys)
void debugDump();

// True if at least one slot is in use with a non-zero peerId.
bool anyConfigured();

// Match an inbound peerId to a stored peer, or nullptr. outIndex receives the
// table index (needed for acceptInboundSeq()).
const Peer *resolve(uint32_t peerId, int &outIndex);

// INBOUND anti-replay. Returns true — and durably records the new high-water —
// iff `seq` is strictly greater than this peer's highest accepted seq AND the new
// high-water was confirmed-written to NVS. Fail-closed: a stale/equal seq OR a
// persist failure returns false (caller MUST drop, never actuate).
bool acceptInboundSeq(int peerIndex, uint32_t seq);

// This device's next OUTBOUND C2 sequence (reboot-safe, block-reserved). Returns
// SEQ_INVALID on a persist failure → caller MUST NOT emit (a non-durable seq could
// repeat across reboot and be rejected as a replay by peers).
uint32_t nextTxSeq();

// Portal accessors.
size_t      peerCount();          // == MAX_PEERS
const Peer &peer(int i);
void        setPeer(int i, const Peer &p);
void        clearPeer(int i);
uint32_t    currentTxSeq();       // for the portal/debug display

}  // namespace CamC2Config

#endif  // BRIDGE_ROLE_CAMERA || BRIDGE_CAM_COMMANDER
