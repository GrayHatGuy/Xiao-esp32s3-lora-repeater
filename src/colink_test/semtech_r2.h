// semtech_r2.h — Path B M0: plain-C++ facade over the vendored Semtech
// lr11xx_driver for R2 (the Core1121/LR1121). The RadioLib-using translation
// unit (main.cpp) includes ONLY this header, never the Semtech headers — so the
// two driver stacks live in SEPARATE translation units and the LINK is the
// co-link proof. This mirrors the real Path B split (SemtechLR1121Hal.cpp).
//
// All SPI access is serialised by the shared FreeRTOS mutex passed to r2_begin().
#pragma once
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

// Reset + wake the LR1121 and read its version over the shared SPI bus.
// Returns true if the chip answered. Captures the firmware version (see r2_fw()).
bool r2_begin(SemaphoreHandle_t spiMutex);

// Firmware version captured at r2_begin() (used as the "did the answer stay sane"
// reference during the concurrency test).
uint16_t r2_fw();

// Re-read the version over SPI (mutex-guarded). True iff the chip answered AND
// the firmware version matches expectFw (i.e. the bus wasn't corrupted by R1).
bool r2_ping(uint16_t expectFw);
