// DedupCache.cpp — see DedupCache.h for design notes.

#include "DedupCache.h"

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace DedupCache {

struct Entry {
    uint32_t hash;
    uint32_t stampMs;   // millis() at last record; 0 = empty slot
    bool     used;
};

static Entry s_table[BRIDGE_DEDUP_TABLE_SIZE];

static SemaphoreHandle_t s_mutex = nullptr;
static inline void lock()   { if (s_mutex) xSemaphoreTake(s_mutex, portMAX_DELAY); }
static inline void unlock() { if (s_mutex) xSemaphoreGive(s_mutex); }

void begin() {
    if (!s_mutex) s_mutex = xSemaphoreCreateMutex();
    for (size_t i = 0; i < BRIDGE_DEDUP_TABLE_SIZE; i++) s_table[i].used = false;
}

uint32_t hash(const uint8_t *body, size_t len, uint32_t srcId) {
    // FNV-1a 32-bit.
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < len; i++) {
        h ^= body[i];
        h *= 16777619u;
    }
    for (int b = 0; b < 4; b++) {           // fold srcId, little-endian
        h ^= (uint8_t)(srcId >> (8 * b));
        h *= 16777619u;
    }
    return h;
}

// Caller holds the lock. True if `h` is present and still inside the window.
static bool isSeen(uint32_t h, uint32_t now) {
    for (size_t i = 0; i < BRIDGE_DEDUP_TABLE_SIZE; i++) {
        if (s_table[i].used && s_table[i].hash == h &&
            (uint32_t)(now - s_table[i].stampMs) < (uint32_t)BRIDGE_DEDUP_TTL_MS)
            return true;
    }
    return false;
}

// Caller holds the lock. Insert/refresh `h`, reusing an empty/expired slot or
// evicting the oldest. Refreshing an existing entry's timestamp keeps a live
// loop suppressed for as long as its echoes keep arriving.
static void insert(uint32_t h, uint32_t now) {
    int    freeSlot  = -1;
    int    oldest    = 0;
    uint32_t oldestAge = 0;
    for (size_t i = 0; i < BRIDGE_DEDUP_TABLE_SIZE; i++) {
        if (s_table[i].used && s_table[i].hash == h) {   // refresh in place
            s_table[i].stampMs = now;
            return;
        }
        if (!s_table[i].used) {
            if (freeSlot < 0) freeSlot = (int)i;
        } else if ((uint32_t)(now - s_table[i].stampMs) >= (uint32_t)BRIDGE_DEDUP_TTL_MS) {
            if (freeSlot < 0) freeSlot = (int)i;          // expired == free
        } else {
            uint32_t age = (uint32_t)(now - s_table[i].stampMs);
            if (age >= oldestAge) { oldestAge = age; oldest = (int)i; }
        }
    }
    int slot = (freeSlot >= 0) ? freeSlot : oldest;       // else evict oldest
    s_table[slot].hash    = h;
    s_table[slot].stampMs = now;
    s_table[slot].used    = true;
}

bool seenAndRecord(uint32_t h) {
    uint32_t now = millis();
    lock();
    bool seen = isSeen(h, now);
    insert(h, now);   // record (new) or refresh (echo) either way
    unlock();
    return seen;
}

void record(uint32_t h) {
    uint32_t now = millis();
    lock();
    insert(h, now);
    unlock();
}

}  // namespace DedupCache
