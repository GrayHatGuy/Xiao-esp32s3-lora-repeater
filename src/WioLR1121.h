// WioLR1121.h
// ---------------------------------------------------------------------------
// LoraRadio wrapper for the Seeed Wio-LR1121 module (Semtech LR1121), on top
// of RadioLib's LR1121 class. Same surface as WioSX1262 so either chip can
// occupy a radio slot — see LR1121-SPEC.md.
//
// Differences from WioSX1262:
//   - No ANT_SW pin: the Wio-LR1121 module has an integrated RF switch.
//   - RadioLib's LR11x0 begin() takes no frequency; frequency is set
//     separately. `high` selects the >1 GHz path (2.4 GHz) and is derived
//     from the configured frequency.
// ---------------------------------------------------------------------------

#pragma once

#include <Arduino.h>
#include <SPI.h>
#include <RadioLib.h>
#include "LoraRadio.h"

class WioLR1121 : public LoraRadio {
public:
    // irq = the LR1121 DIO9 line (generic IRQ); busy = LR_BUSY pad.
    WioLR1121(int nss, int irq, int reset, int busy,
              SPIClass &spi, SemaphoreHandle_t mutex,
              const char *name, const LoraConfig &config);
    ~WioLR1121() override;

    bool    begin() override;
    bool    available() override;
    int16_t read(uint8_t *buf, size_t &len, float *rssi, float *snr) override;
    int16_t transmit(const uint8_t *buf, size_t len) override;
    void    startReceive() override;

    // Written by the IRAM ISR trampoline; read by the polling task.
    volatile bool _rxFlag = false;

private:
    Module           *_mod;
    LR1121           *_radio;
    SemaphoreHandle_t _mutex;
    int               _reset;   // NRESET pin — used for manual pre-init wait
    int               _busy;    // BUSY pin   — polled before SPI starts
    const char       *_name;
    LoraConfig        _config;
};
