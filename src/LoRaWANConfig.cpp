// LoRaWANConfig.cpp — see LoRaWANConfig.h for design notes.

#include "LoRaWANConfig.h"
#include "BridgeConfig.h"   // Protocol enum (SRC_PROTO matching)

#include <Preferences.h>
#include <string.h>

namespace LoRaWANConfig {

static const char *NVS_NS    = "lwabp";
static const char *KEY_TABLE = "devs";

static Device   s_dev[MAX_DEVICES];
static uint32_t s_fcnt[MAX_DEVICES];          // next FCnt to issue (RAM)
static uint32_t s_fcntReserved[MAX_DEVICES];  // persisted high-water (>= s_fcnt)

static void fcntKey(int i, char *out, size_t cap) { snprintf(out, cap, "fc%d", i); }

void begin() {
    memset(s_dev, 0, sizeof(s_dev));
    for (size_t i = 0; i < MAX_DEVICES; i++) { s_fcnt[i] = 0; s_fcntReserved[i] = 0; }

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
    // Resume each device's FCnt at its persisted reservation high-water so an
    // uplink after a reboot never reuses a value the LNS already saw.
    char k[8];
    for (size_t i = 0; i < MAX_DEVICES; i++) {
        fcntKey((int)i, k, sizeof(k));
        uint32_t reserved = p.getUInt(k, 0);
        s_fcntReserved[i] = reserved;
        s_fcnt[i]         = reserved;
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

// `specificity` gates the resolve() priority pass: 2 = MT-node, 1 = protocol,
// 0 = ANY default. An entry only matches at the pass equal to its selector's
// specificity, so a more specific device always wins.
static bool matches(const Device &d, uint8_t srcProto, uint32_t srcId, int specificity) {
    if (!d.inUse || d.devAddr == 0) return false;
    switch (d.srcSel) {
        case SRC_MT_NODE: return specificity == 2 &&
                                 srcProto == BridgeConfig::PROTO_MT && srcId == d.srcMatch;
        case SRC_PROTO:   return specificity == 1 && srcProto == (uint8_t)d.srcMatch;
        case SRC_ANY:     return specificity == 0;
        default:          return false;
    }
}

const Device *resolve(uint8_t srcProto, uint32_t srcId, int &outIndex) {
    for (int spec = 2; spec >= 0; --spec) {
        for (size_t i = 0; i < MAX_DEVICES; i++) {
            if (matches(s_dev[i], srcProto, srcId, spec)) {
                outIndex = (int)i;
                return &s_dev[i];
            }
        }
    }
    outIndex = -1;
    return nullptr;
}

uint32_t nextFcnt(int i) {
    if (i < 0 || i >= (int)MAX_DEVICES) return 0;
    uint32_t v = s_fcnt[i];
    s_fcnt[i] += 1;
    if (s_fcnt[i] > s_fcntReserved[i]) {              // consumed the reservation
        s_fcntReserved[i] = s_fcnt[i] + FCNT_RESERVE;  // reserve the next block
        Preferences p;
        if (p.begin(NVS_NS, /*readOnly=*/false)) {
            char k[8]; fcntKey(i, k, sizeof(k));
            p.putUInt(k, s_fcntReserved[i]);
            p.end();
        }
    }
    return v;
}

size_t deviceCount() { return MAX_DEVICES; }

const Device &device(int i) {
    static Device empty = {};
    if (i < 0 || i >= (int)MAX_DEVICES) return empty;
    return s_dev[i];
}

void setDevice(int i, const Device &d) {
    if (i >= 0 && i < (int)MAX_DEVICES) s_dev[i] = d;
}

void clearDevice(int i) {
    if (i >= 0 && i < (int)MAX_DEVICES) memset(&s_dev[i], 0, sizeof(Device));
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
