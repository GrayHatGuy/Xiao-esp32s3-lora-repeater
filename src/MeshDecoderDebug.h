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
#include <mbedtls/base64.h>

#include "MeshCoreConfig.h"     // key + channelHash + channelName

namespace MeshDecoderDebug {

// --- Sync-word constants (match the platformio.ini build flags) -------------
static constexpr uint8_t SYNC_WORD_MESHCORE   = 0x12;
static constexpr uint8_t SYNC_WORD_MESHTASTIC = 0x2B;
static constexpr uint8_t SYNC_WORD_RETICULUM  = 0x42;   // RNode / Reticulum Network Suite

// MeshCore channel state (key + channel hash + display name) now lives
// in MeshCoreConfig and is initialised in setup() so private channels can
// be selected via build flags or, later, the WiFi captive-portal config.
// Decoders below read MeshCoreConfig::key / MeshCoreConfig::channelHash
// directly.

// --- Meshtastic default channel ---------------------------------------------
// AES-128 key for the Meshtastic default LongFast channel ("AQ==").
// Derivation (firmware: CryptoEngine::setKey + defaultpsk):
//   defaultpsk = d4 f1 bb 3a 20 29 07 59 f0 bc ff ab cf 4e 69 01
//   short PSK 0x01 from "AQ==" replaces the last byte, which is already 0x01,
//   so the expanded key equals defaultpsk verbatim.
//   Equivalently the base64 string "1PG7OiApB1nwvP+rz05pAQ==".
static const uint8_t MESHTASTIC_DEFAULT_KEY[16] = {
    0xd4, 0xf1, 0xbb, 0x3a, 0x20, 0x29, 0x07, 0x59,
    0xf0, 0xbc, 0xff, 0xab, 0xcf, 0x4e, 0x69, 0x01
};
// XOR of the channel name "LongFast" with each byte of MESHTASTIC_DEFAULT_KEY.
// Used as a coarse gate before attempting decryption with the default key.
static constexpr uint8_t MESHTASTIC_LONGFAST_CHANNEL_HASH = 0x08;

// Meshtastic PortNum values we name in output; everything else prints as "?".
static const char *meshtasticPortName(uint32_t n) {
    switch (n) {
        case 1:  return "TEXT_MESSAGE_APP";
        case 3:  return "POSITION_APP";
        case 4:  return "NODEINFO_APP";
        case 5:  return "ROUTING_APP";
        case 6:  return "ADMIN_APP";
        case 67: return "TELEMETRY_APP";
        case 70: return "AUDIO_APP";
        case 71: return "TRACEROUTE_APP";
        default: return "?";
    }
}

// --- Minimal protobuf varint walker (only what Meshtastic Data needs) -------
static inline bool pbVarint(const uint8_t *b, size_t n, size_t &p, uint64_t &v) {
    v = 0;
    int shift = 0;
    while (p < n) {
        uint8_t byte = b[p++];
        v |= (uint64_t)(byte & 0x7F) << shift;
        if (!(byte & 0x80)) return true;
        shift += 7;
        if (shift >= 64) return false;   // varint too long, malformed
    }
    return false;
}

static inline bool pbSkip(const uint8_t *b, size_t n, size_t &p, int wt) {
    uint64_t v;
    switch (wt) {
        case 0:  return pbVarint(b, n, p, v);                       // varint
        case 1:  if (p + 8 > n) return false; p += 8; return true;  // 64-bit
        case 2:                                                     // length-delim
            if (!pbVarint(b, n, p, v) || p + v > n) return false;
            p += (size_t)v; return true;
        case 5:  if (p + 4 > n) return false; p += 4; return true;  // 32-bit
        default: return false;                                       // unknown
    }
}

// Read a wire-type-5 (fixed32) payload. sfixed32 and fixed32 both arrive
// here as 4 little-endian bytes; the caller reinterprets the resulting
// uint32_t as int32_t / float as appropriate.
static inline bool pbReadFixed32(const uint8_t *b, size_t n, size_t &p, uint32_t &v) {
    if (p + 4 > n) return false;
    v = (uint32_t)b[p]
      | ((uint32_t)b[p + 1] << 8)
      | ((uint32_t)b[p + 2] << 16)
      | ((uint32_t)b[p + 3] << 24);
    p += 4;
    return true;
}

// Reinterpret a wire-type-5 word as IEEE 754 float without aliasing UB.
static inline float pbFixed32AsFloat(uint32_t v) {
    float f;
    memcpy(&f, &v, sizeof(f));
    return f;
}


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

    if (channelHash != MeshCoreConfig::channelHash) return false;
    if (ctLen == 0 || (ctLen % 16) != 0)             return false;
    if (ctLen > 224)                                 return false;  // sanity

