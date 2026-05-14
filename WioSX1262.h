/**
 * WioSX1262.h
 * ===========
 * Interrupt-driven async wrapper around RadioLib's SX1262 for the
 * Seeed Wio SX1262 shield on the XIAO ESP32S3 Sense.
 *
 * Architecture
 * ------------
 * RadioLib's blocking transmit()/receive() hold the SPI bus for the
 * entire LoRa airtime (up to ~2.5 s at SF9/BW125).  With two radios
 * sharing one SPI bus this makes one radio completely deaf while the
 * other transmits.
 *
 * This wrapper uses RadioLib's non-blocking API instead:
 *   startReceive()   — arms the RX FIFO and returns immediately
 *   startTransmit()  — begins TX and returns immediately
 *
 * The SX1262 asserts DIO1 when an event is ready (packet received
 * or TX complete).  A GPIO ISR catches that edge and posts a FreeRTOS
 * task notification to wake the owner task.  The SPI bus is only
 * locked for the brief FIFO read/write (milliseconds), never for
 * the full airtime.
 *
 * Usage (see main.cpp)
 * --------------------
 *   SPIClass          spi(HSPI);
 *   SemaphoreHandle_t spiMutex = xSemaphoreCreateMutex();
 *
 *   WioSX1262 radio1(R1_NSS, R1_DIO1, R1_RESET, R1_BUSY,
 *                    R1_ANT_SW, spi, spiMutex, "Radio1");
 *   WioSX1262 radio2(R2_NSS, R2_DIO1, R2_RESET, R2_BUSY,
 *                    -1,       spi, spiMutex, "Radio2");
 *
 *   radio1.begin();
 *   radio2.begin();
 *
 *   // Register the owner task BEFORE arming RX
 *   radio1.setOwnerTask(xTaskGetCurrentTaskHandle());
 *   radio1.startReceive();
 *
 * When DIO1 fires the owner task is notified via
 * xTaskNotifyFromISR().  The task calls handleIrq() to let RadioLib
 * process the event, then inspects lastEvent() to decide whether to
 * read a packet or re-arm RX after TX.
 */

#pragma once

#include <Arduino.h>
#include <RadioLib.h>
#include <SPI.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

// ============================================================
//  LoRa RF settings
//  All values are guarded by #ifndef so any of them can be
//  overridden from platformio.ini build_flags without editing
//  this header, e.g.:
//    build_flags = -DLORA_FREQUENCY=868.0f -DLORA_TX_POWER=14
// ============================================================

// Carrier frequency (MHz)
//   915.0 — US / AU    868.0 — EU    433.0 — ITU R1 / Asia
#ifndef LORA_FREQUENCY
#  define LORA_FREQUENCY      915.0f
#endif

// Bandwidth (kHz): 7.8 / 10.4 / 15.6 / 20.8 / 31.25 /
//                  41.7 / 62.5 / 125.0 / 250.0 / 500.0
#ifndef LORA_BANDWIDTH
#  define LORA_BANDWIDTH      125.0f
#endif

// Spreading factor 5–12  (higher = longer range, lower data rate)
#ifndef LORA_SPREAD_FACTOR
#  define LORA_SPREAD_FACTOR  9
#endif

// Coding rate 5=4/5 … 8=4/8  (higher = more FEC overhead)
#ifndef LORA_CODING_RATE
#  define LORA_CODING_RATE    7
#endif

// TX power dBm: -9 … +22  (SX1262 hardware max is 22 dBm)
#ifndef LORA_TX_POWER
#  define LORA_TX_POWER       20
#endif

// Sync word: 0x12 = private LoRa network  0x34 = LoRaWAN public
#ifndef LORA_SYNC_WORD
#  define LORA_SYNC_WORD      0x12
#endif

// Preamble length (symbols)
#ifndef LORA_PREAMBLE_LEN
#  define LORA_PREAMBLE_LEN   8
#endif

// CRC: true = enabled (strongly recommended)
#ifndef LORA_CRC
#  define LORA_CRC            true
#endif

// TCXO reference voltage for the Wio SX1262 module (volts)
#ifndef LORA_TCXO_VOLTAGE
#  define LORA_TCXO_VOLTAGE   1.8f
#endif

// Maximum payload buffer size (bytes)
#ifndef LORA_MAX_PACKET
#  define LORA_MAX_PACKET     256
#endif


