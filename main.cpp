/**
 * main.cpp
 * ========
 * Dual Wio SX1262 LoRa crossover bridge — interrupt-driven async.
 * XIAO ESP32S3 Sense  +  two Wio SX1262 shields.
 *
 * Bridge logic:
 *   Radio1 RX  →  Radio2 TX
 *   Radio2 RX  →  Radio1 TX
 *
 * Key improvement over blocking version
 * --------------------------------------
 * The SPI bus is held only for the brief FIFO read/write
 * (milliseconds), never for the full LoRa airtime (up to ~2.5 s).
 * While one radio is transmitting on-air the other radio can still
 * receive — the SPI bus is free.
 *
 * Task flow (per radio)
 * ---------------------
 *   1. startReceive()          — arm RX, returns immediately
 *   2. ulTaskNotifyTake()      — sleep until DIO1 ISR wakes us
 *   3. handleIrq()             — read IRQ flags, classify event
 *   4a. if RX_DONE → read()   — grab packet from FIFO
 *              → peer->startTransmit()  — hand off to other radio
 *              → startReceive()         — re-arm our own RX
 *   4b. if TX_DONE → startReceive()    — re-arm RX (we just finished
 *                                         forwarding the last packet)
 *   4c. if RX_ERROR → startReceive()   — discard, re-arm
 *
 * Project layout:
 *   your_project/
 *   ├── platformio.ini
 *   └── src/
 *       ├── main.cpp        ← this file
 *       ├── WioSX1262.h
 *       └── WioSX1262.cpp
 *
 * All LoRa RF settings live in WioSX1262.h and can be overridden
 * from platformio.ini without editing any source file:
 *   build_flags = -DLORA_FREQUENCY=868.0f -DLORA_TX_POWER=14
 */

#include <Arduino.h>
#include <SPI.h>
#include "WioSX1262.h"

// ============================================================
//  Shared SPI bus
//  XIAO ESP32S3: SCK=GPIO7(D8)  MOSI=GPIO9(D10)  MISO=GPIO8(D9)
// ============================================================
#define SPI_SCK   7   // D8
#define SPI_MOSI  9   // D10
#define SPI_MISO  8   // D9

SPIClass spi(HSPI);

// ============================================================
//  Radio 1 — Wio SX1262 via 40-pin B2B header
//  GPIOs 38–42 are only reachable through the B2B connector.
// ============================================================
#define R1_NSS      41  // SPI chip-select  (B2B / A11)
#define R1_DIO1     39  // IRQ / DIO1       (B2B)
#define R1_RESET    42  // Reset            (B2B / A12)
#define R1_BUSY     40  // Busy             (B2B)
#define R1_ANT_SW   38  // Antenna switch   (B2B)

// ============================================================
//  Radio 2 — Wio SX1262 via perimeter (edge) header pins
//  Adjust if D0–D3 are needed elsewhere in your project.
// ============================================================
#define R2_NSS      4   // SPI chip-select  (D3 / GPIO4)
#define R2_DIO1     2   // IRQ / DIO1       (D1 / GPIO2)
#define R2_RESET    3   // Reset            (D2 / GPIO3)
#define R2_BUSY     1   // Busy             (D0 / GPIO1)
#define R2_ANT_SW  -1   // Not fitted on standalone shield

// ============================================================
//  FreeRTOS task configuration
// ============================================================
#define BRIDGE_TASK_STACK   4096
#define BRIDGE_TASK_PRIO    3        // high enough to pre-empt Arduino loop
#define NOTIFY_TIMEOUT_MS   5000     // watchdog: re-arm RX if no IRQ in 5 s

// ============================================================
//  Shared SPI mutex
// ============================================================
SemaphoreHandle_t spiMutex = NULL;

// ============================================================
//  Radio objects — pointers so mutex exists before construction
// ============================================================
WioSX1262 *radio1 = nullptr;
WioSX1262 *radio2 = nullptr;

// ============================================================
//  Forward declarations
// ============================================================
void radio1Task(void *pvParameters);
void radio2Task(void *pvParameters);

// ============================================================
//  FreeRTOS task — Radio1 RX → Radio2 TX
//
//  State machine:
//    IDLE_RX : waiting for DIO1 notification
//    on wake : handleIrq() → classify event
//              RX_DONE  → read FIFO → startTransmit on radio2
//              TX_DONE  → re-arm radio1 RX  (TX was on radio2,
//                         but radio1 may have exited RX during
//                         the SPI FIFO read; re-arm to be safe)
//              RX_ERROR → log, re-arm
//              timeout  → watchdog re-arm
// ============================================================
void radio1Task(void *pvParameters)
{
    uint8_t buf[LORA_MAX_PACKET];

    // Tell the wrapper which task to notify when DIO1 fires
    radio1->setOwnerTask(xTaskGetCurrentTaskHandle());

    // Arm RX — returns immediately
    radio1->startReceive();
    Serial.printf("[%s] Listening (async)\n", radio1->label());

    for (;;) {
        // Sleep until the ISR posts a notification, or timeout fires
        uint32_t notified = ulTaskNotifyTake(
            pdTRUE,                              // clear on exit
            pdMS_TO_TICKS(NOTIFY_TIMEOUT_MS)     // watchdog timeout
        );

        if (notified == 0) {
            // Watchdog timeout — no IRQ received; re-arm RX defensively
            Serial.printf("[%s] Watchdog: re-arming RX\n", radio1->label());
            radio1->startReceive();
            continue;
        }

        // Process the pending IRQ flags
        int16_t irqState = radio1->handleIrq();

        switch (radio1->lastEvent()) {

            case WioEvent::RX_DONE: {
                size_t len  = sizeof(buf);
                float  rssi = 0.0f;
                float  snr  = 0.0f;

                int16_t rdState = radio1->read(buf, len, &rssi, &snr);

                if (rdState == RADIOLIB_ERR_NONE && len > 0) {
                    Serial.printf("[R1→R2] %u bytes  RSSI %.1f dBm  SNR %.1f dB\n",
                                  (unsigned)len, rssi, snr);

                    // Forward to radio2 — non-blocking, returns immediately.
                    // radio2Task will re-arm radio2 RX when its TX_DONE fires.
                    int16_t txState = radio2->startTransmit(buf, len);
                    if (txState != RADIOLIB_ERR_NONE) {
                        Serial.printf("[R1→R2] startTransmit error %d\n", txState);
                    }
                } else {
                    Serial.printf("[R1] read error %d\n", rdState);
                }

                // Re-arm radio1 RX regardless of TX outcome
                radio1->startReceive();
                break;
            }

            case WioEvent::TX_DONE:
                // radio1 finished a TX (forwarding a radio2 packet).
                // Re-arm radio1 RX.
                radio1->startReceive();
                break;

            case WioEvent::RX_ERROR:
                Serial.printf("[R1] RX error (CRC/header) — re-arming\n");
                radio1->startReceive();
                break;

            default:
                // Spurious or NONE — re-arm to stay in a known state
                radio1->startReceive();
                break;
        }
    }
}

