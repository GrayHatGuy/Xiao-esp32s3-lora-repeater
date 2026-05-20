// BridgeConfig.cpp — see BridgeConfig.h for design notes.

#include "BridgeConfig.h"

#include <Preferences.h>
#include <string.h>

// --- Build-flag defaults ---------------------------------------------------
// These are the values used on a fresh device (or after resetToDefaults).
// They mirror the BRIDGE_* macros in platformio.ini so existing builds
// behave identically until the user opens the captive portal.

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
// MT channel: empty PSK string == the LongFast default channel, so the
// out-of-box behaviour is unchanged from before F5.
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

namespace BridgeConfig {

// On-flash schema. The version byte at the start of the blob + the blob's
// total size together identify the layout, so begin() can migrate older
// blobs forward instead of discarding the user's saved config.
static constexpr uint8_t SCHEMA_VERSION = 2;

// Schema v1 — kept verbatim so a v1 blob written by a pre-F5 build can be
// read and migrated. Do not edit this struct.
struct PersistedV1 {
    uint8_t  version;
    uint8_t  configured;
    uint8_t  positionEnabled;
    uint8_t  telemetryEnabled;
    uint32_t mtNodeId;
    char     mtNodeIdStr [MT_NODE_ID_STR_MAX + 1];
    char     mtLongName  [MT_LONG_NAME_MAX   + 1];
    char     mtShortName [MT_SHORT_NAME_MAX  + 1];
    char     mcKeyHex    [MC_KEY_HEX_LEN     + 1];
    char     mcChannelName[MC_CHANNEL_NAME_MAX + 1];
};

// Schema v2 — adds the Meshtastic channel name + base64 PSK (F5).
struct PersistedV2 {
    uint8_t  version;
    uint8_t  configured;
    uint8_t  positionEnabled;
    uint8_t  telemetryEnabled;
    uint32_t mtNodeId;
    char     mtNodeIdStr   [MT_NODE_ID_STR_MAX  + 1];
    char     mtLongName    [MT_LONG_NAME_MAX    + 1];
    char     mtShortName   [MT_SHORT_NAME_MAX   + 1];
    char     mcKeyHex      [MC_KEY_HEX_LEN      + 1];
    char     mcChannelName [MC_CHANNEL_NAME_MAX + 1];
    char     mtChannelName [MT_CHANNEL_NAME_MAX + 1];
    char     mtPskBase64   [MT_PSK_B64_MAX      + 1];
};

static PersistedV2 s_cfg;

static const char *NVS_NAMESPACE = "bridgecfg";
// Storage key — opaque identifier, not tied to schema version (the version
// byte lives inside the blob). Kept as "v1" so a pre-F5 blob is still found.
static const char *NVS_KEY_BLOB  = "v1";

// Copy a C string into a fixed-size buffer with null termination.
static void copyStr(char *dst, size_t dstCap, const char *src) {
    if (!dst || dstCap == 0) return;
    if (!src) src = "";
    strncpy(dst, src, dstCap - 1);
    dst[dstCap - 1] = 0;
}

static void loadDefaults() {
    memset(&s_cfg, 0, sizeof(s_cfg));
    s_cfg.version          = SCHEMA_VERSION;
    s_cfg.configured       = 0;
    s_cfg.mtNodeId         = (uint32_t)BRIDGE_MT_NODE_ID;
    s_cfg.positionEnabled  = (BRIDGE_MT_POSITION  ? 1 : 0);
    s_cfg.telemetryEnabled = (BRIDGE_MT_TELEMETRY ? 1 : 0);
    copyStr(s_cfg.mtNodeIdStr,   sizeof(s_cfg.mtNodeIdStr),   BRIDGE_MT_NODE_ID_STR);
    copyStr(s_cfg.mtLongName,    sizeof(s_cfg.mtLongName),    BRIDGE_MT_LONG_NAME);
    copyStr(s_cfg.mtShortName,   sizeof(s_cfg.mtShortName),   BRIDGE_MT_SHORT_NAME);
    copyStr(s_cfg.mcKeyHex,      sizeof(s_cfg.mcKeyHex),      BRIDGE_MC_KEY_HEX);
    copyStr(s_cfg.mcChannelName, sizeof(s_cfg.mcChannelName), BRIDGE_MC_CHANNEL_NAME);
    copyStr(s_cfg.mtChannelName, sizeof(s_cfg.mtChannelName), BRIDGE_MT_CHANNEL_NAME);
    copyStr(s_cfg.mtPskBase64,   sizeof(s_cfg.mtPskBase64),   BRIDGE_MT_PSK_B64);
}

// Defensively force null-termination on every string field.
static void terminateAll() {
    s_cfg.mtNodeIdStr   [sizeof(s_cfg.mtNodeIdStr)   - 1] = 0;
    s_cfg.mtLongName    [sizeof(s_cfg.mtLongName)    - 1] = 0;
    s_cfg.mtShortName   [sizeof(s_cfg.mtShortName)   - 1] = 0;
    s_cfg.mcKeyHex      [sizeof(s_cfg.mcKeyHex)      - 1] = 0;
    s_cfg.mcChannelName [sizeof(s_cfg.mcChannelName) - 1] = 0;
    s_cfg.mtChannelName [sizeof(s_cfg.mtChannelName) - 1] = 0;
    s_cfg.mtPskBase64   [sizeof(s_cfg.mtPskBase64)   - 1] = 0;
}

void begin() {
    loadDefaults();
    Preferences prefs;
    if (!prefs.begin(NVS_NAMESPACE, /*readOnly=*/false)) {
        Serial.printf("[BridgeConfig] prefs.begin failed; using build-flag defaults\n");
        return;
    }
    size_t blobSize = prefs.getBytesLength(NVS_KEY_BLOB);

    if (blobSize == sizeof(PersistedV2)) {
        PersistedV2 tmp;
        size_t got = prefs.getBytes(NVS_KEY_BLOB, &tmp, sizeof(tmp));
        if (got == sizeof(PersistedV2) && tmp.version == 2) {
            s_cfg = tmp;
            terminateAll();
            Serial.printf("[BridgeConfig] loaded v2 blob from NVS (configured=%u)\n",
                          (unsigned)s_cfg.configured);
        } else {
            Serial.printf("[BridgeConfig] v2-sized blob bad (got %u B, ver %u); keeping defaults\n",
                          (unsigned)got, (unsigned)tmp.version);
        }
    } else if (blobSize == sizeof(PersistedV1)) {
        // Migrate a pre-F5 v1 blob: copy the eight fields it had, leave the
        // two new MT-channel fields at their build-flag defaults (which is
        // the LongFast channel — identical behaviour to the v1 firmware).
        PersistedV1 v1;
        size_t got = prefs.getBytes(NVS_KEY_BLOB, &v1, sizeof(v1));
        if (got == sizeof(PersistedV1) && v1.version == 1) {
            s_cfg.configured       = v1.configured;
            s_cfg.positionEnabled  = v1.positionEnabled;
            s_cfg.telemetryEnabled = v1.telemetryEnabled;
            s_cfg.mtNodeId         = v1.mtNodeId;
            copyStr(s_cfg.mtNodeIdStr,   sizeof(s_cfg.mtNodeIdStr),   v1.mtNodeIdStr);
            copyStr(s_cfg.mtLongName,    sizeof(s_cfg.mtLongName),    v1.mtLongName);
            copyStr(s_cfg.mtShortName,   sizeof(s_cfg.mtShortName),   v1.mtShortName);
            copyStr(s_cfg.mcKeyHex,      sizeof(s_cfg.mcKeyHex),      v1.mcKeyHex);
            copyStr(s_cfg.mcChannelName, sizeof(s_cfg.mcChannelName), v1.mcChannelName);
            terminateAll();
            Serial.printf("[BridgeConfig] migrated v1 blob -> v2 (configured=%u); "
                          "MT channel defaulted to LongFast\n",
                          (unsigned)s_cfg.configured);
            // Persist the upgraded blob so the migration only runs once.
            s_cfg.version = SCHEMA_VERSION;
            prefs.putBytes(NVS_KEY_BLOB, &s_cfg, sizeof(s_cfg));
        } else {
            Serial.printf("[BridgeConfig] v1-sized blob bad (got %u B, ver %u); keeping defaults\n",
                          (unsigned)got, (unsigned)v1.version);
        }
    } else if (blobSize > 0) {
        Serial.printf("[BridgeConfig] NVS blob unexpected size %u B; keeping defaults\n",
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

bool isConfigured()         { return s_cfg.configured != 0; }
uint32_t    mtNodeId()      { return s_cfg.mtNodeId; }
const char *mtNodeIdStr()   { return s_cfg.mtNodeIdStr; }
const char *mtLongName()    { return s_cfg.mtLongName; }
const char *mtShortName()   { return s_cfg.mtShortName; }
const char *mcKeyHex()      { return s_cfg.mcKeyHex; }
const char *mcChannelName() { return s_cfg.mcChannelName; }
const char *mtChannelName() { return s_cfg.mtChannelName; }
const char *mtPskBase64()   { return s_cfg.mtPskBase64; }
bool        positionEnabled()  { return s_cfg.positionEnabled  != 0; }
bool        telemetryEnabled() { return s_cfg.telemetryEnabled != 0; }

void setMtNodeId(uint32_t v)         { s_cfg.mtNodeId = v; }
void setMtNodeIdStr(const char *s)   { copyStr(s_cfg.mtNodeIdStr,   sizeof(s_cfg.mtNodeIdStr),   s); }
void setMtLongName(const char *s)    { copyStr(s_cfg.mtLongName,    sizeof(s_cfg.mtLongName),    s); }
void setMtShortName(const char *s)   { copyStr(s_cfg.mtShortName,   sizeof(s_cfg.mtShortName),   s); }
void setMcKeyHex(const char *s)      { copyStr(s_cfg.mcKeyHex,      sizeof(s_cfg.mcKeyHex),      s); }
void setMcChannelName(const char *s) { copyStr(s_cfg.mcChannelName, sizeof(s_cfg.mcChannelName), s); }
void setMtChannelName(const char *s) { copyStr(s_cfg.mtChannelName, sizeof(s_cfg.mtChannelName), s); }
void setMtPskBase64(const char *s)   { copyStr(s_cfg.mtPskBase64,   sizeof(s_cfg.mtPskBase64),   s); }
void setPositionEnabled(bool v)      { s_cfg.positionEnabled  = v ? 1 : 0; }
void setTelemetryEnabled(bool v)     { s_cfg.telemetryEnabled = v ? 1 : 0; }

void debugDump() {
    Serial.printf("[BridgeConfig] v%u configured=%u\n"
                  "  mtNodeId      = 0x%08lX (%s)\n"
                  "  mtLongName    = \"%s\"\n"
                  "  mtShortName   = \"%s\"\n"
                  "  mtChannel     = \"%s\"\n"
                  "  mtPskBase64   = \"%s\"\n"
                  "  mcChannel     = \"%s\"\n"
                  "  mcKeyHex      = %s\n"
                  "  positionEnabled  = %u\n"
                  "  telemetryEnabled = %u\n",
                  (unsigned)s_cfg.version, (unsigned)s_cfg.configured,
                  (unsigned long)s_cfg.mtNodeId, s_cfg.mtNodeIdStr,
                  s_cfg.mtLongName, s_cfg.mtShortName,
                  s_cfg.mtChannelName, s_cfg.mtPskBase64,
                  s_cfg.mcChannelName, s_cfg.mcKeyHex,
                  (unsigned)s_cfg.positionEnabled,
                  (unsigned)s_cfg.telemetryEnabled);
}

}  // namespace BridgeConfig
