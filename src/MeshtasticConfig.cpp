// MeshtasticConfig.cpp — see MeshtasticConfig.h for design notes.

#include "MeshtasticConfig.h"

#include <Arduino.h>
#include <string.h>
#include <mbedtls/base64.h>

namespace MeshtasticConfig {

// Meshtastic defaultpsk — the base key behind the "AQ==" / LongFast channel.
static const uint8_t DEFAULT_PSK[16] = {
    0xd4, 0xf1, 0xbb, 0x3a, 0x20, 0x29, 0x07, 0x59,
    0xf0, 0xbc, 0xff, 0xab, 0xcf, 0x4e, 0x69, 0x01
};

// XOR-fold a byte range to a single byte — Meshtastic's channel-hash primitive.
static uint8_t xorFold(const uint8_t *data, size_t len) {
    uint8_t h = 0;
    for (size_t i = 0; i < len; i++) h ^= data[i];
    return h;
}

void resolve(const char *pskB64, const char *name, RadioChannel &out) {
    out.protocol = 0x2B;     // SYNC_WORD_MESHTASTIC
    memset(out.key, 0, sizeof(out.key));

    if (!name) name = "";
    strncpy(out.name, name, sizeof(out.name) - 1);
    out.name[sizeof(out.name) - 1] = 0;

    uint8_t raw[32];
    size_t  rawLen = 0;
    bool    decodeOk = true;
    if (pskB64 && pskB64[0]) {
        int rc = mbedtls_base64_decode(raw, sizeof(raw), &rawLen,
                                        (const unsigned char *)pskB64,
                                        strlen(pskB64));
        if (rc != 0) decodeOk = false;
    } else {
        rawLen = 0;          // empty PSK string => default/primary channel
    }

    if (!decodeOk) {
        Serial.printf("[MT] PSK base64 malformed (\"%s\"); using LongFast default\n", pskB64);
        rawLen = 0;
    }

    if (rawLen == 0) {
        memcpy(out.key, DEFAULT_PSK, 16);
        out.keyLen = 16;
    } else if (rawLen == 1) {
        uint8_t idx = raw[0];
        if (idx == 0) {
            Serial.printf("[MT] PSK index 0 (encryption off) unsupported; "
                          "using LongFast default\n");
            memcpy(out.key, DEFAULT_PSK, 16);
            out.keyLen = 16;
        } else {
            memcpy(out.key, DEFAULT_PSK, 16);
            out.key[15] = (uint8_t)(DEFAULT_PSK[15] + idx - 1);
            out.keyLen  = 16;
        }
    } else if (rawLen == 16 || rawLen == 32) {
        memcpy(out.key, raw, rawLen);
        out.keyLen = (uint8_t)rawLen;
    } else {
        Serial.printf("[MT] PSK length %u invalid (expected 0/1/16/32 B); "
                      "using LongFast default\n", (unsigned)rawLen);
        memcpy(out.key, DEFAULT_PSK, 16);
        out.keyLen = 16;
    }

    out.channelHash = (uint8_t)(xorFold((const uint8_t *)out.name, strlen(out.name))
                              ^ xorFold(out.key, out.keyLen));

    Serial.printf("[MT] channel=\"%s\" hash=0x%02X keyLen=%u key=",
                  out.name, out.channelHash, (unsigned)out.keyLen);
    for (size_t i = 0; i < out.keyLen; i++) Serial.printf("%02x", out.key[i]);
    Serial.println();
}

}  // namespace MeshtasticConfig
