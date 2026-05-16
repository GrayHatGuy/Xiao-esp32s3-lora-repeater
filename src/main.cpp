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
#include "MeshDecoderDebug.h"
#include "MeshEncoderDebug.h"
// Per-radio LoRa settings — fall back to the generic LORA_* defaults
// from WioSX1262.h for any value not defined in platformio.ini.
#ifndef LORA_RADIO1_FREQUENCY
  #define LORA_RADIO1_FREQUENCY    LORA_FREQUENCY
#endif
#ifndef LORA_RADIO1_BANDWIDTH
  #define LORA_RADIO1_BANDWIDTH    LORA_BANDWIDTH
#endif
#ifndef LORA_RADIO1_SPREAD_FACTOR
  #define LORA_RADIO1_SPREAD_FACTOR LORA_SPREAD_FACTOR
#endif
#ifndef LORA_RADIO1_CODING_RATE
  #define LORA_RADIO1_CODING_RATE  LORA_CODING_RATE
#endif
#ifndef LORA_RADIO1_SYNC_WORD
  #define LORA_RADIO1_SYNC_WORD    LORA_SYNC_WORD
#endif
#ifndef LORA_RADIO1_TX_POWER
  #define LORA_RADIO1_TX_POWER     LORA_TX_POWER
#endif
#ifndef LORA_RADIO1_PREAMBLE_LEN
  #define LORA_RADIO1_PREAMBLE_LEN LORA_PREAMBLE_LEN
#endif
#ifndef LORA_RADIO1_TCXO_VOLTAGE
  #define LORA_RADIO1_TCXO_VOLTAGE LORA_TCXO_VOLTAGE
#endif

#ifndef LORA_RADIO2_FREQUENCY
  #define LORA_RADIO2_FREQUENCY    LORA_FREQUENCY
#endif
#ifndef LORA_RADIO2_BANDWIDTH
  #define LORA_RADIO2_BANDWIDTH    LORA_BANDWIDTH
#endif
#ifndef LORA_RADIO2_SPREAD_FACTOR
  #define LORA_RADIO2_SPREAD_FACTOR LORA_SPREAD_FACTOR
#endif
#ifndef LORA_RADIO2_CODING_RATE
  #define LORA_RADIO2_CODING_RATE  LORA_CODING_RATE
#endif
#ifndef LORA_RADIO2_SYNC_WORD
  #define LORA_RADIO2_SYNC_WORD    LORA_SYNC_WORD
#endif
#ifndef LORA_RADIO2_TX_POWER
  #define LORA_RADIO2_TX_POWER     LORA_TX_POWER
#endif
#ifndef LORA_RADIO2_PREAMBLE_LEN
  #define LORA_RADIO2_PREAMBLE_LEN LORA_PREAMBLE_LEN
#endif
#ifndef LORA_RADIO2_TCXO_VOLTAGE
  #define LORA_RADIO2_TCXO_VOLTAGE LORA_TCXO_VOLTAGE
#endif

static const LoraConfig radio1Config = {
    LORA_RADIO1_FREQUENCY,
    LORA_RADIO1_BANDWIDTH,
    LORA_RADIO1_SPREAD_FACTOR,
    LORA_RADIO1_CODING_RATE,
    LORA_RADIO1_SYNC_WORD,
    LORA_RADIO1_TX_POWER,
    LORA_RADIO1_PREAMBLE_LEN,
    LORA_RADIO1_TCXO_VOLTAGE
};

static const LoraConfig radio2Config = {
    LORA_RADIO2_FREQUENCY,
    LORA_RADIO2_BANDWIDTH,
    LORA_RADIO2_SPREAD_FACTOR,
    LORA_RADIO2_CODING_RATE,
    LORA_RADIO2_SYNC_WORD,
    LORA_RADIO2_TX_POWER,
    LORA_RADIO2_PREAMBLE_LEN,
    LORA_RADIO2_TCXO_VOLTAGE
};

// ============================================================
//  Shared SPI bus
//  XIAO ESP32S3 default SPI: SCK=GPIO7(D8), MOSI=GPIO9(D10),
//                             MISO=GPIO8(D9)
// ============================================================
#define SPI_SCK   7   // D8
#define SPI_MOSI  9   // D10
#define SPI_MISO  8   // D9

