// LoraRadio.h
// ---------------------------------------------------------------------------
// Abstract interface for a LoRa radio wrapper, plus the shared LoraConfig
// struct and the compile-time board constants. Lets the bridge pipeline hold
// radios as LoraRadio* without knowing the underlying chip — WioSX1262 and
// WioLR1121 both implement this. The bridge dispatcher is RF-agnostic: it
// branches on the LoRa sync word, not the chip.
// ---------------------------------------------------------------------------

#pragma once

#include <Arduino.h>

// Compile-time RF constants — board facts, not user settings.
#ifndef LORA_PREAMBLE_LEN
  #define LORA_PREAMBLE_LEN   8        // symbols — universal
#endif
#ifndef LORA_TCXO_VOLTAGE
  #define LORA_TCXO_VOLTAGE   1.8f     // Wio SX1262 module TCXO
#endif
#ifndef LR1121_TCXO_VOLTAGE
  #define LR1121_TCXO_VOLTAGE 3.0f     // Wio-LR1121 module TCXO (bench-verify)
#endif
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
    virtual int16_t transmit(const uint8_t *buf, size_t len) = 0;

    // Put the radio back into continuous receive mode.
    virtual void startReceive() = 0;
};