// ============================================================
//  FreeRTOS task — Radio2 RX → Radio1 TX
//  Mirror image of radio1Task.
// ============================================================
void radio2Task(void *pvParameters)
{
    uint8_t buf[LORA_MAX_PACKET];

    radio2->setOwnerTask(xTaskGetCurrentTaskHandle());
    radio2->startReceive();
    Serial.printf("[%s] Listening (async)\n", radio2->label());

    for (;;) {
        uint32_t notified = ulTaskNotifyTake(
            pdTRUE,
            pdMS_TO_TICKS(NOTIFY_TIMEOUT_MS)
        );

        if (notified == 0) {
            Serial.printf("[%s] Watchdog: re-arming RX\n", radio2->label());
            radio2->startReceive();
            continue;
        }

        int16_t irqState = radio2->handleIrq();

        switch (radio2->lastEvent()) {

            case WioEvent::RX_DONE: {
                size_t len  = sizeof(buf);
                float  rssi = 0.0f;
                float  snr  = 0.0f;

                int16_t rdState = radio2->read(buf, len, &rssi, &snr);

                if (rdState == RADIOLIB_ERR_NONE && len > 0) {
                    Serial.printf("[R2→R1] %u bytes  RSSI %.1f dBm  SNR %.1f dB\n",
                                  (unsigned)len, rssi, snr);

                    int16_t txState = radio1->startTransmit(buf, len);
                    if (txState != RADIOLIB_ERR_NONE) {
                        Serial.printf("[R2→R1] startTransmit error %d\n", txState);
                    }
                } else {
                    Serial.printf("[R2] read error %d\n", rdState);
                }

                radio2->startReceive();
                break;
            }

            case WioEvent::TX_DONE:
                radio2->startReceive();
                break;

            case WioEvent::RX_ERROR:
                Serial.printf("[R2] RX error (CRC/header) — re-arming\n");
                radio2->startReceive();
                break;

            default:
                radio2->startReceive();
                break;
        }
    }
}

// ============================================================
//  setup()
// ============================================================
void setup()
{
    Serial.begin(115200);
    while (!Serial && millis() < 3000);
    Serial.println("\n=== XIAO ESP32S3 Dual SX1262 Async Crossover Bridge ===");

    // Start shared SPI bus with explicit XIAO ESP32S3 pin mapping
    spi.begin(SPI_SCK, SPI_MISO, SPI_MOSI);

    // Mutex must exist before any WioSX1262 is constructed
    spiMutex = xSemaphoreCreateMutex();
    configASSERT(spiMutex != NULL);

    // Construct both radio objects
    radio1 = new WioSX1262(R1_NSS, R1_DIO1, R1_RESET, R1_BUSY,
                            R1_ANT_SW, spi, spiMutex, "Radio1-B2B");

    radio2 = new WioSX1262(R2_NSS, R2_DIO1, R2_RESET, R2_BUSY,
                            R2_ANT_SW, spi, spiMutex, "Radio2-Edge");

    // Initialise — configures TCXO, DIO2 switch, attaches DIO1 ISR
    bool r1ok = radio1->begin();
    bool r2ok = radio2->begin();

    if (!r1ok || !r2ok) {
        Serial.println("\nFATAL: radio init failed. Check wiring. Halting.");
        while (true) { vTaskDelay(pdMS_TO_TICKS(1000)); }
    }

    Serial.println("\nSpawning bridge tasks...");

    // Each task registers its own handle with its radio inside the task,
    // so spawn them before loop() runs.  Pin to separate cores for true
    // parallelism — ISR and task notification latency is minimised.
    xTaskCreatePinnedToCore(
        radio1Task, "R1_task",
        BRIDGE_TASK_STACK, NULL,
        BRIDGE_TASK_PRIO,  NULL,
        0   // core 0
    );

    xTaskCreatePinnedToCore(
        radio2Task, "R2_task",
        BRIDGE_TASK_STACK, NULL,
        BRIDGE_TASK_PRIO,  NULL,
        1   // core 1
    );

    Serial.println("Bridge active.\n");
}

// ============================================================
//  loop() — everything runs in FreeRTOS tasks
// ============================================================
void loop()
{
    vTaskDelay(pdMS_TO_TICKS(1000));
}
