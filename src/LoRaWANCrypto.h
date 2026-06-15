// LoRaWANCrypto.h
// ---------------------------------------------------------------------------
// LoRaWAN 1.0.x ABP *uplink* encoder for the cross-protocol bridge (ABP P1).
//
// Mints a complete, LNS-ingestible Class-A Data-Up PHYPayload from a bridge-held
// ABP identity (DevAddr + NwkSKey + AppSKey) plus an application payload. This is
// the *keyed* counterpart to v8.3's *keyless* LoRaWAN capture/relay: emitting
// valid LoRaWAN is fundamentally keyed (a correct MIC + encrypted FRMPayload
// require the device keys), so an encoder cannot be keyless (ABP-LORAWAN-SPEC §2/§7).
//
//   PHYPayload = MHDR(1) | MACPayload | MIC(4)
//   MACPayload = FHDR | FPort(1) | FRMPayload
//   FHDR       = DevAddr(4 LE) | FCtrl(1) | FCnt(2 LE = low 16 of the 32-bit ctr)
//   MHDR       = 0x40 Unconfirmed Data Up (Major 0); 0x80 = Confirmed (avoid)
//   FCtrl(up)  = 0x00  (ADR=0, no FOpts — ABP-LORAWAN-SPEC §4)
//
// Crypto sourcing (ABP-LORAWAN-SPEC §12, finding A4):
//   - AES-128 ECB is present & proven in the prebuilt esp32s3 mbedTLS (the
//     MeshCore/Meshtastic encoders already call mbedtls_aes_crypt_ecb).
//   - AES-CMAC is NOT linkable: CONFIG_MBEDTLS_CMAC_C is compiled OFF in the
//     arduino-esp32 2.0.17 prebuilt lib, so mbedtls_cipher_cmac() link-errors.
//     We therefore carry a self-contained RFC 4493 AES-CMAC over AES-ECB here —
//     the only new crypto primitive ABP needs.
//   - FRMPayload encryption is LoRaWAN's CTR-like mode: keystream block i is
//     AES-ECB(K, A_i) for i = 1,2,...; built explicitly below so there is no
//     dependence on mbedTLS CTR counter endianness/carry semantics.
//
// Largest scratch buffer is one 16-byte AES block (plus a <=267 B MIC staging
// buffer inside encodeUplink), so this is safe to call from the
// BRIDGE_TASK_STACK=4096 radio/dispatch tasks.
// ---------------------------------------------------------------------------

#pragma once

#include <Arduino.h>
#include <stdint.h>
#include <string.h>
#include <mbedtls/aes.h>

namespace LoRaWANCrypto {

// --- AES-128 ECB, single block (encrypt) -----------------------------------
inline void aesEcb(const uint8_t key[16], const uint8_t in[16], uint8_t out[16]) {
    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);
    mbedtls_aes_setkey_enc(&aes, key, 128);
    mbedtls_aes_crypt_ecb(&aes, MBEDTLS_AES_ENCRYPT, in, out);
    mbedtls_aes_free(&aes);
}

// --- RFC 4493 §6.1 subkey generation ---------------------------------------
inline void cmacShiftLeft(const uint8_t in[16], uint8_t out[16]) {
    uint8_t carry = 0;
    for (int i = 15; i >= 0; --i) {
        uint8_t b = in[i];
        out[i] = (uint8_t)((b << 1) | carry);
        carry  = (b & 0x80) ? 1 : 0;
    }
}

inline void cmacSubkeys(const uint8_t key[16], uint8_t k1[16], uint8_t k2[16]) {
    static const uint8_t Rb = 0x87;
    uint8_t L[16];
    uint8_t zero[16] = {0};
    aesEcb(key, zero, L);              // L = AES-128(K, 0^128)
    cmacShiftLeft(L, k1);
    if (L[0]  & 0x80) k1[15] ^= Rb;
    cmacShiftLeft(k1, k2);
    if (k1[0] & 0x80) k2[15] ^= Rb;
}

