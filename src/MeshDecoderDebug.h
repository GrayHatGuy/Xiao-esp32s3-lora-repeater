// MeshDecoderDebug.h
// ---------------------------------------------------------------------------
// Inline helper for the dual-radio bridge: after main.cpp dumps a captured
// packet's raw hex bytes, call MeshDecoderDebug::print() to also emit a
// decoded summary, picking the protocol parser by the LoRa sync word the
// capturing radio is configured for.
//
//   sync 0x12 -> MeshCore: if it's a GRP_TXT on the public channel
//                          (channel hash 0x11), AES-128-ECB decrypt and
//                          print timestamp / hops / sender / message.
//   sync 0x2B -> Meshtastic: prints the 16-byte header; full AES-CTR +
//                            protobuf parse is a TODO.
//
// Uses mbedTLS (already included in arduino-esp32 — no new lib_deps needed).
// Drop this file in src/ next to main.cpp and WioSX1262.h.
// ---------------------------------------------------------------------------

#pragma once

#include <Arduino.h>
#include <stdint.h>
#include <string.h>
#include <mbedtls/aes.h>
#include <mbedtls/md.h>

namespace MeshDecoderDebug {

// --- Sync-word constants (match the platformio.ini build flags) -------------
static constexpr uint8_t SYNC_WORD_MESHCORE   = 0x12;
static constexpr uint8_t SYNC_WORD_MESHTASTIC = 0x2B;

// --- MeshCore public channel ------------------------------------------------
// AES-128 key for the default MeshCore public channel.
// SHA-256(key)[0] == 0x11, which is the on-air channel hash.
static const uint8_t MESHCORE_PUBLIC_KEY[16] = {
    0x8b, 0x33, 0x87, 0xe9, 0xc5, 0xcd, 0xea, 0x6a,
    0xc9, 0xe5, 0xed, 0xba, 0xa1, 0x15, 0xcd, 0x72
};
static constexpr uint8_t MESHCORE_PUBLIC_CHANNEL_HASH = 0x11;


// --- MeshCore public-channel GRP_TXT decoder --------------------------------
// Returns true iff a public-channel GRP_TXT was decoded and printed.
inline bool printMeshCore(const uint8_t *buf, size_t len, const char *tag) {
    if (len < 6) return false;

    // Header: 0bVVPPPPRR
    uint8_t header      = buf[0];
    uint8_t version     = (header >> 6) & 0x03;
    uint8_t payloadType = (header >> 2) & 0x0F;
    uint8_t routeType   =  header       & 0x03;
    if (version != 0)        return false;
    if (payloadType != 0x05) return false;  // not GRP_TXT — skip silently

    size_t off = 1;
    if (routeType == 0x00 || routeType == 0x03) {
        if (len < off + 4) return false;
        off += 4;  // skip 4-byte transport codes
    }
    if (len < off + 1) return false;
    uint8_t pathLen = buf[off++];
    if (pathLen > 64 || len < off + pathLen) return false;
    off += pathLen;

    if (len < off + 3) return false;
    uint8_t channelHash = buf[off];
    const uint8_t *mac  = &buf[off + 1];
    const uint8_t *ct   = &buf[off + 3];
    size_t ctLen        = len - (off + 3);

    if (channelHash != MESHCORE_PUBLIC_CHANNEL_HASH) return false;
    if (ctLen == 0 || (ctLen % 16) != 0)             return false;
    if (ctLen > 224)                                 return false;  // sanity

    // Verify HMAC-SHA256(key, ciphertext)[:2]
    uint8_t hmacOut[32];
    mbedtls_md_context_t mdCtx;
    mbedtls_md_init(&mdCtx);
    const mbedtls_md_info_t *info =
        mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    mbedtls_md_setup(&mdCtx, info, /*hmac=*/1);
    mbedtls_md_hmac_starts(&mdCtx, MESHCORE_PUBLIC_KEY,
                           sizeof(MESHCORE_PUBLIC_KEY));
    mbedtls_md_hmac_update(&mdCtx, ct, ctLen);
    mbedtls_md_hmac_finish(&mdCtx, hmacOut);
    mbedtls_md_free(&mdCtx);
    bool macValid = (hmacOut[0] == mac[0] && hmacOut[1] == mac[1]);

    // AES-128-ECB decrypt
    uint8_t pt[224];
    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);
    mbedtls_aes_setkey_dec(&aes, MESHCORE_PUBLIC_KEY, 128);
    for (size_t i = 0; i < ctLen; i += 16) {
        mbedtls_aes_crypt_ecb(&aes, MBEDTLS_AES_DECRYPT, ct + i, pt + i);
    }
    mbedtls_aes_free(&aes);

