// BridgeConfig.cpp — see BridgeConfig.h for design notes.

#include "BridgeConfig.h"

#include <Preferences.h>
#include <string.h>

// --- Build-flag defaults ---------------------------------------------------
// Values used on a fresh device (or after resetToDefaults). Mirror the
// BRIDGE_* / LORA_RADIO* macros in platformio.ini. As of v8 these are
// first-boot defaults only — the portal owns the live config.

#ifndef BRIDGE_MT_NODE_ID
  #define BRIDGE_MT_NODE_ID      0xB16B00B5u
#endif
#ifndef BRIDGE_MT_NODE_ID_STR
  #define BRIDGE_MT_NODE_ID_STR  "!b16b00b5"
#endif
#ifndef BRIDGE_MT_LONG_NAME
  #define BRIDGE_MT_LONG_NAME    "LoRa Bridge"
#endif
#ifndef BRIDGE_MT_SHORT_NAME
  #define BRIDGE_MT_SHORT_NAME   "BR"
#endif
#ifndef BRIDGE_MC_KEY_HEX
  #define BRIDGE_MC_KEY_HEX      "8b3387e9c5cdea6ac9e5edbaa115cd72"
#endif
#ifndef BRIDGE_MC_CHANNEL_NAME
  #define BRIDGE_MC_CHANNEL_NAME "public"
#endif
#ifndef BRIDGE_MT_CHANNEL_NAME
  #define BRIDGE_MT_CHANNEL_NAME "LongFast"
#endif
#ifndef BRIDGE_MT_PSK_B64
  #define BRIDGE_MT_PSK_B64      ""
#endif
#ifndef BRIDGE_MT_POSITION
  #define BRIDGE_MT_POSITION     1
#endif
#ifndef BRIDGE_MT_TELEMETRY
  #define BRIDGE_MT_TELEMETRY    1
#endif
// Per-radio protocol — needed to pick each radio's default channel + to
// migrate a v2 blob's protocol-specific channels into per-radio slots.
#ifndef LORA_RADIO1_SYNC_WORD
  #define LORA_RADIO1_SYNC_WORD  0x2B
#endif
#ifndef LORA_RADIO2_SYNC_WORD
  #define LORA_RADIO2_SYNC_WORD  0x12
#endif
// Per-radio RF — first-boot defaults for the v8 schema RF fields.
#ifndef LORA_RADIO1_FREQUENCY
  #define LORA_RADIO1_FREQUENCY      906.875f
#endif
#ifndef LORA_RADIO1_BANDWIDTH
  #define LORA_RADIO1_BANDWIDTH      250.0f
#endif
#ifndef LORA_RADIO1_SPREAD_FACTOR
  #define LORA_RADIO1_SPREAD_FACTOR  11
#endif
#ifndef LORA_RADIO1_CODING_RATE
  #define LORA_RADIO1_CODING_RATE    5
#endif
#ifndef LORA_RADIO1_TX_POWER
  #define LORA_RADIO1_TX_POWER       20
#endif
#ifndef LORA_RADIO2_FREQUENCY
  #define LORA_RADIO2_FREQUENCY      910.525f
#endif
#ifndef LORA_RADIO2_BANDWIDTH
  #define LORA_RADIO2_BANDWIDTH      250.0f
#endif
#ifndef LORA_RADIO2_SPREAD_FACTOR
  #define LORA_RADIO2_SPREAD_FACTOR  11
#endif
#ifndef LORA_RADIO2_CODING_RATE
  #define LORA_RADIO2_CODING_RATE    5
#endif
#ifndef LORA_RADIO2_TX_POWER
  #define LORA_RADIO2_TX_POWER       20
#endif
// Per-radio RF — first-boot defaults for the v8.5 R3/R4 slots (SX1262 on the
// second XIAO, reached over the UART crossover). R3/R4 default to PROTO_NONE
// (disabled) in loadDefaults(); these values only seed sane RF so a
// portal-enabled slot starts from a usable sub-GHz plan.
#ifndef LORA_RADIO3_SYNC_WORD
  #define LORA_RADIO3_SYNC_WORD      0x2B
#endif
#ifndef LORA_RADIO3_FREQUENCY
  #define LORA_RADIO3_FREQUENCY      906.875f
#endif
#ifndef LORA_RADIO3_BANDWIDTH
  #define LORA_RADIO3_BANDWIDTH      250.0f
#endif
#ifndef LORA_RADIO3_SPREAD_FACTOR
  #define LORA_RADIO3_SPREAD_FACTOR  11
#endif
#ifndef LORA_RADIO3_CODING_RATE
  #define LORA_RADIO3_CODING_RATE    5
#endif
#ifndef LORA_RADIO3_TX_POWER
  #define LORA_RADIO3_TX_POWER       20
#endif
#ifndef LORA_RADIO4_SYNC_WORD
  #define LORA_RADIO4_SYNC_WORD      0x12
