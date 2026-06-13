// VirtualNodeMap.cpp — see VirtualNodeMap.h for design notes.

#include "VirtualNodeMap.h"

#include <string.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace VirtualNodeMap {

struct Entry {
    uint32_t id;
    uint32_t lastNodeInfoMs;   // 0 = never advertised
    bool     used;
};

static Entry s_table[BRIDGE_VIRT_NODES_MAX];

static SemaphoreHandle_t s_mutex = nullptr;
static inline void lock()   { if (s_mutex) xSemaphoreTake(s_mutex, portMAX_DELAY); }
static inline void unlock() { if (s_mutex) xSemaphoreGive(s_mutex); }

void begin() {
    if (!s_mutex) s_mutex = xSemaphoreCreateMutex();
    for (size_t i = 0; i < BRIDGE_VIRT_NODES_MAX; i++) s_table[i].used = false;
}

// A reserved MT node id is one Meshtastic itself treats specially: the low 256
// (network/role-reserved) and the broadcast id. Mapping onto one would make the
// virtual node invisible or behave oddly, so we nudge past it.
static inline bool isReserved(uint32_t id) {
    return id <= 0x000000FFu || id == 0xFFFFFFFFu;
}

// FNV-1a 32-bit over "MC|" + label. Domain-prefixed so the MC namespace can't
// collide by construction with any future "RNS|"/"LW|" virtual scheme.
static uint32_t hashLabel(const char *label) {
    uint32_t h = 2166136261u;
    const char *prefix = "MC|";
    for (const char *p = prefix; *p; p++) { h ^= (uint8_t)*p; h *= 16777619u; }
    for (const char *p = label; p && *p; p++) { h ^= (uint8_t)*p; h *= 16777619u; }
    return h;
}

// Caller holds the lock. Touch/create an LRU entry for `id`.
static void touch(uint32_t id) {
    int freeSlot = -1, oldest = 0;
    uint32_t oldestStamp = 0xFFFFFFFFu;
    for (size_t i = 0; i < BRIDGE_VIRT_NODES_MAX; i++) {
        if (s_table[i].used && s_table[i].id == id) return;     // already present
        if (!s_table[i].used) { if (freeSlot < 0) freeSlot = (int)i; }
        else if (s_table[i].lastNodeInfoMs <= oldestStamp) {
            oldestStamp = s_table[i].lastNodeInfoMs; oldest = (int)i;
        }
    }
    int slot = (freeSlot >= 0) ? freeSlot : oldest;             // evict oldest
    s_table[slot].id             = id;
    s_table[slot].lastNodeInfoMs = 0;                           // never advertised
    s_table[slot].used           = true;
}

uint32_t idForLabel(const char *label, uint32_t avoidId) {
    uint32_t id = hashLabel(label);
    // Nudge out of reserved ranges and off the bridge's own id. Re-mix rather
    // than ++ so we don't cluster sequential ids after a reserved hit.
    int guard = 0;
    while ((isReserved(id) || id == avoidId) && guard++ < 8) {
        id ^= 0x9E3779B9u; id *= 16777619u;
        if (isReserved(id)) id |= 0x00010000u;                  // force out of low range
    }
    lock();
    touch(id);
    unlock();
    return id;
}

bool nodeInfoDue(uint32_t id) {
    uint32_t now = millis();
    bool due = false;
    lock();
    for (size_t i = 0; i < BRIDGE_VIRT_NODES_MAX; i++) {
        if (s_table[i].used && s_table[i].id == id) {
            if (s_table[i].lastNodeInfoMs == 0 ||
                (uint32_t)(now - s_table[i].lastNodeInfoMs) >= (uint32_t)BRIDGE_VIRT_NODEINFO_PERIOD_MS) {
                s_table[i].lastNodeInfoMs = now ? now : 1;      // never store 0 (=means "never")
                due = true;
            }
            break;
        }
    }
    unlock();
    return due;
}

}  // namespace VirtualNodeMap
