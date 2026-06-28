// CamC2.cpp — see CamC2.h for the wire frame + security construction.

#include "CamC2.h"
#if defined(BRIDGE_ROLE_CAMERA) || defined(BRIDGE_CAM_COMMANDER)

#include "CamC2Config.h"
#include "BridgeConfig.h"       // mtNodeId() = this device's C2 id
#include "LoRaWANCrypto.h"      // aesEcb / aesCmac / cryptFrmPayload / selfTest
#include "SerialLog.h"          // serialized console (C2 runs in radioTask context)
#include "CameraNode.h"         // Phase 2: OV2640 capture (self-gated BRIDGE_ROLE_CAMERA)
#include <string.h>

namespace CamC2 {

static EmitFn s_emit = nullptr;
static bool   s_ok   = false;       // crypto self-test latch (fail-closed)
static uint8_t s_lastCmd = CMD_NONE;
static uint8_t s_lastRes = RES_OK;

// --- little-endian helpers --------------------------------------------------
static inline void put32le(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static inline uint32_t get32le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// Constant-time equality (no early-out timing oracle on the auth tag).
static bool ctEqual(const uint8_t *a, const uint8_t *b, size_t n) {
    uint8_t d = 0;
    for (size_t i = 0; i < n; i++) d |= (uint8_t)(a[i] ^ b[i]);
    return d == 0;
}

// This device's C2 identity (senderId in outbound frames + the recipient match on
// inbound). A bench build can PIN it via -DBRIDGE_C2_MY_ID so two boards can
// whitelist each other from build flags (the MAC-derived node id isn't known until
// runtime); otherwise it is the MAC-derived BridgeConfig::mtNodeId().
static uint32_t myC2Id() {
#ifdef BRIDGE_C2_MY_ID
    return (uint32_t)(BRIDGE_C2_MY_ID);
#else
    return BridgeConfig::mtNodeId();
#endif
}

// Derive distinct MAC and encryption keys from the 16-byte peer PSK (domain
// separation: AES-ECB(psk, label) for two different label blocks — the PSK is
// never used directly for both the CMAC and the CTR keystream).
static void deriveKeys(const uint8_t psk[16], uint8_t macKey[16], uint8_t encKey[16]) {
    uint8_t lbl[16];
    memset(lbl, 0, sizeof(lbl)); lbl[0] = 0x01; LoRaWANCrypto::aesEcb(psk, lbl, macKey);
    memset(lbl, 0, sizeof(lbl)); lbl[0] = 0x02; LoRaWANCrypto::aesEcb(psk, lbl, encKey);
}

// Build a complete signed+encrypted frame into out[]. Returns the length, or 0 if
// the payload won't fit. Pure (no config lookup) so it is self-test-able.
static size_t buildFrame(uint8_t type, uint32_t senderId, uint32_t recipientId,
                         uint32_t seq, const uint8_t psk[16],
                         const uint8_t *payload, size_t payloadLen,
                         uint8_t *out, size_t cap) {
    if (payloadLen > MAX_PAYLOAD) return 0;
    const size_t frameLen = HDR_LEN + payloadLen + TAG_LEN;
    if (frameLen > cap) return 0;

    out[0] = MAGIC_VER;
    out[1] = type;
    put32le(out + 2,  senderId);
    put32le(out + 6,  recipientId);
    put32le(out + 10, seq);
    if (payloadLen) memcpy(out + HDR_LEN, payload, payloadLen);

    uint8_t macKey[16], encKey[16];
    deriveKeys(psk, macKey, encKey);
    // Encrypt the payload in place. nonce = (senderId as devAddr, seq as fcnt) is
    // unique per (peer-key, message) since seq is monotonic per sender.
    LoRaWANCrypto::cryptFrmPayload(encKey, senderId, seq, out + HDR_LEN, payloadLen);

    const size_t authLen = HDR_LEN + payloadLen;     // encrypt-then-MAC: MAC covers header+ciphertext
    uint8_t full[16];
    LoRaWANCrypto::aesCmac(macKey, out, authLen, full);
    memcpy(out + authLen, full, TAG_LEN);
    return authLen + TAG_LEN;
}

// Verify the CMAC of a received frame under `psk` and (on success) decrypt the
// payload into plainOut. Pure crypto — no config lookup, no replay check. Returns
// true iff the tag is valid. On success: *type/*sender/*recip/*seq + plain[*plainLen].
static bool verifyAndDecrypt(const uint8_t *buf, size_t len, const uint8_t psk[16],
                             uint8_t *type, uint32_t *sender, uint32_t *recip,
                             uint32_t *seq, uint8_t *plainOut, size_t *plainLen) {
    if (len < MIN_FRAME || len > MAX_FRAME) return false;
    if (buf[0] != MAGIC_VER) return false;

    const size_t authLen = len - TAG_LEN;
    const size_t payloadLen = authLen - HDR_LEN;
    if (payloadLen > MAX_PAYLOAD) return false;

    uint8_t macKey[16], encKey[16];
    deriveKeys(psk, macKey, encKey);
    uint8_t full[16];
    LoRaWANCrypto::aesCmac(macKey, buf, authLen, full);
    if (!ctEqual(full, buf + authLen, TAG_LEN)) return false;   // bad tag → reject

    *type   = buf[1];
    *sender = get32le(buf + 2);
    *recip  = get32le(buf + 6);
    *seq    = get32le(buf + 10);
    if (payloadLen) {
        memcpy(plainOut, buf + HDR_LEN, payloadLen);
        LoRaWANCrypto::cryptFrmPayload(encKey, *sender, *seq, plainOut, payloadLen);
    }
    *plainLen = payloadLen;
    return true;
}

// Self-test the CamC2 layer itself: a build->verify round-trip recovers the
// plaintext, and a one-bit tag flip is rejected. Validates key derivation,
// encrypt-then-MAC, and the constant-time compare on top of LoRaWANCrypto's KATs.
static bool camSelfTest() {
    uint8_t psk[16]; for (int i = 0; i < 16; i++) psk[i] = (uint8_t)(i + 1);
    const uint8_t pt[3] = { CMD_GET_STATUS, 0xAA, 0xBB };
    uint8_t frame[MAX_FRAME];
    size_t n = buildFrame(T_CMD, 0x11223344u, 0x55667788u, 7, psk, pt, sizeof(pt),
                          frame, sizeof(frame));
    if (n == 0) { Serial.println("[CamC2-selftest] build : FAIL"); return false; }

    uint8_t ty; uint32_t sn, rc, sq; uint8_t plain[MAX_PAYLOAD]; size_t pl;
    bool good = verifyAndDecrypt(frame, n, psk, &ty, &sn, &rc, &sq, plain, &pl);
    good = good && ty == T_CMD && sn == 0x11223344u && rc == 0x55667788u && sq == 7
                && pl == sizeof(pt) && memcmp(plain, pt, sizeof(pt)) == 0;
    Serial.printf("[CamC2-selftest] roundtrip : %s\n", good ? "PASS" : "FAIL");

    frame[n - 1] ^= 0x01;                                       // flip one tag bit
    bool rejected = !verifyAndDecrypt(frame, n, psk, &ty, &sn, &rc, &sq, plain, &pl);
    Serial.printf("[CamC2-selftest] tamper-reject : %s\n", rejected ? "PASS" : "FAIL");

    return good && rejected;
}

void begin(EmitFn emit) {
    s_emit = emit;
    CamC2Config::begin();
    bool kat = LoRaWANCrypto::selfTest();   // primitives (aesEcb/cmac/cryptFrmPayload)
    bool cam = camSelfTest();               // the CamC2 frame layer
    s_ok = kat && cam;
    Serial.printf("[CamC2] crypto self-test %s; ready=%d peers=%d emit=%s\n",
                  s_ok ? "PASS" : "FAIL", s_ok ? 1 : 0,
                  CamC2Config::anyConfigured() ? 1 : 0, s_emit ? "wired" : "null");
}

bool ready() { return s_ok; }

// --- shared inbound auth path ----------------------------------------------
// Resolve the peer by senderId, enforce addressing, verify the MAC under the
// peer's key, then the replay counter. Returns true (and fills the decoded
// fields) only for a fully-trusted frame. Logs a drop reason on rejection.
static bool authInbound(int radioIdx, const uint8_t *buf, size_t len,
                        uint8_t *type, uint32_t *sender, uint32_t *recip,
                        uint32_t *seq, uint8_t *plain, size_t *plainLen) {
    if (len < MIN_FRAME || len > MAX_FRAME) return false;
    if (buf[0] != MAGIC_VER) return false;           // not a C2 frame — let normal Custom handling run

    uint32_t s = get32le(buf + 2);
    uint32_t r = get32le(buf + 6);
    uint32_t myId = myC2Id();
    if (r != 0 && r != myId) return false;           // addressed to someone else — ignore quietly

    int idx;
    const CamC2Config::Peer *pr = CamC2Config::resolve(s, idx);
    if (!pr) {
        // Non-evt prefix: a non-C2 Custom frame (or RF flooder) that happens to start
        // with 0xC2 must not be able to inject lines into the machine-parseable evt=
        // stream. (adversarial review, protocol/do-no-harm lens)
        SerialLog::logf("[CamC2] drop radio=%d from=0x%08lx reason=not-whitelisted\n",
                        radioIdx, (unsigned long)s);
        return false;
    }
    if (!verifyAndDecrypt(buf, len, pr->psk, type, sender, recip, seq, plain, plainLen)) {
        SerialLog::logf("[CamC2] drop radio=%d from=0x%08lx reason=bad-mic\n",
                        radioIdx, (unsigned long)s);
        return false;
    }
    if (!CamC2Config::acceptInboundSeq(idx, *seq)) { // replay / stale / persist-fail
        SerialLog::logf("evt=C2DROP radio=%d from=0x%08lx seq=%lu reason=replay\n",
                        radioIdx, (unsigned long)s, (unsigned long)*seq);
        return false;
    }
    return true;
}

bool handleInbound(int radioIdx, const uint8_t *buf, size_t len) {
    if (!s_ok) return false;                          // crypto self-test failed → refuse to act

    uint8_t type; uint32_t sender, recip, seq; uint8_t plain[MAX_PAYLOAD]; size_t plen;
    if (!authInbound(radioIdx, buf, len, &type, &sender, &recip, &seq, plain, &plen))
        return false;

#if defined(BRIDGE_ROLE_CAMERA)
    if (type == T_CMD) {
        uint8_t cmd = (plen >= 1) ? plain[0] : CMD_NONE;
        const uint8_t *args = (plen > 1) ? plain + 1 : nullptr;
        size_t argLen = (plen > 1) ? plen - 1 : 0;
        uint8_t ack[MAX_PAYLOAD]; size_t ackLen = 0;
        uint8_t res = executeCommand(cmd, args, argLen, ack, sizeof(ack), ackLen);
        SerialLog::logf("evt=C2CMD radio=%d from=0x%08lx cmd=%u seq=%lu res=%u\n",
                        radioIdx, (unsigned long)sender, (unsigned)cmd,
                        (unsigned long)seq, (unsigned)res);
        // ACK is unicast back to the commander, signed under the same peer key.
        int idx; const CamC2Config::Peer *pr = CamC2Config::resolve(sender, idx);
        if (pr && s_emit) {
            uint32_t myId = myC2Id();
            uint32_t txSeq = CamC2Config::nextTxSeq();
            if (txSeq != CamC2Config::SEQ_INVALID) {
                uint8_t frame[MAX_FRAME];
                size_t n = buildFrame(T_ACK, myId, sender, txSeq, pr->psk, ack, ackLen,
                                      frame, sizeof(frame));
                if (n) s_emit(radioIdx, frame, n);
            }
        }
        return true;
    }
#endif

#if defined(BRIDGE_CAM_COMMANDER)
    if (type == T_ACK || type == T_BEACON || type == T_EVT) {
        SerialLog::logf("evt=C2RX radio=%d from=0x%08lx type=%u seq=%lu len=%u\n",
                        radioIdx, (unsigned long)sender, (unsigned)type,
                        (unsigned long)seq, (unsigned)plen);
        // Phase 3: surface ACK result / beacon status to the repeater portal.
        return true;
    }
#endif

    return true;   // a valid C2 frame, but no handler in this role — consumed, do not re-route
}

#if defined(BRIDGE_ROLE_CAMERA)

// Pack this device's status into out[]: battery%, sdFree%, uptimeSec(LE), lastCmd,
// lastResult. Phase 1 = stub values (no battery ADC / SD yet — wired in Phase 2).
static size_t writeStatus(uint8_t *out, size_t cap) {
    if (cap < 8) return 0;
    uint32_t uptime = (uint32_t)(millis() / 1000UL);
    out[0] = 100;                          // battery %  (Phase 2: real ADC)
    out[1] = 100;                          // SD free %  (Phase 2: real SD)
    put32le(out + 2, uptime);
    out[6] = s_lastCmd;
    out[7] = s_lastRes;
    return 8;
}

uint8_t executeCommand(uint8_t cmd, const uint8_t *args, size_t argLen,
                       uint8_t *ackOut, size_t ackCap, size_t &ackLen) {
    // Total-safe for any caller (the LoRa path passes ackCap=64; a future local
    // portal caller could pass a tight buffer). Guard so `ackCap - 1` below never
    // wraps a size_t to SIZE_MAX. (adversarial review, memory-safety lens)
    if (ackCap < 1) { ackLen = 0; return RES_ERR; }
    uint8_t res = RES_OK;
    size_t extra = 0;                      // bytes written after ackOut[0]

    switch (cmd) {
        case CMD_GET_STATUS:
            extra = writeStatus(ackOut + 1, ackCap - 1);
            break;
        case CMD_START_CAPTURE: {
            char fn[40]; uint16_t w = 0, h = 0;
            size_t n = CameraNode::ready() ? CameraNode::snap(fn, sizeof(fn), &w, &h) : 0;
            if (n == 0) { res = RES_NOCAM; break; }   // no camera / capture failed
            size_t fl = strlen(fn);
            if (1 + fl <= ackCap) { memcpy(ackOut + 1, fn, fl); extra = fl; }
            SerialLog::logf("[CamC2] CAPTURE %ux%u %uB -> %s\n",
                            (unsigned)w, (unsigned)h, (unsigned)n, fn);
            break;
        }
        case CMD_RECORD: {
            uint16_t dur = (argLen >= 2) ? (uint16_t)(args[0] | (args[1] << 8)) : 0;
            const char *fn = "stub.avi";
            size_t fl = strlen(fn);
            if (fl + 1 <= ackCap) { memcpy(ackOut + 1, fn, fl); extra = fl; }
            SerialLog::logf("[CamC2] (stub) RECORD %us -> %s\n", (unsigned)dur, fn);
            break;
        }
        case CMD_STOP:
            SerialLog::logf("[CamC2] (stub) STOP\n");
            break;
        case CMD_SET_QUALITY:
        case CMD_SET_FRAMESIZE:
            if (argLen < 1) res = RES_BADARG;
            else SerialLog::logf("[CamC2] (stub) set cmd=%u val=%u\n",
                                 (unsigned)cmd, (unsigned)args[0]);
            break;
        default:
            res = RES_BADARG;
            break;
    }

    ackOut[0] = res;
    ackLen = 1 + extra;
    s_lastCmd = cmd;
    s_lastRes = res;
    return res;
}

void emitBeacon(int radioIdx) {
    if (!s_ok || !s_emit) return;
    uint8_t status[8]; size_t sl = writeStatus(status, sizeof(status));
    uint32_t myId = myC2Id();

    // Unicast a signed beacon to each whitelisted peer (a master). A single
    // broadcast frame can't be MAC-verified by multiple per-peer keys.
    for (size_t i = 0; i < CamC2Config::peerCount(); i++) {
        const CamC2Config::Peer &pr = CamC2Config::peer((int)i);
        if (!pr.inUse || pr.peerId == 0) continue;
        uint32_t txSeq = CamC2Config::nextTxSeq();
        if (txSeq == CamC2Config::SEQ_INVALID) return;
        uint8_t frame[MAX_FRAME];
        size_t n = buildFrame(T_BEACON, myId, pr.peerId, txSeq, pr.psk, status, sl,
                              frame, sizeof(frame));
        if (n) s_emit(radioIdx, frame, n);
    }
}

#endif  // BRIDGE_ROLE_CAMERA

#if defined(BRIDGE_CAM_COMMANDER)

bool sendCommand(int radioIdx, uint32_t camId, uint8_t cmd,
                 const uint8_t *args, size_t argLen) {
    if (!s_ok || !s_emit) return false;
    int idx;
    const CamC2Config::Peer *pr = CamC2Config::resolve(camId, idx);
    if (!pr) {
        SerialLog::logf("evt=C2DROP reason=no-such-cam id=0x%08lx\n", (unsigned long)camId);
        return false;
    }
    if (1 + argLen > MAX_PAYLOAD) return false;

    uint32_t myId  = myC2Id();
    uint32_t txSeq = CamC2Config::nextTxSeq();
    if (txSeq == CamC2Config::SEQ_INVALID) return false;

    uint8_t payload[MAX_PAYLOAD];
    payload[0] = cmd;
    if (argLen) memcpy(payload + 1, args, argLen);

    uint8_t frame[MAX_FRAME];
    size_t n = buildFrame(T_CMD, myId, camId, txSeq, pr->psk, payload, 1 + argLen,
                          frame, sizeof(frame));
    if (!n) return false;
    SerialLog::logf("evt=C2TX radio=%d to=0x%08lx cmd=%u seq=%lu\n",
                    radioIdx, (unsigned long)camId, (unsigned)cmd, (unsigned long)txSeq);
    return s_emit(radioIdx, frame, n);
}

#endif  // BRIDGE_CAM_COMMANDER

}  // namespace CamC2

#endif  // BRIDGE_ROLE_CAMERA || BRIDGE_CAM_COMMANDER
