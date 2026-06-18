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
// Per-radio RF — first-boot defaults for the v8 schema-v4 RF fields.
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
// Global region — first-boot default. REGION_UNSET (0) until set in portal.
#ifndef BRIDGE_REGION
  #define BRIDGE_REGION              0
#endif

namespace BridgeConfig {

static constexpr uint8_t SCHEMA_VERSION = 4;

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

// Per-radio protocol + full RF plan (v8 schema v4).
struct RadioRf {
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

// Schema v4 — global region + per-radio protocol/RF.
struct PersistedV4 {
    uint8_t  version;
    uint8_t  configured;
    uint8_t  positionEnabled;
    uint8_t  telemetryEnabled;
    uint8_t  region;       // BridgeConfig::Region
    uint8_t  _pad[3];
    uint32_t mtNodeId;
    char     mtNodeIdStr  [MT_NODE_ID_STR_MAX     + 1];
    char     mtLongName   [MT_LONG_NAME_MAX       + 1];
    char     mtShortName  [MT_SHORT_NAME_MAX      + 1];
    char     r1ChannelName[RADIO_CHANNEL_NAME_MAX + 1];
    char     r1ChannelKey [RADIO_CHANNEL_KEY_MAX  + 1];
    char     r2ChannelName[RADIO_CHANNEL_NAME_MAX + 1];
    char     r2ChannelKey [RADIO_CHANNEL_KEY_MAX  + 1];
    RadioRf  radio[2];
};

static PersistedV4 s_cfg;

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
    radioDefaultChannel((uint8_t)LORA_RADIO1_SYNC_WORD,
                        s_cfg.r1ChannelName, sizeof(s_cfg.r1ChannelName),
                        s_cfg.r1ChannelKey,  sizeof(s_cfg.r1ChannelKey));
    radioDefaultChannel((uint8_t)LORA_RADIO2_SYNC_WORD,
                        s_cfg.r2ChannelName, sizeof(s_cfg.r2ChannelName),
                        s_cfg.r2ChannelKey,  sizeof(s_cfg.r2ChannelKey));

    // Optional PER-RADIO channel name/key first-boot overrides (v8.4.1). The
    // global BRIDGE_MT_*/BRIDGE_MC_* defaults above are picked by each radio's
    // sync word, so two radios on the SAME protocol (e.g. MT public -> MT
    // private) would otherwise share one channel. Defining these lets each radio
    // preload a distinct channel from platformio.ini. Do-no-harm: only applied
    // when the macro is defined (otherwise the sync-word default above stands).
#ifdef LORA_RADIO1_CHANNEL_NAME
    copyStr(s_cfg.r1ChannelName, sizeof(s_cfg.r1ChannelName), LORA_RADIO1_CHANNEL_NAME);
#endif
#ifdef LORA_RADIO1_CHANNEL_KEY
    copyStr(s_cfg.r1ChannelKey,  sizeof(s_cfg.r1ChannelKey),  LORA_RADIO1_CHANNEL_KEY);
#endif
#ifdef LORA_RADIO2_CHANNEL_NAME
    copyStr(s_cfg.r2ChannelName, sizeof(s_cfg.r2ChannelName), LORA_RADIO2_CHANNEL_NAME);
#endif
#ifdef LORA_RADIO2_CHANNEL_KEY
    copyStr(s_cfg.r2ChannelKey,  sizeof(s_cfg.r2ChannelKey),  LORA_RADIO2_CHANNEL_KEY);
#endif

    s_cfg.radio[0].syncWord  = (uint8_t)LORA_RADIO1_SYNC_WORD;
    s_cfg.radio[0].protocol  = protocolFromSync((uint8_t)LORA_RADIO1_SYNC_WORD);
    s_cfg.radio[0].frequency = (float)(LORA_RADIO1_FREQUENCY);
    s_cfg.radio[0].bandwidth = (float)(LORA_RADIO1_BANDWIDTH);
    s_cfg.radio[0].sf        = (uint8_t)(LORA_RADIO1_SPREAD_FACTOR);
    s_cfg.radio[0].cr        = (uint8_t)(LORA_RADIO1_CODING_RATE);
    s_cfg.radio[0].txPower   = (int8_t)(LORA_RADIO1_TX_POWER);