// Use the board-level SPI object so XIAO's variant pin mapping is
// already in effect.  A separate SPIClass(HSPI) instance re-enters
// spi_bus_initialize with default ESP32-S3 pins until begin() is
// called, introducing a window where both MOSI/MISO are wrong.
#define spi SPI

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
//  Pin order on module left side (top → bottom): D0, DIO1, RST, BUSY, NSS, RF_SW
//  XIAO ESP32S3: D0=GPIO1, D1=GPIO2, D2=GPIO3, D3=GPIO4, D4=GPIO5, D5=GPIO6
// ============================================================
#define R2_NSS      5   // SPI chip-select  (D4 / GPIO5)
#define R2_DIO1     2   // IRQ              (D1 / GPIO2)
#define R2_RESET    3   // Reset            (D2 / GPIO3)
#define R2_BUSY     4   // Busy             (D3 / GPIO4)
#define R2_ANT_SW   6   // RF switch        (D5 / GPIO6)

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
//  Cross-protocol bridge core
//  ----------------------------------------------------------------
//  One generic per-packet bridge step driven by the two radios' LoRa
//  sync words. Supports three protocols today:
//    0x12 MeshCore   <-> [MC] marker, body = decoded text
//    0x2B Meshtastic <-> [MT] marker, body = decoded text
//    0x42 Reticulum  <-> [rns] marker, body = uppercase hex of raw bytes
//
//  Behaviour:
//    1. Decode source per srcSync. Bail silently if it isn't a recognised
//       payload we know how to extract (e.g. non-text Meshtastic portnums,
//       non-public-channel MeshCore packets).
//    2. Drop if the body already starts with ANY of the three markers —
//       that means the packet is one we (or another bridge) previously
//       re-encoded and it's now looping back to us.
//    3. Prepend the source marker so the destination side can recognise
//       it as a bridged packet (and so step 2 catches the echo).
//    4. If dstSync == 0x42 (Reticulum), log "No TX 2 RNS:" and drop — the
//       RNS encoder is a TODO. Otherwise encode for the destination and
//       transmit on dstRadio.
// ============================================================
static void bridgePacket(uint8_t srcSync, uint8_t dstSync,
                         WioSX1262 *dstRadio, const char *srcTag,
                         const uint8_t *buf, size_t len)
{
    char body[256];
    const char *srcMarker = nullptr;
    bool        decoded   = false;

    switch (srcSync) {
        case MeshDecoderDebug::SYNC_WORD_MESHTASTIC:
            decoded = MeshDecoderDebug::extractMeshtasticBody(buf, len, body, sizeof(body));
            srcMarker = "[MT]";
            break;
        case MeshDecoderDebug::SYNC_WORD_MESHCORE:
            decoded = MeshDecoderDebug::extractMeshCoreBody(buf, len, body, sizeof(body));
            srcMarker = "[MC]";
            break;
        case MeshDecoderDebug::SYNC_WORD_RETICULUM:
            decoded = MeshDecoderDebug::extractReticulumHex(buf, len, body, sizeof(body));
            srcMarker = "[rns]";
            break;
        default:
            return;
    }
    if (!decoded) return;

    // Loop check — drop anything already carrying a bridge marker
    if (strncmp(body, "[MT]",  4) == 0 ||
        strncmp(body, "[MC]",  4) == 0 ||
        strncmp(body, "[rns]", 5) == 0) {
        Serial.printf("[%8lu ms][%s bridge] loop-drop: \"%s\"\n",
                      millis(), srcTag, body);
        return;
    }

    char marked[280];
    snprintf(marked, sizeof(marked), "%s %s", srcMarker, body);

    // Destination is Reticulum — log and drop. No RNS encoder yet.
    if (dstSync == MeshDecoderDebug::SYNC_WORD_RETICULUM) {
        Serial.printf("[%8lu ms][%s->RNS bridge] No TX 2 RNS: %s\n",
                      millis(), srcTag, marked);
        return;
    }

    uint8_t outPkt[256];
    size_t  outLen  = 0;
    bool    encoded = false;
    const char *dstName = "?";
    switch (dstSync) {
        case MeshDecoderDebug::SYNC_WORD_MESHTASTIC:
            encoded = MeshEncoderDebug::encodeMeshtasticText(
                          marked, outPkt, sizeof(outPkt), outLen);
            dstName = "MT";
            break;
        case MeshDecoderDebug::SYNC_WORD_MESHCORE:
            encoded = MeshEncoderDebug::encodeMeshCoreGrpTxt(
                          marked, /*ts=*/0, outPkt, sizeof(outPkt), outLen);
            dstName = "MC";
            break;
        default:
            Serial.printf("[%8lu ms][%s bridge] unknown dst sync 0x%02X — drop\n",
                          millis(), srcTag, dstSync);
            return;
    }

    if (!encoded) {
        Serial.printf("[%8lu ms][%s->%s bridge] encode failed (body too long?)\n",
                      millis(), srcTag, dstName);
        return;
    }

    Serial.printf("[%8lu ms][%s->%s bridge] re-encoded %u B: \"%s\"\n",
                  millis(), srcTag, dstName, (unsigned)outLen, marked);
    int16_t txState = dstRadio->transmit(outPkt, outLen);
    if (txState == RADIOLIB_ERR_NONE) {
        Serial.printf("[%8lu ms][%s->%s bridge] TX OK\n",
                      millis(), srcTag, dstName);
    } else {
        Serial.printf("[%8lu ms][%s->%s bridge] TX ERROR %d\n",
                      millis(), srcTag, dstName, txState);
    }
}

