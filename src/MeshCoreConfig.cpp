// MeshCoreConfig.cpp — see MeshCoreConfig.h for design notes.
//
// As of F4 this module no longer reads build-flag macros directly. Instead
// it pulls the key (as 32-char hex) and channel display name from
// BridgeConfig, which itself defaults to the build-flag values but can be
// overridden via the captive portal at runtime.

#include "MeshCoreConfig.h"
#include "BridgeConfig.h"

#include <Arduino.h>
#include <string.h>
#include <mbedtls/sha256.h>

namespace MeshCoreConfig {

// Public-channel fallback used if the BridgeConfig key string is malformed
// (e.g. wrong length or non-hex characters). Identical to the historical
// hard-coded constant.
static const uint8_t FALLBACK_PUBLIC_KEY[16] = {
    0x8b, 0x33, 0x87, 0xe9, 0xc5, 0xcd, 0xea, 0x6a,
    0xc9, 0xe5, 0xed, 0xba, 0xa1, 0x15, 0xcd, 0x72
};

uint8_t      key[16]      = {};
uint8_t      channelHash  = 0;
const char  *channelName  = "public";

static int hexNibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Parse exactly 32 hex chars from `hex` into out[16]. Returns false on
// any non-hex character or wrong length.
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

void begin() {
    if (!parseKeyHex(BridgeConfig::mcKeyHex(), key)) {
        Serial.printf("[MC] BridgeConfig::mcKeyHex malformed (\"%s\"); "
                      "falling back to public channel\n",
                      BridgeConfig::mcKeyHex());
        memcpy(key, FALLBACK_PUBLIC_KEY, sizeof(key));
    }
    channelName = BridgeConfig::mcChannelName();

    // SHA-256(key)[0] is the on-air channel hash byte.
    uint8_t hash[32];
    mbedtls_sha256(key, sizeof(key), hash, /*is224=*/0);
    channelHash = hash[0];

    Serial.printf("[MC] channel=\"%s\" hash=0x%02X key=", channelName, channelHash);
    for (size_t i = 0; i < sizeof(key); i++) Serial.printf("%02x", key[i]);
    Serial.println();
}

}  // namespace MeshCoreConfig