    s_cfg.radio[1].syncWord  = (uint8_t)LORA_RADIO2_SYNC_WORD;
    s_cfg.radio[1].protocol  = protocolFromSync((uint8_t)LORA_RADIO2_SYNC_WORD);
    s_cfg.radio[1].frequency = (float)(LORA_RADIO2_FREQUENCY);
    s_cfg.radio[1].bandwidth = (float)(LORA_RADIO2_BANDWIDTH);
    s_cfg.radio[1].sf        = (uint8_t)(LORA_RADIO2_SPREAD_FACTOR);
    s_cfg.radio[1].cr        = (uint8_t)(LORA_RADIO2_CODING_RATE);
    s_cfg.radio[1].txPower   = (int8_t)(LORA_RADIO2_TX_POWER);
}

static void terminateAll() {
    s_cfg.mtNodeIdStr  [sizeof(s_cfg.mtNodeIdStr)   - 1] = 0;
    s_cfg.mtLongName   [sizeof(s_cfg.mtLongName)    - 1] = 0;
    s_cfg.mtShortName  [sizeof(s_cfg.mtShortName)   - 1] = 0;
    s_cfg.r1ChannelName[sizeof(s_cfg.r1ChannelName) - 1] = 0;
    s_cfg.r1ChannelKey [sizeof(s_cfg.r1ChannelKey)  - 1] = 0;
    s_cfg.r2ChannelName[sizeof(s_cfg.r2ChannelName) - 1] = 0;
    s_cfg.r2ChannelKey [sizeof(s_cfg.r2ChannelKey)  - 1] = 0;
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

void begin() {
    // loadDefaults() fully populates s_cfg with v4 build-flag defaults; the
    // migration paths below overwrite only the fields a v2/v3 blob carries,
    // leaving region + per-radio protocol/RF at their build-flag values.
    loadDefaults();
    Preferences prefs;
    if (!prefs.begin(NVS_NAMESPACE, /*readOnly=*/false)) {
        Serial.printf("[BridgeConfig] prefs.begin failed; using build-flag defaults\n");
        return;
    }
    size_t blobSize = prefs.getBytesLength(NVS_KEY_BLOB);

    if (blobSize == sizeof(PersistedV4)) {
        PersistedV4 tmp;
        size_t got = prefs.getBytes(NVS_KEY_BLOB, &tmp, sizeof(tmp));
        if (got == sizeof(PersistedV4) && tmp.version == 4) {
            s_cfg = tmp;
            terminateAll();
            Serial.printf("[BridgeConfig] loaded v4 blob from NVS (configured=%u)\n",
                          (unsigned)s_cfg.configured);
        } else {
            Serial.printf("[BridgeConfig] v4-sized blob bad (got %u B, ver %u); keeping defaults\n",
                          (unsigned)got, (unsigned)tmp.version);
        }
    } else if (blobSize == sizeof(PersistedV3)) {
        PersistedV3 v3;
        size_t got = prefs.getBytes(NVS_KEY_BLOB, &v3, sizeof(v3));
        if (got == sizeof(PersistedV3) && v3.version == 3) {
            s_cfg.configured       = v3.configured;
            s_cfg.positionEnabled  = v3.positionEnabled;
            s_cfg.telemetryEnabled = v3.telemetryEnabled;
            s_cfg.mtNodeId         = v3.mtNodeId;
            copyStr(s_cfg.mtNodeIdStr,   sizeof(s_cfg.mtNodeIdStr),   v3.mtNodeIdStr);
            copyStr(s_cfg.mtLongName,    sizeof(s_cfg.mtLongName),    v3.mtLongName);
            copyStr(s_cfg.mtShortName,   sizeof(s_cfg.mtShortName),   v3.mtShortName);
            copyStr(s_cfg.r1ChannelName, sizeof(s_cfg.r1ChannelName), v3.r1ChannelName);
            copyStr(s_cfg.r1ChannelKey,  sizeof(s_cfg.r1ChannelKey),  v3.r1ChannelKey);
            copyStr(s_cfg.r2ChannelName, sizeof(s_cfg.r2ChannelName), v3.r2ChannelName);
            copyStr(s_cfg.r2ChannelKey,  sizeof(s_cfg.r2ChannelKey),  v3.r2ChannelKey);
            // region + per-radio protocol/RF stay at build-flag defaults
            // (set by loadDefaults) so an upgraded v3 device keeps running.
            terminateAll();
            s_cfg.version = SCHEMA_VERSION;
            prefs.putBytes(NVS_KEY_BLOB, &s_cfg, sizeof(s_cfg));   // persist upgrade
            Serial.printf("[BridgeConfig] migrated v3 blob -> v4 (configured=%u)\n",
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
                             s_cfg.r1ChannelName, sizeof(s_cfg.r1ChannelName),
                             s_cfg.r1ChannelKey,  sizeof(s_cfg.r1ChannelKey));
            migrateV2Channel((uint8_t)LORA_RADIO2_SYNC_WORD, v2,
                             s_cfg.r2ChannelName, sizeof(s_cfg.r2ChannelName),
                             s_cfg.r2ChannelKey,  sizeof(s_cfg.r2ChannelKey));
            terminateAll();
            s_cfg.version = SCHEMA_VERSION;
            prefs.putBytes(NVS_KEY_BLOB, &s_cfg, sizeof(s_cfg));   // persist upgrade
            Serial.printf("[BridgeConfig] migrated v2 blob -> v4 (configured=%u)\n",
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

static int clampRadio(int radio) { return (radio == 1) ? 1 : 0; }

bool isConfigured()           { return s_cfg.configured != 0; }
uint32_t    mtNodeId()        { return s_cfg.mtNodeId; }
const char *mtNodeIdStr()     { return s_cfg.mtNodeIdStr; }
const char *mtLongName()      { return s_cfg.mtLongName; }
const char *mtShortName()     { return s_cfg.mtShortName; }
const char *radio1ChannelName() { return s_cfg.r1ChannelName; }
const char *radio1ChannelKey()  { return s_cfg.r1ChannelKey; }
const char *radio2ChannelName() { return s_cfg.r2ChannelName; }
const char *radio2ChannelKey()  { return s_cfg.r2ChannelKey; }
bool        positionEnabled()  { return s_cfg.positionEnabled  != 0; }
bool        telemetryEnabled() { return s_cfg.telemetryEnabled != 0; }

uint8_t  region()                  { return s_cfg.region; }
uint8_t  radioProtocol(int radio)  { return s_cfg.radio[clampRadio(radio)].protocol; }
float    radioFrequency(int radio) { return s_cfg.radio[clampRadio(radio)].frequency; }
float    radioBandwidth(int radio) { return s_cfg.radio[clampRadio(radio)].bandwidth; }
uint8_t  radioSf(int radio)        { return s_cfg.radio[clampRadio(radio)].sf; }
uint8_t  radioCr(int radio)        { return s_cfg.radio[clampRadio(radio)].cr; }
uint8_t  radioSyncWord(int radio)  { return s_cfg.radio[clampRadio(radio)].syncWord; }
int8_t   radioTxPower(int radio)   { return s_cfg.radio[clampRadio(radio)].txPower; }
uint8_t  radioLwRegion(int radio)  { return s_cfg.radio[clampRadio(radio)].lwRegion; }

void setMtNodeId(uint32_t v)           { s_cfg.mtNodeId = v; }
void setMtNodeIdStr(const char *s)     { copyStr(s_cfg.mtNodeIdStr, sizeof(s_cfg.mtNodeIdStr), s); }
void setMtLongName(const char *s)      { copyStr(s_cfg.mtLongName,  sizeof(s_cfg.mtLongName),  s); }
void setMtShortName(const char *s)     { copyStr(s_cfg.mtShortName, sizeof(s_cfg.mtShortName), s); }
void setRadio1ChannelName(const char *s) { copyStr(s_cfg.r1ChannelName, sizeof(s_cfg.r1ChannelName), s); }
void setRadio1ChannelKey(const char *s)  { copyStr(s_cfg.r1ChannelKey,  sizeof(s_cfg.r1ChannelKey),  s); }
void setRadio2ChannelName(const char *s) { copyStr(s_cfg.r2ChannelName, sizeof(s_cfg.r2ChannelName), s); }
void setRadio2ChannelKey(const char *s)  { copyStr(s_cfg.r2ChannelKey,  sizeof(s_cfg.r2ChannelKey),  s); }
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

void debugDump() {
    Serial.printf("[BridgeConfig] v%u configured=%u region=%u\n"
                  "  mtNodeId      = 0x%08lX (%s)\n"
                  "  mtLongName    = \"%s\"\n"
                  "  mtShortName   = \"%s\"\n"
                  "  radio1 channel= \"%s\"  key=\"%s\"\n"
                  "  radio2 channel= \"%s\"  key=\"%s\"\n"
                  "  positionEnabled  = %u\n"
                  "  telemetryEnabled = %u\n",
                  (unsigned)s_cfg.version, (unsigned)s_cfg.configured,
                  (unsigned)s_cfg.region,
                  (unsigned long)s_cfg.mtNodeId, s_cfg.mtNodeIdStr,
                  s_cfg.mtLongName, s_cfg.mtShortName,
                  s_cfg.r1ChannelName, s_cfg.r1ChannelKey,
                  s_cfg.r2ChannelName, s_cfg.r2ChannelKey,
                  (unsigned)s_cfg.positionEnabled,
                  (unsigned)s_cfg.telemetryEnabled);
    for (int i = 0; i < 2; i++) {
        const RadioRf &r = s_cfg.radio[i];
        Serial.printf("  radio%d RF     = proto=%u sync=0x%02X "
                      "%.3f MHz BW%.1f SF%u CR%u TX%ddBm\n",
                      i + 1, (unsigned)r.protocol, (unsigned)r.syncWord,
                      r.frequency, r.bandwidth, (unsigned)r.sf,
                      (unsigned)r.cr, (int)r.txPower);
    }
}

}  // namespace BridgeConfig
