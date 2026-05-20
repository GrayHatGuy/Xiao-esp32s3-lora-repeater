// BridgeConfig.h
// ---------------------------------------------------------------------------
// Single source of truth for runtime-configurable bridge settings. Replaces
// reading the BRIDGE_* build-flag macros directly throughout the code.
//
// Lifetime:
//   1. setup() calls begin() — loads NVS namespace 'bridgecfg' if present,
//      otherwise initialises every field from its build-flag default.
//   2. The CaptivePortal writes new values through the field accessors and
//      calls save(), which marks configured=true and flushes to NVS.
//   3. Downstream modules (MeshCoreConfig, the encoder helpers, the
//      bridgePacket pipeline) read from the public field accessors.
//
// Storage: one PersistedV1 blob in the 'bridgecfg' Preferences namespace.
// On a schema bump, increment the version byte and handle migration in
// begin() — old blobs older than the latest version fall back to defaults.
//
// Thread-safety: read-mostly. All writes come from CaptivePortal (running
// in setup() before tasks start) or from setup() initialisation. The
// radio tasks only ever read.
// ---------------------------------------------------------------------------

#pragma once

#include <Arduino.h>
#include <stdint.h>

namespace BridgeConfig {

constexpr size_t MT_NODE_ID_STR_MAX = 15;   // "!12345678" plus null
constexpr size_t MT_LONG_NAME_MAX   = 39;   // Meshtastic long_name canonical max 40
constexpr size_t MT_SHORT_NAME_MAX  = 8;    // Meshtastic short_name typical 4
constexpr size_t MC_KEY_HEX_LEN     = 32;   // 32 hex chars => 16 raw bytes
constexpr size_t MC_CHANNEL_NAME_MAX = 23;  // arbitrary; fits within marker budget

void begin();           // load from NVS or initialise from build-flag defaults
void save();            // persist current values + set configured=true
void resetToDefaults(); // erase NVS blob; next boot will use build-flag defaults
bool isConfigured();    // true once save() has been called at least once

// Accessors — reads are O(1) reference returns, fine to call from any task.
uint32_t    mtNodeId();
const char *mtNodeIdStr();
const char *mtLongName();
const char *mtShortName();
const char *mcKeyHex();           // 32-char lowercase hex string
const char *mcChannelName();
bool        positionEnabled();    // BRIDGE_MT_POSITION analogue
bool        telemetryEnabled();   // BRIDGE_MT_TELEMETRY analogue

// Setters — used by the captive portal to apply form input. Validation is
// caller-side; setters do bounds-clamping and string-null-termination only.
void setMtNodeId(uint32_t v);
void setMtNodeIdStr(const char *s);
void setMtLongName(const char *s);
void setMtShortName(const char *s);
void setMcKeyHex(const char *s);
void setMcChannelName(const char *s);
void setPositionEnabled(bool v);
void setTelemetryEnabled(bool v);

void debugDump();       // pretty-print all values to Serial

}  // namespace BridgeConfig