    // Verify HMAC-SHA256(key, ciphertext)[:2]
    uint8_t hmacOut[32];
    mbedtls_md_context_t mdCtx;
    mbedtls_md_init(&mdCtx);
    const mbedtls_md_info_t *info =
        mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    mbedtls_md_setup(&mdCtx, info, /*hmac=*/1);
    mbedtls_md_hmac_starts(&mdCtx, MeshCoreConfig::key,
                           sizeof(MeshCoreConfig::key));
    mbedtls_md_hmac_update(&mdCtx, ct, ctLen);
    mbedtls_md_hmac_finish(&mdCtx, hmacOut);
    mbedtls_md_free(&mdCtx);
    bool macValid = (hmacOut[0] == mac[0] && hmacOut[1] == mac[1]);

    // AES-128-ECB decrypt
    uint8_t pt[224];
    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);
    mbedtls_aes_setkey_dec(&aes, MeshCoreConfig::key, 128);
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


// --- Meshtastic decoder -----------------------------------------------------
// Header (16 B):
//   [dest:4 LE][src:4 LE][packet_id:4 LE][flags:1][channel_hash:1]
//   [next_hop:1][relay_node:1]
// Body: AES-128-CTR ciphertext, encrypting a protobuf-encoded Data message.
// Nonce: [packet_id:4 LE][00 00 00 00][src:4 LE][00 00 00 00].
// We try the default LongFast channel key. If the channel_hash doesn't match
// the LongFast hash 0x08 the plaintext will be garbage; the protobuf walker
// will simply fail and we'll print "(no decodable protobuf)".
inline bool printMeshtastic(const uint8_t *buf, size_t len, const char *tag) {
    if (len < 17) return false;   // 16-byte header + at least 1 byte ciphertext

    uint32_t dest = (uint32_t)buf[0]  | ((uint32_t)buf[1]  << 8)
                  | ((uint32_t)buf[2] << 16) | ((uint32_t)buf[3] << 24);
    uint32_t src  = (uint32_t)buf[4]  | ((uint32_t)buf[5]  << 8)
                  | ((uint32_t)buf[6] << 16) | ((uint32_t)buf[7] << 24);
    uint32_t pid  = (uint32_t)buf[8]  | ((uint32_t)buf[9]  << 8)
                  | ((uint32_t)buf[10] << 16) | ((uint32_t)buf[11] << 24);
    uint8_t flags       = buf[12];
    uint8_t channelHash = buf[13];
    uint8_t nextHop     = buf[14];
    uint8_t relayNode   = buf[15];

    const uint8_t *ct = buf + 16;
    size_t ctLen      = len - 16;

    Serial.printf("[%8lu ms][%s decoded] Meshtastic "
                  "src=0x%08lX dest=0x%08lX id=0x%08lX "
                  "flags=0x%02X ch=0x%02X next=0x%02X relay=0x%02X ct=%u B\n",
                  millis(), tag,
                  (unsigned long)src, (unsigned long)dest, (unsigned long)pid,
                  flags, channelHash, nextHop, relayNode, (unsigned)ctLen);

    if (ctLen > 240) {
        Serial.printf("[%8lu ms][%s decoded] ciphertext too long, skip\n",
                      millis(), tag);
        return true;
    }

    // Build AES-CTR initial counter block.
    uint8_t nonce[16] = {
        (uint8_t)(pid >>  0), (uint8_t)(pid >>  8),
        (uint8_t)(pid >> 16), (uint8_t)(pid >> 24),
        0, 0, 0, 0,
        (uint8_t)(src >>  0), (uint8_t)(src >>  8),
        (uint8_t)(src >> 16), (uint8_t)(src >> 24),
        0, 0, 0, 0
    };

    // AES-128-CTR: setkey_enc is correct here — CTR encrypts the counter,
    // XORs against data, so decrypt and encrypt use the same direction.
    uint8_t pt[240];
    {
        mbedtls_aes_context aes;
        mbedtls_aes_init(&aes);
        mbedtls_aes_setkey_enc(&aes, MESHTASTIC_DEFAULT_KEY, 128);
        uint8_t stream[16] = {};
        size_t  nc_off = 0;
        mbedtls_aes_crypt_ctr(&aes, ctLen, &nc_off, nonce, stream, ct, pt);
        mbedtls_aes_free(&aes);
    }

    // Walk the protobuf Data message — just field 1 (portnum, varint) and
    // field 2 (payload, length-delimited bytes). Skip unknown fields.
    size_t   pos          = 0;
    bool     gotPortnum   = false;
    uint32_t portnum      = 0;
    const uint8_t *payload = nullptr;
    size_t   payloadLen   = 0;
    bool     parseOk      = true;

    while (pos < ctLen) {
        uint64_t tagv;
        if (!pbVarint(pt, ctLen, pos, tagv)) { parseOk = false; break; }
        int fieldNum = (int)(tagv >> 3);
        int wireType = (int)(tagv & 0x07);

        if (fieldNum == 1 && wireType == 0) {
            uint64_t v;
            if (!pbVarint(pt, ctLen, pos, v)) { parseOk = false; break; }
            portnum    = (uint32_t)v;
            gotPortnum = true;
        } else if (fieldNum == 2 && wireType == 2) {
            uint64_t plen;
            if (!pbVarint(pt, ctLen, pos, plen) || pos + plen > ctLen) {
                parseOk = false; break;
            }
            payload    = pt + pos;
            payloadLen = (size_t)plen;
            pos       += payloadLen;
        } else {
            if (!pbSkip(pt, ctLen, pos, wireType)) { parseOk = false; break; }
        }
    }

    if (!parseOk && !gotPortnum) {
        Serial.printf("[%8lu ms][%s decoded] (no decodable protobuf — "
                      "wrong key/channel?)\n", millis(), tag);
        return true;
    }

    Serial.printf("[%8lu ms][%s decoded] portnum=%lu(%s) payload=%u B\n",
                  millis(), tag,
                  (unsigned long)portnum, meshtasticPortName(portnum),
                  (unsigned)payloadLen);

    if (portnum == 1 /*TEXT_MESSAGE_APP*/ && payload && payloadLen > 0) {
        Serial.printf("[%8lu ms][%s decoded] text: \"", millis(), tag);
        for (size_t i = 0; i < payloadLen; i++) {
            uint8_t c = payload[i];
            Serial.write((c >= 0x20 && c < 0x7F) ? c : '?');
        }
        Serial.println("\"");
    }
    return true;
}


