// MeshEncoderDebug.h
// ---------------------------------------------------------------------------
// Cross-protocol bridge counterpart to MeshDecoderDebug.h. Given a short
// UTF-8 text body, build a complete on-air packet for either MeshCore
// (sync 0x12, GRP_TXT on the public channel) or Meshtastic (sync 0x2B,
// TEXT_MESSAGE_APP on the default LongFast channel).
//
// All crypto runs in-place inside the caller's output buffer — no large
// stack scratch buffers — so encoders are safe to call from the
// BRIDGE_TASK_STACK=4096 radio tasks.
//
// Loop prevention is the caller's responsibility (see main.cpp): a marker
// like "[MT] " or "[MC] " must be prepended before encoding, and the
// receiving side must drop bodies that already carry the other side's
// marker.
// ---------------------------------------------------------------------------

#pragma once

#include <Arduino.h>
#include <stdint.h>
#include <string.h>
#include <esp_random.h>
#include <mbedtls/aes.h>
#include <mbedtls/md.h>

#include "MeshDecoderDebug.h"   // for key constants + channel-hash constants

// Cross-protocol bridge identity (Meshtastic side) is now owned by
// BridgeConfig. The encoders below take srcNodeId as a parameter so they
// stay decoupled from any particular configuration source; the call sites
// in main.cpp read BridgeConfig::mtNodeId() and pass the value in.

