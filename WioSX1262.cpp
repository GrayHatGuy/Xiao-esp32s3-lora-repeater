/**
 * WioSX1262.cpp
 * =============
 * Interrupt-driven async implementation of the WioSX1262 wrapper.
 * See WioSX1262.h for full architecture notes and API documentation.
 */

#include "WioSX1262.h"

// ============================================================
//  Static instance registry
//  RadioLib requires a plain C function pointer for the DIO1
//  callback, so we keep a global list of up to 2 instances and
//  route the ISR back to the correct object via trampolines.
// ============================================================
WioSX1262 *WioSX1262::_instances[2]  = { nullptr, nullptr };
uint8_t    WioSX1262::_instanceCount = 0;

// ============================================================
//  ISR trampolines — stored in IRAM for fast execution
// ============================================================
void IRAM_ATTR WioSX1262::isrTrampoline0()
{
    if (_instances[0]) { _instances[0]->onDio1Isr(); }
}

void IRAM_ATTR WioSX1262::isrTrampoline1()
{
    if (_instances[1]) { _instances[1]->onDio1Isr(); }
}

// ============================================================
//  onDio1Isr()
//  Runs in ISR context — keep it minimal.
//  Posts a task notification to wake the owner task.
// ============================================================
void IRAM_ATTR WioSX1262::onDio1Isr()
{
    if (_ownerTask == nullptr) { return; }

    BaseType_t higherPriorityTaskWoken = pdFALSE;

    // Notify value 1 — the task checks lastEvent() after waking
    vTaskNotifyGiveFromISR(_ownerTask, &higherPriorityTaskWoken);

    // Yield to the notified task immediately if it has higher priority
    portYIELD_FROM_ISR(higherPriorityTaskWoken);
}

// ============================================================
//  Constructor
// ============================================================
WioSX1262::WioSX1262(int               nss,
                     int               dio1,
                     int               reset,
                     int               busy,
                     int               antSw,
                     SPIClass         &spi,
                     SemaphoreHandle_t mutex,
                     const char       *label)
    : _module(nss, dio1, reset, busy, spi)
    , _radio(&_module)
    , _mutex(mutex)
    , _ownerTask(nullptr)
    , _antSw(antSw)
    , _dio1Pin(dio1)
    , _label(label)
    , _lastEvent(WioEvent::NONE)
{
    // Register this instance so the trampoline can find it
    if (_instanceCount < 2) {
        _instances[_instanceCount++] = this;
    } else {
        Serial.println("[WioSX1262] ERROR: more than 2 instances created!");
    }
}

// ============================================================
//  begin()
// ============================================================
bool WioSX1262::begin()
{
    Serial.printf("[%s] Initialising SX1262 (async/IRQ mode)... ", _label);

    // Drive the external antenna-switch GPIO if one is wired
    if (_antSw >= 0) {
        pinMode(_antSw, OUTPUT);
        digitalWrite(_antSw, HIGH);
    }

    lock();

    // Primary radio init — applies all LORA_* modulation settings
    int16_t state = _radio.begin(
        LORA_FREQUENCY,
        LORA_BANDWIDTH,
        LORA_SPREAD_FACTOR,
        LORA_CODING_RATE,
        LORA_SYNC_WORD,
        LORA_TX_POWER,
        LORA_PREAMBLE_LEN
    );

    if (state != RADIOLIB_ERR_NONE) {
        unlock();
        Serial.printf("FAILED (err %d)\n", state);
        return false;
    }

    // Wio SX1262 module has a 1.8 V TCXO — required for stable TX/RX
    state = _radio.setTCXO(LORA_TCXO_VOLTAGE);
    if (state != RADIOLIB_ERR_NONE) {
        Serial.printf("[%s] Warning: TCXO config failed (err %d)\n",
                      _label, state);
    }

    // DIO2 controls the internal RF switch on the Wio module
    state = _radio.setDio2AsRfSwitch(true);
    if (state != RADIOLIB_ERR_NONE) {
        Serial.printf("[%s] Warning: DIO2 RF-switch failed (err %d)\n",
                      _label, state);
    }

    _radio.setCRC(LORA_CRC);

    // Attach the DIO1 interrupt — rising edge triggers on packet/TX done
    // Select the correct trampoline based on instance index
    void (*isr)() = nullptr;
    for (uint8_t i = 0; i < _instanceCount; i++) {
        if (_instances[i] == this) {
            isr = (i == 0) ? isrTrampoline0 : isrTrampoline1;
            break;
        }
    }

    if (isr) {
        // Tell RadioLib which function to call when DIO1 fires
        _radio.setDio1Action(isr);
    } else {
        Serial.printf("[%s] ERROR: could not find ISR trampoline!\n", _label);
        unlock();
        return false;
    }

    unlock();

    Serial.printf("OK\n");
    Serial.printf("  [%s] %.3f MHz  BW %.1f kHz  SF%d  CR 4/%d  "
                  "%d dBm  SW 0x%02X\n",
                  _label,
                  LORA_FREQUENCY, LORA_BANDWIDTH,
                  LORA_SPREAD_FACTOR, LORA_CODING_RATE,
                  LORA_TX_POWER, LORA_SYNC_WORD);

    return true;
}