// --- Body-only extractors (no Serial output) --------------------------------
// Used by the cross-protocol bridge to re-encode text into the other protocol.
// Each fills bodyOut with a null-terminated UTF-8 string (truncated if needed)
// and returns true iff a text body was successfully extracted. Non-text
// portnums, wrong-channel packets, and decrypt failures return false.

inline bool extractMeshCoreBody(const uint8_t *buf, size_t len,
                                char *bodyOut, size_t bodyOutCap) {
    if (!bodyOut || bodyOutCap < 2) return false;
    bodyOut[0] = 0;
    if (len < 6) return false;

    uint8_t header      = buf[0];
    uint8_t version     = (header >> 6) & 0x03;
    uint8_t payloadType = (header >> 2) & 0x0F;
    uint8_t routeType   =  header       & 0x03;
    if (version != 0)        return false;
    if (payloadType != 0x05) return false;  // not GRP_TXT

    size_t off = 1;
    if (routeType == 0x00 || routeType == 0x03) {
        if (len < off + 4) return false;
        off += 4;
    }
    if (len < off + 1) return false;
    uint8_t pathLen = buf[off++];
    if (pathLen > 64 || len < off + pathLen) return false;
    off += pathLen;

    if (len < off + 3) return false;
    uint8_t channelHash = buf[off];
    const uint8_t *ct   = &buf[off + 3];
    size_t ctLen        = len - (off + 3);

    if (channelHash != MeshCoreConfig::channelHash) return false;
    if (ctLen == 0 || (ctLen % 16) != 0 || ctLen > 224) return false;

    uint8_t pt[224];
    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);
    mbedtls_aes_setkey_dec(&aes, MeshCoreConfig::key, 128);
    for (size_t i = 0; i < ctLen; i += 16) {
        mbedtls_aes_crypt_ecb(&aes, MBEDTLS_AES_DECRYPT, ct + i, pt + i);
    }
    mbedtls_aes_free(&aes);

    if (ctLen < 6) return false;
    size_t bodyMax = ctLen - 5;
    size_t bodyLen = 0;
    while (bodyLen < bodyMax && pt[5 + bodyLen] != 0) bodyLen++;
    if (bodyLen == 0) return false;

    size_t copyLen = (bodyLen < bodyOutCap - 1) ? bodyLen : bodyOutCap - 1;
    memcpy(bodyOut, pt + 5, copyLen);
    bodyOut[copyLen] = 0;
    return true;
}

