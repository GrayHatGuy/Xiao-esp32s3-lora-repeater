#pragma once

#include <Arduino.h>
#include <SPI.h>
#include <RadioLib.h>

// Compile-time RF constants — board facts, not user settings (v8 spec §6).
// The per-radio RF plan (frequency, bandwidth, SF, CR, sync word, TX power)
// is resolved at runtime from BridgeConfig; there is deliberately NO hard
// fallback chain for those values — an unconfigured radio resolves to null
// and forces the captive portal rather than guessing 915.0f / SF9 / 0x12.
#ifndef LORA_PREAMBLE_LEN
  #define LORA_PREAMBLE_LEN  8        // symbols — universal
#endif
#ifndef LORA_TCXO_VOLTAGE
  #define LORA_TCXO_VOLTAGE  1.8f     // Wio SX1262 hardware property
#endif
#ifndef LORA_MAX_PACKET
  #define LORA_MAX_PACKET    256
#endif

struct LoraConfig {
    float    frequency;
    float    bandwidth;
    uint8_t  spreadFactor;
    uint8_t  codingRate;
    uint8_t  syncWord;
    int8_t   txPower;
    uint16_t preambleLen;
    float    tcxoVoltage;
};

class WioSX1262 {
public:
    WioSX1262(int nss, int dio1, int reset, int busy,
              int antSw, SPIClass &spi, SemaphoreHandle_t mutex,
              const char *name, const LoraConfig &config);
    ~WioSX1262();

    bool    begin();
    bool    available();
    int16_t read(uint8_t *buf, size_t &len, float *rssi, float *snr);
    int16_t transmit(const uint8_t *buf, size_t len);
    void    startReceive();

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