    // Plaintext: [timestamp:4 LE][flags:1][body...\0][zero-pad]
    if (ctLen < 5) return false;
    uint32_t ts = (uint32_t)pt[0]
                | ((uint32_t)pt[1] << 8)
                | ((uint32_t)pt[2] << 16)
                | ((uint32_t)pt[3] << 24);
    uint8_t flags   = pt[4];
    uint8_t attempt = flags & 0x03;
    uint8_t subtype = flags >> 2;

    // Bound the null search to plaintext we actually have
    size_t bodyMax = ctLen - 5;
    size_t bodyEnd = 0;
    while (bodyEnd < bodyMax && pt[5 + bodyEnd] != 0) bodyEnd++;

    Serial.printf("[%8lu ms][%s decoded] MeshCore GRP_TXT "
                  "ch=0x%02X hops=%u mac=%02X%02X(%s) "
                  "ts=%lu flags=0x%02X(att=%u,sub=%u)\n",
                  millis(), tag, channelHash, (unsigned)pathLen,
                  mac[0], mac[1], macValid ? "ok" : "BAD",
                  (unsigned long)ts, flags,
                  (unsigned)attempt, (unsigned)subtype);
    Serial.printf("[%8lu ms][%s decoded] body: \"", millis(), tag);
    for (size_t i = 0; i < bodyEnd; i++) {
        uint8_t c = pt[5 + i];
        Serial.write((c >= 0x20 && c < 0x7F) ? c : '?');
    }
    Serial.println("\"");
    return true;
}


// --- Meshtastic decoder (stub: header parse only) ---------------------------
// AES-CTR decryption + protobuf parsing are TODO. For now, emit the
// 16-byte transport header so you can see who's talking on the mesh.
inline bool printMeshtastic(const uint8_t *buf, size_t len, const char *tag) {
    if (len < 16) return false;
    uint32_t dest = (uint32_t)buf[0]  | ((uint32_t)buf[1]  << 8)
                  | ((uint32_t)buf[2] << 16) | ((uint32_t)buf[3] << 24);
    uint32_t src  = (uint32_t)buf[4]  | ((uint32_t)buf[5]  << 8)
                  | ((uint32_t)buf[6] << 16) | ((uint32_t)buf[7] << 24);
    uint32_t pid  = (uint32_t)buf[8]  | ((uint32_t)buf[9]  << 8)
                  | ((uint32_t)buf[10] << 16) | ((uint32_t)buf[11] << 24);
    uint8_t flags        = buf[12];
    uint8_t channelHash  = buf[13];
    Serial.printf("[%8lu ms][%s decoded] Meshtastic "
                  "src=0x%08lX dest=0x%08lX id=0x%08lX "
                  "flags=0x%02X ch=0x%02X payload=%u B "
                  "(crypto/protobuf TODO)\n",
                  millis(), tag,
                  (unsigned long)src, (unsigned long)dest,
                  (unsigned long)pid, flags, channelHash,
                  (unsigned)(len - 16));
    return true;
}


// --- Public entry point: dispatch by sync word ------------------------------
inline void print(const uint8_t *buf, size_t len, uint8_t syncWord, const char *tag) {
    if (syncWord == SYNC_WORD_MESHCORE) {
        printMeshCore(buf, len, tag);
    } else if (syncWord == SYNC_WORD_MESHTASTIC) {
        printMeshtastic(buf, len, tag);
    }
    // Other sync words: silently ignore — we don't know that protocol.
}

}  // namespace MeshDecoderDebug
