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
constexpr uint8_t  FLAG_TAG_SRC = 0x01;  // Device.flags: prepend a [proto][srcId] source tag
constexpr uint32_t FCNT_INVALID = 0xFFFFFFFFu;  // nextFcnt*() sentinel: a durable
                                                // reservation could not be written
                                                // → caller MUST drop the uplink (a
                                                // non-durable FCnt risks a reboot replay)

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
    uint8_t  flags;        // bit0 = FLAG_TAG_SRC (prepend [proto][srcId] to FRMPayload)
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

// True if any in-use slot holds this DevAddr (used to warn about a build-flag
// fallback identity colliding with a provisioned per-source device).
bool hasDevAddr(uint32_t devAddr);

// Map a source radio's LoRa sync word to a BridgeConfig::Protocol value
// (0x2B->MT=1, 0x12->MC=2, 0x42->RNS=3, 0x34->LoRaWAN=5, else CUSTOM=4). Public
// so the encode seam can stamp the small protocol enum (not the raw sync word)
// into the [proto][srcId] source tag, matching tools/chirpstack-codec.js.
uint8_t protoOf(uint8_t syncWord);

// Resolve the best-match device for an inbound source (srcSync = the source
// radio's LoRa sync word, mapped to a protocol internally), or nullptr.
// Preference: an exact MT-node match, then a protocol match, then a SRC_ANY
// default. outIndex receives the table index (needed for nextFcnt()).
const Device *resolve(uint8_t srcSync, uint32_t srcId, int &outIndex);

// Return the next FCntUp for a device and advance it. The counter is persisted
// by DevAddr (NOT slot index) so an identity keeps its high-water across portal
// row moves, and a fresh reservation is written BEFORE the first value of each
// block is issued (fail-closed): if the NVS write cannot be confirmed this
// returns FCNT_INVALID and the caller MUST drop the uplink rather than emit a
// counter that isn't durably reserved (which a reboot could replay).
uint32_t nextFcnt(int deviceIndex);

// Same reboot-safe, DevAddr-keyed FCntUp for an identity NOT in the device
// table (the build-flag fallback creds). One identity is cached at a time.
// Returns FCNT_INVALID on a persist failure, like nextFcnt().
uint32_t nextFcntForDevAddr(uint32_t devAddr);

// Portal accessors.
size_t        deviceCount();              // == MAX_DEVICES
const Device &device(int i);
void          setDevice(int i, const Device &d);
void          clearDevice(int i);
uint32_t      currentFcnt(int i);         // for the portal/debug display

}  // namespace LoRaWANConfig
