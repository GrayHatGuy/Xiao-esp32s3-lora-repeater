// VirtualNodeMap.h
// ---------------------------------------------------------------------------
// Source-identity preservation for the cross-protocol MC -> MT path
// (V8.2-SPEC.md §5.3).
//
// MeshCore group text has no per-sender id field on the wire — the sender's
// name rides inside the body by convention ("<name>: <text>"). Meshtastic, by
// contrast, wants a 32-bit node id in the packet header. To make a bridged
// MeshCore message appear to a Meshtastic client as coming from its real
// sender (rather than from the bridge), we mint a DETERMINISTIC virtual MT node
// id from the MeshCore sender name and advertise a synthetic NodeInfo for it so
// the MT client shows the message as "from Alice @MC".
//
// Deterministic: virtualId = FNV-1a32("MC|" + name), nudged out of the MT
// reserved ranges. The same MC sender therefore maps to the same MT node id
// across reboots and even across multiple independent bridges. A 32-bit
// collision with a real node is possible but negligible at mesh scale and is a
// documented residual risk (V8.2-SPEC §11).
//
// This map exists only to RATE-LIMIT the synthetic NodeInfo emissions (so we
// don't re-advertise a node on every message). The id itself is a pure
// function of the name, so a name absent from the table still maps correctly;
// the table just won't have a "last advertised" timestamp for it yet.
//
// Thread-safety: an internal mutex guards the table. In the 2-radio bridge only
// the MeshCore-source task calls in, but the lock keeps it correct if a future
// build routes more than one MeshCore radio into MT.
// ---------------------------------------------------------------------------

#pragma once

#include <Arduino.h>
#include <stdint.h>

// Max distinct virtual nodes tracked for NodeInfo rate-limiting (LRU by last
// advertisement). Each entry is 8 B, so the default table is 256 B.
#ifndef BRIDGE_VIRT_NODES_MAX
  #define BRIDGE_VIRT_NODES_MAX 32
#endif

// How often (ms) to re-advertise a virtual node's NodeInfo while it is active.
// First sighting always advertises; then no more often than this.
#ifndef BRIDGE_VIRT_NODEINFO_PERIOD_MS
  #define BRIDGE_VIRT_NODEINFO_PERIOD_MS 900000UL   // 15 min
#endif

namespace VirtualNodeMap {

// Create the internal mutex. Call once from setup(). Safe to call repeatedly.
void begin();

// Deterministic virtual MT node id for a label (an MC sender name, or a
// per-channel catch-all label). Never returns a reserved id (0x00000000..
// 0x000000FF or 0xFFFFFFFF). Also ensures an LRU entry exists so nodeInfoDue()
// can rate-limit advertisements for it. `avoidId` (e.g. the bridge's own MT
// node id) is nudged past if the hash lands on it.
uint32_t idForLabel(const char *label, uint32_t avoidId);

// True if a synthetic NodeInfo should be emitted for `id` now — i.e. it has
// never been advertised, or BRIDGE_VIRT_NODEINFO_PERIOD_MS has elapsed since
// the last one. Marks it advertised-now when it returns true.
bool nodeInfoDue(uint32_t id);

}  // namespace VirtualNodeMap