// --- AES-CMAC (RFC 4493), full 16-byte tag ---------------------------------
// msg may be NULL iff len == 0.
inline void aesCmac(const uint8_t key[16], const uint8_t *msg, size_t len,
                    uint8_t mac[16]) {
    uint8_t k1[16], k2[16];
    cmacSubkeys(key, k1, k2);

    size_t n;                          // block count
    bool   lastComplete;
    if (len == 0)        { n = 1; lastComplete = false; }
    else                 { n = (len + 15) / 16; lastComplete = (len % 16) == 0; }

    const uint8_t *lastBlock = msg + (n - 1) * 16;
    uint8_t mLast[16];
    if (lastComplete) {
        for (int i = 0; i < 16; ++i) mLast[i] = lastBlock[i] ^ k1[i];
    } else {
        size_t rem = (len == 0) ? 0 : (len % 16);   // bytes in the partial final block
        for (size_t i = 0; i < 16; ++i) {
            uint8_t b = (i < rem) ? lastBlock[i] : (i == rem ? 0x80 : 0x00);
            mLast[i] = b ^ k2[i];
        }
    }

    uint8_t x[16] = {0};
    uint8_t y[16];
    for (size_t i = 0; i + 1 < n; ++i) {
        for (int j = 0; j < 16; ++j) y[j] = x[j] ^ msg[i * 16 + j];
        aesEcb(key, y, x);
    }
    for (int j = 0; j < 16; ++j) y[j] = x[j] ^ mLast[j];
    aesEcb(key, y, mac);
}

// --- FRMPayload (de/)encryption, in place — LoRaWAN CTR-like keystream ------
// Symmetric (encrypt == decrypt). dir hard-coded to 0 (uplink). For FPort>0
// pass AppSKey; for FPort==0 (MAC-only) pass NwkSKey.
inline void cryptFrmPayload(const uint8_t key[16], uint32_t devAddr,
                            uint32_t fcnt32, uint8_t *payload, size_t len) {
    if (len == 0) return;
    uint8_t a[16];
    uint8_t s[16];
    a[0] = 0x01;
    a[1] = a[2] = a[3] = a[4] = 0x00;
    a[5] = 0x00;                                   // Dir = 0 (uplink)
    a[6]  = (uint8_t)(devAddr >> 0);
    a[7]  = (uint8_t)(devAddr >> 8);
    a[8]  = (uint8_t)(devAddr >> 16);
    a[9]  = (uint8_t)(devAddr >> 24);
    a[10] = (uint8_t)(fcnt32 >> 0);
    a[11] = (uint8_t)(fcnt32 >> 8);
    a[12] = (uint8_t)(fcnt32 >> 16);
    a[13] = (uint8_t)(fcnt32 >> 24);
    a[14] = 0x00;
    size_t blocks = (len + 15) / 16;
    for (size_t i = 1; i <= blocks; ++i) {
        a[15] = (uint8_t)i;                        // block index, 1-based
        aesEcb(key, a, s);
        size_t off = (i - 1) * 16;
        size_t take = (len - off < 16) ? (len - off) : 16;
        for (size_t j = 0; j < take; ++j) payload[off + j] ^= s[j];
    }
}