namespace MeshEncoderDebug {

// --- MeshCore: build a GRP_TXT packet on the public channel ----------------
// body : null-terminated UTF-8 string. Long bodies that don't fit in a single
//        224-byte plaintext block return false (caller should truncate).
// ts   : value placed in the plaintext timestamp field. The decoder prints it
//        but doesn't otherwise act on it; main.cpp passes 0 for MT->MC.
// outBuf / outBufCap : caller-provided buffer for the assembled packet.
// outLen : set to the byte count actually written on success.
// Returns true on success.
inline bool encodeMeshCoreGrpTxt(const RadioChannel &ch,
                                  const char *body, uint32_t ts,
                                  uint8_t *outBuf, size_t outBufCap,
                                  size_t  &outLen) {
    using namespace MeshDecoderDebug;
    if (!body || !outBuf) return false;
    size_t bodyLen = strlen(body);

    // Plaintext layout: [ts:4 LE][flags:1=0][body bytes][null][zero-pad to 16]
    size_t ptCore = 4 + 1 + bodyLen + 1;
    size_t ptLen  = (ptCore + 15u) & ~15u;
    if (ptLen > 224) return false;                          // sanity cap

    const size_t hdrBytes = 5;       // hdr + pathLen + chanHash + mac[2]
    if (hdrBytes + ptLen > outBufCap) return false;

    // Stage plaintext directly into outBuf at offset hdrBytes, then encrypt
    // in place. Zero-pad fills the trailing bytes via memset.
    uint8_t *p = outBuf + hdrBytes;
    memset(p, 0, ptLen);
    p[0] = (uint8_t)(ts >>  0);
    p[1] = (uint8_t)(ts >>  8);
    p[2] = (uint8_t)(ts >> 16);
    p[3] = (uint8_t)(ts >> 24);
    // p[4] flags = 0 (already zero)
    memcpy(p + 5, body, bodyLen);
    // p[5 + bodyLen] null terminator = 0 (already zero)

    // AES-128-ECB encrypt in place
    {
        mbedtls_aes_context aes;
        mbedtls_aes_init(&aes);
        mbedtls_aes_setkey_enc(&aes, ch.key, 128);
        for (size_t i = 0; i < ptLen; i += 16) {
            mbedtls_aes_crypt_ecb(&aes, MBEDTLS_AES_ENCRYPT, p + i, p + i);
        }
        mbedtls_aes_free(&aes);
    }

    // HMAC-SHA256(key, ciphertext)[:2] — matches the decoder's verification
    uint8_t hmac[32];
    {
        mbedtls_md_context_t mdCtx;
        mbedtls_md_init(&mdCtx);
        const mbedtls_md_info_t *info =
            mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
        mbedtls_md_setup(&mdCtx, info, /*hmac=*/1);
        mbedtls_md_hmac_starts(&mdCtx, ch.key, 16);   // MeshCore: 16-byte key
        mbedtls_md_hmac_update(&mdCtx, p, ptLen);
        mbedtls_md_hmac_finish(&mdCtx, hmac);
        mbedtls_md_free(&mdCtx);
    }

    // Fill the 5-byte packet header.
    // Header byte: version=0, payload_type=GRP_TXT(0x05), routeType=FLOOD(0x01).
    // routeType 0x01 leaves the optional 4-byte transport-codes field absent.
    outBuf[0] = (uint8_t)((0 << 6) | (0x05 << 2) | 0x01);
    outBuf[1] = 0;                                  // pathLen — no relays
    outBuf[2] = ch.channelHash;                    // configured MC channel hash
    outBuf[3] = hmac[0];
    outBuf[4] = hmac[1];

    outLen = hdrBytes + ptLen;
    return true;
}


// --- Meshtastic: build a TEXT_MESSAGE_APP packet on LongFast ---------------
// body : null-terminated UTF-8 string. Bodies > ~200 bytes return false.
// outBuf / outBufCap : caller-provided buffer for the assembled packet.
// outLen : set to the byte count actually written on success.
// outPid : optional — set to the random packet id stamped into the header, so
//          the caller can fold it into the dedup hash (V8.2-SPEC §15.1).
//          Meshtastic relays/replays preserve (src, packet_id), so keying dedup
//          on the pid lets genuine distinct messages with identical text
//          through while echoes of our own emission still match and drop.
// Returns true on success.
inline bool encodeMeshtasticText(const RadioChannel &ch,
                                  uint32_t srcNodeId, const char *body,
                                  uint8_t *outBuf, size_t outBufCap,
                                  size_t  &outLen, uint32_t *outPid = nullptr) {
    using namespace MeshDecoderDebug;
    if (!body || !outBuf) return false;
    size_t bodyLen = strlen(body);
    if (bodyLen > 200) return false;                // keep packet < 256 B

    // Worst-case packet length: 16-byte header + protobuf(3 + 2 + bodyLen)
    const size_t lenVarintBytes = (bodyLen < 128) ? 1u : 2u;
    const size_t pbLen = 2 + 1 + lenVarintBytes + bodyLen;
    if (16 + pbLen > outBufCap) return false;

    // Build the Data protobuf directly into outBuf at offset 16:
    //   field 1 (portnum,  varint)         : tag 0x08, value 1
    //   field 2 (payload,  length-delim)   : tag 0x12, len, raw bytes
    size_t off = 16;
    outBuf[off++] = 0x08;   // tag: field 1, wire type 0 (varint)
    outBuf[off++] = 0x01;   // portnum = TEXT_MESSAGE_APP
    outBuf[off++] = 0x12;   // tag: field 2, wire type 2 (length-delim)
    if (bodyLen < 128) {
        outBuf[off++] = (uint8_t)bodyLen;
    } else {
        outBuf[off++] = (uint8_t)((bodyLen & 0x7Fu) | 0x80u);
        outBuf[off++] = (uint8_t)(bodyLen >> 7);
    }
    memcpy(outBuf + off, body, bodyLen);
    off += bodyLen;

    // Random packet id; avoid 0 (some Meshtastic code paths treat 0 as
    // "no id assigned").
    uint32_t pid = esp_random();
    if (pid == 0) pid = 1;
    if (outPid) *outPid = pid;   // expose for dedup keying (V8.2-SPEC §15.1)

    const uint32_t dest = 0xFFFFFFFFu;                  // broadcast
    const uint32_t src  = srcNodeId;

    // 16-byte transport header
    outBuf[0]  = (uint8_t)(dest >>  0);
    outBuf[1]  = (uint8_t)(dest >>  8);
    outBuf[2]  = (uint8_t)(dest >> 16);
    outBuf[3]  = (uint8_t)(dest >> 24);
    outBuf[4]  = (uint8_t)(src  >>  0);
    outBuf[5]  = (uint8_t)(src  >>  8);
    outBuf[6]  = (uint8_t)(src  >> 16);
    outBuf[7]  = (uint8_t)(src  >> 24);
    outBuf[8]  = (uint8_t)(pid  >>  0);
    outBuf[9]  = (uint8_t)(pid  >>  8);
    outBuf[10] = (uint8_t)(pid  >> 16);
    outBuf[11] = (uint8_t)(pid  >> 24);
    // flags byte layout (Meshtastic on-air): [hop_start:3][via_mqtt:1][want_ack:1][hop_limit:3]
    // Originator convention: hop_start == hop_limit. A real captured packet had
    // 0x84 (hop_start=4, hop_limit=4). 0x07 (hop_start=0, hop_limit=7) is
    // structurally invalid and current Meshtastic firmware silently drops it.
    outBuf[12] = 0x63;                                  // hop_start=3, hop_limit=3
    outBuf[13] = ch.channelHash;         // configured channel
    outBuf[14] = 0;                                     // next_hop
    outBuf[15] = 0;                                     // relay_node

    // AES-128-CTR nonce = [pid:4 LE][0:4][src:4 LE][0:4]
    uint8_t nonce[16] = {
        (uint8_t)(pid >>  0), (uint8_t)(pid >>  8),
        (uint8_t)(pid >> 16), (uint8_t)(pid >> 24),
        0, 0, 0, 0,
        (uint8_t)(src >>  0), (uint8_t)(src >>  8),
        (uint8_t)(src >> 16), (uint8_t)(src >> 24),
        0, 0, 0, 0
    };

    {
        mbedtls_aes_context aes;
        mbedtls_aes_init(&aes);
        mbedtls_aes_setkey_enc(&aes, ch.key,
                               (unsigned)ch.keyLen * 8);
        uint8_t stream[16] = {};
        size_t  nc_off = 0;
        mbedtls_aes_crypt_ctr(&aes, pbLen, &nc_off, nonce, stream,
                              outBuf + 16, outBuf + 16);
        mbedtls_aes_free(&aes);
    }

    outLen = 16 + pbLen;
    return true;
}

// --- Meshtastic: build a NODEINFO_APP packet --------------------------------
// Wraps a small User submessage (id, long_name, short_name) inside a Data
// message with portnum=NODEINFO_APP(4), then encrypts and prepends the
// 16-byte transport header just like encodeMeshtasticText().
//
// Sending this on a regular interval (every ~5 min is conventional) makes
// Meshtastic clients recognise the bridge as a known node, so text messages
// from BRIDGE_MT_NODE_ID surface in their UI instead of being silently
// hidden as "from an unknown sender".
inline bool encodeMeshtasticNodeInfo(const RadioChannel &ch,
                                      uint32_t srcNodeId,
                                      const char *id,
                                      const char *longName,
                                      const char *shortName,
                                      uint8_t *outBuf, size_t outBufCap,
                                      size_t  &outLen) {
    using namespace MeshDecoderDebug;
    if (!id || !longName || !shortName || !outBuf) return false;

    size_t idLen    = strlen(id);
    size_t longLen  = strlen(longName);
    size_t shortLen = strlen(shortName);
    // Keep every string field under 128 B so its varint length is one byte.
    if (idLen >= 128 || longLen >= 128 || shortLen >= 128) return false;

    // User submessage layout:
    //   field 1 (id,         string) : 0x0A <len> <bytes>
    //   field 2 (long_name,  string) : 0x12 <len> <bytes>
    //   field 3 (short_name, string) : 0x1A <len> <bytes>
    const size_t userLen = (1 + 1 + idLen)
                         + (1 + 1 + longLen)
                         + (1 + 1 + shortLen);

    // Data wrapper:
    //   field 1 (portnum, varint)        : 0x08 0x04  (NODEINFO_APP = 4)
    //   field 2 (payload, length-delim)  : 0x12 <len-varint> <User bytes>
    const size_t userLenVarintBytes = (userLen < 128) ? 1u : 2u;
    const size_t dataLen = 2 + 1 + userLenVarintBytes + userLen;

    if (16 + dataLen > outBufCap) return false;

    // Stage the Data protobuf in place at outBuf[16..]
    size_t off = 16;
    outBuf[off++] = 0x08;                       // portnum tag
    outBuf[off++] = 0x04;                       // NODEINFO_APP
    outBuf[off++] = 0x12;                       // payload tag
    if (userLen < 128) {
        outBuf[off++] = (uint8_t)userLen;
    } else {
        outBuf[off++] = (uint8_t)((userLen & 0x7Fu) | 0x80u);
        outBuf[off++] = (uint8_t)(userLen >> 7);
    }
    // User.id
    outBuf[off++] = 0x0A;
    outBuf[off++] = (uint8_t)idLen;
    memcpy(outBuf + off, id, idLen);  off += idLen;
    // User.long_name
    outBuf[off++] = 0x12;
    outBuf[off++] = (uint8_t)longLen;
    memcpy(outBuf + off, longName, longLen);  off += longLen;
    // User.short_name
    outBuf[off++] = 0x1A;
    outBuf[off++] = (uint8_t)shortLen;
    memcpy(outBuf + off, shortName, shortLen);  off += shortLen;
    // off == 16 + dataLen

    // Random packet id (non-zero, as in the text encoder)
    uint32_t pid = esp_random();
    if (pid == 0) pid = 1;

    const uint32_t dest = 0xFFFFFFFFu;            // broadcast
    const uint32_t src  = srcNodeId;

    // 16-byte transport header (identical layout to encodeMeshtasticText)
    outBuf[0]  = (uint8_t)(dest >>  0);
    outBuf[1]  = (uint8_t)(dest >>  8);
    outBuf[2]  = (uint8_t)(dest >> 16);
    outBuf[3]  = (uint8_t)(dest >> 24);
    outBuf[4]  = (uint8_t)(src  >>  0);
    outBuf[5]  = (uint8_t)(src  >>  8);
    outBuf[6]  = (uint8_t)(src  >> 16);
    outBuf[7]  = (uint8_t)(src  >> 24);
    outBuf[8]  = (uint8_t)(pid  >>  0);
    outBuf[9]  = (uint8_t)(pid  >>  8);
    outBuf[10] = (uint8_t)(pid  >> 16);
    outBuf[11] = (uint8_t)(pid  >> 24);
    outBuf[12] = 0x63;                            // hop_start=3, hop_limit=3
    outBuf[13] = ch.channelHash;
    outBuf[14] = 0;
    outBuf[15] = 0;

    // AES-128-CTR encrypt the Data protobuf in place
    uint8_t nonce[16] = {
        (uint8_t)(pid >>  0), (uint8_t)(pid >>  8),
        (uint8_t)(pid >> 16), (uint8_t)(pid >> 24),
        0, 0, 0, 0,
        (uint8_t)(src >>  0), (uint8_t)(src >>  8),
        (uint8_t)(src >> 16), (uint8_t)(src >> 24),
        0, 0, 0, 0
    };

    {
        mbedtls_aes_context aes;
        mbedtls_aes_init(&aes);
        mbedtls_aes_setkey_enc(&aes, ch.key,
                               (unsigned)ch.keyLen * 8);
        uint8_t stream[16] = {};
        size_t  nc_off = 0;
        mbedtls_aes_crypt_ctr(&aes, dataLen, &nc_off, nonce, stream,
                              outBuf + 16, outBuf + 16);
        mbedtls_aes_free(&aes);
    }

    outLen = 16 + dataLen;
    return true;
}

// --- Reticulum / RNode encoder (NOT IMPLEMENTED) ---------------------------
// Building a proper RNS packet (header byte, IFAC, hop count, destination
// hash, context byte, encrypted payload framing) is non-trivial and not
// done yet. Until it is, this stub always returns false, and the bridge
// dispatcher in main.cpp treats "destination sync == 0x42" as a log-and-drop
// path with the "No TX 2 RNS:" prefix rather than calling this function.
//
// The RX -> RNS source side IS implemented (see bridgePacket() in main.cpp):
// raw RNS bytes are base64-encoded and fragmented as needed across one or
// more MT/MC text packets of the form "[rns <seq> <x>/<y>] <base64>".
// When this encoder lands, the reverse direction will need:
//   - reassembleReticulumFragment() from MeshDecoderDebug.h to glue fragments
//     back into raw bytes,
//   - this function to wrap those bytes in a valid RNS LoRa frame and emit
//     them on the radio whose sync word is SYNC_WORD_RETICULUM.
inline bool encodeReticulum(const char * /*body*/,
                             uint8_t * /*outBuf*/, size_t /*outBufCap*/,
                             size_t  & /*outLen*/) {
    return false;
}

}  // namespace MeshEncoderDebug
