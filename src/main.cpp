/**
 * main.cpp
 * ========
 * Dual Wio SX1262 LoRa crossover bridge for XIAO ESP32S3 Sense.
 * Uses the WioSX1262 wrapper class (WioSX1262.h / WioSX1262.cpp).
 *
 * Bridge logic:
 *   Radio1 RX  →  Radio2 TX
 *   Radio2 RX  →  Radio1 TX
 *
 * Project layout:
 *   your_project/
 *   ├── platformio.ini
 *   └── src/
 *       ├── main.cpp        ← this file
 *       ├── WioSX1262.h
 *       └── WioSX1262.cpp
 *
 * All LoRa RF settings (frequency, BW, SF, CR, power …) are
 * defined as preprocessor macros in WioSX1262.h.
 * Override any of them in platformio.ini build_flags, e.g.:
 *   build_flags = -DLORA_FREQUENCY=868.0f -DLORA_TX_POWER=14
 */

#include <Arduino.h>
#include <SPI.h>
#include "WioSX1262.h"

// ============================================================
//  Shared SPI bus
//  XIAO ESP32S3 default SPI: SCK=GPIO7(D8), MOSI=GPIO9(D10),
//                             MISO=GPIO8(D9)
// ============================================================
#define SPI_SCK   7   // D8
#define SPI_MOSI  9   // D10
#define SPI_MISO  8   // D9

SPIClass spi(HSPI);

// ============================================================
//  Radio 1 — Wio SX1262 via 40-pin B2B header
//  GPIOs 38–42 are only accessible via the B2B connector.
// ============================================================
#define R1_NSS      41  // SPI chip-select  (B2B / A11)
#define R1_DIO1     39  // IRQ              (B2B)
#define R1_RESET    42  // Reset            (B2B / A12)
#define R1_BUSY     40  // Busy             (B2B)
#define R1_ANT_SW   38  // Antenna switch   (B2B)

// ============================================================
//  Radio 2 — Wio SX1262 via perimeter (edge) header pins
//  Adjust if D0–D3 are needed elsewhere in your project.
// ============================================================
#define R2_NSS      4   // SPI chip-select  (D3 / GPIO4)
#define R2_DIO1     2   // IRQ              (D1 / GPIO2)
#define R2_RESET    3   // Reset            (D2 / GPIO3)
#define R2_BUSY     1   // Busy             (D0 / GPIO1)
//  No dedicated antenna-switch on the standalone shield:
#define R2_ANT_SW  -1

// ============================================================
//  FreeRTOS configuration
// ============================================================
#define BRIDGE_TASK_STACK  4096
#define BRIDGE_TASK_PRIO   2       // above idle, below system
#define BRIDGE_POLL_MS     1       // ms between RX polls

// ============================================================
//  Shared SPI mutex — created in setup(), passed to both radios
// ============================================================
SemaphoreHandle_t spiMutex = NULL;

// ============================================================
//  Radio objects — constructed as pointers so the mutex exists
//  before the WioSX1262 constructors run.
// ============================================================
WioSX1262 *radio1 = nullptr;
WioSX1262 *radio2 = nullptr;

// ============================================================
//  Forward declarations
// ============================================================
void radio1Task(void *pvParameters);
void radio2Task(void *pvParameters);

// ============================================================
//  FreeRTOS task: Radio1 RX → Radio2 TX
// ============================================================
void radio1Task(void *pvParameters)
{
    uint8_t buf[LORA_MAX_PACKET];

    for (;;) {
        if (radio1->available()) {
            size_t len  = sizeof(buf);
            float  rssi = 0.0f;
            float  snr  = 0.0f;

            int16_t state = radio1->read(buf, len, &rssi, &snr);

            if (state == RADIOLIB_ERR_NONE && len > 0) {
                Serial.printf("[R1→R2] %u bytes  RSSI %.1f dBm  SNR %.1f dB\n",
                              (unsigned)len, rssi, snr);

                int16_t txState = radio2->transmit(buf, len);
                if (txState != RADIOLIB_ERR_NONE) {
                    Serial.printf("[R1→R2] TX error %d\n", txState);
                }

                // RadioLib exits RX mode on packet receipt;
                // restore it before polling again.
                radio1->startReceive();

            } else if (state != RADIOLIB_ERR_NONE) {
                Serial.printf("[R1] RX error %d\n", state);
                radio1->startReceive();
            }
        }

        vTaskDelay(pdMS_TO_TICKS(BRIDGE_POLL_MS));
    }
}

// ============================================================
//  FreeRTOS task: Radio2 RX → Radio1 TX
// ============================================================
void radio2Task(void *pvParameters)
{
    uint8_t buf[LORA_MAX_PACKET];

    for (;;) {
        if (radio2->available()) {
            size_t len  = sizeof(buf);
            float  rssi = 0.0f;
            float  snr  = 0.0f;

            int16_t state = radio2->read(buf, len, &rssi, &snr);

            if (state == RADIOLIB_ERR_NONE && len > 0) {
                Serial.printf("[R2→R1] %u bytes  RSSI %.1f dBm  SNR %.1f dB\n",
                              (unsigned)len, rssi, snr);

                int16_t txState = radio1->transmit(buf, len);
                if (txState != RADIOLIB_ERR_NONE) {
                    Serial.printf("[R2→R1] TX error %d\n", txState);
                }

                radio2->startReceive();

            } else if (state != RADIOLIB_ERR_NONE) {
                Serial.printf("[R2] RX error %d\n", state);
                radio2->startReceive();
            }
        }

        vTaskDelay(pdMS_TO_TICKS(BRIDGE_POLL_MS));
    }
}

// ============================================================
//  setup()
// ============================================================
void setup()
{
    Serial.begin(115200);
    while (!Serial && millis() < 3000);
    Serial.println("\n=== XIAO ESP32S3 Dual SX1262 Crossover Bridge ===");

    // Start shared SPI bus with explicit XIAO ESP32S3 pin mapping
    spi.begin(SPI_SCK, SPI_MISO, SPI_MOSI);

    // Mutex must exist before any WioSX1262 object is constructed
    spiMutex = xSemaphoreCreateMutex();
    configASSERT(spiMutex != NULL);

    // Construct radio objects now the mutex and SPI bus are ready
    radio1 = new WioSX1262(R1_NSS, R1_DIO1, R1_RESET, R1_BUSY,
                            R1_ANT_SW, spi, spiMutex, "Radio1-B2B");

    radio2 = new WioSX1262(R2_NSS, R2_DIO1, R2_RESET, R2_BUSY,
                            R2_ANT_SW, spi, spiMutex, "Radio2-Edge");

    // Initialise — applies all LORA_* settings from WioSX1262.h
    bool r1ok = radio1->begin();
    bool r2ok = radio2->begin();

    if (!r1ok || !r2ok) {
        Serial.println("\nFATAL: radio init failed. Check wiring. Halting.");
        while (true) { vTaskDelay(pdMS_TO_TICKS(1000)); }
    }

    // Start both radios listening
    radio1->startReceive();
    radio2->startReceive();

    Serial.println("\nBridge active — both radios listening.\n");

    // Spawn one FreeRTOS task per radio, pinned to separate cores
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
}

// ============================================================
//  loop() — bridge runs entirely in FreeRTOS tasks
// ============================================================
void loop()
{
    vTaskDelay(pdMS_TO_TICKS(1000));
}
