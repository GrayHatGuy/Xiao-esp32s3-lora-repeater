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
// Schema v6 (T_LORA_QUAD, Phase 2) grows the radio table from 2 to NUM_RADIOS
// (4): R1/R2 = SX1262 on the XIAO, R3/R4 = LR1121 on the T-Lora-Dual reached
// over UART. Each slot adds a `band` (sub-GHz / 2.4 GHz / S-band-stub) and a
// `routeMask` (which other radios it bridges its RX to — the configurable
// routing matrix). The per-radio channel name/key now live inside the radio
// slot rather than in top-level r1/r2 fields. begin() migrates a v2/v3/v5
// blob forward.
//
// The channel key string is interpreted per the radio's protocol:
//   - MeshCore   radio: 32-char hex AES key
//   - Meshtastic radio: base64 PSK ("" = LongFast default)
//   - Reticulum  radio: unused
//   - Custom     radio: free RF, channel key per the resolved decoder
//
// Thread-safety: read-mostly; writes come only from CaptivePortal / setup().
// ---------------------------------------------------------------------------

#pragma once

#include <Arduino.h>
#include <stdint.h>

namespace BridgeConfig {

// Number of radio slots. 0/1 = SX1262 on the XIAO (direct SPI); 2/3 = LR1121
// on the T-Lora-Dual co-processor (over UART). Indices are 0..NUM_RADIOS-1.
constexpr int    NUM_RADIOS             = 4;

constexpr size_t MT_NODE_ID_STR_MAX     = 15;   // "!12345678" plus null
constexpr size_t MT_LONG_NAME_MAX       = 39;   // Meshtastic long_name canonical max 40
constexpr size_t MT_SHORT_NAME_MAX      = 8;    // Meshtastic short_name typical 4
constexpr size_t RADIO_CHANNEL_NAME_MAX = 23;   // per-radio channel display name
constexpr size_t RADIO_CHANNEL_KEY_MAX  = 47;   // 32-hex MC key or ~44-char base64 MT PSK

// Per-radio protocol. A radio set to PROTO_NONE is disabled (skipped at
// setup()); useful as a single-radio debug mode or to park a slot for a
// radio that is not fitted yet.
enum Protocol : uint8_t {
    PROTO_NONE   = 0,
    PROTO_MT     = 1,   // Meshtastic   (sync 0x2B)
    PROTO_MC     = 2,   // MeshCore     (sync 0x12)
    PROTO_RNS    = 3,   // Reticulum    (sync 0x42)
    PROTO_CUSTOM = 4,   // user-entered RF; decoder derived from sync word
};

// Radio chip per slot. In the MIXED build profile Radio 2's chip is
// portal-selectable; the DUAL_* profiles fix both at compile time. R3/R4 are
// always LR1121 (they live on the T-Lora-Dual).
enum Chip : uint8_t {
    CHIP_SX1262 = 0,
    CHIP_LR1121 = 1,
};

// Per-radio band. SX1262 is sub-GHz only; the LR1121 adds 2.4 GHz and an
// S-band that is accepted in config but disabled (stub) at the co-processor.
enum Band : uint8_t {
    BAND_SUBGHZ = 0,
    BAND_2G4    = 1,   // 2400-2483.5 MHz ISM, region-exempt (LR1121)
    BAND_SBAND  = 2,   // stub — accepted but the co-processor refuses to key up
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

void begin();           // load from NVS or initialise from build-flag defaults
void save();            // persist current values + set configured=true
void resetToDefaults(); // erase NVS blob; next boot will use build-flag defaults
bool isConfigured();    // true once save() has been called at least once

// Accessors.
uint32_t    mtNodeId();
const char *mtNodeIdStr();
const char *mtLongName();
const char *mtShortName();
bool        positionEnabled();
bool        telemetryEnabled();

// Per-radio channel name/key. Indexed form is the canonical API (radio
// 0..NUM_RADIOS-1); the radio1/radio2 helpers are kept as thin wrappers for
// existing callers.
const char *radioChannelName(int radio);
const char *radioChannelKey(int radio);
const char *radio1ChannelName();
const char *radio1ChannelKey();
const char *radio2ChannelName();
const char *radio2ChannelKey();

// Region + per-radio protocol/RF/chip/band/route. Radio index is 0..NUM_RADIOS-1.
uint8_t  region();
uint8_t  radioProtocol(int radio);
float    radioFrequency(int radio);
float    radioBandwidth(int radio);
uint8_t  radioSf(int radio);
uint8_t  radioCr(int radio);
uint8_t  radioSyncWord(int radio);
int8_t   radioTxPower(int radio);
uint8_t  radioChip(int radio);
uint8_t  radioBand(int radio);
uint8_t  radioRouteMask(int radio);   // bit j set => bridge this radio's RX to radio j

// Setters — used by the captive portal. Bounds-clamp + null-terminate only.
void setMtNodeId(uint32_t v);
void setMtNodeIdStr(const char *s);
void setMtLongName(const char *s);
void setMtShortName(const char *s);
void setPositionEnabled(bool v);
void setTelemetryEnabled(bool v);

void setRadioChannelName(int radio, const char *s);
void setRadioChannelKey(int radio, const char *s);
void setRadio1ChannelName(const char *s);
void setRadio1ChannelKey(const char *s);
void setRadio2ChannelName(const char *s);
void setRadio2ChannelKey(const char *s);

void setRegion(uint8_t v);
void setRadioProtocol(int radio, uint8_t v);
void setRadioFrequency(int radio, float v);
void setRadioBandwidth(int radio, float v);
void setRadioSf(int radio, uint8_t v);
void setRadioCr(int radio, uint8_t v);
void setRadioSyncWord(int radio, uint8_t v);
void setRadioTxPower(int radio, int8_t v);
void setRadioChip(int radio, uint8_t v);
void setRadioBand(int radio, uint8_t v);
void setRadioRouteMask(int radio, uint8_t v);

void debugDump();

}  // namespace BridgeConfig
