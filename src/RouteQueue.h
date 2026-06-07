// RouteQueue.h
// ---------------------------------------------------------------------------
// One per-destination outbound queue for the RX-priority pipeline
// (ROUTING-REDESIGN.md §3.2). After a received packet is decoded, de-duplicated
// and re-encoded for a destination radio, the finished on-air bytes are pushed
// here; that destination's TX scheduler later pops them, CAD-gates, and sends.
// Decoupling fan-out (cheap, in the RX path) from transmit (slow, on the air)
// is what lets RX stay live while a TX is pending — the whole point.
//
// Binding: the queue is bounded primarily by MAX AGE — a popped entry older
// than maxAgeMs is discarded rather than delivered, because stale mesh text is
// not worth airtime. A generous depth ceiling is the backstop; on overflow the
// policy is drop-OLDEST (the freshest traffic wins).
//
// Storage: the ring is allocated in PSRAM (ps_malloc) when available — the XIAO
// ESP32-S3 has it — so depth can be large without pressuring internal RAM;
// it falls back to the internal heap if PSRAM is absent. Each slot is a full
// LORA_MAX_PACKET so any packet fits without sub-allocation.
//
// Thread-safety: an internal mutex guards all access. push() runs on the source
// radio's task; pop() runs on the destination radio's task — different tasks,
// so the lock is required.
// ---------------------------------------------------------------------------

#pragma once

#include <Arduino.h>
#include <stdint.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "LoraRadio.h"   // LORA_MAX_PACKET

class RouteQueue {
public:
    struct Entry {
        uint8_t  data[LORA_MAX_PACKET];
        uint16_t len;
        uint32_t enqueueMs;
    };

    RouteQueue() = default;
    ~RouteQueue();

    // Allocate the ring (PSRAM-preferred) and create the mutex. Returns false
    // if allocation fails. `name` is used only for the diagnostic print.
    bool begin(size_t depth, uint32_t maxAgeMs, const char *name);

    // Append a finished on-air packet. On a full ring the oldest entry is
    // dropped to make room (drop-oldest). Returns false only if len is invalid
    // or the queue was never begun().
    bool push(const uint8_t *data, size_t len);

    // Pop the oldest entry that is still within maxAgeMs into `out`, discarding
    // any expired entries ahead of it. Returns false if the queue is empty
    // (after expiry pruning).
    bool pop(Entry &out);

    size_t   count() const;
    uint32_t dropped() const { return _dropped; }   // lifetime drop+expire count

private:
    Entry            *_ring     = nullptr;
    size_t            _cap      = 0;
    size_t            _head     = 0;   // oldest
    size_t            _tail     = 0;   // next free
    size_t            _count    = 0;
    uint32_t          _maxAgeMs = 0;
    uint32_t          _dropped  = 0;
    SemaphoreHandle_t _mutex    = nullptr;
};