// --- Encode a complete Data-Up PHYPayload ----------------------------------
// Returns the frame length on success, or 0 on error (bad args / buffer too
// small / payload over the LoRaWAN cap).
inline size_t encodeUplink(uint32_t devAddr,
                           const uint8_t nwkSKey[16], const uint8_t appSKey[16],
                           uint32_t fcnt32, uint8_t fport,
                           const uint8_t *payload, size_t payloadLen,
                           bool confirmed,
                           uint8_t *out, size_t cap) {
    if (!out) return 0;
    if (payloadLen > 242) return 0;                // hard cap (region DR limits are tighter)
    const size_t frameLen = 1 + 4 + 1 + 2 + 1 + payloadLen + 4;
    if (frameLen > cap) return 0;

    size_t o = 0;
    out[o++] = confirmed ? 0x80 : 0x40;            // MHDR
    out[o++] = (uint8_t)(devAddr >> 0);            // DevAddr (LE)
    out[o++] = (uint8_t)(devAddr >> 8);
    out[o++] = (uint8_t)(devAddr >> 16);
    out[o++] = (uint8_t)(devAddr >> 24);
    out[o++] = 0x00;                               // FCtrl: ADR=0, no FOpts
    out[o++] = (uint8_t)(fcnt32 >> 0);             // FCnt low 16 (LE)
    out[o++] = (uint8_t)(fcnt32 >> 8);
    out[o++] = fport;

    if (payloadLen) memcpy(out + o, payload, payloadLen);
    const uint8_t *k = (fport == 0) ? nwkSKey : appSKey;
    cryptFrmPayload(k, devAddr, fcnt32, out + o, payloadLen);
    o += payloadLen;                               // o == len(msg) = frame minus MIC

    // MIC = AES-CMAC(NwkSKey, B0 || msg)[0:4]
    uint8_t b0[16];
    b0[0] = 0x49;
    b0[1] = b0[2] = b0[3] = b0[4] = 0x00;
    b0[5] = 0x00;                                  // Dir = 0 (uplink)
    b0[6]  = (uint8_t)(devAddr >> 0);
    b0[7]  = (uint8_t)(devAddr >> 8);
    b0[8]  = (uint8_t)(devAddr >> 16);
    b0[9]  = (uint8_t)(devAddr >> 24);
    b0[10] = (uint8_t)(fcnt32 >> 0);
    b0[11] = (uint8_t)(fcnt32 >> 8);
    b0[12] = (uint8_t)(fcnt32 >> 16);
    b0[13] = (uint8_t)(fcnt32 >> 24);
    b0[14] = 0x00;
    b0[15] = (uint8_t)o;                           // len(msg), <= 251

    uint8_t micBuf[16 + 251];                      // B0(16) | msg(<=251)
    memcpy(micBuf, b0, 16);
    memcpy(micBuf + 16, out, o);
    uint8_t fullMac[16];
    aesCmac(nwkSKey, micBuf, 16 + o, fullMac);

    out[o++] = fullMac[0];
    out[o++] = fullMac[1];
    out[o++] = fullMac[2];
    out[o++] = fullMac[3];
    return o;
}

