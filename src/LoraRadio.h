// LoraRadio.h
// ---------------------------------------------------------------------------
// Abstract interface for a LoRa radio wrapper. Lets the bridge pipeline hold
// radios as LoraRadio* without knowing the underlying chip — WioSX1262 today,
// WioLR1121 in the future (see LR1121-SPEC.md). The bridge dispatcher is
// already RF-agnostic: it branches on the LoRa sync word, not the chip.
//
// All methods are also the surface a future WioLR1121 must implement, so the
// two radio slots can be any mix of supported chips.
// ---------------------------------------------------------------------------

#pragma once

#include <Arduino.h>

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