// ============================================================
//  FreeRTOS task: Radio1 RX → Radio2 TX
// ============================================================
void radio1Task(void *pvParameters)
{
    uint8_t buf[LORA_MAX_PACKET];

    // First Meshtastic NodeInfo goes out ~10 s after task start, then every
    // 5 min. Without this, Meshtastic clients tend to hide text messages from
    // a node they've never seen NodeInfo for, so the bridge appears silent.
    // The guard on the sync word keeps the broadcast silent if R1 is ever
    // reconfigured to a non-Meshtastic protocol (it's a compile-time check,
    // so unused code drops out of the binary).
    uint32_t nextNodeInfoMs = millis() + 10000;

    for (;;) {
        if (LORA_RADIO1_SYNC_WORD == MeshDecoderDebug::SYNC_WORD_MESHTASTIC &&
            (int32_t)(millis() - nextNodeInfoMs) >= 0) {
            uint8_t niPkt[256];
            size_t  niLen = 0;
            if (MeshEncoderDebug::encodeMeshtasticNodeInfo(
                    BRIDGE_MT_NODE_ID_STR,
                    BRIDGE_MT_LONG_NAME,
                    BRIDGE_MT_SHORT_NAME,
                    niPkt, sizeof(niPkt), niLen)) {
                Serial.printf("[%8lu ms][R1 NodeInfo TX] %u B id=%s name=\"%s\"\n",
                              millis(), (unsigned)niLen,
                              BRIDGE_MT_NODE_ID_STR, BRIDGE_MT_LONG_NAME);
                int16_t txState = radio1->transmit(niPkt, niLen);
                if (txState != RADIOLIB_ERR_NONE) {
                    Serial.printf("[%8lu ms][R1 NodeInfo TX] ERROR %d\n",
                                  millis(), txState);
                }
                radio1->startReceive();
            } else {
                Serial.printf("[%8lu ms][R1 NodeInfo TX] encode failed\n",
                              millis());
            }
            nextNodeInfoMs = millis() + 300000;   // every 5 min
        }

        if (radio1->available()) {
            size_t len  = sizeof(buf);
            float  rssi = 0.0f;
            float  snr  = 0.0f;

            int16_t state = radio1->read(buf, len, &rssi, &snr);

            if (state == RADIOLIB_ERR_NONE && len > 0) {
                Serial.printf("[%8lu ms][R1 RX] %u bytes  RSSI %.1f dBm  SNR %.1f dB\n",
                              millis(), (unsigned)len, rssi, snr);
                Serial.printf("[%8lu ms][R1 RX] data: ", millis());
                for (size_t i = 0; i < len; i++) { Serial.printf("%02X ", buf[i]); }
                Serial.println();

                MeshDecoderDebug::print(buf, len,
                    LORA_RADIO1_SYNC_WORD, "R1");

                bridgePacket(LORA_RADIO1_SYNC_WORD, LORA_RADIO2_SYNC_WORD,
                             radio2, "R1", buf, len);

                // RadioLib exits RX mode on packet receipt;
                // restore it before polling again.
                radio1->startReceive();

            } else if (state != RADIOLIB_ERR_NONE) {
                Serial.printf("[%8lu ms][R1 RX] ERROR %d\n", millis(), state);
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
                Serial.printf("[%8lu ms][R2 RX] %u bytes  RSSI %.1f dBm  SNR %.1f dB\n",
                              millis(), (unsigned)len, rssi, snr);
                Serial.printf("[%8lu ms][R2 RX] data: ", millis());
                for (size_t i = 0; i < len; i++) { Serial.printf("%02X ", buf[i]); }
                Serial.println();

                MeshDecoderDebug::print(buf, len,
                    LORA_RADIO2_SYNC_WORD, "R2");

                bridgePacket(LORA_RADIO2_SYNC_WORD, LORA_RADIO1_SYNC_WORD,
                             radio1, "R2", buf, len);

                radio2->startReceive();

            } else if (state != RADIOLIB_ERR_NONE) {
                Serial.printf("[%8lu ms][R2 RX] ERROR %d\n", millis(), state);
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
                            R1_ANT_SW, spi, spiMutex, "Radio1-B2B", radio1Config);

    radio2 = new WioSX1262(R2_NSS, R2_DIO1, R2_RESET, R2_BUSY,
                            R2_ANT_SW, spi, spiMutex, "Radio2-Edge", radio2Config);

    // Allow B2B power rail and SX1262 TCXO to settle before first SPI access.
    delay(150);

    // Pre-flight: manually pulse each RESET and watch BUSY drain to LOW.
    // If BUSY stays HIGH for 50 ms after reset that radio's module is absent
    // or unpowered — wiring problem, not a software bug.
    auto busyWait = [](int resetPin, int busyPin, const char *label) {
        pinMode(resetPin, OUTPUT);
        digitalWrite(resetPin, LOW);
        delay(2);
        digitalWrite(resetPin, HIGH);
        uint32_t t0 = millis();
        while (digitalRead(busyPin) && millis() - t0 < 50);
        Serial.printf("[diag] %s  BUSY after reset = %d  (%lu ms)  %s\n",
                      label, digitalRead(busyPin), millis() - t0,
                      digitalRead(busyPin) ? "STUCK-HIGH -> module absent/unpowered!" : "OK");
    };
    busyWait(R1_RESET, R1_BUSY, "R1");
    busyWait(R2_RESET, R2_BUSY, "R2");

    // Raw SPI probe on R1: send SX1262 GetStatus opcode (0xC0) and read
    // the response byte.  This bypasses RadioLib entirely so we can see
    // what MISO actually carries.
    //   0x00 or 0xFF → MISO not connected to this chip
    //   0x20..0x2E   → valid chip status byte (SPI path works)
    {
        SPI.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));
        digitalWrite(R1_NSS, LOW);
        delayMicroseconds(2);
        SPI.transfer(0xC0);           // GetStatus opcode
        uint8_t r1Status = SPI.transfer(0x00);  // read status byte
        digitalWrite(R1_NSS, HIGH);
        SPI.endTransaction();
        Serial.printf("[diag] R1 raw SPI GetStatus = 0x%02X  "
                      "(0x00/0xFF = MISO open; 0x20-0x2E = chip alive)\n", r1Status);
    }

    // Raw ReadRegister 0x0320 — read 6 bytes of the version string.
    // RadioLib's findChip() compares this to "SX1261".
    // Format: opcode 0x1D + addr 0x0320 + 1 NOP + data bytes.
    {
        uint8_t ver[6] = {};
        SPI.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));
        digitalWrite(R1_NSS, LOW);
        delayMicroseconds(2);
        SPI.transfer(0x1D);   // ReadRegister opcode
        SPI.transfer(0x03);   // address MSB
        SPI.transfer(0x20);   // address LSB
        SPI.transfer(0x00);   // NOP transition byte (required before data)
        for (int n = 0; n < 6; n++) { ver[n] = SPI.transfer(0x00); }
        digitalWrite(R1_NSS, HIGH);
        SPI.endTransaction();
        Serial.printf("[diag] R1 reg 0x0320 raw: "
                      "%02X %02X %02X %02X %02X %02X  = '%.6s'\n",
                      ver[0], ver[1], ver[2], ver[3], ver[4], ver[5],
                      (char*)ver);
        Serial.printf("[diag] RadioLib expects '%.6s' at 0x0320\n", "SX1261");
    }

    // Initialise — applies all LORA_* settings from WioSX1262.h
    bool r1ok = radio1->begin();
    bool r2ok = radio2->begin();

    if (!r1ok) {
        Serial.println("\nFATAL: Radio1 init failed. Check wiring. Halting.");
        while (true) { vTaskDelay(pdMS_TO_TICKS(1000)); }
    }
    if (!r2ok) {
        Serial.println("\n[WARN] Radio2 init failed — running Radio1 only.\n");
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
