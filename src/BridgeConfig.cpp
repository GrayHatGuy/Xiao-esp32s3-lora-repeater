// BridgeConfig.cpp — see BridgeConfig.h for design notes.

#include "BridgeConfig.h"

#include <Preferences.h>
#include <string.h>

// --- Build-flag defaults ---------------------------------------------------
// These are the values used on a fresh device (or after resetToDefaults).
// They mirror the existing BRIDGE_* macros in platformio.ini so existing
// builds behave identically until the user opens the captive portal.

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
#ifndef BRIDGE_MT_POSITION
  #define BRIDGE_MT_POSITION     1
#endif
#ifndef BRIDGE_MT_TELEMETRY
  #define BRIDGE_MT_TELEMETRY    1
#endif

namespace BridgeConfig {

// On-flash schema. Version byte at the start so future migrations can be
// detected; the rest is fixed-size POD so we can write/read in one blob.
static constexpr uint8_t SCHEMA_VERSION = 1;

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

static PersistedV1 s_cfg;

static const char *NVS_NAMESPACE = "bridgecfg";
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
}

void begin() {
    loadDefaults();
    Preferences prefs;
    if (!prefs.begin(NVS_NAMESPACE, /*readOnly=*/false)) {
        Serial.printf("[BridgeConfig] prefs.begin failed; using build-flag defaults\n");
        return;
    }
    size_t blobSize = prefs.getBytesLength(NVS_KEY_BLOB);
    if (blobSize == sizeof(PersistedV1)) {
        PersistedV1 tmp;
        size_t got = prefs.getBytes(NVS_KEY_BLOB, &tmp, sizeof(tmp));
        if (got == sizeof(PersistedV1) && tmp.version == SCHEMA_VERSION) {
            // Force string null-termination defensively even when source NVS
            // is well-formed; a bad write in older firmware could otherwise
            // leave the field unterminated.
            tmp.mtNodeIdStr   [sizeof(tmp.mtNodeIdStr)    - 1] = 0;
            tmp.mtLongName    [sizeof(tmp.mtLongName)     - 1] = 0;
            tmp.mtShortName   [sizeof(tmp.mtShortName)    - 1] = 0;
            tmp.mcKeyHex      [sizeof(tmp.mcKeyHex)       - 1] = 0;
            tmp.mcChannelName [sizeof(tmp.mcChannelName)  - 1] = 0;
            s_cfg = tmp;
            Serial.printf("[BridgeConfig] loaded v%u blob from NVS (configured=%u)\n",
                          (unsigned)s_cfg.version, (unsigned)s_cfg.configured);
        } else {
            Serial.printf("[BridgeConfig] NVS blob version/size mismatch (got %u B, ver %u); "
                          "keeping defaults\n",
                          (unsigned)got, (unsigned)tmp.version);
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
bool        positionEnabled()  { return s_cfg.positionEnabled  != 0; }
bool        telemetryEnabled() { return s_cfg.telemetryEnabled != 0; }

void setMtNodeId(uint32_t v)         { s_cfg.mtNodeId = v; }
void setMtNodeIdStr(const char *s)   { copyStr(s_cfg.mtNodeIdStr,   sizeof(s_cfg.mtNodeIdStr),   s); }
void setMtLongName(const char *s)    { copyStr(s_cfg.mtLongName,    sizeof(s_cfg.mtLongName),    s); }
void setMtShortName(const char *s)   { copyStr(s_cfg.mtShortName,   sizeof(s_cfg.mtShortName),   s); }
void setMcKeyHex(const char *s)      { copyStr(s_cfg.mcKeyHex,      sizeof(s_cfg.mcKeyHex),      s); }
void setMcChannelName(const char *s) { copyStr(s_cfg.mcChannelName, sizeof(s_cfg.mcChannelName), s); }
void setPositionEnabled(bool v)      { s_cfg.positionEnabled  = v ? 1 : 0; }
void setTelemetryEnabled(bool v)     { s_cfg.telemetryEnabled = v ? 1 : 0; }

void debugDump() {
    Serial.printf("[BridgeConfig] v%u configured=%u\n"
                  "  mtNodeId      = 0x%08lX (%s)\n"
                  "  mtLongName    = \"%s\"\n"
                  "  mtShortName   = \"%s\"\n"
                  "  mcChannel     = \"%s\"\n"
                  "  mcKeyHex      = %s\n"
                  "  positionEnabled  = %u\n"
                  "  telemetryEnabled = %u\n",
                  (unsigned)s_cfg.version, (unsigned)s_cfg.configured,
                  (unsigned long)s_cfg.mtNodeId, s_cfg.mtNodeIdStr,
                  s_cfg.mtLongName, s_cfg.mtShortName,
                  s_cfg.mcChannelName, s_cfg.mcKeyHex,
                  (unsigned)s_cfg.positionEnabled,
                  (unsigned)s_cfg.telemetryEnabled);
}

}  // namespace BridgeConfig
