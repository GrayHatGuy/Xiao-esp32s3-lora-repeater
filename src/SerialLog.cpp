// SerialLog.cpp — see SerialLog.h.

#include "SerialLog.h"
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <stdarg.h>
#include <stdio.h>

namespace SerialLog {

// Recursive so a task holding lock() (e.g. around the decoder dump) can still
// call logf() without self-deadlocking.
static SemaphoreHandle_t s_mux = nullptr;

void begin() {
    if (!s_mux) s_mux = xSemaphoreCreateRecursiveMutex();
}

void lock() {
    if (s_mux) xSemaphoreTakeRecursive(s_mux, portMAX_DELAY);
}

void unlock() {
    if (s_mux) xSemaphoreGiveRecursive(s_mux);
}

void logf(const char *fmt, ...) {
    char line[300];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    lock();
    Serial.print(line);
    unlock();
}

}  // namespace SerialLog