// ============================================================
//  Event type reported to the owner task after IRQ handling
// ============================================================
enum class WioEvent : uint8_t {
    NONE    = 0,   ///< No event (should not normally be seen)
    RX_DONE,       ///< Packet received — call read()
    TX_DONE,       ///< Transmission complete — re-arm RX if desired
    RX_ERROR,      ///< CRC or header error on receive
    TX_ERROR,      ///< Error during transmission
};


// ============================================================
//  WioSX1262 class
// ============================================================
class WioSX1262 {
public:

    /**
     * Constructor
     *
     * @param nss     SPI chip-select GPIO
     * @param dio1    DIO1 / IRQ GPIO  (must support attachInterrupt)
     * @param reset   Reset GPIO
     * @param busy    Busy GPIO
     * @param antSw   Antenna-switch GPIO  (-1 = not fitted)
     * @param spi     Shared SPIClass instance
     * @param mutex   FreeRTOS mutex guarding the shared SPI bus
     * @param label   Human-readable name used in log output
     */
    WioSX1262(int               nss,
              int               dio1,
              int               reset,
              int               busy,
              int               antSw,
              SPIClass         &spi,
              SemaphoreHandle_t mutex,
              const char       *label = "SX1262");

    /**
     * Initialise the radio: applies all LORA_* settings,
     * configures TCXO and DIO2 RF-switch, sets up the DIO1 ISR.
     * Call once from setup() after SPI has been started.
     *
     * @return true on success
     */
    bool begin();

    /**
     * Register the FreeRTOS task that owns this radio.
     * The ISR will notify this task when DIO1 fires.
     * Must be called before startReceive().
     */
    void setOwnerTask(TaskHandle_t task) { _ownerTask = task; }

    /**
     * Arm the receiver (non-blocking).
     * Returns immediately; DIO1 fires when a packet arrives.
     *
     * @return RadioLib status code
     */
    int16_t startReceive();

    /**
     * Begin transmitting buf (non-blocking).
     * Returns immediately; DIO1 fires when TX is complete.
     *
     * @return RadioLib status code
     */
    int16_t startTransmit(const uint8_t *buf, size_t len);

    /** Convenience overload — transmit a null-terminated C string. */
    int16_t startTransmit(const char *str);

    /**
     * Call from the owner task after it has been notified by the ISR.
     * Lets RadioLib process the pending IRQ flags and updates the
     * internal lastEvent() value.
     *
     * @return RadioLib status code
     */
    int16_t handleIrq();

    /**
     * Read a received packet into buf.
     * Call only after handleIrq() returns and lastEvent() == RX_DONE.
     *
     * @param buf   Destination buffer (>= LORA_MAX_PACKET bytes)
     * @param len   In: buffer capacity.  Out: bytes received.
     * @param rssi  Optional — last packet RSSI (dBm)
     * @param snr   Optional — last packet SNR  (dB)
     * @return RadioLib status code
     */
    int16_t read(uint8_t *buf, size_t &len,
                 float *rssi = nullptr, float *snr = nullptr);

    /** Last event set by handleIrq(). */
    WioEvent lastEvent() const { return _lastEvent; }

    /** Human-readable label supplied at construction. */
    const char *label() const { return _label; }

    /**
     * Direct access to the underlying RadioLib SX1262 object
     * for advanced configuration (CAD, frequency hopping, etc.)
     */
    SX1262 &radio() { return _radio; }

    // --- Static ISR trampolines (one per radio instance) ----------
    // RadioLib requires a plain C function pointer for the IRQ
    // callback.  These static methods bridge back to the instance.
    static void IRAM_ATTR isrTrampoline0();   // radio instance 0
    static void IRAM_ATTR isrTrampoline1();   // radio instance 1

    // Global registry — up to 2 instances (one per radio)
    static WioSX1262 *_instances[2];
    static uint8_t    _instanceCount;

private:
    // Acquire / release the shared SPI mutex
    void lock()   { xSemaphoreTake(_mutex, portMAX_DELAY); }
    void unlock() { xSemaphoreGive(_mutex); }

    // Called from the ISR trampoline for this instance
    void IRAM_ATTR onDio1Isr();

    Module            _module;
    SX1262            _radio;
    SemaphoreHandle_t _mutex;
    TaskHandle_t      _ownerTask;
    int               _antSw;
    int               _dio1Pin;
    const char       *_label;
    volatile WioEvent _lastEvent;
};
