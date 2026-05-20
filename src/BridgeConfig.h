// BridgeConfig.h
// ---------------------------------------------------------------------------
// Single source of truth for runtime-configurable bridge settings, persisted
// in NVS namespace 'bridgecfg'.
//
// As of v2 the channel config is per-radio: each radio slot carries its own
// channel name + key string, so the bridge can relay same-protocol between
// two channels (MC↔MC, MT↔MT) as well as cross-protocol (MT↔MC). A radio's
// PROTOCOL is still a build-flag decision (LORA_RADIO*_SYNC_WORD); only the
// channel name/key are stored here and editable in the captive portal.
//
// The channel key string is interpreted per the radio's protocol:
//   - MeshCore  radio: 32-char hex AES key
//   - Meshtastic radio: base64 PSK ("" = LongFast default)
//   - Reticulum radio: unused
//
// Storage: one PersistedV3 blob. begin() migrates a v2 blob forward.
// Thread-safety: read-mostly; writes come only from CaptivePortal / setup().
// ---------------------------------------------------------------------------

#pragma once

#include <Arduino.h>
#include <stdint.h>

namespace BridgeConfig {

constexpr size_t MT_NODE_ID_STR_MAX     = 15;   // "!12345678" plus null
constexpr size_t MT_LONG_NAME_MAX       = 39;   // Meshtastic long_name canonical max 40
constexpr size_t MT_SHORT_NAME_MAX      = 8;    // Meshtastic short_name typical 4
constexpr size_t RADIO_CHANNEL_NAME_MAX = 23;   // per-radio channel display name
constexpr size_t RADIO_CHANNEL_KEY_MAX  = 47;   // 32-hex MC key or ~44-char base64 MT PSK

void begin();           // load from NVS or initialise from build-flag defaults
void save();            // persist current values + set configured=true
void resetToDefaults(); // erase NVS blob; next boot will use build-flag defaults
bool isConfigured();    // true once save() has been called at least once

// Accessors.
uint32_t    mtNodeId();
const char *mtNodeIdStr();
const char *mtLongName();
const char *mtShortName();
const char *radio1ChannelName();
const char *radio1ChannelKey();
const char *radio2ChannelName();
const char *radio2ChannelKey();
bool        positionEnabled();
bool        telemetryEnabled();

// Setters — used by the captive portal. Bounds-clamp + null-terminate only.
void setMtNodeId(uint32_t v);
void setMtNodeIdStr(const char *s);
void setMtLongName(const char *s);
void setMtShortName(const char *s);
void setRadio1ChannelName(const char *s);
void setRadio1ChannelKey(const char *s);
void setRadio2ChannelName(const char *s);
void setRadio2ChannelKey(const char *s);
void setPositionEnabled(bool v);
void setTelemetryEnabled(bool v);

void debugDump();

}  // namespace BridgeConfig
