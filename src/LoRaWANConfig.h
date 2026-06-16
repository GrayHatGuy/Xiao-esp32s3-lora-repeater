// LoRaWANConfig.h
// ---------------------------------------------------------------------------
// v8.4 (P2): persisted per-source LoRaWAN ABP device identities (M1) + a
// monotonic, reboot-safe uplink frame counter per device.
//
// The v8.4 keyed ABP uplink encoder (src/LoRaWANCrypto.h) needs, per source, an
// ABP identity — DevAddr + NwkSKey + AppSKey + FPort — plus an FCntUp that never
// goes backwards across reboots (anti-replay; a wrong/low FCnt is dropped by the
// LNS). P1 sourced ONE identity from build flags with an in-RAM counter; P2
// moves this into a small NVS-persisted table the captive portal edits, so a
// deployed bridge keeps its credentials and counters without a rebuild.
//
// Storage is a dedicated NVS namespace ("lwabp"), independent of the main
// BridgeConfig blob — the ABP feature is opt-in and isolated, so there is no
// BridgeConfig schema migration. The device table is one blob ("devs"); each
// device's reserved FCnt high-water mark is a separate small key ("fc0".."fcN"),
// advanced by BLOCK RESERVATION (one NVS write per FCNT_RESERVE uplinks, not per
// uplink) so the counter survives a reboot without churning flash.
//
// Mirrors the MeshCoreConfig / MeshtasticConfig "resolve()" pattern.
// ---------------------------------------------------------------------------

#pragma once

#include <Arduino.h>
#include <stdint.h>

namespace LoRaWANConfig {

constexpr size_t   MAX_DEVICES  = 4;     // bounded per-source ABP identity table
constexpr uint32_t FCNT_RESERVE = 32;    // FCnt values reserved per NVS write

// How a device entry is matched to an inbound source at the encode seam.
enum SrcSel : uint8_t {
    SRC_ANY     = 0,   // catch-all: matches any source (a default device)
    SRC_MT_NODE = 1,   // match a specific Meshtastic node id (srcMatch = node id)
    SRC_PROTO   = 2,   // match a whole source protocol        (srcMatch = Protocol)
};

struct Device {
    uint8_t  inUse;        // 0 = empty slot
    uint8_t  srcSel;       // SrcSel
    uint8_t  fport;        // 1..223 (0 = MAC-only; 224+ reserved by LoRaWAN)
    uint8_t  _pad;
    uint32_t srcMatch;     // node id / protocol per srcSel
    uint32_t devAddr;      // ABP DevAddr
    uint8_t  nwkSKey[16];  // network session key (MIC)
    uint8_t  appSKey[16];  // application session key (FRMPayload)
};

void begin();        // load the table + FCnt high-water marks from NVS
void saveTable();    // persist the device table (NOT the FCnt keys)
void debugDump();

// True if at least one slot is in use with a non-zero DevAddr.
bool anyConfigured();

// Resolve the best-match device for an inbound source, or nullptr. Preference:
// an exact MT-node match, then a protocol match, then a SRC_ANY default.
// outIndex receives the table index (needed for nextFcnt()).
const Device *resolve(uint8_t srcProto, uint32_t srcId, int &outIndex);

// Return the next FCntUp for a device and advance it; persists a fresh
// reservation every FCNT_RESERVE uplinks so the counter survives reboots
// without a flash write per packet.
uint32_t nextFcnt(int deviceIndex);

// Portal accessors.
size_t        deviceCount();              // == MAX_DEVICES
const Device &device(int i);
void          setDevice(int i, const Device &d);
void          clearDevice(int i);
uint32_t      currentFcnt(int i);         // for the portal/debug display

}  // namespace LoRaWANConfig
