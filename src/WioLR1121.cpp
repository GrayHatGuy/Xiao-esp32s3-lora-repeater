// WioLR1121.cpp — see WioLR1121.h for design notes.

#include "WioLR1121.h"

// Static ISR trampolines — supports up to 2 simultaneous instances.
// Each ISR just sets a flag; SPI reads happen later in the polling task.
static WioLR1121 *_inst[2]  = {};
static uint8_t    _instCount = 0;

static void IRAM_ATTR _isr0() { if (_inst[0]) _inst[0]->_rxFlag = true; }
static void IRAM_ATTR _isr1() { if (_inst[1]) _inst[1]->_rxFlag = true; }

static void (* const _isrs[2])() = { _isr0, _isr1 };

// ---------------------------------------------------------------------------

WioLR1121::WioLR1121(int nss, int irq, int reset, int busy,
                     SPIClass &spi, SemaphoreHandle_t mutex,
                     const char *name, const LoraConfig &config)
    : _mutex(mutex), _reset(reset), _busy(busy), _name(name), _config(config)
{
    // Deassert CS immediately so this chip doesn't corrupt the shared bus
    // while the other radio's begin() runs its SPI init sequence.
    pinMode(nss, OUTPUT);
    digitalWrite(nss, HIGH);

    // RadioLib Module for LR11x0: (cs, irq, rst, busy, spi, spiSettings).
    _mod   = new Module(nss, irq, reset, busy, spi,
                        SPISettings(1000000, MSBFIRST, SPI_MODE0));
    _radio = new LR1121(_mod);
    if (_instCount < 2) {
        _inst[_instCount++] = this;
    }
}

WioLR1121::~WioLR1121()
{
    delete _radio;
    delete _mod;
}

bool WioLR1121::begin()
{
    xSemaphoreTake(_mutex, portMAX_DELAY);

    // --- Manual reset + extended BUSY-wait ---------------------------------
    // LA-confirmed on the Wio-LR1121 module: BUSY stays HIGH for ~120-150 ms
    // after NRESET rises (the boot ROM takes that long to release). RadioLib's
    // internal waitForBusy times out well before that, so the driver starts
    // pumping SPI commands while the chip is still asserting BUSY — chip
    // ignores them and findChip() reports "not found". We drive reset
    // manually and poll BUSY directly before handing off to RadioLib.
    if (_reset >= 0) {
        pinMode(_reset, OUTPUT);
        digitalWrite(_reset, LOW);
        delay(2);
        digitalWrite(_reset, HIGH);
    }
    delay(50);                          // initial boot-ROM margin

    if (_busy >= 0) {
        pinMode(_busy, INPUT);
        uint32_t t0 = millis();
        while (digitalRead(_busy) == HIGH && (millis() - t0) < 1000) {
            delay(1);
        }
        uint32_t busyElapsed = millis() - t0;
        if (digitalRead(_busy) == HIGH) {
            Serial.printf("[%s] BUSY stuck HIGH after %lu ms — aborting begin()\n",
                          _name, (unsigned long)busyElapsed);
            xSemaphoreGive(_mutex);
            return false;
        }
        Serial.printf("[%s] BUSY released after %lu ms (post 50ms boot margin)\n",
                      _name, (unsigned long)busyElapsed);
    }
    // --- end pre-init wait -------------------------------------------------

    // RadioLib 7.x LR11x0 begin() signature (via LR1120 parent class):
    //   begin(freq, bw, sf, cr, syncWord, power, preambleLength, tcxoVoltage)
    // The 6.x `bool high` parameter is gone — setFrequency() inside begin()
    // accepts 150-960 MHz, 1900-2200 MHz, and 2400-2500 MHz directly.
    // TX power and frequency are now applied inside begin(); no separate
    // setFrequency()/setOutputPower() calls are needed afterwards.
    bool is2g4 = (_config.frequency > 1000.0f);

    int16_t state = _radio->begin(
        _config.frequency,
        _config.bandwidth,
        _config.spreadFactor,
        _config.codingRate,
        _config.syncWord,
        _config.txPower,
        _config.preambleLen,
        _config.tcxoVoltage
    );

    if (state != RADIOLIB_ERR_NONE) {
        Serial.printf("[%s] begin() failed: %d\n", _name, state);
        xSemaphoreGive(_mutex);
        return false;
    }

    Serial.printf("[%s] ready — %.3f MHz  BW %.1f kHz  SF%d  CR4/%d  %d dBm  "
                  "sync 0x%02X  %s\n",
                  _name,
                  _config.frequency, _config.bandwidth,
                  _config.spreadFactor, _config.codingRate,
                  _config.txPower, _config.syncWord,
                  is2g4 ? "2.4GHz" : "sub-GHz");

    // The Wio-LR1121 module has an integrated RF switch driven by the
    // LR1121's own RFSW lines; RadioLib's defaults are used here. If the
    // module needs an explicit switch table, configure it via
    // setRfSwitchTable() — bench item, see LR1121-SPEC.md open question (a).

    // Wire the DIO9 interrupt to this instance's ISR trampoline.
    for (uint8_t i = 0; i < _instCount; i++) {
        if (_inst[i] == this) {
            _radio->setPacketReceivedAction(_isrs[i]);
            break;
        }
    }

    xSemaphoreGive(_mutex);
    return true;
}

bool WioLR1121::available()
{
    return _rxFlag;
}

int16_t WioLR1121::read(uint8_t *buf, size_t &len, float *rssi, float *snr)
{
    _rxFlag = false;

    xSemaphoreTake(_mutex, portMAX_DELAY);

    size_t  pktLen = _radio->getPacketLength();
    size_t  toRead = (pktLen < len) ? pktLen : len;
    int16_t state  = _radio->readData(buf, toRead);
    len = (state == RADIOLIB_ERR_NONE) ? toRead : 0;

    if (state == RADIOLIB_ERR_NONE) {
        if (rssi) *rssi = _radio->getRSSI();
        if (snr)  *snr  = _radio->getSNR();
    }

    xSemaphoreGive(_mutex);
    return state;
}

int16_t WioLR1121::transmit(const uint8_t *buf, size_t len)
{
    xSemaphoreTake(_mutex, portMAX_DELAY);
    int16_t state = _radio->transmit(const_cast<uint8_t *>(buf), len);
    // Auto-return to RX so the bridge keeps listening without the caller
    // having to explicitly call startReceive() on the transmitting radio.
    _rxFlag = false;
    _radio->startReceive();
    xSemaphoreGive(_mutex);
    return state;
}

void WioLR1121::startReceive()
{
    _rxFlag = false;
    xSemaphoreTake(_mutex, portMAX_DELAY);
    _radio->startReceive();
    xSemaphoreGive(_mutex);
}