inline bool extractMeshtasticBody(const uint8_t *buf, size_t len,
                                   char *bodyOut, size_t bodyOutCap) {
    if (!bodyOut || bodyOutCap < 2) return false;
    bodyOut[0] = 0;
    if (len < 17) return false;

    uint8_t channelHash = buf[13];
    if (channelHash != MESHTASTIC_LONGFAST_CHANNEL_HASH) return false;

    uint32_t src = (uint32_t)buf[4]  | ((uint32_t)buf[5]  << 8)
                 | ((uint32_t)buf[6] << 16) | ((uint32_t)buf[7] << 24);
    uint32_t pid = (uint32_t)buf[8]  | ((uint32_t)buf[9]  << 8)
                 | ((uint32_t)buf[10] << 16) | ((uint32_t)buf[11] << 24);

    const uint8_t *ct = buf + 16;
    size_t ctLen      = len - 16;
    if (ctLen > 240) return false;

    uint8_t nonce[16] = {
        (uint8_t)(pid >>  0), (uint8_t)(pid >>  8),
        (uint8_t)(pid >> 16), (uint8_t)(pid >> 24),
        0, 0, 0, 0,
        (uint8_t)(src >>  0), (uint8_t)(src >>  8),
        (uint8_t)(src >> 16), (uint8_t)(src >> 24),
        0, 0, 0, 0
    };

    uint8_t pt[240];
    {
        mbedtls_aes_context aes;
        mbedtls_aes_init(&aes);
        mbedtls_aes_setkey_enc(&aes, MESHTASTIC_DEFAULT_KEY, 128);
        uint8_t stream[16] = {};
        size_t  nc_off = 0;
        mbedtls_aes_crypt_ctr(&aes, ctLen, &nc_off, nonce, stream, ct, pt);
        mbedtls_aes_free(&aes);
    }

    size_t   pos        = 0;
    uint32_t portnum    = 0;
    const uint8_t *payload = nullptr;
    size_t   payloadLen = 0;

    while (pos < ctLen) {
        uint64_t tagv;
        if (!pbVarint(pt, ctLen, pos, tagv)) break;
        int fieldNum = (int)(tagv >> 3);
        int wireType = (int)(tagv & 0x07);

        if (fieldNum == 1 && wireType == 0) {
            uint64_t v;
            if (!pbVarint(pt, ctLen, pos, v)) break;
            portnum = (uint32_t)v;
        } else if (fieldNum == 2 && wireType == 2) {
            uint64_t plen;
            if (!pbVarint(pt, ctLen, pos, plen) || pos + plen > ctLen) break;
            payload    = pt + pos;
            payloadLen = (size_t)plen;
            pos       += payloadLen;
        } else {
            if (!pbSkip(pt, ctLen, pos, wireType)) break;
        }
    }

    if (portnum != 1 /*TEXT_MESSAGE_APP*/ || !payload || payloadLen == 0)
        return false;

    size_t copyLen = (payloadLen < bodyOutCap - 1) ? payloadLen : bodyOutCap - 1;
    memcpy(bodyOut, payload, copyLen);
    bodyOut[copyLen] = 0;
    return true;
}


// --- Meshtastic NODEINFO_APP (portnum 4) decoder ----------------------------
// Pulls the User submessage out of a Meshtastic NodeInfo broadcast. On
// success, fills outNodeId with the packet's src field (the canonical
// 32-bit ID), and writes the User long_name (field 2) and short_name
// (field 3) into the caller's buffers as null-terminated strings. Returns
// false for any of: not a LongFast packet, decrypt boundary error, portnum
// != 4, missing payload, malformed protobuf.
//
// Crypto mirrors extractMeshtasticBody(); the duplication is intentional —
// it keeps each decoder readable as a single linear function rather than
// chasing a shared helper.
inline bool extractMeshtasticNodeInfo(const uint8_t *buf, size_t len,
                                       uint32_t &outNodeId,
                                       char *shortNameOut, size_t shortCap,
                                       char *longNameOut,  size_t longCap) {
    outNodeId = 0;
    if (shortNameOut && shortCap) shortNameOut[0] = 0;
    if (longNameOut  && longCap)  longNameOut[0]  = 0;
    if (len < 17) return false;

    uint8_t channelHash = buf[13];
    if (channelHash != MESHTASTIC_LONGFAST_CHANNEL_HASH) return false;

    uint32_t src = (uint32_t)buf[4]  | ((uint32_t)buf[5]  << 8)
                 | ((uint32_t)buf[6] << 16) | ((uint32_t)buf[7] << 24);
    uint32_t pid = (uint32_t)buf[8]  | ((uint32_t)buf[9]  << 8)
                 | ((uint32_t)buf[10] << 16) | ((uint32_t)buf[11] << 24);

    const uint8_t *ct = buf + 16;
    size_t ctLen      = len - 16;
    if (ctLen > 240) return false;

    uint8_t nonce[16] = {
        (uint8_t)(pid >>  0), (uint8_t)(pid >>  8),
        (uint8_t)(pid >> 16), (uint8_t)(pid >> 24),
        0, 0, 0, 0,
        (uint8_t)(src >>  0), (uint8_t)(src >>  8),
        (uint8_t)(src >> 16), (uint8_t)(src >> 24),
        0, 0, 0, 0
    };

    uint8_t pt[240];
    {
        mbedtls_aes_context aes;
        mbedtls_aes_init(&aes);
        mbedtls_aes_setkey_enc(&aes, MESHTASTIC_DEFAULT_KEY, 128);
        uint8_t stream[16] = {};
        size_t  nc_off = 0;
        mbedtls_aes_crypt_ctr(&aes, ctLen, &nc_off, nonce, stream, ct, pt);
        mbedtls_aes_free(&aes);
    }

    // Walk Data protobuf, find portnum and payload.
    size_t   pos        = 0;
    uint32_t portnum    = 0;
    const uint8_t *payload = nullptr;
    size_t   payloadLen = 0;
    while (pos < ctLen) {
        uint64_t tagv;
        if (!pbVarint(pt, ctLen, pos, tagv)) break;
        int fieldNum = (int)(tagv >> 3);
        int wireType = (int)(tagv & 0x07);
        if (fieldNum == 1 && wireType == 0) {
            uint64_t v;
            if (!pbVarint(pt, ctLen, pos, v)) break;
            portnum = (uint32_t)v;
        } else if (fieldNum == 2 && wireType == 2) {
            uint64_t plen;
            if (!pbVarint(pt, ctLen, pos, plen) || pos + plen > ctLen) break;
            payload    = pt + pos;
            payloadLen = (size_t)plen;
            pos       += payloadLen;
        } else {
            if (!pbSkip(pt, ctLen, pos, wireType)) break;
        }
    }
    if (portnum != 4 /*NODEINFO_APP*/ || !payload || payloadLen == 0)
        return false;

    // Walk User submessage. We care about long_name (field 2) and
    // short_name (field 3); field 1 is the "!hexid" string which we ignore
    // in favour of the header src field.
    size_t up = 0;
    while (up < payloadLen) {
        uint64_t tagv;
        if (!pbVarint(payload, payloadLen, up, tagv)) return false;
        int field = (int)(tagv >> 3);
        int wt    = (int)(tagv & 0x07);
        if (wt == 2) {
            uint64_t slen;
            if (!pbVarint(payload, payloadLen, up, slen) ||
                up + slen > payloadLen) return false;
            if (field == 2 && longNameOut && longCap > 1) {
                size_t copy = (slen < longCap - 1) ? (size_t)slen : longCap - 1;
                memcpy(longNameOut, payload + up, copy);
                longNameOut[copy] = 0;
            } else if (field == 3 && shortNameOut && shortCap > 1) {
                size_t copy = (slen < shortCap - 1) ? (size_t)slen : shortCap - 1;
                memcpy(shortNameOut, payload + up, copy);
                shortNameOut[copy] = 0;
            }
            up += (size_t)slen;
        } else {
            if (!pbSkip(payload, payloadLen, up, wt)) return false;
        }
    }

    outNodeId = src;
    return true;
}