// ============================================================
//  startReceive()  — non-blocking RX arm
// ============================================================
int16_t WioSX1262::startReceive()
{
    lock();
    _lastEvent = WioEvent::NONE;
    int16_t state = _radio.startReceive();
    unlock();
    return state;
}

// ============================================================
//  startTransmit(buf, len)  — non-blocking TX start
// ============================================================
int16_t WioSX1262::startTransmit(const uint8_t *buf, size_t len)
{
    lock();
    _lastEvent = WioEvent::NONE;
    // RadioLib takes non-const; cast is safe — buffer is not modified
    int16_t state = _radio.startTransmit(const_cast<uint8_t *>(buf),
                                         static_cast<size_t>(len));
    unlock();
    return state;
}

// ============================================================
//  startTransmit(str)  — convenience C-string overload
// ============================================================
int16_t WioSX1262::startTransmit(const char *str)
{
    return startTransmit(reinterpret_cast<const uint8_t *>(str),
                         strlen(str));
}

// ============================================================
//  handleIrq()
//  Called from the owner task after ulTaskNotifyTake() returns.
//  Reads the IRQ flags from the SX1262, classifies the event,
//  and stores it in _lastEvent for the task to inspect.
// ============================================================
int16_t WioSX1262::handleIrq()
{
    lock();

    // Ask RadioLib to read and clear the IRQ status register
    uint32_t irqFlags = _radio.getIrqStatus();
    _radio.clearIrqStatus();

    int16_t state = RADIOLIB_ERR_NONE;

    if (irqFlags & RADIOLIB_SX126X_IRQ_RX_DONE) {
        // Check for CRC error — SX1262 sets both bits on a bad packet
        if (irqFlags & RADIOLIB_SX126X_IRQ_CRC_ERR) {
            _lastEvent = WioEvent::RX_ERROR;
            state      = RADIOLIB_ERR_CRC_MISMATCH;
        } else {
            _lastEvent = WioEvent::RX_DONE;
        }
    } else if (irqFlags & RADIOLIB_SX126X_IRQ_TX_DONE) {
        _lastEvent = WioEvent::TX_DONE;
    } else if (irqFlags & RADIOLIB_SX126X_IRQ_HEADER_ERR) {
        _lastEvent = WioEvent::RX_ERROR;
        state      = RADIOLIB_ERR_LORA_HEADER_DAMAGED;
    } else {
        // Spurious IRQ — re-arm RX to stay in a known state
        _lastEvent = WioEvent::NONE;
    }

    unlock();
    return state;
}

// ============================================================
//  read()
//  Call only after handleIrq() sets lastEvent() == RX_DONE.
// ============================================================
int16_t WioSX1262::read(uint8_t *buf, size_t &len,
                        float *rssi, float *snr)
{
    lock();

    int16_t state = _radio.readData(buf, len);

    // If RadioLib didn't write back len, query it directly
    if (len == 0) {
        len = static_cast<size_t>(_radio.getPacketLength());
    }

    if (rssi) { *rssi = _radio.getRSSI(); }
    if (snr)  { *snr  = _radio.getSNR();  }

    unlock();
    return state;
}
