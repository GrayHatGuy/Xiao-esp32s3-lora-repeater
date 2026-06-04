// NodeDB.h
// ---------------------------------------------------------------------------
// Persistent cache of Meshtastic sender identities, keyed by 32-bit node ID.
//
// Populated by the bridge whenever it decodes a NODEINFO_APP packet on the
// MT side (see extractMeshtasticNodeInfo() in MeshDecoderDebug.h), then
// consulted by bridgePacket() so MT -> MC bridged messages can carry the
// actual sender's short_name ("[MT KN5J] ...") instead of an anonymous
// "[MT]" prefix.
//
// Storage model:
//   - Full table is kept in RAM (MAX_NODES entries, ~50 B each).
//   - Persisted to ESP32 NVS via Arduino Preferences as a single blob.
//   - Reads are O(N) linear search; N <= 64 so this is cheap.
//   - LRU eviction (by in-RAM lastSeenMs) kicks in when the table is full.
//
// Thread-safety: all public functions take an internal mutex (created in
// begin()), so multiple radio tasks may call upsert()/lookupShortName()
// concurrently. This matters from the quad routing-matrix onward, where more
// than one Meshtastic-facing radio task can touch NodeDB.
// ---------------------------------------------------------------------------

#pragma once

#include <Arduino.h>
#include <stdint.h>

namespace NodeDB {

constexpr size_t MAX_NODES      = 64;
constexpr size_t MAX_SHORT_NAME = 8;    // Meshtastic short_name is typically 4 chars; allow 8.
constexpr size_t MAX_LONG_NAME  = 40;   // Meshtastic long_name canonical max is 40.

struct Entry {
    uint32_t nodeId;
    uint32_t lastSeenMs;                // in-RAM only; not persisted
    char     shortName[MAX_SHORT_NAME + 1];
    char     longName [MAX_LONG_NAME  + 1];
};

// Load the table from NVS and create the internal mutex. Call once from
// setup() before the radio tasks start serving packets. Safe to call
// repeatedly; subsequent calls are no-ops aside from a debug print.
void begin();

// Insert or update an entry. If the table is full, evicts the entry with
// the oldest lastSeenMs. Empty/broadcast nodeIds are rejected. Returns
// true on success.
bool upsert(uint32_t nodeId, const char *shortName, const char *longName);

// Copy the short name for nodeId into `out` (always null-terminated when
// outCap > 0) and return true if found; returns false with out[0]='\0' if
// not known. Copy-based — rather than returning a pointer into the table —
// so the result stays valid even if another task mutates the table
// concurrently. Also bumps that entry's lastSeenMs.
bool lookupShortName(uint32_t nodeId, char *out, size_t outCap);

// Pretty-print the table to Serial. Useful for diagnostics; call from
// setup() right after begin() to see what was restored from flash.
void debugDump();

}  // namespace NodeDB