// --- Meshtastic POSITION_APP (portnum 3) decoder ----------------------------
// Pulls latitude_i (field 1, sfixed32), longitude_i (field 2, sfixed32) and
// altitude (field 3, int32 varint) out of the Position submessage. The
// "_i" coordinates are stored as integer degrees * 1e7; callers divide by
// 1e7 when formatting. Returns false unless portnum == 3 AND at least one
// of lat/lon/alt was present.
struct MeshtasticPositionInfo {
    bool    hasLat;
    bool    hasLon;
    bool    hasAlt;
    int32_t latI;       // latitude  * 1e7   (e.g. 407234567 -> 40.7234567 N)
    int32_t lonI;       // longitude * 1e7
    int32_t altM;       // altitude in metres
};

inline bool extractMeshtasticPosition(const uint8_t *buf, size_t len,
                                       MeshtasticPositionInfo &out) {
    out = {};
    if (len < 17) return false;
    if (buf[13] != MESHTASTIC_LONGFAST_CHANNEL_HASH) return false;

    uint32_t src = (uint32_t)buf[4]  | ((uint32_t)buf[5]  << 8)
                 | ((uint32_t)buf[6] << 16) | ((uint32_t)buf[7] << 24);
    uint32_t pid = (uint32_t)buf[8]  | ((uint32_t)buf[9]  << 8)
                 | ((uint32_t)buf[10] << 16) | ((uint32_t)buf[11] << 24);

    const uint8_t *ct = buf + 16;
    size_t ctLen      = len - 16;
    if (ctLen > 240) return false;

    uint8_t nonce[16] = {
        (uint8_t)(pid >>  0), (uint8_t)(pid >>  8),
        (uint8_t)(pid >> 16), (uint8_t)(pid >> 24),
        0, 0, 0, 0,
        (uint8_t)(src >>  0), (uint8_t)(src >>  8),
        (uint8_t)(src >> 16), (uint8_t)(src >> 24),
        0, 0, 0, 0
    };

    uint8_t pt[240];
    {
        mbedtls_aes_context aes;
        mbedtls_aes_init(&aes);
        mbedtls_aes_setkey_enc(&aes, MESHTASTIC_DEFAULT_KEY, 128);
        uint8_t stream[16] = {};
        size_t  nc_off = 0;
        mbedtls_aes_crypt_ctr(&aes, ctLen, &nc_off, nonce, stream, ct, pt);
        mbedtls_aes_free(&aes);
    }

    // Walk Data protobuf
    size_t   pos        = 0;
    uint32_t portnum    = 0;
    const uint8_t *payload = nullptr;
    size_t   payloadLen = 0;
    while (pos < ctLen) {
        uint64_t tagv;
        if (!pbVarint(pt, ctLen, pos, tagv)) break;
        int fieldNum = (int)(tagv >> 3);
        int wireType = (int)(tagv & 0x07);
        if (fieldNum == 1 && wireType == 0) {
            uint64_t v;
            if (!pbVarint(pt, ctLen, pos, v)) break;
            portnum = (uint32_t)v;
        } else if (fieldNum == 2 && wireType == 2) {
            uint64_t plen;
            if (!pbVarint(pt, ctLen, pos, plen) || pos + plen > ctLen) break;
            payload    = pt + pos;
            payloadLen = (size_t)plen;
            pos       += payloadLen;
        } else {
            if (!pbSkip(pt, ctLen, pos, wireType)) break;
        }
    }
    if (portnum != 3 /*POSITION_APP*/ || !payload || payloadLen == 0)
        return false;

    // Walk Position submessage
    size_t p = 0;
    while (p < payloadLen) {
        uint64_t tagv;
        if (!pbVarint(payload, payloadLen, p, tagv)) return false;
        int field = (int)(tagv >> 3);
        int wt    = (int)(tagv & 0x07);
        if (field == 1 && wt == 5) {            // latitude_i, sfixed32
            uint32_t v; if (!pbReadFixed32(payload, payloadLen, p, v)) return false;
            out.latI = (int32_t)v; out.hasLat = true;
        } else if (field == 2 && wt == 5) {     // longitude_i, sfixed32
            uint32_t v; if (!pbReadFixed32(payload, payloadLen, p, v)) return false;
            out.lonI = (int32_t)v; out.hasLon = true;
        } else if (field == 3 && wt == 0) {     // altitude, int32 varint
            uint64_t v; if (!pbVarint(payload, payloadLen, p, v)) return false;
            out.altM = (int32_t)v; out.hasAlt = true;
        } else {
            if (!pbSkip(payload, payloadLen, p, wt)) return false;
        }
    }

    return out.hasLat || out.hasLon || out.hasAlt;
}


