// DedupCache.h
// ---------------------------------------------------------------------------
// TTL-windowed content hash cache — the loop/dup guard that REPLACES the old
// "[MT]/[MC]/[rns]" text marker (V8.2-SPEC.md §3.1).
//
// Why a content hash instead of a prepended marker:
//   - The marker polluted the far-side message, only worked for text, and
//     false-dropped a genuine user message that happened to start with "[MT".
//   - Across a cross-protocol re-encode every on-air byte changes; only the
//     decoded *content* is invariant. So we hash the decoded body (+ srcId),
//     not the raw bytes, and forward the body verbatim — no prepend.
//
// How loops are still prevented without the marker:
//   - On RX, after decoding, the bridge calls seenAndRecord(hash(body,srcId)).
//     A hash already in the window => loop/duplicate => drop. This catches both
//     mesh-flood replays of one message AND the same packet heard on two radios.
//   - On every packet the bridge EMITS, it calls record(hash(body,txSrcId))
//     with the srcId it stamped into the re-encoded packet (the bridge's own MT
//     node id for an MT destination; 0 for MeshCore, which has no stable
//     per-sender id). When that emission is later heard back, its hash matches
//     the recorded one => dropped. This is what closes the loop the marker used
//     to close, including same-channel cross-band MT/MC twins.
//
// srcId policy: fold in the Meshtastic 32-bit src so identical text from two
// different senders is not false-dropped. MeshCore GRP_TXT carries no stable
// per-sender id, so MeshCore uses srcId = 0 (content-only) and accepts the
// slightly higher collision chance, exactly as the design notes.
//
// Storage: a fixed RAM table with oldest-slot eviction, guarded by an internal
// mutex — a sibling of NodeDB, same concurrency model (both radio tasks may
// call in). Entries are tiny (8 B), so the table lives in internal RAM, not
// PSRAM.
// ---------------------------------------------------------------------------

#pragma once

#include <Arduino.h>
#include <stdint.h>

namespace DedupCache {

// Window after which a recorded hash is considered stale and ignored. Mesh
// flood replays and loop echoes recur within seconds; 60 s covers slow relays
// while keeping the false-drop window (two senders, identical short text)
// short. Override with -DBRIDGE_DEDUP_TTL_MS=... in platformio.ini.
#ifndef BRIDGE_DEDUP_TTL_MS
  #define BRIDGE_DEDUP_TTL_MS 60000UL
#endif

// Number of distinct recent (body,srcId) hashes remembered. 512 * 12 B = 6 KB.
// Sized with headroom so a busy bridge doesn't saturate the table within one
// TTL window (saturation would force eviction of still-live entries and let a
// duplicate slip through). Override with -D as needed.
#ifndef BRIDGE_DEDUP_TABLE_SIZE
  #define BRIDGE_DEDUP_TABLE_SIZE 512
#endif

// Create the internal mutex. Call once from setup() before the radio tasks
// start. Safe to call more than once.
void begin();

// FNV-1a 32-bit over the body bytes followed by srcId (little-endian). Pass
// srcId = 0 for protocols without a stable per-sender id.
uint32_t hash(const uint8_t *body, size_t len, uint32_t srcId);

// If `h` is already in the live window, refresh its timestamp and return true
// (caller should treat the packet as a loop/duplicate and drop it). Otherwise
// record `h` and return false. Used on RX.
bool seenAndRecord(uint32_t h);

// Unconditionally insert/refresh `h`. Used to remember a packet the bridge has
// just emitted, so its echo is recognised as a loop on the next RX.
void record(uint32_t h);

}  // namespace DedupCache
