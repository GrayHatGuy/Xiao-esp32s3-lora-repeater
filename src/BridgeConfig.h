// BridgeConfig.h
// ---------------------------------------------------------------------------
// Single source of truth for runtime-configurable bridge settings, persisted
// in NVS namespace 'bridgecfg'.
//
// As of v8 (schema v4) the bridge is fully portal-configurable: each radio
// slot carries its own PROTOCOL and full RF plan (frequency, bandwidth, SF,
// CR, sync word, TX power), plus its channel name + key; a global REGION
// governs the sub-GHz band. The platformio.ini LORA_RADIO* / BRIDGE_* build
// flags become optional first-boot defaults only — a no-flag image first-boots
// straight into the captive portal.
//
// The channel key string is interpreted per the radio's protocol:
//   - MeshCore   radio: 32-char hex AES key
//   - Meshtastic radio: base64 PSK ("" = LongFast default)
//   - Reticulum  radio: unused
//   - Custom     radio: free RF, channel key per the resolved decoder
//
// Storage: one PersistedV4 blob. begin() migrates a v2 or v3 blob forward.
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

// Per-radio protocol. A radio set to PROTO_NONE is disabled (skipped at
// setup()); useful as a single-radio debug mode or to park a slot for a
// future 2.4 GHz radio.
enum Protocol : uint8_t {
    PROTO_NONE   = 0,
    PROTO_MT     = 1,   // Meshtastic   (sync 0x2B)
    PROTO_MC     = 2,   // MeshCore     (sync 0x12)
    PROTO_RNS    = 3,   // Reticulum    (sync 0x42)
    PROTO_CUSTOM = 4,   // user-entered RF; decoder derived from sync word
    PROTO_LORAWAN = 5,  // LoRaWAN      (sync 0x34) — keyless capture/relay/mesh (v8.3)
};

// Global device region — governs the sub-GHz band. A 2.4 GHz radio is
// region-exempt (ISM 2400-2483.5 MHz, licence-free worldwide).
enum Region : uint8_t {
    REGION_UNSET  = 0,
    REGION_US     = 1,
    REGION_EU_868 = 2,
    REGION_EU_433 = 3,
    REGION_ANZ    = 4,
    REGION_CN     = 5,
    REGION_JP     = 6,
    REGION_IN     = 7,
    REGION_KR     = 8,
    REGION_RU     = 9,
    REGION_CUSTOM = 10,
};

// Per-radio LoRaWAN region (CO-9; v8.4.1). Stored in the RadioRf spare bytes;
// only meaningful for a PROTO_LORAWAN radio. Indexes the captive-portal
// region/slot picker (US915/AU915/AS923/EU868). 0 = unset / manual.
enum LwRegion : uint8_t {
    LW_REGION_UNSET = 0,
    LW_REGION_US915 = 1,
    LW_REGION_AU915 = 2,
    LW_REGION_AS923 = 3,
    LW_REGION_EU868 = 4,
};

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

// Region + per-radio protocol/RF. Radio index is 0 or 1.
uint8_t  region();
uint8_t  radioProtocol(int radio);
float    radioFrequency(int radio);
float    radioBandwidth(int radio);
uint8_t  radioSf(int radio);
uint8_t  radioCr(int radio);
uint8_t  radioSyncWord(int radio);
int8_t   radioTxPower(int radio);
uint8_t  radioLwRegion(int radio);   // CO-9: LoRaWAN region index (0 = unset)

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

void setRegion(uint8_t v);
void setRadioProtocol(int radio, uint8_t v);
void setRadioFrequency(int radio, float v);
void setRadioBandwidth(int radio, float v);
void setRadioSf(int radio, uint8_t v);
void setRadioCr(int radio, uint8_t v);
void setRadioSyncWord(int radio, uint8_t v);
void setRadioTxPower(int radio, int8_t v);
void setRadioLwRegion(int radio, uint8_t v);

void debugDump();

}  // namespace BridgeConfig
