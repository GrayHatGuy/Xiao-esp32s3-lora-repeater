// main.cpp — Path B Milestone 0: RadioLib + Semtech co-link smoke test.
//
// Proves the gating unknown for Path B: that RadioLib's SX126x (R1 = Wio-SX1262)
// and Semtech's vendored lr11xx_driver (R2 = Core1121/LR1121) build, LINK, and
// share ONE SPI bus + ONE FreeRTOS mutex in a single binary — BEFORE any real
// porting. Two proofs:
//   (1) STATIC  — the env links with no symbol/HAL conflict (verified by `pio run`;
//                 this TU uses RadioLib, semtech_r2.cpp uses the Semtech driver,
//                 and they link into one image — the real Path B split).
//   (2) RUNTIME — flash + watch: both chips report identity, then a 10 s task
//                 alternates R1/R2 SPI under the shared mutex; PASS = both respond
//                 with zero errors (no bus corruption, no deadlock).
//
// Only this dir compiles in [env:xiao_esp32s3_colink_test]; the bridge and OEM
// envs are excluded. R1 mirrors WioSX1262.cpp's Module setup exactly.
//
// Build/flash:  pio run -e xiao_esp32s3_colink_test -t upload -t monitor
#include <Arduino.h>
#include <RadioLib.h>
#include "semtech_r2.h"   // NOTE: NOT the Semtech headers — separate TU (the co-link boundary)

// --- Shared SPI bus pins (mirror src/main.cpp) ---
#define SPI_SCK   7   // D8
#define SPI_MISO  8   // D9
#define SPI_MOSI  9   // D10

// --- R1 = Wio-SX1262 on the 40-pin B2B header ---
#define R1_NSS   41
#define R1_DIO1  39
#define R1_RESET 42
#define R1_BUSY  40
// R2 pins (CS=5/RESET=3/BUSY=4/IRQ=2) are owned by semtech_r2.cpp via the vendored header.

static SemaphoreHandle_t spiMutex = nullptr;

// R1: RadioLib SX1262 on the shared global SPI (same Module pattern as WioSX1262.cpp:26).
static Module r1mod(R1_NSS, R1_DIO1, R1_RESET, R1_BUSY, SPI,
                    SPISettings(1000000, MSBFIRST, SPI_MODE0));
static SX1262 r1(&r1mod);

static volatile uint32_t s_r1ok = 0, s_r1err = 0, s_r2ok = 0, s_r2err = 0;

// R1 SPI liveness: standby() round-trips a command (RadioLib brackets its own SPI
// transaction; we add only the shared mutex, exactly as WioSX1262 does).
static bool r1Ping()
{
    bool ok = false;
    if (xSemaphoreTake(spiMutex, pdMS_TO_TICKS(2000)) == pdTRUE) {
        ok = (r1.standby() == RADIOLIB_ERR_NONE);
        xSemaphoreGive(spiMutex);
    }
    return ok;
}

static void colinkTask(void*)
{
    const uint16_t fwRef = r2_fw();
    const uint32_t start = millis();
    while (millis() - start < 10000) {
        if (r2_ping(fwRef)) s_r2ok++; else s_r2err++;   // Semtech (R2) SPI under mutex
        if (r1Ping())       s_r1ok++; else s_r1err++;   // RadioLib (R1) SPI under mutex
        delay(5);
    }
    const bool pass = (s_r1ok > 0 && s_r1err == 0 && s_r2ok > 0 && s_r2err == 0);
    Serial.println();
    Serial.println("[colink] === RESULT: 10 s of alternating shared-SPI access ===");
    Serial.printf ("[colink] R1 (RadioLib SX1262): ok=%lu err=%lu\n",
                   (unsigned long)s_r1ok, (unsigned long)s_r1err);
    Serial.printf ("[colink] R2 (Semtech LR1121):  ok=%lu err=%lu  (fw held 0x%04X)\n",
                   (unsigned long)s_r2ok, (unsigned long)s_r2err, fwRef);
    Serial.printf ("[colink] *** CO-LINK %s *** (static link proven by booting; "
                   "runtime SPI/mutex sharing %s)\n",
                   pass ? "PASS" : "FAIL", pass ? "clean" : "showed ERRORS");
    vTaskDelete(nullptr);
}

void setup()
{
    Serial.begin(115200);
    delay(600);
    Serial.println();
    Serial.println("=== Path B M0 — co-link smoke test (RadioLib SX1262 + Semtech LR1121) ===");

    SPI.begin(SPI_SCK, SPI_MISO, SPI_MOSI);   // one-shot bus init on the XIAO pins
    spiMutex = xSemaphoreCreateMutex();
    configASSERT(spiMutex != nullptr);

    // CRITICAL for a shared SPI bus: park R1's (SX1262) chip-select HIGH so it stays
    // OFF the bus until its own begin(). Otherwise its un-driven CS leaves the SX1262
    // selected and it fights the LR1121 on MISO during R2's reads (→ R2 reads garbage).
    // The bridge does this in each radio's ctor (WioSX1262.cpp:21-24, Core1121 too).
    pinMode(R1_NSS, OUTPUT);
    digitalWrite(R1_NSS, HIGH);

    // --- R2 (Semtech lr11xx_driver) bring-up — separate TU, mutex-guarded ---
    const bool r2ok = r2_begin(spiMutex);

    // --- R1 (RadioLib SX1262) bring-up — same args as WioSX1262.cpp; 1.8 V Wio TCXO ---
    int16_t st = RADIOLIB_ERR_UNKNOWN;
    if (xSemaphoreTake(spiMutex, portMAX_DELAY) == pdTRUE) {
        st = r1.begin(910.525, 62.5, 7, 5, 0x12, 10, 8, 1.8f);
        if (st == RADIOLIB_ERR_NONE) r1.setDio2AsRfSwitch(true);   // Wio module RF switch on DIO2
        xSemaphoreGive(spiMutex);
    }
    Serial.printf("[colink] R1 SX1262 begin = %d (%s)\n",
                  (int)st, st == RADIOLIB_ERR_NONE ? "OK" : "non-zero -> RadioLib err code");

    if (!r2ok || st != RADIOLIB_ERR_NONE) {
        Serial.println("[colink] NOTE: a chip didn't init — the 10 s task will still test SPI "
                       "sharing, but check wiring/power if one side shows all-err.");
    }
    Serial.println("[colink] launching 10 s alternating shared-SPI task...");
    xTaskCreatePinnedToCore(colinkTask, "colink", 4096, nullptr, 2, nullptr, 1);
}

void loop()
{
    delay(100);
}