// --- Meshtastic TELEMETRY_APP (portnum 67) decoder --------------------------
// Telemetry wraps a oneof variant — we surface the two most useful ones:
//   field 2 (DeviceMetrics):       battery_level (f1, float), voltage (f2, float)
//   field 3 (EnvironmentMetrics):  temperature (f1, float), relative_humidity
//                                  (f2, float), barometric_pressure (f3, float)
// Other variants (air_quality, power, local_stats, health) set kind=OTHER
// and leave the floats untouched. Returns false unless portnum == 67 AND a
// recognised variant carried at least one field we know about.
struct MeshtasticTelemetryInfo {
    enum class Kind { NONE, DEVICE, ENVIRONMENT, OTHER } kind;
    // DeviceMetrics
    bool  hasBatteryPct;  float batteryPct;
    bool  hasVoltage;     float voltage;
    // EnvironmentMetrics
    bool  hasTempC;       float tempC;
    bool  hasRhPct;       float rhPct;
    bool  hasPressureHpa; float pressureHpa;
};

inline bool extractMeshtasticTelemetry(const uint8_t *buf, size_t len,
                                        MeshtasticTelemetryInfo &out) {
    out = {};
    out.kind = MeshtasticTelemetryInfo::Kind::NONE;
    if (len < 17) return false;
    if (buf[13] != MESHTASTIC_LONGFAST_CHANNEL_HASH) return false;

    uint32_t src = (uint32_t)buf[4]  | ((uint32_t)buf[5]  << 8)
                 | ((uint32_t)buf[6] << 16) | ((uint32_t)buf[7] << 24);
    uint32_t pid = (uint32_t)buf[8]  | ((uint32_t)buf[9]  << 8)
                 | ((uint32_t)buf[10] << 16) | ((uint32_t)buf[11] << 24);

    const uint8_t *ct = buf + 16;
    size_t ctLen      = len - 16;
    if (ctLen > 240) return false;

    uint8_t nonce[16] = {
        (uint8_t)(pid >>  0), (uint8_t)(pid >>  8),
        (uint8_t)(pid >> 16), (uint8_t)(pid >> 24),
        0, 0, 0, 0,
        (uint8_t)(src >>  0), (uint8_t)(src >>  8),
        (uint8_t)(src >> 16), (uint8_t)(src >> 24),
        0, 0, 0, 0
    };

    uint8_t pt[240];
    {
        mbedtls_aes_context aes;
        mbedtls_aes_init(&aes);
        mbedtls_aes_setkey_enc(&aes, MESHTASTIC_DEFAULT_KEY, 128);
        uint8_t stream[16] = {};
        size_t  nc_off = 0;
        mbedtls_aes_crypt_ctr(&aes, ctLen, &nc_off, nonce, stream, ct, pt);
        mbedtls_aes_free(&aes);
    }

    // Walk Data protobuf
    size_t   pos        = 0;
    uint32_t portnum    = 0;
    const uint8_t *payload = nullptr;
    size_t   payloadLen = 0;
    while (pos < ctLen) {
        uint64_t tagv;
        if (!pbVarint(pt, ctLen, pos, tagv)) break;
        int fieldNum = (int)(tagv >> 3);
        int wireType = (int)(tagv & 0x07);
        if (fieldNum == 1 && wireType == 0) {
            uint64_t v;
            if (!pbVarint(pt, ctLen, pos, v)) break;
            portnum = (uint32_t)v;
        } else if (fieldNum == 2 && wireType == 2) {
            uint64_t plen;
            if (!pbVarint(pt, ctLen, pos, plen) || pos + plen > ctLen) break;
            payload    = pt + pos;
            payloadLen = (size_t)plen;
            pos       += payloadLen;
        } else {
            if (!pbSkip(pt, ctLen, pos, wireType)) break;
        }
    }
    if (portnum != 67 /*TELEMETRY_APP*/ || !payload || payloadLen == 0)
        return false;

    // Walk Telemetry submessage. We expect either field 2 (DeviceMetrics)
    // or field 3 (EnvironmentMetrics) as a length-delimited submessage;
    // field 1 is the time and we ignore it. Anything else maps to OTHER.
    size_t p = 0;
    while (p < payloadLen) {
        uint64_t tagv;
        if (!pbVarint(payload, payloadLen, p, tagv)) return false;
        int field = (int)(tagv >> 3);
        int wt    = (int)(tagv & 0x07);

        if (field == 2 && wt == 2) {
            // DeviceMetrics submessage
            uint64_t slen;
            if (!pbVarint(payload, payloadLen, p, slen) ||
                p + slen > payloadLen) return false;
            const uint8_t *sb = payload + p;
            size_t sn = (size_t)slen;
            p += sn;
            out.kind = MeshtasticTelemetryInfo::Kind::DEVICE;
            size_t sp = 0;
            while (sp < sn) {
                uint64_t st;
                if (!pbVarint(sb, sn, sp, st)) break;
                int sf = (int)(st >> 3);
                int sw = (int)(st & 0x07);
                if (sf == 1 && sw == 5) {
                    uint32_t v; if (!pbReadFixed32(sb, sn, sp, v)) break;
                    out.batteryPct = pbFixed32AsFloat(v); out.hasBatteryPct = true;
                } else if (sf == 2 && sw == 5) {
                    uint32_t v; if (!pbReadFixed32(sb, sn, sp, v)) break;
                    out.voltage = pbFixed32AsFloat(v); out.hasVoltage = true;
                } else {
                    if (!pbSkip(sb, sn, sp, sw)) break;
                }
            }
        } else if (field == 3 && wt == 2) {
            // EnvironmentMetrics submessage
            uint64_t slen;
            if (!pbVarint(payload, payloadLen, p, slen) ||
                p + slen > payloadLen) return false;
            const uint8_t *sb = payload + p;
            size_t sn = (size_t)slen;
            p += sn;
            out.kind = MeshtasticTelemetryInfo::Kind::ENVIRONMENT;
            size_t sp = 0;
            while (sp < sn) {
                uint64_t st;
                if (!pbVarint(sb, sn, sp, st)) break;
                int sf = (int)(st >> 3);
                int sw = (int)(st & 0x07);
                if (sf == 1 && sw == 5) {
                    uint32_t v; if (!pbReadFixed32(sb, sn, sp, v)) break;
                    out.tempC = pbFixed32AsFloat(v); out.hasTempC = true;
                } else if (sf == 2 && sw == 5) {
                    uint32_t v; if (!pbReadFixed32(sb, sn, sp, v)) break;
                    out.rhPct = pbFixed32AsFloat(v); out.hasRhPct = true;
                } else if (sf == 3 && sw == 5) {
                    uint32_t v; if (!pbReadFixed32(sb, sn, sp, v)) break;
                    out.pressureHpa = pbFixed32AsFloat(v); out.hasPressureHpa = true;
                } else {
                    if (!pbSkip(sb, sn, sp, sw)) break;
                }
            }
        } else if (field >= 4 && field <= 7 && wt == 2) {
            // air_quality / power / local_stats / health — skip but mark
            uint64_t slen;
            if (!pbVarint(payload, payloadLen, p, slen) ||
                p + slen > payloadLen) return false;
            p += (size_t)slen;
            if (out.kind == MeshtasticTelemetryInfo::Kind::NONE)
                out.kind = MeshtasticTelemetryInfo::Kind::OTHER;
        } else {
            if (!pbSkip(payload, payloadLen, p, wt)) return false;
        }
    }

    return out.kind == MeshtasticTelemetryInfo::Kind::DEVICE
        || out.kind == MeshtasticTelemetryInfo::Kind::ENVIRONMENT;
}