// --- On-device known-answer self-tests (P1 acceptance) ---------------------
// The new crypto (AES-CMAC) is verified against the canonical RFC 4493 vectors;
// the FRMPayload keystream against its A_1 definition; and frame assembly via an
// encrypt->decrypt round-trip plus a structural byte check. End-to-end against a
// live LNS is the separate bench step (a wrong MIC is silently dropped, so this
// catches the failure on the bench instead of in ChirpStack). Logs each line and
// returns true iff every check passes.
inline bool selfTest() {
    bool ok = true;

    // RFC 4493 example key + 64-byte message.
    static const uint8_t K[16] = {
        0x2b,0x7e,0x15,0x16, 0x28,0xae,0xd2,0xa6,
        0xab,0xf7,0x15,0x88, 0x09,0xcf,0x4f,0x3c };
    static const uint8_t M[64] = {
        0x6b,0xc1,0xbe,0xe2, 0x2e,0x40,0x9f,0x96, 0xe9,0x3d,0x7e,0x11, 0x73,0x93,0x17,0x2a,
        0xae,0x2d,0x8a,0x57, 0x1e,0x03,0xac,0x9c, 0x9e,0xb7,0x6f,0xac, 0x45,0xaf,0x8e,0x51,
        0x30,0xc8,0x1c,0x46, 0xa3,0x5c,0xe4,0x11, 0xe5,0xfb,0xc1,0x19, 0x1a,0x0a,0x52,0xef,
        0xf6,0x9f,0x24,0x45, 0xdf,0x4f,0x9b,0x17, 0xad,0x2b,0x41,0x7b, 0xe6,0x6c,0x37,0x10 };
    struct { size_t len; uint8_t exp[16]; } kat[4] = {
        {  0, {0xbb,0x1d,0x69,0x29,0xe9,0x59,0x37,0x28,0x7f,0xa3,0x7d,0x12,0x9b,0x75,0x67,0x46} },
        { 16, {0x07,0x0a,0x16,0xb4,0x6b,0x4d,0x41,0x44,0xf7,0x9b,0xdd,0x9d,0xd0,0x4a,0x28,0x7c} },
        { 40, {0xdf,0xa6,0x67,0x47,0xde,0x9a,0xe6,0x30,0x30,0xca,0x32,0x61,0x14,0x97,0xc8,0x27} },
        { 64, {0x51,0xf0,0xbe,0xbf,0x7e,0x3b,0x9d,0x92,0xfc,0x49,0x74,0x17,0x79,0x36,0x3c,0xfe} },
    };
    for (int i = 0; i < 4; ++i) {
        uint8_t mac[16];
        aesCmac(K, M, kat[i].len, mac);
        bool pass = memcmp(mac, kat[i].exp, 16) == 0;
        ok = ok && pass;
        Serial.printf("[lw-selftest] RFC4493 CMAC len=%2u : %s\n",
                      (unsigned)kat[i].len, pass ? "PASS" : "FAIL");
    }

    // FRMPayload keystream: encrypting 16 zero bytes must yield AES-ECB(K, A_1).
    {
        uint8_t buf[16] = {0};
        cryptFrmPayload(K, 0x01020304u, 0x00000005u, buf, sizeof(buf));
        uint8_t a1[16] = { 0x01,0x00,0x00,0x00,0x00, 0x00,
                           0x04,0x03,0x02,0x01, 0x05,0x00,0x00,0x00, 0x00, 0x01 };
        uint8_t s1[16];
        aesEcb(K, a1, s1);
        bool pass = memcmp(buf, s1, 16) == 0;
        ok = ok && pass;
        Serial.printf("[lw-selftest] FRMPayload A_1 keystream : %s\n", pass ? "PASS" : "FAIL");
    }

    // Frame assembly: encode, then independently decrypt the FRMPayload region
    // and confirm it round-trips to the plaintext, plus a structural byte check.
    {
        uint8_t nwk[16], app[16];
        for (int i = 0; i < 16; ++i) { nwk[i] = K[i]; app[i] = (uint8_t)(K[i] ^ 0xFF); }
        const uint32_t devAddr = 0x26011B22u;
        const uint32_t fcnt    = 0x00000001u;
        const uint8_t  fport   = 13;
        const char    *pt      = "weatherX";
        const size_t   ptLen   = strlen(pt);
        uint8_t frame[64];
        size_t n = encodeUplink(devAddr, nwk, app, fcnt, fport,
                                (const uint8_t *)pt, ptLen, false, frame, sizeof(frame));

        bool pass = (n == 1 + 4 + 1 + 2 + 1 + ptLen + 4);
        pass = pass && frame[0] == 0x40;                                  // MHDR
        pass = pass && frame[1] == 0x22 && frame[2] == 0x1B               // DevAddr LE
                    && frame[3] == 0x01 && frame[4] == 0x26;
        pass = pass && frame[5] == 0x00;                                  // FCtrl
        pass = pass && frame[6] == 0x01 && frame[7] == 0x00;              // FCnt low16 LE
        pass = pass && frame[8] == fport;                                 // FPort
        if (pass) {
            uint8_t dec[16];
            memcpy(dec, frame + 9, ptLen);
            cryptFrmPayload(app, devAddr, fcnt, dec, ptLen);              // decrypt
            pass = memcmp(dec, pt, ptLen) == 0;
        }
        ok = ok && pass;
        Serial.printf("[lw-selftest] frame assembly + round-trip : %s\n", pass ? "PASS" : "FAIL");
    }

    Serial.printf("[lw-selftest] overall : %s\n", ok ? "PASS" : "FAIL");
    return ok;
}

}  // namespace LoRaWANCrypto
