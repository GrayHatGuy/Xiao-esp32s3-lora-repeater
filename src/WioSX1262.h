#pragma once

#include <Arduino.h>
#include <SPI.h>
#include <RadioLib.h>
#include "LoraRadio.h"

// LoraConfig + the compile-time board constants now live in LoraRadio.h,
// shared by every radio wrapper.

class WioSX1262 : public LoraRadio {
public:
    WioSX1262(int nss, int dio1, int reset, int busy,
              int antSw, SPIClass &spi, SemaphoreHandle_t mutex,
              const char *name, const LoraConfig &config);
    ~WioSX1262() override;

    bool    begin() override;
    bool    available() override;
    int16_t read(uint8_t *buf, size_t &len, float *rssi, float *snr) override;
    int16_t transmit(const uint8_t *buf, size_t len) override;
    void    startReceive() override;

    // Written by the IRAM ISR trampoline; read by the polling task.
    volatile bool _rxFlag = false;

private:
    Module           *_mod;
    SX1262           *_radio;
    SemaphoreHandle_t _mutex;
    int               _antSw;
    const char       *_name;
    LoraConfig        _config;

    void _setAnt(bool tx);
};
