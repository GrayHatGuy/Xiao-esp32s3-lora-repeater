// LoRaWANConfig.cpp — see LoRaWANConfig.h for design notes.

#include "LoRaWANConfig.h"
#include "BridgeConfig.h"   // Protocol enum (SRC_PROTO matching)
#include "SerialLog.h"      // serialized console path (anti-garble); FCnt errs run in radioTask

#include <Preferences.h>
#include <string.h>

namespace LoRaWANConfig {

static const char *NVS_NS    = "lwabp";
static const char *KEY_TABLE = "devs";

static Device   s_dev[MAX_DEVICES];
static uint32_t s_fcnt[MAX_DEVICES];          // next FCnt to issue (RAM)
static uint32_t s_fcntReserved[MAX_DEVICES];  // RAM high-water (== persisted on success)
static uint32_t s_fcntPersisted[MAX_DEVICES]; // last value CONFIRMED written to NVS

// Build-flag fallback identity (not in the device table). One identity cached.
static uint32_t s_extDevAddr  = 0;
static uint32_t s_extFcnt     = 0;
static uint32_t s_extReserved = 0;            // == persisted high-water for s_extDevAddr

// FCnt is persisted keyed by DevAddr (NOT slot index) so an identity keeps its
// high-water across portal row moves / clear+re-add. Key fits NVS's 15-char cap:
// "fc_" + 8 hex = 11 chars.
static void fcntKeyForAddr(uint32_t devAddr, char *out, size_t cap) {
    snprintf(out, cap, "fc_%08lx", (unsigned long)devAddr);
}

// Read a DevAddr's persisted reservation high-water (0 if none / no namespace).
static uint32_t loadReservedForAddr(uint32_t devAddr) {
    if (devAddr == 0) return 0;
    Preferences p;
    if (!p.begin(NVS_NS, /*readOnly=*/true)) return 0;
    char k[16]; fcntKeyForAddr(devAddr, k, sizeof(k));
    uint32_t v = p.getUInt(k, 0);
    p.end();
    return v;
}

// Durably persist a DevAddr's reservation high-water. Returns true only on a
// CONFIRMED write (begin RW ok AND putUInt wrote 4 bytes).
static bool persistReservedForAddr(uint32_t devAddr, uint32_t val) {
    if (devAddr == 0) return false;
    Preferences p;
    if (!p.begin(NVS_NS, /*readOnly=*/false)) return false;
    char k[16]; fcntKeyForAddr(devAddr, k, sizeof(k));
    size_t wrote = p.putUInt(k, val);
    p.end();
    return wrote == sizeof(uint32_t);
}

void begin() {
    memset(s_dev, 0, sizeof(s_dev));
    for (size_t i = 0; i < MAX_DEVICES; i++) {
        s_fcnt[i] = s_fcntReserved[i] = s_fcntPersisted[i] = 0;
    }
    s_extDevAddr = s_extFcnt = s_extReserved = 0;

    Preferences p;
    if (!p.begin(NVS_NS, /*readOnly=*/true)) {
        Serial.println("[LoRaWANConfig] no NVS namespace yet (no ABP devices configured)");
        return;
    }
    size_t got = p.getBytesLength(KEY_TABLE);
    if (got == sizeof(s_dev)) {
        p.getBytes(KEY_TABLE, s_dev, sizeof(s_dev));
    } else if (got != 0) {
        Serial.printf("[LoRaWANConfig] device table size mismatch (%u B); ignoring\n",
                      (unsigned)got);
        memset(s_dev, 0, sizeof(s_dev));
    }
    // Resume each device's FCnt at its persisted (DevAddr-keyed) reservation
    // high-water so an uplink after a reboot never reuses a value the LNS saw.
    char k[16];
    for (size_t i = 0; i < MAX_DEVICES; i++) {
        if (s_dev[i].devAddr == 0) continue;
        fcntKeyForAddr(s_dev[i].devAddr, k, sizeof(k));
        uint32_t reserved = p.getUInt(k, 0);
        s_fcntReserved[i] = s_fcnt[i] = s_fcntPersisted[i] = reserved;
    }
    p.end();
    Serial.printf("[LoRaWANConfig] loaded %u ABP device slots (anyConfigured=%d)\n",
                  (unsigned)MAX_DEVICES, anyConfigured() ? 1 : 0);
    debugDump();
}

void saveTable() {
    Preferences p;
    if (!p.begin(NVS_NS, /*readOnly=*/false)) {
        Serial.println("[LoRaWANConfig] saveTable: prefs.begin RW failed");
        return;
    }
    size_t wrote = p.putBytes(KEY_TABLE, s_dev, sizeof(s_dev));
    p.end();
    Serial.printf("[LoRaWANConfig] device table saved (%u B)\n", (unsigned)wrote);
}

bool anyConfigured() {
    for (size_t i = 0; i < MAX_DEVICES; i++)
        if (s_dev[i].inUse && s_dev[i].devAddr != 0) return true;
    return false;
}

bool hasDevAddr(uint32_t devAddr) {
    if (devAddr == 0) return false;
    for (size_t i = 0; i < MAX_DEVICES; i++)
        if (s_dev[i].inUse && s_dev[i].devAddr == devAddr) return true;
    return false;
}

// `specificity` gates the resolve() priority pass: 2 = MT-node, 1 = protocol,
// 0 = ANY default. An entry only matches at the pass equal to its selector's
// specificity, so a more specific device always wins.
// Map a source radio's LoRa sync word to a BridgeConfig::Protocol for matching.
uint8_t protoOf(uint8_t sync) {
    if (sync == 0x2B) return BridgeConfig::PROTO_MT;
    if (sync == 0x12) return BridgeConfig::PROTO_MC;
    if (sync == 0x42) return BridgeConfig::PROTO_RNS;
    if (sync == 0x34) return BridgeConfig::PROTO_LORAWAN;
    return BridgeConfig::PROTO_CUSTOM;
}

static bool matches(const Device &d, uint8_t proto, uint32_t srcId, int specificity) {
    if (!d.inUse || d.devAddr == 0) return false;
    switch (d.srcSel) {
        case SRC_MT_NODE: return specificity == 2 &&
                                 proto == BridgeConfig::PROTO_MT && srcId == d.srcMatch;
        case SRC_PROTO:   return specificity == 1 && proto == (uint8_t)d.srcMatch;
        case SRC_ANY:     return specificity == 0;
        default:          return false;
    }
}

const Device *resolve(uint8_t srcSync, uint32_t srcId, int &outIndex) {
    uint8_t proto = protoOf(srcSync);
    for (int spec = 2; spec >= 0; --spec) {
        for (size_t i = 0; i < MAX_DEVICES; i++) {
            if (matches(s_dev[i], proto, srcId, spec)) {
                outIndex = (int)i;
                return &s_dev[i];
            }
        }
    }
    outIndex = -1;
    return nullptr;
}

// Fail-closed reserve-before-issue core, shared by both FCnt entry points.
// `fcnt`/`persisted` are the caller's RAM counter + confirmed-NVS high-water for
// `devAddr`. On a block boundary it persists the NEXT reservation FIRST; only on
// a confirmed write does it advance and return the value. Returns FCNT_INVALID
// (without advancing) if the space is exhausted or the write cannot be confirmed.
static uint32_t advanceFcnt(uint32_t devAddr, uint32_t &fcnt, uint32_t &persisted) {
    uint32_t v = fcnt;
    if (v >= persisted) {                              // v not yet durably reserved
        if (v > UINT32_MAX - 1 - FCNT_RESERVE) {       // would wrap → space exhausted
            SerialLog::logf("[LoRaWANConfig] FCNT_EXHAUSTED addr=0x%08lx (rejoin/rekey)\n",
                            (unsigned long)devAddr);
            return FCNT_INVALID;
        }
        uint32_t nr = v + 1 + FCNT_RESERVE;
        if (!persistReservedForAddr(devAddr, nr)) {
            SerialLog::logf("[LoRaWANConfig] FCNT_PERSIST_FAIL addr=0x%08lx — dropping uplink\n",
                            (unsigned long)devAddr);
            return FCNT_INVALID;                        // fail closed: caller drops
        }
        persisted = nr;
    }
    fcnt = v + 1;
    return v;
}

uint32_t nextFcnt(int i) {
    if (i < 0 || i >= (int)MAX_DEVICES) return FCNT_INVALID;
    uint32_t devAddr = s_dev[i].devAddr;
    if (devAddr == 0) return FCNT_INVALID;
    uint32_t v = advanceFcnt(devAddr, s_fcnt[i], s_fcntPersisted[i]);
    s_fcntReserved[i] = s_fcntPersisted[i];
    return v;
}

uint32_t nextFcntForDevAddr(uint32_t devAddr) {
    if (devAddr == 0) return FCNT_INVALID;
    if (devAddr != s_extDevAddr) {                     // (re)bind: load its high-water
        s_extDevAddr  = devAddr;
        s_extFcnt     = s_extReserved = loadReservedForAddr(devAddr);
    }
    return advanceFcnt(devAddr, s_extFcnt, s_extReserved);
}

size_t deviceCount() { return MAX_DEVICES; }

const Device &device(int i) {
    static Device empty = {};
    if (i < 0 || i >= (int)MAX_DEVICES) return empty;
    return s_dev[i];
}

void setDevice(int i, const Device &d) {
    if (i < 0 || i >= (int)MAX_DEVICES) return;
    // If this slot's identity changes, re-seed the RAM counter from the NEW
    // DevAddr's persisted high-water so a moved/re-added device never inherits
    // the previous identity's counter (which could replay below the LNS).
    if (d.devAddr != s_dev[i].devAddr) {
        uint32_t reserved = loadReservedForAddr(d.devAddr);
        s_fcntReserved[i] = s_fcnt[i] = s_fcntPersisted[i] = reserved;
    }
    s_dev[i] = d;
}

void clearDevice(int i) {
    if (i < 0 || i >= (int)MAX_DEVICES) return;
    memset(&s_dev[i], 0, sizeof(Device));
    s_fcntReserved[i] = s_fcnt[i] = s_fcntPersisted[i] = 0;
}

uint32_t currentFcnt(int i) {
    if (i < 0 || i >= (int)MAX_DEVICES) return 0;
    return s_fcnt[i];
}

void debugDump() {
    for (size_t i = 0; i < MAX_DEVICES; i++) {
        const Device &d = s_dev[i];
        if (!d.inUse) continue;
        Serial.printf("[LoRaWANConfig] dev%u sel=%u match=0x%08lx devaddr=0x%08lx "
                      "fport=%u fcnt=%lu\n",
                      (unsigned)i, (unsigned)d.srcSel, (unsigned long)d.srcMatch,
                      (unsigned long)d.devAddr, (unsigned)d.fport,
                      (unsigned long)s_fcnt[i]);
    }
}

}  // namespace LoRaWANConfig
