#pragma once

#include <Arduino.h>
#include <SPI.h>
#include <RadioLib.h>

// LoRa RF defaults — override any via platformio.ini build_flags
#ifndef LORA_FREQUENCY
  #define LORA_FREQUENCY     915.0f
#endif
#ifndef LORA_BANDWIDTH
  #define LORA_BANDWIDTH     125.0f
#endif
#ifndef LORA_SPREAD_FACTOR
  #define LORA_SPREAD_FACTOR 9
#endif
#ifndef LORA_CODING_RATE
  #define LORA_CODING_RATE   7
#endif
#ifndef LORA_TX_POWER
  #define LORA_TX_POWER      20
#endif
#ifndef LORA_SYNC_WORD
  #define LORA_SYNC_WORD     0x12
#endif
#ifndef LORA_PREAMBLE_LEN
  #define LORA_PREAMBLE_LEN  8
#endif
#ifndef LORA_TCXO_VOLTAGE
  #define LORA_TCXO_VOLTAGE  1.8f
#endif
#ifndef LORA_MAX_PACKET
  #define LORA_MAX_PACKET    256
#endif

class WioSX1262 {
public:
    WioSX1262(int nss, int dio1, int reset, int busy,
              int antSw, SPIClass &spi, SemaphoreHandle_t mutex,
              const char *name);
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

    void _setAnt(bool tx);
};