// --- Reticulum / RNode raw decoder (stub) -----------------------------------
// Reticulum's on-air framing (RNS packet header, addresses, hops, IFAC) is
// not yet parsed. As an interim measure for cross-protocol bridging, the
// "decoder" simply base64-encodes the raw LoRa payload so the bytes can ride
// inside a Meshtastic or MeshCore text packet. base64 (≈1.33x expansion) is
// preferred over hex (2x expansion) so a single RNS announce typically fits
// in one MT/MC packet without fragmentation; the bridge in main.cpp adds
// fragmentation for the rare cases that don't fit.

// CRC-16/CCITT (poly 0x1021, init 0xFFFF). The low byte is used as a per-
// source-frame sequence ID so concurrent fragmented bridges can be
// disambiguated by the receiving side.
static inline uint16_t crc16_ccitt(const uint8_t *data, size_t len) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021)
                                  : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

inline bool printReticulum(const uint8_t *buf, size_t len, const char *tag) {
    if (len == 0) return false;
    // base64 expansion is at most ceil(N/3)*4 ≈ 1.34*N + small. Cap the
    // scratch buffer; if a frame is larger than what fits, truncate the
    // diagnostic dump (the bridge handles the full frame separately).
    static const size_t MAX_DUMP = 1024;
    unsigned char b64[MAX_DUMP];
    size_t b64Len = 0;
    int rc = mbedtls_base64_encode(b64, sizeof(b64), &b64Len, buf, len);
    Serial.printf("[%8lu ms][%s decoded] Reticulum/RNode raw %u B (b64=%u%s): ",
                  millis(), tag, (unsigned)len, (unsigned)b64Len,
                  rc == 0 ? "" : ",TRUNCATED");
    for (size_t i = 0; i < b64Len; i++) Serial.write((char)b64[i]);
    Serial.println();
    return true;
}

