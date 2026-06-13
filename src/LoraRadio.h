// LoraRadio.h
// ---------------------------------------------------------------------------
// Abstract interface for a LoRa radio wrapper, plus the shared LoraConfig
// struct and the compile-time board constants. Lets the bridge pipeline hold
// radios as LoraRadio* without knowing the underlying chip — WioSX1262 (and,
// on the quad line, WioLR1121/RemoteRadio) implement this. The bridge
// dispatcher is RF-agnostic: it branches on the LoRa sync word, not the chip.
// ---------------------------------------------------------------------------

#pragma once

#include <Arduino.h>
#include <RadioLib.h>   // RADIOLIB_* status codes used by the CSMA defaults below

// Compile-time RF constants — board facts, not user settings.
#ifndef LORA_PREAMBLE_LEN
  #define LORA_PREAMBLE_LEN   8        // symbols — universal
#endif
#ifndef LORA_TCXO_VOLTAGE
  #define LORA_TCXO_VOLTAGE   1.8f     // Wio SX1262 module TCXO
#endif
#ifndef LR1121_TCXO_VOLTAGE
  #define LR1121_TCXO_VOLTAGE 3.0f     // inert here (SX1262-only build); kept
#endif                                 // for parity with the quad line
#ifndef LORA_MAX_PACKET
  #define LORA_MAX_PACKET     256
#endif

// Resolved RF plan for one radio. Built at runtime from BridgeConfig.
struct LoraConfig {
    float    frequency;     // MHz
    float    bandwidth;     // kHz
    uint8_t  spreadFactor;
    uint8_t  codingRate;
    uint8_t  syncWord;
    int8_t   txPower;       // dBm
    uint16_t preambleLen;
    float    tcxoVoltage;   // V — chip-dependent
};

class LoraRadio {
public:
    virtual ~LoraRadio() {}

    // Initialise the radio hardware. Returns true on success.
    virtual bool begin() = 0;

    // True when a packet has been received and is waiting to be read.
    virtual bool available() = 0;

    // Read the pending packet into buf. `len` is the buffer capacity on
    // entry and the byte count read on return. Fills rssi/snr when non-null.
    // Returns a RadioLib status code.
    virtual int16_t read(uint8_t *buf, size_t &len, float *rssi, float *snr) = 0;

    // Transmit `len` bytes; the radio auto-returns to RX afterwards.
    // Blocking — holds the radio for the whole on-air time. The RX-priority
    // pipeline (ROUTING-REDESIGN.md / V8.2-SPEC.md) uses the non-blocking trio
    // below instead; this remains for any caller that wants the simple
    // synchronous path.
    virtual int16_t transmit(const uint8_t *buf, size_t len) = 0;

    // Put the radio back into continuous receive mode.
    virtual void startReceive() = 0;

    // ---- RX-priority CSMA extensions (V8.2-SPEC.md §3.3/§4) ----------------
    // Default implementations fall back to the legacy blocking path so radio
    // wrappers that don't override them keep working unchanged. WioSX1262
    // (R1/R2) provides real implementations.

    // Channel Activity Detection (listen-before-talk). Returns
    // RADIOLIB_CHANNEL_FREE when it is clear to transmit, RADIOLIB_LORA_DETECTED
    // when the channel is busy, or another RADIOLIB_* status on error. The
    // scheduler treats anything that is not LORA_DETECTED as "go" (fail open),
    // so a radio without CAD support (this default) simply always transmits.
    virtual int16_t scanChannel() { return RADIOLIB_CHANNEL_FREE; }

    // Begin a non-blocking transmit. On RADIOLIB_ERR_NONE the caller must poll
    // txDone() and then call finishTransmit(). Default: blocking transmit().
    virtual int16_t startTransmit(const uint8_t *buf, size_t len) {
        return transmit(buf, len);
    }

    // True once a non-blocking transmit started by startTransmit() has
    // completed. Default (blocking fallback already finished): always true.
    virtual bool txDone() { return true; }

    // Clean up after a non-blocking transmit and return the radio to RX.
    // Default: just re-arm RX.
    virtual void finishTransmit() { startReceive(); }
};
