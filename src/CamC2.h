// CamC2.h
// ---------------------------------------------------------------------------
// LoRaCam command-and-control codec — the encrypted, sender-whitelisted,
// replay-protected C2 channel that rides a PROTO_CUSTOM radio (sync 0x33).
// One module, two roles (flag-gated):
//   - RESPONDER  (BRIDGE_ROLE_CAMERA):  RX CMD -> auth -> executeCommand -> ACK; beacons.
//   - COMMANDER  (BRIDGE_CAM_COMMANDER): TX signed CMD; RX ACK/telemetry.
//
// Wire frame (see LORACAM-SPEC §5):
//   [ver:1=0xC2][type:1][senderId:4 LE][recipientId:4 LE][seq:4 LE][ciphertext:var][cmac:8]
//   - senderId    = transmitter's C2 id (== BridgeConfig::mtNodeId()); the receiver
//                   looks up the peer + PSK by this.
//   - recipientId = target id; 0 = broadcast. In the MAC'd region (tamper-proof).
//   - ciphertext  = AES-CTR(encKey, nonce=senderId||seq) of the payload.
//   - cmac        = AES-CMAC(macKey, bytes[0..end-of-ciphertext)) truncated to 8.
//   macKey/encKey are DERIVED from the 16-byte peer PSK (domain separation — the
//   PSK is never used directly for both MAC and cipher).
//
// Security construction: encrypt-then-MAC; a valid 8-byte CMAC both authenticates
// the sender (only a whitelisted PSK-holder forges it) and integrity-protects the
// frame. Replay is gated by CamC2Config::acceptInboundSeq() AFTER the MAC verifies.
// All actuation is fail-closed behind a boot crypto self-test (ready()).
// ---------------------------------------------------------------------------

#pragma once
#if defined(BRIDGE_ROLE_CAMERA) || defined(BRIDGE_CAM_COMMANDER)

#include <Arduino.h>
#include <stdint.h>

namespace CamC2 {

constexpr uint8_t MAGIC_VER   = 0xC2;   // LoRaCam C2 magic + version 1
constexpr size_t  HDR_LEN     = 14;     // ver1 + type1 + senderId4 + recipientId4 + seq4
constexpr size_t  TAG_LEN     = 8;      // truncated AES-CMAC
constexpr size_t  MAX_PAYLOAD = 64;     // plaintext payload cap (keeps frames small)
constexpr size_t  MIN_FRAME   = HDR_LEN + TAG_LEN;               // 22
constexpr size_t  MAX_FRAME   = HDR_LEN + MAX_PAYLOAD + TAG_LEN; // 86

enum Type : uint8_t { T_CMD = 1, T_ACK = 2, T_EVT = 3, T_BEACON = 4 };

enum Cmd : uint8_t {
    CMD_NONE = 0, CMD_START_CAPTURE = 1, CMD_RECORD = 2, CMD_STOP = 3,
    CMD_SET_QUALITY = 4, CMD_SET_FRAMESIZE = 5, CMD_GET_STATUS = 6,
};

enum Result : uint8_t {
    RES_OK = 0, RES_ERR = 1, RES_BUSY = 2, RES_BADARG = 3, RES_NOCAM = 4,
    // (prefixed RES_* — bare R_OK/W_OK/X_OK/F_OK are POSIX access() macros via <Arduino.h>)
};

// Outbound emit seam — provided by main.cpp (records dedup + g_routeQ[radioIdx].push()).
// Decouples CamC2 from main.cpp's statics; null until begin() wires it (TX no-op).
typedef bool (*EmitFn)(int radioIdx, const uint8_t *buf, size_t len);

void begin(EmitFn emit);   // CamC2Config::begin() + crypto self-test (latches ready())
bool ready();              // crypto self-test passed — fail-closed gate for all actuation

// Try to consume a raw Custom-radio frame as a C2 frame. Returns true iff it was a
// valid, authenticated, in-whitelist, non-replay C2 frame addressed to us (handled),
// so the caller stops normal Custom handling.
bool handleInbound(int radioIdx, const uint8_t *buf, size_t len);

#if defined(BRIDGE_ROLE_CAMERA)
// Shared actuation point: the local portal calls this DIRECTLY (local trust); the
// LoRa path calls it ONLY after auth/whitelist/replay. Fills the ACK payload
// (ackOut[0] = Result, then optional filename/status). Returns the Result code.
// (Camera/SD are wired in Phase 2; Phase 1 = logged stubs.)
uint8_t executeCommand(uint8_t cmd, const uint8_t *args, size_t argLen,
                       uint8_t *ackOut, size_t ackCap, size_t &ackLen);
// Emit an unsolicited status beacon to each whitelisted master (unicast + signed
// under that master's key — a single broadcast frame can't be MAC-verified by
// multiple per-master keys without a shared fleet key; see LORACAM-SPEC §1A note).
void emitBeacon(int radioIdx);
#endif

#if defined(BRIDGE_CAM_COMMANDER)
// Build + queue a signed CMD to a cam peer (by its C2 id). Returns true if queued.
bool sendCommand(int radioIdx, uint32_t camId, uint8_t cmd,
                 const uint8_t *args, size_t argLen);
#endif

}  // namespace CamC2

#endif  // BRIDGE_ROLE_CAMERA || BRIDGE_CAM_COMMANDER
