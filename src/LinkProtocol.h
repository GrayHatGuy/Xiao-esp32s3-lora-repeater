// LinkProtocol.h
// ---------------------------------------------------------------------------
// Wire protocol for the XIAO host <-> T-Lora-Dual co-processor UART link
// (QUAD-SPEC §"Inter-board UART link"). Shared by BOTH firmwares — include
// this identical header on the host and on the co-processor so the two ends
// never drift.
//
// Frame:
//   [0] 0xAA  [1] 0x55      preamble
//   [2] type               LinkProtocol::Type
//   [3] radio              co-proc-local radio index (0=R3, 1=R4) or RADIO_NONE
//   [4] len_lo [5] len_hi  payload length, little-endian
//   [6..6+len-1] payload
//   [..] crc_lo crc_hi     CRC-16/CCITT over bytes [2 .. 6+len-1] (type..payload)
//
// Endianness: both ends are little-endian ESP32, so the packed payload structs
// (floats, uint16) are transferred byte-for-byte without marshalling.
// ---------------------------------------------------------------------------

#pragma once

#include <stdint.h>
#include <stddef.h>
#include <string.h>

namespace LinkProtocol {

constexpr uint8_t PRE0 = 0xAA;
constexpr uint8_t PRE1 = 0x55;

constexpr size_t HEADER_LEN  = 6;     // pre0, pre1, type, radio, len_lo, len_hi
constexpr size_t CRC_LEN     = 2;
constexpr size_t MAX_PAYLOAD = 300;   // 256 LoRa bytes + RxHeader(8) + margin
constexpr size_t MAX_FRAME   = HEADER_LEN + MAX_PAYLOAD + CRC_LEN;

constexpr uint8_t RADIO_NONE = 0xFF;

// Message types. High bit set = co-processor -> host.
enum Type : uint8_t {
    // host -> co-processor
    MSG_CFG_RADIO = 0x01,   // payload = CfgRadio
    MSG_TX        = 0x02,   // payload = raw LoRa bytes to transmit
    MSG_START_RX  = 0x03,   // no payload
    MSG_PING      = 0x04,   // no payload
    MSG_RESET     = 0x05,   // no payload
    // co-processor -> host
    MSG_RX        = 0x81,   // payload = RxHeader + raw LoRa bytes
    MSG_TX_DONE   = 0x82,   // payload = int16 status
    MSG_READY     = 0x83,   // payload = version/cap info (free-form)
    MSG_LOG       = 0x84,   // payload = ASCII (not necessarily null-terminated)
    MSG_PONG      = 0x85,   // payload = uint32 uptime ms (optional)
};

// Radio band. Values match BridgeConfig::Band so the host can pass them
// straight through. S-band is accepted but the co-processor refuses to key up.
enum Band : uint8_t {
    BAND_SUBGHZ = 0,
    BAND_2G4    = 1,
    BAND_SBAND  = 2,
};

// CFG_RADIO payload — one radio's full plan. Packed; little-endian.
struct __attribute__((packed)) CfgRadio {
    uint8_t  enable;       // 1 = configure + begin, 0 = leave disabled
    uint8_t  band;         // LinkProtocol::Band
    float    frequency;    // MHz
    float    bandwidth;    // kHz
    uint8_t  sf;           // spreading factor 5..12
    uint8_t  cr;           // coding rate 5..8
    uint8_t  syncWord;     // LoRa sync word
    int8_t   txPower;      // dBm
    uint16_t preambleLen;  // symbols
};

// RX payload prefix — followed immediately by the raw LoRa bytes. Packed.
struct __attribute__((packed)) RxHeader {
    float rssi;   // dBm
    float snr;    // dB
};

// CRC-16/CCITT (poly 0x1021, init 0xFFFF). Self-contained so the co-processor
// project — which has no MeshDecoderDebug — shares the exact implementation.
static inline uint16_t crc16(const uint8_t *d, size_t n) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < n; i++) {
        crc ^= (uint16_t)d[i] << 8;
        for (int b = 0; b < 8; b++)
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021)
                                 : (uint16_t)(crc << 1);
    }
    return crc;
}

// Build a complete frame into `out`. Returns the frame length, or 0 if the
// payload is too large or the buffer is too small.
static inline size_t buildFrame(uint8_t *out, size_t outCap, uint8_t type,
                                uint8_t radio, const uint8_t *payload, size_t len) {
    if (len > MAX_PAYLOAD) return 0;
    size_t total = HEADER_LEN + len + CRC_LEN;
    if (total > outCap) return 0;
    out[0] = PRE0;
    out[1] = PRE1;
    out[2] = type;
    out[3] = radio;
    out[4] = (uint8_t)(len & 0xFF);
    out[5] = (uint8_t)((len >> 8) & 0xFF);
    if (payload && len) memcpy(out + HEADER_LEN, payload, len);
    uint16_t crc = crc16(out + 2, (HEADER_LEN - 2) + len);   // type..payload
    out[HEADER_LEN + len]     = (uint8_t)(crc & 0xFF);
    out[HEADER_LEN + len + 1] = (uint8_t)((crc >> 8) & 0xFF);
    return total;
}

}  // namespace LinkProtocol
