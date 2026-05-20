// MeshtasticConfig.cpp — see MeshtasticConfig.h for design notes.

#include "MeshtasticConfig.h"
#include "BridgeConfig.h"

#include <Arduino.h>
#include <string.h>
#include <mbedtls/base64.h>

namespace MeshtasticConfig {

// Meshtastic defaultpsk — the base key behind the "AQ==" / LongFast channel.
// Identical to the historical hard-coded MESHTASTIC_DEFAULT_KEY constant.
static const uint8_t DEFAULT_PSK[16] = {
    0xd4, 0xf1, 0xbb, 0x3a, 0x20, 0x29, 0x07, 0x59,
    0xf0, 0xbc, 0xff, 0xab, 0xcf, 0x4e, 0x69, 0x01
};

uint8_t      key[32]      = {};
uint8_t      keyLen       = 16;
uint8_t      channelHash  = 0;
const char  *channelName  = "LongFast";

// XOR-fold a byte range to a single byte — Meshtastic's channel-hash primitive.
static uint8_t xorFold(const uint8_t *data, size_t len) {
    uint8_t h = 0;
    for (size_t i = 0; i < len; i++) h ^= data[i];
    return h;
}

// Install the LongFast default key (also the fallback on any malformed PSK).
static void useDefaultChannel() {
    memcpy(key, DEFAULT_PSK, sizeof(DEFAULT_PSK));
    keyLen = 16;
}

void begin() {
    channelName = BridgeConfig::mtChannelName();

    const char *pskB64 = BridgeConfig::mtPskBase64();
    uint8_t raw[32];
    size_t  rawLen = 0;

    if (pskB64 && pskB64[0]) {
        int rc = mbedtls_base64_decode(raw, sizeof(raw), &rawLen,
                                        (const unsigned char *)pskB64,
                                        strlen(pskB64));
        if (rc != 0) {
            Serial.printf("[MT] PSK base64 malformed (\"%s\"); using LongFast default\n",
                          pskB64);
            rawLen = 0;
            useDefaultChannel();
            goto hash;
        }
    } else {
        rawLen = 0;     // empty PSK string => default/primary channel
    }

    if (rawLen == 0) {
        useDefaultChannel();
    } else if (rawLen == 1) {
        uint8_t idx = raw[0];
        if (idx == 0) {
            // Index 0 means "encryption off" in Meshtastic. The bridge's
            // decoders always decrypt, so a plaintext channel isn't
            // supported — fall back to the default channel with a warning.
            Serial.printf("[MT] PSK index 0 (encryption off) unsupported; "
                          "using LongFast default\n");
            useDefaultChannel();
        } else {
            memcpy(key, DEFAULT_PSK, sizeof(DEFAULT_PSK));
            key[15] = (uint8_t)(DEFAULT_PSK[15] + idx - 1);
            keyLen  = 16;
        }
    } else if (rawLen == 16 || rawLen == 32) {
        memcpy(key, raw, rawLen);
        keyLen = (uint8_t)rawLen;
    } else {
        Serial.printf("[MT] PSK length %u invalid (expected 0/1/16/32 B); "
                      "using LongFast default\n", (unsigned)rawLen);
        useDefaultChannel();
    }

hash:
    // channel hash = XOR-fold(name) ^ XOR-fold(expanded key)
    channelHash = (uint8_t)(xorFold((const uint8_t *)channelName,
                                     strlen(channelName))
                          ^ xorFold(key, keyLen));

    Serial.printf("[MT] channel=\"%s\" hash=0x%02X keyLen=%u key=",
                  channelName, channelHash, (unsigned)keyLen);
    for (size_t i = 0; i < keyLen; i++) Serial.printf("%02x", key[i]);
    Serial.println();
}

}  // namespace MeshtasticConfig
