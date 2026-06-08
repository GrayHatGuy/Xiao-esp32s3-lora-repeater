// SerialLog.cpp — see SerialLog.h.
#include "SerialLog.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

SemaphoreHandle_t g_serialMutex = NULL;

void serialLogBegin() {
    if (g_serialMutex == NULL) {
        g_serialMutex = xSemaphoreCreateRecursiveMutex();
        configASSERT(g_serialMutex != NULL);
    }
}

void logf(const char *fmt, ...) {
    const bool locked = (g_serialMutex != NULL);
    if (locked) xSemaphoreTakeRecursive(g_serialMutex, portMAX_DELAY);

    // Format into a stack buffer first; spill to the heap only for the rare
    // line that exceeds it (bridged text, base64 dumps) so nothing truncates.
    char    stackBuf[256];
    va_list ap;
    va_start(ap, fmt);
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(stackBuf, sizeof(stackBuf), fmt, ap);
    va_end(ap);

    if (n < 0) {
        // formatting error — emit nothing
    } else if (n < (int)sizeof(stackBuf)) {
        Serial.write((const uint8_t *)stackBuf, (size_t)n);
    } else {
        char *heapBuf = (char *)malloc((size_t)n + 1);
        if (heapBuf) {
            vsnprintf(heapBuf, (size_t)n + 1, fmt, ap2);
            Serial.write((const uint8_t *)heapBuf, (size_t)n);
            free(heapBuf);
        } else {
            Serial.write((const uint8_t *)stackBuf, sizeof(stackBuf) - 1);
        }
    }
    va_end(ap2);

    if (locked) xSemaphoreGiveRecursive(g_serialMutex);
}
