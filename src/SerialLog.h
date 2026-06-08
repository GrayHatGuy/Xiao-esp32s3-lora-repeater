// SerialLog.h
// ---------------------------------------------------------------------------
// One recursive FreeRTOS mutex serialises ALL diagnostic output to the USB-CDC
// Serial device. The bridge runs two radio tasks pinned to separate cores
// (main.cpp: radio1Task on core 0, radio2Task on core 1), and both they and the
// radio drivers (Core1121 / WioSX1262) print from their own core. Without a
// shared lock those writes byte-interleave on the single Serial endpoint and
// the captured logs garble — which corrupts the per-read `freqErr=` and
// `[Rn RX]` lines the LR1121 RX-offset investigation depends on (CLAUDE.md §0).
//
// Usage:
//   - serialLogBegin()   once in setup(), right after Serial.begin() and BEFORE
//                        the radio tasks are spawned.
//   - logf("...", ...)   drop-in replacement for a single Serial.printf() line:
//                        formats and writes the whole line atomically under the
//                        lock. Truncation-safe (long lines spill to the heap).
//   - SerialLogGuard g;  hold the lock across a multi-call burst (e.g. a decoder
//                        dump that mixes printf/write/println) so the whole
//                        burst stays contiguous. The mutex is recursive, so
//                        logf() calls nested inside a guarded region are safe.
//
// Lock order: the driver debug prints are emitted while the shared SPI mutex is
// held, so the acquisition order is always spiMutex -> serialMutex. Nothing
// takes the SPI mutex while holding the serial mutex, so the pair cannot
// deadlock.
//
// Not covered: RadioLib's own RADIOLIB_DEBUG_BASIC spew bypasses this lock
// (it writes straight to the debug port from inside RadioLib). It is still
// serialised against itself by the SPI mutex, but can interleave with our
// task-level logf() lines. For fully clean captures, drop -DRADIOLIB_DEBUG_BASIC
// in platformio.ini once bring-up is done.
// ---------------------------------------------------------------------------

#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

// Created by serialLogBegin(). NULL until then — callers fall back to an
// unlocked write so very-early-boot logging (before the mutex exists) still
// appears.
extern SemaphoreHandle_t g_serialMutex;

// Create the serial mutex. Idempotent. Call once in setup() before the radio
// tasks start.
void serialLogBegin();

// Locked, truncation-safe printf to Serial. Same format semantics as
// Serial.printf(). Use for any single-line log emitted from a radio task or a
// radio driver.
void logf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

// RAII helper: hold the serial mutex for the lifetime of the object so a
// sequence of Serial.* / logf() calls is emitted as one contiguous block.
// Recursive, so logf() (and nested guards) inside the region are safe.
struct SerialLogGuard {
    SerialLogGuard() {
        if (g_serialMutex) xSemaphoreTakeRecursive(g_serialMutex, portMAX_DELAY);
    }
    ~SerialLogGuard() {
        if (g_serialMutex) xSemaphoreGiveRecursive(g_serialMutex);
    }
    SerialLogGuard(const SerialLogGuard &) = delete;             // owns a lock
    SerialLogGuard &operator=(const SerialLogGuard &) = delete;
};