// Caller-buffer base64 of the raw RNS bytes. Useful as a low-level helper
// (not used by the fragmenting bridge in main.cpp, which streams base64 per
// fragment to keep memory low). Output is base64 with '=' padding, null-
// terminated. Returns false if the destination buffer is too small.
inline bool extractReticulumBase64(const uint8_t *buf, size_t len,
                                    char *bodyOut, size_t bodyOutCap) {
    if (!bodyOut || bodyOutCap < 2) return false;
    bodyOut[0] = 0;
    if (len == 0) return false;
    size_t b64Len = 0;
    int rc = mbedtls_base64_encode((unsigned char *)bodyOut, bodyOutCap, &b64Len,
                                    buf, len);
    if (rc != 0) {                       // MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL
        bodyOut[0] = 0;
        return false;
    }
    bodyOut[b64Len] = 0;
    return true;
}

// --- Reticulum fragment reassembly (STUB) -----------------------------------
// TODO: when the MT/MC -> RNS bridge direction lands, the bridge will receive
// fragmented bodies of the form "[rns AA 1/3] base64data==" on its MT or MC
// RX path. To reconstruct the original RNS frame for re-transmission on the
// RNS radio, the bridge needs to:
//   1. Parse "[rns <seq:hex> <x>/<y>] <base64>" out of the text body.
//   2. Look up (or create) a reassembly slot keyed on <seq>.
//   3. base64-decode <base64> and store the raw bytes at the slot's
//      (x-1)*rawPerFrag offset; mark fragment x as received.
//   4. When all 1..y fragments have been received, return the assembled raw
//      RNS frame in outBuf.
//   5. Time out incomplete slots after ~30 s so stale state doesn't
//      accumulate.
//
// This stub returns false unconditionally; the real reassembly engine lands
// alongside the RNS encoder in MeshEncoderDebug.h.
inline bool reassembleReticulumFragment(const char * /*body*/,
                                         uint8_t * /*outBuf*/,
                                         size_t   /*outBufCap*/,
                                         size_t  & /*outLen*/) {
    return false;
}


// --- Public entry point: dispatch by sync word ------------------------------
inline void print(const uint8_t *buf, size_t len, uint8_t syncWord, const char *tag) {
    if (syncWord == SYNC_WORD_MESHCORE) {
        printMeshCore(buf, len, tag);
    } else if (syncWord == SYNC_WORD_MESHTASTIC) {
        printMeshtastic(buf, len, tag);
    } else if (syncWord == SYNC_WORD_RETICULUM) {
        printReticulum(buf, len, tag);
    }
    // Other sync words: silently ignore — we don't know that protocol.
}

}  // namespace MeshDecoderDebug
