// NodeDB.cpp — see NodeDB.h for design notes.

#include "NodeDB.h"

#include <Preferences.h>
#include <string.h>

namespace NodeDB {

// In-RAM state. Populated by begin() from NVS, mutated by upsert(),
// queried by lookupShortName().
static Entry  s_table[MAX_NODES];
static size_t s_count = 0;

// NVS namespace + key for the serialised entries blob.
static const char *NVS_NAMESPACE = "nodedb";
static const char *NVS_KEY_BLOB  = "entries";

// On-flash record. Skips lastSeenMs because that's a relative millis()
// value with no meaning after reboot.
struct PersistedEntry {
    uint32_t nodeId;
    char     shortName[MAX_SHORT_NAME + 1];
    char     longName [MAX_LONG_NAME  + 1];
};

// Scratch buffer used by both begin() (read) and saveToNvs() (write).
// File-scope (BSS) on purpose: a local PersistedEntry[64] is ~3.3 KB and
// would blow the 4-8 KB radio-task stack when saveToNvs runs nested under
// bridgePacket -> extractMeshtasticNodeInfo (which itself holds a 240 B pt[]
// plus an AES context). NodeDB is single-threaded — only radio1Task calls
// upsert/lookup — so a shared static buffer is safe.
static PersistedEntry s_scratch[MAX_NODES];

static void saveToNvs() {
    for (size_t i = 0; i < s_count; i++) {
        s_scratch[i].nodeId = s_table[i].nodeId;
        // copy and force null termination so stale tail bytes don't leak
        strncpy(s_scratch[i].shortName, s_table[i].shortName, sizeof(s_scratch[i].shortName));
        s_scratch[i].shortName[sizeof(s_scratch[i].shortName) - 1] = 0;
        strncpy(s_scratch[i].longName, s_table[i].longName, sizeof(s_scratch[i].longName));
        s_scratch[i].longName[sizeof(s_scratch[i].longName) - 1] = 0;
    }
    Preferences prefs;
    if (!prefs.begin(NVS_NAMESPACE, /*readOnly=*/false)) {
        Serial.printf("[NodeDB] saveToNvs: prefs.begin RW failed\n");
        return;
    }
    size_t bytes = s_count * sizeof(PersistedEntry);
    size_t wrote = prefs.putBytes(NVS_KEY_BLOB, s_scratch, bytes);
    prefs.end();
    if (wrote != bytes) {
        Serial.printf("[NodeDB] saveToNvs: wrote %u of %u B\n",
                      (unsigned)wrote, (unsigned)bytes);
    }
}

void begin() {
    s_count = 0;
    Preferences prefs;
    // RW open so NVS auto-creates the namespace on a fresh device. A
    // read-only open would log "[E] nvs_open failed: NOT_FOUND" on every
    // first boot, which looks alarming but is harmless. We never call
    // putBytes from begin(), so flash isn't written here.
    if (!prefs.begin(NVS_NAMESPACE, /*readOnly=*/false)) {
        Serial.printf("[NodeDB] prefs.begin failed; starting empty\n");
        return;
    }
    size_t blobSize = prefs.getBytesLength(NVS_KEY_BLOB);
    if (blobSize == 0 || (blobSize % sizeof(PersistedEntry)) != 0) {
        prefs.end();
        Serial.printf("[NodeDB] NVS blob missing or malformed (%u B), starting empty\n",
                      (unsigned)blobSize);
        return;
    }
    size_t n = blobSize / sizeof(PersistedEntry);
    if (n > MAX_NODES) n = MAX_NODES;
    size_t got = prefs.getBytes(NVS_KEY_BLOB, s_scratch, n * sizeof(PersistedEntry));
    prefs.end();
    if (got != n * sizeof(PersistedEntry)) {
        Serial.printf("[NodeDB] NVS getBytes returned %u, expected %u\n",
                      (unsigned)got, (unsigned)(n * sizeof(PersistedEntry)));
        return;
    }
    for (size_t i = 0; i < n; i++) {
        s_table[s_count].nodeId     = s_scratch[i].nodeId;
        s_table[s_count].lastSeenMs = 0;
        strncpy(s_table[s_count].shortName, s_scratch[i].shortName, MAX_SHORT_NAME);
        s_table[s_count].shortName[MAX_SHORT_NAME] = 0;
        strncpy(s_table[s_count].longName,  s_scratch[i].longName,  MAX_LONG_NAME);
        s_table[s_count].longName[MAX_LONG_NAME] = 0;
        s_count++;
    }
    Serial.printf("[NodeDB] loaded %u entries from NVS\n", (unsigned)s_count);
}

bool upsert(uint32_t nodeId, const char *shortName, const char *longName) {
    if (nodeId == 0 || nodeId == 0xFFFFFFFFu) return false;
    if (!shortName) shortName = "";
    if (!longName)  longName  = "";

    // Existing entry — update in place
    for (size_t i = 0; i < s_count; i++) {
        if (s_table[i].nodeId == nodeId) {
            strncpy(s_table[i].shortName, shortName, MAX_SHORT_NAME);
            s_table[i].shortName[MAX_SHORT_NAME] = 0;
            strncpy(s_table[i].longName,  longName,  MAX_LONG_NAME);
            s_table[i].longName[MAX_LONG_NAME] = 0;
            s_table[i].lastSeenMs = millis();
            saveToNvs();
            return true;
        }
    }

    // New entry — append or evict LRU
    size_t slot;
    if (s_count < MAX_NODES) {
        slot = s_count++;
    } else {
        slot = 0;
        for (size_t i = 1; i < s_count; i++) {
            if (s_table[i].lastSeenMs < s_table[slot].lastSeenMs) slot = i;
        }
        Serial.printf("[NodeDB] evicting !%08lX to make room for !%08lX\n",
                      (unsigned long)s_table[slot].nodeId,
                      (unsigned long)nodeId);
    }
    s_table[slot].nodeId = nodeId;
    strncpy(s_table[slot].shortName, shortName, MAX_SHORT_NAME);
    s_table[slot].shortName[MAX_SHORT_NAME] = 0;
    strncpy(s_table[slot].longName,  longName,  MAX_LONG_NAME);
    s_table[slot].longName[MAX_LONG_NAME] = 0;
    s_table[slot].lastSeenMs = millis();
    saveToNvs();
    return true;
}

const char *lookupShortName(uint32_t nodeId) {
    for (size_t i = 0; i < s_count; i++) {
        if (s_table[i].nodeId == nodeId) {
            s_table[i].lastSeenMs = millis();
            return s_table[i].shortName;
        }
    }
    return nullptr;
}

void debugDump() {
    Serial.printf("[NodeDB] %u/%u entries:\n",
                  (unsigned)s_count, (unsigned)MAX_NODES);
    for (size_t i = 0; i < s_count; i++) {
        Serial.printf("  !%08lX  short=\"%s\"  long=\"%s\"  last=%lu ms\n",
                      (unsigned long)s_table[i].nodeId,
                      s_table[i].shortName, s_table[i].longName,
                      (unsigned long)s_table[i].lastSeenMs);
    }
}

}  // namespace NodeDB
