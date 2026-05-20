// MeshCoreConfig.cpp — see MeshCoreConfig.h for design notes.

#include "MeshCoreConfig.h"

#include <Arduino.h>
#include <string.h>
#include <mbedtls/sha256.h>

namespace MeshCoreConfig {

// Public-channel fallback used if the supplied key string is malformed.
static const uint8_t FALLBACK_PUBLIC_KEY[16] = {
    0x8b, 0x33, 0x87, 0xe9, 0xc5, 0xcd, 0xea, 0x6a,
    0xc9, 0xe5, 0xed, 0xba, 0xa1, 0x15, 0xcd, 0x72
};

static int hexNibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Parse exactly 32 hex chars from `hex` into out[16].
static bool parseKeyHex(const char *hex, uint8_t out[16]) {
    if (!hex) return false;
    for (size_t i = 0; i < 16; i++) {
        int hi = hexNibble(hex[2 * i]);
        int lo = hexNibble(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return hex[32] == 0;     // require exactly 32 hex chars
}

void resolve(const char *keyHex, const char *name, RadioChannel &out) {
    out.protocol = 0x12;     // SYNC_WORD_MESHCORE
    out.keyLen   = 16;

    if (!parseKeyHex(keyHex, out.key)) {
        Serial.printf("[MC] key hex malformed (\"%s\"); falling back to public channel\n",
                      keyHex ? keyHex : "(null)");
        memcpy(out.key, FALLBACK_PUBLIC_KEY, 16);
    }
    memset(out.key + 16, 0, 16);   // upper half unused for AES-128

    // name (bounded copy)
    if (!name) name = "";
    strncpy(out.name, name, sizeof(out.name) - 1);
    out.name[sizeof(out.name) - 1] = 0;

    // SHA-256(key)[0] is the on-air channel hash byte.
    uint8_t hash[32];
    mbedtls_sha256(out.key, 16, hash, /*is224=*/0);
    out.channelHash = hash[0];

    Serial.printf("[MC] channel=\"%s\" hash=0x%02X key=", out.name, out.channelHash);
    for (size_t i = 0; i < 16; i++) Serial.printf("%02x", out.key[i]);
    Serial.println();
}

}  // namespace MeshCoreConfig
