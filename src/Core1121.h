// Core1121.h
// ---------------------------------------------------------------------------
// LoraRadio wrapper for the WaveShare Core1121-XF module (Semtech LR1121), on
// top of RadioLib's LR1121 class. Same surface as WioSX1262 so either chip can
// occupy a radio slot — see CORE1121.md.
//
// This driver replaces the earlier Seeed Wio-LR1121 driver (WioLR1121.*) on the
// CORE1121 branch. It is the same LR1121 silicon, so the chip-level init carries
// over verbatim; only the board-specific RF-switch table and TCXO reference
// differ. Those board facts are derived from the WaveShare-published schematic
// `Core1121_XF_Sch.pdf` (recorded in docs/datasheets/waveshare/CORE1121-RF-SWITCH.md),
// not guessed — see Core1121.cpp for the schematic-cited truth table.
//
// Core1121-XF board facts (schematic-verified):
//   - RF switch  : pSemi PE4259 SPDT on the sub-GHz path, driven by
//                  DIO5 (RFSW0_V1) + DIO6 (RFSW1_V2). RFC = LORA_ANT (ANT2).
//                  Truth table:  V1=1,V2=0 → RFC→RF2 (RFI_LF / LNA, RX);
//                                V1=0,V2=1 → RFC→RF1 (RFO_HP_LF / PA, TX).
//   - 2.4 GHz    : RFIO_HF routes to a SEPARATE antenna (ANT1) and bypasses the
//                  PE4259 entirely → the switch is held isolated for HF.
//   - TCXO       : 32 MHz, reference 3.0 V (LR1121_TCXO_VOLTAGE).
//   - DIO10/DIO11: carry the 32.768 kHz crystal (Y2) on this board — they are
//                  NOT RF-switch outputs and must stay RADIOLIB_NC.
//
// Differences from WioSX1262:
//   - No ANT_SW pin: the Core1121 module has an integrated RF switch the LR1121
//     drives itself via DIO5/DIO6 (configured by setRfSwitchTable).
//   - RadioLib's LR11x0 begin() takes no frequency; frequency is set
//     separately. `high` selects the >1 GHz path (2.4 GHz) and is derived
//     from the configured frequency.
// ---------------------------------------------------------------------------

#pragma once

#include <Arduino.h>
#include <SPI.h>
#include <RadioLib.h>
#include "LoraRadio.h"

class Core1121 : public LoraRadio {
public:
    // irq = the LR1121 DIO9 line (generic IRQ); busy = LR_BUSY pad.
    Core1121(int nss, int irq, int reset, int busy,
             SPIClass &spi, SemaphoreHandle_t mutex,
             const char *name, const LoraConfig &config);
    ~Core1121() override;

    bool    begin() override;
    bool    available() override;
    int16_t read(uint8_t *buf, size_t &len, float *rssi, float *snr) override;
    int16_t transmit(const uint8_t *buf, size_t len) override;
    void    startReceive() override;

    // Diagnostic: raw LR11x0 IRQ status register (getIrqFlags). Lets the
    // heartbeat / transmit log see whether the chip latched RX_DONE (bit3=0x08)
    // or TxDone even when the DIO9 edge ISR never fired. Mutex-protected SPI.
    uint32_t debugIrqStatus();

    // Written by the IRAM ISR trampoline; read by the polling task.
    volatile bool     _rxFlag   = false;
    // Diagnostic: number of times the DIO9 ISR has fired since boot. If this
    // counter never advances past 1, DIO9 is wedged HIGH and no further
    // rising edges are being delivered to the ESP32.
    volatile uint32_t _isrCount = 0;

private:
    Module           *_mod;
    LR1121           *_radio;
    SemaphoreHandle_t _mutex;
    int               _reset;   // NRESET pin — used for manual pre-init wait
    int               _busy;    // BUSY pin   — polled before SPI starts
    const char       *_name;
    LoraConfig        _config;
};
