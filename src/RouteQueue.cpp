// RouteQueue.cpp — see RouteQueue.h for design notes.

#include "RouteQueue.h"
#include <string.h>   // ps_malloc/malloc/free come via Arduino.h

RouteQueue::~RouteQueue() {
    if (_ring) free(_ring);
    if (_mutex) vSemaphoreDelete(_mutex);
}

bool RouteQueue::begin(size_t depth, uint32_t maxAgeMs, const char *name) {
    if (depth == 0) return false;
    size_t bytes = depth * sizeof(Entry);
    // Prefer PSRAM; fall back to internal RAM so a no-PSRAM board still works.
    _ring = (Entry *)ps_malloc(bytes);
    const char *where = "PSRAM";
    if (!_ring) { _ring = (Entry *)malloc(bytes); where = "internal RAM"; }
    if (!_ring) {
        Serial.printf("[RouteQueue:%s] alloc of %u B FAILED\n",
                      name ? name : "?", (unsigned)bytes);
        return false;
    }
    _cap      = depth;
    _head = _tail = _count = 0;
    _dropped  = 0;
    _maxAgeMs = maxAgeMs;
    _mutex    = xSemaphoreCreateMutex();
    Serial.printf("[RouteQueue:%s] depth=%u (%u B in %s) maxAge=%lu ms\n",
                  name ? name : "?", (unsigned)depth, (unsigned)bytes, where,
                  (unsigned long)maxAgeMs);
    return _mutex != nullptr;
}

bool RouteQueue::push(const uint8_t *data, size_t len) {
    if (!_ring || !data || len == 0 || len > LORA_MAX_PACKET) return false;
    xSemaphoreTake(_mutex, portMAX_DELAY);
    if (_count == _cap) {            // full -> drop oldest to make room
        _head = (_head + 1) % _cap;
        _count--;
        _dropped++;
    }
    Entry &e = _ring[_tail];
    memcpy(e.data, data, len);
    e.len       = (uint16_t)len;
    e.enqueueMs = millis();
    _tail  = (_tail + 1) % _cap;
    _count++;
    xSemaphoreGive(_mutex);
    return true;
}

bool RouteQueue::pop(Entry &out) {
    if (!_ring) return false;
    bool got = false;
    xSemaphoreTake(_mutex, portMAX_DELAY);
    uint32_t now = millis();
    while (_count > 0) {
        Entry &e = _ring[_head];
        _head = (_head + 1) % _cap;
        _count--;
        if (_maxAgeMs && (uint32_t)(now - e.enqueueMs) > _maxAgeMs) {
            _dropped++;             // stale — discard and look at the next one
            continue;
        }
        memcpy(out.data, e.data, e.len);
        out.len       = e.len;
        out.enqueueMs = e.enqueueMs;
        got = true;
        break;
    }
    xSemaphoreGive(_mutex);
    return got;
}

size_t RouteQueue::count() const {
    // _count is a single word; an unlocked read is acceptable for a hint.
    return _count;
}