#endif
#ifndef LORA_RADIO4_FREQUENCY
  #define LORA_RADIO4_FREQUENCY      910.525f
#endif
#ifndef LORA_RADIO4_BANDWIDTH
  #define LORA_RADIO4_BANDWIDTH      250.0f
#endif
#ifndef LORA_RADIO4_SPREAD_FACTOR
  #define LORA_RADIO4_SPREAD_FACTOR  11
#endif
#ifndef LORA_RADIO4_CODING_RATE
  #define LORA_RADIO4_CODING_RATE    5
#endif
#ifndef LORA_RADIO4_TX_POWER
  #define LORA_RADIO4_TX_POWER       20
#endif
// Global region — first-boot default. REGION_UNSET (0) until set in portal.
#ifndef BRIDGE_REGION
  #define BRIDGE_REGION              0
#endif

namespace BridgeConfig {

// v5 (v8.5): the radio table grows from 2 to NUM_RADIOS (4). R1/R2 are the
// local SX1262 on this XIAO; R3/R4 are the SX1262 on the second XIAO, reached
// over the UART crossover. Each slot gains a `routeMask` (which other radios it
// bridges its RX to — the configurable routing matrix) and the per-radio
// channel name/key now live IN the slot rather than in top-level r1/r2 fields.
// begin() migrates a v2/v3/v4 blob forward.
static constexpr uint8_t SCHEMA_VERSION = 5;

// Schema v2 — kept verbatim so a v2 blob (v6.1 build) can be migrated.
struct PersistedV2 {
    uint8_t  version;
    uint8_t  configured;
    uint8_t  positionEnabled;
    uint8_t  telemetryEnabled;
    uint32_t mtNodeId;
    char     mtNodeIdStr  [15 + 1];
    char     mtLongName   [39 + 1];
    char     mtShortName  [8  + 1];
    char     mcKeyHex     [32 + 1];
    char     mcChannelName[23 + 1];
    char     mtChannelName[23 + 1];
    char     mtPskBase64  [47 + 1];
};

// Schema v3 — per-radio channel name + key (v7.0 build).
struct PersistedV3 {
    uint8_t  version;
    uint8_t  configured;
    uint8_t  positionEnabled;
    uint8_t  telemetryEnabled;
    uint32_t mtNodeId;
    char     mtNodeIdStr  [MT_NODE_ID_STR_MAX     + 1];
    char     mtLongName   [MT_LONG_NAME_MAX       + 1];
    char     mtShortName  [MT_SHORT_NAME_MAX      + 1];
    char     r1ChannelName[RADIO_CHANNEL_NAME_MAX + 1];
    char     r1ChannelKey [RADIO_CHANNEL_KEY_MAX  + 1];
    char     r2ChannelName[RADIO_CHANNEL_NAME_MAX + 1];
    char     r2ChannelKey [RADIO_CHANNEL_KEY_MAX  + 1];
};

// Per-radio protocol + full RF plan (schema v4). Kept verbatim so a v4 blob can
// be migrated. The `lwRegion` byte (CO-9, v8.4.1) occupies what was a pad byte
// in earlier v4 builds, so all v4 blobs are byte-identical.
struct RadioRfV4 {
    uint8_t  protocol;     // BridgeConfig::Protocol
    uint8_t  sf;           // spreading factor 5..12
    uint8_t  cr;           // coding rate 5..8
    uint8_t  syncWord;     // LoRa sync word
    int8_t   txPower;      // dBm
    uint8_t  lwRegion;     // CO-9: LoRaWAN region index (was _pad[0]; 0 = unset)
    uint8_t  _pad[2];
    float    frequency;    // MHz
    float    bandwidth;    // kHz
};

// Schema v4 — global region + 2 radios, channel in top-level r1/r2 fields.
// Kept verbatim for migration.
struct PersistedV4 {
    uint8_t   version;
    uint8_t   configured;
    uint8_t   positionEnabled;
    uint8_t   telemetryEnabled;
    uint8_t   region;       // BridgeConfig::Region
    uint8_t   _pad[3];
    uint32_t  mtNodeId;
    char      mtNodeIdStr  [MT_NODE_ID_STR_MAX     + 1];
    char      mtLongName   [MT_LONG_NAME_MAX       + 1];
    char      mtShortName  [MT_SHORT_NAME_MAX      + 1];
    char      r1ChannelName[RADIO_CHANNEL_NAME_MAX + 1];
    char      r1ChannelKey [RADIO_CHANNEL_KEY_MAX  + 1];
    char      r2ChannelName[RADIO_CHANNEL_NAME_MAX + 1];
    char      r2ChannelKey [RADIO_CHANNEL_KEY_MAX  + 1];
    RadioRfV4 radio[2];
};

// Per-radio slot (schema v5). `routeMask` reuses a v4 RadioRf pad byte; the
// channel name/key (previously top-level r1/r2 fields) now live in the slot so
// the table scales to NUM_RADIOS.
struct RadioSlot {
    uint8_t  protocol;     // BridgeConfig::Protocol
    uint8_t  sf;           // spreading factor 5..12
    uint8_t  cr;           // coding rate 5..8
    uint8_t  syncWord;     // LoRa sync word
    int8_t   txPower;      // dBm
    uint8_t  lwRegion;     // CO-9: LoRaWAN region index (0 = unset)
    uint8_t  routeMask;    // bit j => bridge this radio's RX to radio j (v5)
    uint8_t  _pad[1];
    float    frequency;    // MHz
    float    bandwidth;    // kHz
    char     channelName[RADIO_CHANNEL_NAME_MAX + 1];
    char     channelKey [RADIO_CHANNEL_KEY_MAX  + 1];
};

// Schema v5 — global region + NUM_RADIOS per-radio slots (channel in-slot).
struct PersistedV5 {
    uint8_t   version;
    uint8_t   configured;
    uint8_t   positionEnabled;
    uint8_t   telemetryEnabled;
    uint8_t   region;      // BridgeConfig::Region
    uint8_t   _pad[3];
    uint32_t  mtNodeId;
    char      mtNodeIdStr [MT_NODE_ID_STR_MAX + 1];
    char      mtLongName  [MT_LONG_NAME_MAX   + 1];
    char      mtShortName [MT_SHORT_NAME_MAX  + 1];
    RadioSlot radio[NUM_RADIOS];
};

static PersistedV5 s_cfg;

static const char *NVS_NAMESPACE = "bridgecfg";
static const char *NVS_KEY_BLOB  = "v1";   // opaque key; schema version lives in the blob

static void copyStr(char *dst, size_t dstCap, const char *src) {
    if (!dst || dstCap == 0) return;
    if (!src) src = "";
    strncpy(dst, src, dstCap - 1);
    dst[dstCap - 1] = 0;
}

// Map a LoRa sync word to a protocol enum. An unrecognised sync word is a
// Custom radio — RF is received but the bridge can't decode it.
static uint8_t protocolFromSync(uint8_t sync) {
    if (sync == 0x2B) return PROTO_MT;
    if (sync == 0x12) return PROTO_MC;
    if (sync == 0x42) return PROTO_RNS;
    if (sync == 0x34) return PROTO_LORAWAN;
    return PROTO_CUSTOM;
}

// Default channel name/key for a radio, chosen by its build-flag protocol.
static void radioDefaultChannel(uint8_t syncWord,
                                char *nameOut, size_t nameCap,
                                char *keyOut,  size_t keyCap) {
    if (syncWord == 0x2B) {            // Meshtastic
        copyStr(nameOut, nameCap, BRIDGE_MT_CHANNEL_NAME);
        copyStr(keyOut,  keyCap,  BRIDGE_MT_PSK_B64);
    } else if (syncWord == 0x12) {     // MeshCore
        copyStr(nameOut, nameCap, BRIDGE_MC_CHANNEL_NAME);
        copyStr(keyOut,  keyCap,  BRIDGE_MC_KEY_HEX);
    } else {                           // Reticulum / other — no channel
        copyStr(nameOut, nameCap, "");
        copyStr(keyOut,  keyCap,  "");
    }
}

// Seed one radio slot with build-flag RF + the protocol's default channel.
static void seedRadio(int i, uint8_t sync, float freq, float bw,
                      uint8_t sf, uint8_t cr, int8_t txp, uint8_t proto) {
    RadioSlot &r = s_cfg.radio[i];
    r.protocol  = proto;
    r.sf        = sf;
    r.cr        = cr;
    r.syncWord  = sync;
    r.txPower   = txp;
    r.lwRegion  = LW_REGION_UNSET;
    r.routeMask = 0;
    r.frequency = freq;
    r.bandwidth = bw;
    radioDefaultChannel(sync, r.channelName, sizeof(r.channelName),
                        r.channelKey, sizeof(r.channelKey));
}

static void loadDefaults() {
    memset(&s_cfg, 0, sizeof(s_cfg));
    s_cfg.version          = SCHEMA_VERSION;
    s_cfg.configured       = 0;
    s_cfg.region           = (uint8_t)BRIDGE_REGION;
    s_cfg.mtNodeId         = (uint32_t)BRIDGE_MT_NODE_ID;
    s_cfg.positionEnabled  = (BRIDGE_MT_POSITION  ? 1 : 0);
    s_cfg.telemetryEnabled = (BRIDGE_MT_TELEMETRY ? 1 : 0);
    copyStr(s_cfg.mtNodeIdStr, sizeof(s_cfg.mtNodeIdStr), BRIDGE_MT_NODE_ID_STR);
    copyStr(s_cfg.mtLongName,  sizeof(s_cfg.mtLongName),  BRIDGE_MT_LONG_NAME);
    copyStr(s_cfg.mtShortName, sizeof(s_cfg.mtShortName), BRIDGE_MT_SHORT_NAME);

    // R1/R2 = local SX1262, enabled by their build-flag protocol. R3/R4 = the
    // second XIAO's SX1262 over UART, DISABLED by default (PROTO_NONE) so a
    // fresh 4-radio build behaves exactly like the 2-radio bridge until the
    // portal enables them; RF is seeded so an enabled slot starts usable.
    seedRadio(0, (uint8_t)LORA_RADIO1_SYNC_WORD, (float)(LORA_RADIO1_FREQUENCY),
              (float)(LORA_RADIO1_BANDWIDTH), (uint8_t)(LORA_RADIO1_SPREAD_FACTOR),
              (uint8_t)(LORA_RADIO1_CODING_RATE), (int8_t)(LORA_RADIO1_TX_POWER),
              protocolFromSync((uint8_t)LORA_RADIO1_SYNC_WORD));
    seedRadio(1, (uint8_t)LORA_RADIO2_SYNC_WORD, (float)(LORA_RADIO2_FREQUENCY),
              (float)(LORA_RADIO2_BANDWIDTH), (uint8_t)(LORA_RADIO2_SPREAD_FACTOR),
              (uint8_t)(LORA_RADIO2_CODING_RATE), (int8_t)(LORA_RADIO2_TX_POWER),
              protocolFromSync((uint8_t)LORA_RADIO2_SYNC_WORD));
    seedRadio(2, (uint8_t)LORA_RADIO3_SYNC_WORD, (float)(LORA_RADIO3_FREQUENCY),
              (float)(LORA_RADIO3_BANDWIDTH), (uint8_t)(LORA_RADIO3_SPREAD_FACTOR),
              (uint8_t)(LORA_RADIO3_CODING_RATE), (int8_t)(LORA_RADIO3_TX_POWER),
              PROTO_NONE);
    seedRadio(3, (uint8_t)LORA_RADIO4_SYNC_WORD, (float)(LORA_RADIO4_FREQUENCY),
              (float)(LORA_RADIO4_BANDWIDTH), (uint8_t)(LORA_RADIO4_SPREAD_FACTOR),
              (uint8_t)(LORA_RADIO4_CODING_RATE), (int8_t)(LORA_RADIO4_TX_POWER),
              PROTO_NONE);

    // Optional first-boot ENABLE of R3/R4 (v8.5 bench): the remote slots seed to
    // PROTO_NONE for do-no-harm; defining these promotes a slot to the protocol
    // implied by its sync word, so a bench env can stand up a 4-radio config
    // without the captive portal. Do-no-harm: only applied when the macro is set.
#ifdef LORA_RADIO3_ENABLE
    s_cfg.radio[2].protocol = protocolFromSync((uint8_t)LORA_RADIO3_SYNC_WORD);
#endif
#ifdef LORA_RADIO4_ENABLE
    s_cfg.radio[3].protocol = protocolFromSync((uint8_t)LORA_RADIO4_SYNC_WORD);
#endif

    // Optional PER-RADIO channel name/key first-boot overrides (v8.4.1 for R1/R2,
    // extended to R3/R4 in v8.5). The BRIDGE_MT_*/BRIDGE_MC_* defaults above are
    // shared by protocol, so two same-protocol radios would otherwise collide on
    // one channel; defining these lets each radio preload a distinct channel.
    // Do-no-harm: only applied when the macro is defined.
#ifdef LORA_RADIO1_CHANNEL_NAME
    copyStr(s_cfg.radio[0].channelName, sizeof(s_cfg.radio[0].channelName), LORA_RADIO1_CHANNEL_NAME);
#endif
#ifdef LORA_RADIO1_CHANNEL_KEY
    copyStr(s_cfg.radio[0].channelKey,  sizeof(s_cfg.radio[0].channelKey),  LORA_RADIO1_CHANNEL_KEY);
#endif
#ifdef LORA_RADIO2_CHANNEL_NAME
    copyStr(s_cfg.radio[1].channelName, sizeof(s_cfg.radio[1].channelName), LORA_RADIO2_CHANNEL_NAME);
#endif
#ifdef LORA_RADIO2_CHANNEL_KEY
    copyStr(s_cfg.radio[1].channelKey,  sizeof(s_cfg.radio[1].channelKey),  LORA_RADIO2_CHANNEL_KEY);
#endif
#ifdef LORA_RADIO3_CHANNEL_NAME
    copyStr(s_cfg.radio[2].channelName, sizeof(s_cfg.radio[2].channelName), LORA_RADIO3_CHANNEL_NAME);
#endif
#ifdef LORA_RADIO3_CHANNEL_KEY
    copyStr(s_cfg.radio[2].channelKey,  sizeof(s_cfg.radio[2].channelKey),  LORA_RADIO3_CHANNEL_KEY);
#endif
#ifdef LORA_RADIO4_CHANNEL_NAME
    copyStr(s_cfg.radio[3].channelName, sizeof(s_cfg.radio[3].channelName), LORA_RADIO4_CHANNEL_NAME);
#endif
#ifdef LORA_RADIO4_CHANNEL_KEY
    copyStr(s_cfg.radio[3].channelKey,  sizeof(s_cfg.radio[3].channelKey),  LORA_RADIO4_CHANNEL_KEY);
#endif

    // Default routing matrix = the historical R1<->R2 crossover. R3/R4 carry no
    // routes until enabled + configured in the portal.
    s_cfg.radio[0].routeMask = (uint8_t)(1u << 1);   // R1 -> R2
    s_cfg.radio[1].routeMask = (uint8_t)(1u << 0);   // R2 -> R1

    // Optional per-radio routeMask first-boot override (v8.5 bench): bit j => this
    // radio bridges its RX to radio j (bit0=R1 … bit3=R4). Lets a bench env set
    // routing the portal would otherwise own (e.g. R1->R3 = 0x04). Do-no-harm:
    // only applied when the macro is defined.
#ifdef LORA_RADIO1_ROUTE_MASK
    s_cfg.radio[0].routeMask = (uint8_t)(LORA_RADIO1_ROUTE_MASK);
#endif
#ifdef LORA_RADIO2_ROUTE_MASK
    s_cfg.radio[1].routeMask = (uint8_t)(LORA_RADIO2_ROUTE_MASK);
#endif
#ifdef LORA_RADIO3_ROUTE_MASK
    s_cfg.radio[2].routeMask = (uint8_t)(LORA_RADIO3_ROUTE_MASK);
#endif
#ifdef LORA_RADIO4_ROUTE_MASK
    s_cfg.radio[3].routeMask = (uint8_t)(LORA_RADIO4_ROUTE_MASK);
#endif
}

static void terminateAll() {
    s_cfg.mtNodeIdStr [sizeof(s_cfg.mtNodeIdStr)  - 1] = 0;
    s_cfg.mtLongName  [sizeof(s_cfg.mtLongName)   - 1] = 0;
    s_cfg.mtShortName [sizeof(s_cfg.mtShortName)  - 1] = 0;
    for (int i = 0; i < NUM_RADIOS; i++) {
        s_cfg.radio[i].channelName[sizeof(s_cfg.radio[i].channelName) - 1] = 0;
        s_cfg.radio[i].channelKey [sizeof(s_cfg.radio[i].channelKey)  - 1] = 0;
    }
}

// Map a v2 blob's protocol-specific channels onto one radio slot, chosen by
// that radio's build-flag sync word.
static void migrateV2Channel(uint8_t syncWord, const PersistedV2 &v2,
                             char *nameOut, size_t nameCap,
                             char *keyOut,  size_t keyCap) {
    if (syncWord == 0x2B) {            // Meshtastic radio
        copyStr(nameOut, nameCap, v2.mtChannelName);
        copyStr(keyOut,  keyCap,  v2.mtPskBase64);
    } else if (syncWord == 0x12) {     // MeshCore radio
        copyStr(nameOut, nameCap, v2.mcChannelName);
        copyStr(keyOut,  keyCap,  v2.mcKeyHex);
    } else {
        copyStr(nameOut, nameCap, "");
        copyStr(keyOut,  keyCap,  "");
    }
}

// Migrate a v4 blob (2 radios, top-level channels) into the live v5 struct.
// loadDefaults() has already run, so R3/R4 keep their disabled defaults.
static void migrateV4toV5(const PersistedV4 &v4) {
    s_cfg.configured       = v4.configured;
    s_cfg.positionEnabled  = v4.positionEnabled;
    s_cfg.telemetryEnabled = v4.telemetryEnabled;
    s_cfg.region           = v4.region;
    s_cfg.mtNodeId         = v4.mtNodeId;
    copyStr(s_cfg.mtNodeIdStr, sizeof(s_cfg.mtNodeIdStr), v4.mtNodeIdStr);
    copyStr(s_cfg.mtLongName,  sizeof(s_cfg.mtLongName),  v4.mtLongName);
    copyStr(s_cfg.mtShortName, sizeof(s_cfg.mtShortName), v4.mtShortName);
    for (int i = 0; i < 2; i++) {
        RadioSlot       &d = s_cfg.radio[i];
        const RadioRfV4 &s = v4.radio[i];
        d.protocol  = s.protocol;
        d.sf        = s.sf;
        d.cr        = s.cr;
        d.syncWord  = s.syncWord;
        d.txPower   = s.txPower;
        d.lwRegion  = s.lwRegion;      // preserve CO-9 per-radio LoRaWAN region
        d.frequency = s.frequency;
        d.bandwidth = s.bandwidth;
    }
    copyStr(s_cfg.radio[0].channelName, sizeof(s_cfg.radio[0].channelName), v4.r1ChannelName);
    copyStr(s_cfg.radio[0].channelKey,  sizeof(s_cfg.radio[0].channelKey),  v4.r1ChannelKey);
    copyStr(s_cfg.radio[1].channelName, sizeof(s_cfg.radio[1].channelName), v4.r2ChannelName);
    copyStr(s_cfg.radio[1].channelKey,  sizeof(s_cfg.radio[1].channelKey),  v4.r2ChannelKey);
    // Preserve the historical R1<->R2 crossover for the two migrated radios;
    // R3/R4 retain loadDefaults() values (PROTO_NONE, route 0).
    s_cfg.radio[0].routeMask = (uint8_t)(1u << 1);
    s_cfg.radio[1].routeMask = (uint8_t)(1u << 0);
}

void begin() {
    // loadDefaults() fully populates s_cfg (all NUM_RADIOS slots) with
    // build-flag defaults; the migration paths below overwrite only the fields
    // an older blob carries, leaving the rest at their build-flag values.
    loadDefaults();
    Preferences prefs;
    if (!prefs.begin(NVS_NAMESPACE, /*readOnly=*/false)) {
        Serial.printf("[BridgeConfig] prefs.begin failed; using build-flag defaults\n");
        return;
    }
    size_t blobSize = prefs.getBytesLength(NVS_KEY_BLOB);

    if (blobSize == sizeof(PersistedV5)) {
        PersistedV5 tmp;
        size_t got = prefs.getBytes(NVS_KEY_BLOB, &tmp, sizeof(tmp));
        if (got == sizeof(PersistedV5) && tmp.version == 5) {
            s_cfg = tmp;
            terminateAll();
            Serial.printf("[BridgeConfig] loaded v5 blob from NVS (configured=%u)\n",
                          (unsigned)s_cfg.configured);
        } else {
            Serial.printf("[BridgeConfig] v5-sized blob bad (got %u B, ver %u); keeping defaults\n",
                          (unsigned)got, (unsigned)tmp.version);
        }
    } else if (blobSize == sizeof(PersistedV4)) {
        PersistedV4 v4;
        size_t got = prefs.getBytes(NVS_KEY_BLOB, &v4, sizeof(v4));
        if (got == sizeof(PersistedV4) && v4.version == 4) {
            migrateV4toV5(v4);
            terminateAll();
            s_cfg.version = SCHEMA_VERSION;
            prefs.putBytes(NVS_KEY_BLOB, &s_cfg, sizeof(s_cfg));   // persist upgrade
            Serial.printf("[BridgeConfig] migrated v4 blob -> v5 (configured=%u)\n",
                          (unsigned)s_cfg.configured);
        } else {
            Serial.printf("[BridgeConfig] v4-sized blob bad (got %u B, ver %u); keeping defaults\n",
                          (unsigned)got, (unsigned)v4.version);
        }
    } else if (blobSize == sizeof(PersistedV3)) {
        PersistedV3 v3;
        size_t got = prefs.getBytes(NVS_KEY_BLOB, &v3, sizeof(v3));
        if (got == sizeof(PersistedV3) && v3.version == 3) {
            s_cfg.configured       = v3.configured;
            s_cfg.positionEnabled  = v3.positionEnabled;
            s_cfg.telemetryEnabled = v3.telemetryEnabled;
            s_cfg.mtNodeId         = v3.mtNodeId;
            copyStr(s_cfg.mtNodeIdStr, sizeof(s_cfg.mtNodeIdStr), v3.mtNodeIdStr);
            copyStr(s_cfg.mtLongName,  sizeof(s_cfg.mtLongName),  v3.mtLongName);
            copyStr(s_cfg.mtShortName, sizeof(s_cfg.mtShortName), v3.mtShortName);
            copyStr(s_cfg.radio[0].channelName, sizeof(s_cfg.radio[0].channelName), v3.r1ChannelName);
            copyStr(s_cfg.radio[0].channelKey,  sizeof(s_cfg.radio[0].channelKey),  v3.r1ChannelKey);
            copyStr(s_cfg.radio[1].channelName, sizeof(s_cfg.radio[1].channelName), v3.r2ChannelName);
            copyStr(s_cfg.radio[1].channelKey,  sizeof(s_cfg.radio[1].channelKey),  v3.r2ChannelKey);
            // region + per-radio protocol/RF/route stay at build-flag defaults
            // (set by loadDefaults) so an upgraded v3 device keeps running.
            terminateAll();
            s_cfg.version = SCHEMA_VERSION;
            prefs.putBytes(NVS_KEY_BLOB, &s_cfg, sizeof(s_cfg));   // persist upgrade
            Serial.printf("[BridgeConfig] migrated v3 blob -> v5 (configured=%u)\n",
                          (unsigned)s_cfg.configured);
        } else {
            Serial.printf("[BridgeConfig] v3-sized blob bad (got %u B, ver %u); keeping defaults\n",
                          (unsigned)got, (unsigned)v3.version);
        }
    } else if (blobSize == sizeof(PersistedV2)) {
        PersistedV2 v2;
        size_t got = prefs.getBytes(NVS_KEY_BLOB, &v2, sizeof(v2));
        if (got == sizeof(PersistedV2) && v2.version == 2) {
            s_cfg.configured       = v2.configured;
            s_cfg.positionEnabled  = v2.positionEnabled;
            s_cfg.telemetryEnabled = v2.telemetryEnabled;
            s_cfg.mtNodeId         = v2.mtNodeId;
            copyStr(s_cfg.mtNodeIdStr, sizeof(s_cfg.mtNodeIdStr), v2.mtNodeIdStr);
            copyStr(s_cfg.mtLongName,  sizeof(s_cfg.mtLongName),  v2.mtLongName);
            copyStr(s_cfg.mtShortName, sizeof(s_cfg.mtShortName), v2.mtShortName);
            migrateV2Channel((uint8_t)LORA_RADIO1_SYNC_WORD, v2,
                             s_cfg.radio[0].channelName, sizeof(s_cfg.radio[0].channelName),
                             s_cfg.radio[0].channelKey,  sizeof(s_cfg.radio[0].channelKey));
            migrateV2Channel((uint8_t)LORA_RADIO2_SYNC_WORD, v2,
                             s_cfg.radio[1].channelName, sizeof(s_cfg.radio[1].channelName),
                             s_cfg.radio[1].channelKey,  sizeof(s_cfg.radio[1].channelKey));
            terminateAll();
            s_cfg.version = SCHEMA_VERSION;
            prefs.putBytes(NVS_KEY_BLOB, &s_cfg, sizeof(s_cfg));   // persist upgrade
            Serial.printf("[BridgeConfig] migrated v2 blob -> v5 (configured=%u)\n",
                          (unsigned)s_cfg.configured);
        } else {
            Serial.printf("[BridgeConfig] v2-sized blob bad (got %u B, ver %u); keeping defaults\n",
                          (unsigned)got, (unsigned)v2.version);
        }
    } else if (blobSize > 0) {
        Serial.printf("[BridgeConfig] NVS blob unexpected size %u B (pre-v2?); keeping defaults\n",
                      (unsigned)blobSize);
    }
    prefs.end();
}

void save() {
    s_cfg.version    = SCHEMA_VERSION;
    s_cfg.configured = 1;
    Preferences prefs;
    if (!prefs.begin(NVS_NAMESPACE, /*readOnly=*/false)) {
        Serial.printf("[BridgeConfig] save: prefs.begin RW failed\n");
        return;
    }
    size_t wrote = prefs.putBytes(NVS_KEY_BLOB, &s_cfg, sizeof(s_cfg));
    prefs.end();
    Serial.printf("[BridgeConfig] saved %u B to NVS\n", (unsigned)wrote);
}

void resetToDefaults() {
    Preferences prefs;
    if (prefs.begin(NVS_NAMESPACE, /*readOnly=*/false)) {
        prefs.remove(NVS_KEY_BLOB);
        prefs.end();
    }
    loadDefaults();
    Serial.printf("[BridgeConfig] reset to build-flag defaults\n");
}

static int clampRadio(int radio) {
    if (radio < 0) return 0;
    if (radio >= NUM_RADIOS) return NUM_RADIOS - 1;
    return radio;
}

bool isConfigured()           { return s_cfg.configured != 0; }
uint32_t    mtNodeId()        { return s_cfg.mtNodeId; }
const char *mtNodeIdStr()     { return s_cfg.mtNodeIdStr; }
const char *mtLongName()      { return s_cfg.mtLongName; }
const char *mtShortName()     { return s_cfg.mtShortName; }
bool        positionEnabled()  { return s_cfg.positionEnabled  != 0; }
bool        telemetryEnabled() { return s_cfg.telemetryEnabled != 0; }

const char *radioChannelName(int radio) { return s_cfg.radio[clampRadio(radio)].channelName; }
const char *radioChannelKey(int radio)  { return s_cfg.radio[clampRadio(radio)].channelKey; }
const char *radio1ChannelName() { return radioChannelName(0); }
const char *radio1ChannelKey()  { return radioChannelKey(0); }
const char *radio2ChannelName() { return radioChannelName(1); }
const char *radio2ChannelKey()  { return radioChannelKey(1); }

uint8_t  region()                  { return s_cfg.region; }
uint8_t  radioProtocol(int radio)  { return s_cfg.radio[clampRadio(radio)].protocol; }
float    radioFrequency(int radio) { return s_cfg.radio[clampRadio(radio)].frequency; }
float    radioBandwidth(int radio) { return s_cfg.radio[clampRadio(radio)].bandwidth; }
uint8_t  radioSf(int radio)        { return s_cfg.radio[clampRadio(radio)].sf; }
uint8_t  radioCr(int radio)        { return s_cfg.radio[clampRadio(radio)].cr; }
uint8_t  radioSyncWord(int radio)  { return s_cfg.radio[clampRadio(radio)].syncWord; }
int8_t   radioTxPower(int radio)   { return s_cfg.radio[clampRadio(radio)].txPower; }
uint8_t  radioLwRegion(int radio)  { return s_cfg.radio[clampRadio(radio)].lwRegion; }
uint8_t  radioRouteMask(int radio) { return s_cfg.radio[clampRadio(radio)].routeMask; }

void setMtNodeId(uint32_t v)           { s_cfg.mtNodeId = v; }
void setMtNodeIdStr(const char *s)     { copyStr(s_cfg.mtNodeIdStr, sizeof(s_cfg.mtNodeIdStr), s); }
void setMtLongName(const char *s)      { copyStr(s_cfg.mtLongName,  sizeof(s_cfg.mtLongName),  s); }
void setMtShortName(const char *s)     { copyStr(s_cfg.mtShortName, sizeof(s_cfg.mtShortName), s); }

void setRadioChannelName(int radio, const char *s) {
    RadioSlot &r = s_cfg.radio[clampRadio(radio)];
    copyStr(r.channelName, sizeof(r.channelName), s);
}
void setRadioChannelKey(int radio, const char *s) {
    RadioSlot &r = s_cfg.radio[clampRadio(radio)];
    copyStr(r.channelKey, sizeof(r.channelKey), s);
}
void setRadio1ChannelName(const char *s) { setRadioChannelName(0, s); }
void setRadio1ChannelKey(const char *s)  { setRadioChannelKey(0, s); }
void setRadio2ChannelName(const char *s) { setRadioChannelName(1, s); }
void setRadio2ChannelKey(const char *s)  { setRadioChannelKey(1, s); }
void setPositionEnabled(bool v)        { s_cfg.positionEnabled  = v ? 1 : 0; }
void setTelemetryEnabled(bool v)       { s_cfg.telemetryEnabled = v ? 1 : 0; }

void setRegion(uint8_t v)                   { s_cfg.region = v; }
void setRadioProtocol(int radio, uint8_t v) { s_cfg.radio[clampRadio(radio)].protocol  = v; }
void setRadioFrequency(int radio, float v)  { s_cfg.radio[clampRadio(radio)].frequency = v; }
void setRadioBandwidth(int radio, float v)  { s_cfg.radio[clampRadio(radio)].bandwidth = v; }
void setRadioSf(int radio, uint8_t v)       { s_cfg.radio[clampRadio(radio)].sf        = v; }
void setRadioCr(int radio, uint8_t v)       { s_cfg.radio[clampRadio(radio)].cr        = v; }
void setRadioSyncWord(int radio, uint8_t v) { s_cfg.radio[clampRadio(radio)].syncWord  = v; }
void setRadioTxPower(int radio, int8_t v)   { s_cfg.radio[clampRadio(radio)].txPower   = v; }
void setRadioLwRegion(int radio, uint8_t v) { s_cfg.radio[clampRadio(radio)].lwRegion  = v; }
void setRadioRouteMask(int radio, uint8_t v){ s_cfg.radio[clampRadio(radio)].routeMask = v; }

void debugDump() {
    Serial.printf("[BridgeConfig] v%u configured=%u region=%u\n"
                  "  mtNodeId      = 0x%08lX (%s)\n"
                  "  mtLongName    = \"%s\"\n"
                  "  mtShortName   = \"%s\"\n"
                  "  positionEnabled  = %u\n"
                  "  telemetryEnabled = %u\n",
                  (unsigned)s_cfg.version, (unsigned)s_cfg.configured,
                  (unsigned)s_cfg.region,
                  (unsigned long)s_cfg.mtNodeId, s_cfg.mtNodeIdStr,
                  s_cfg.mtLongName, s_cfg.mtShortName,
                  (unsigned)s_cfg.positionEnabled,
                  (unsigned)s_cfg.telemetryEnabled);
    for (int i = 0; i < NUM_RADIOS; i++) {
        const RadioSlot &r = s_cfg.radio[i];
        Serial.printf("  radio%d: proto=%u sync=0x%02X "
                      "%.3f MHz BW%.1f SF%u CR%u TX%ddBm route=0x%X lwreg=%u "
                      "chan=\"%s\" key=\"%s\"\n",
                      i + 1, (unsigned)r.protocol, (unsigned)r.syncWord,
                      r.frequency, r.bandwidth, (unsigned)r.sf,
                      (unsigned)r.cr, (int)r.txPower, (unsigned)r.routeMask,
                      (unsigned)r.lwRegion, r.channelName, r.channelKey);
    }
}

}  // namespace BridgeConfig
