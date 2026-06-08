#include "WioSX1262.h"
#include "SerialLog.h"   // logf() — serialise USB-CDC log across the two cores

// Static ISR trampolines — supports up to 2 simultaneous instances.
// Each ISR just sets a flag; SPI reads happen later in the polling task.
static WioSX1262 *_inst[2]  = {};
static uint8_t    _instCount = 0;

static void IRAM_ATTR _isr0() { if (_inst[0]) _inst[0]->_rxFlag = true; }
static void IRAM_ATTR _isr1() { if (_inst[1]) _inst[1]->_rxFlag = true; }

static void (* const _isrs[2])() = { _isr0, _isr1 };

// ---------------------------------------------------------------------------

WioSX1262::WioSX1262(int nss, int dio1, int reset, int busy,
                     int antSw, SPIClass &spi, SemaphoreHandle_t mutex,
                     const char *name, const LoraConfig &config)
    : _mutex(mutex), _antSw(antSw), _name(name), _config(config)
{
    // Deassert CS immediately so this chip doesn't corrupt the bus
    // while the other radio's begin() runs its SPI init sequence.
    pinMode(nss, OUTPUT);
    digitalWrite(nss, HIGH);

    _mod   = new Module(nss, dio1, reset, busy, spi, SPISettings(1000000, MSBFIRST, SPI_MODE0));
    _radio = new SX1262(_mod);
    if (_instCount < 2) {
        _inst[_instCount++] = this;
    }
}

WioSX1262::~WioSX1262()
{
    delete _radio;
    delete _mod;
}

void WioSX1262::_setAnt(bool tx)
{
    if (_antSw < 0) return;
    digitalWrite(_antSw, tx ? HIGH : LOW);
}

bool WioSX1262::begin()
{
    if (_antSw >= 0) {
        pinMode(_antSw, OUTPUT);
        _setAnt(false);
    }

    xSemaphoreTake(_mutex, portMAX_DELAY);

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
        logf("[%s] begin() failed: %d\n", _name, state);
        xSemaphoreGive(_mutex);
        return false;
    }

    logf("[%s] ready — %.3f MHz  BW %.1f kHz  SF%d  CR4/%d  %d dBm  sync 0x%02X\n",
                  _name,
                  _config.frequency, _config.bandwidth,
                  _config.spreadFactor, _config.codingRate,
                  _config.txPower, _config.syncWord);

    // DIO2 drives the Wio SX1262 module's internal RF switch.
    _radio->setDio2AsRfSwitch(true);

    // Wire the DIO1 interrupt to this instance's ISR trampoline.
    for (uint8_t i = 0; i < _instCount; i++) {
        if (_inst[i] == this) {
            _radio->setPacketReceivedAction(_isrs[i]);
            break;
        }
    }

    xSemaphoreGive(_mutex);
    return true;
}

bool WioSX1262::available()
{
    return _rxFlag;
}

int16_t WioSX1262::read(uint8_t *buf, size_t &len, float *rssi, float *snr)
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

    // Carrier frequency-error readout — the R1 (SX1262) reference for the
    // R2 (LR1121) mistuning comparison. R1 should read ~0 Hz; a large R2
    // freqErr on the same packets confirms an LR1121 RX frequency offset.
    // (R2-deaf-at-BW62.5 investigation — see CLAUDE.md §0.)
    logf("[%s] read: pktLen=%u state=%d len=%u  freqErr=%.0f Hz\n",
                  _name, (unsigned)pktLen, (int)state, (unsigned)len,
                  _radio->getFrequencyError());

    xSemaphoreGive(_mutex);
    return state;
}

int16_t WioSX1262::transmit(const uint8_t *buf, size_t len)
{
    _setAnt(true);

    xSemaphoreTake(_mutex, portMAX_DELAY);
    int16_t state = _radio->transmit(const_cast<uint8_t *>(buf), len);
    // Auto-return to RX so the bridge keeps listening without the caller
    // having to explicitly call startReceive() on the transmitting radio.
    _rxFlag = false;
    _radio->startReceive();
    xSemaphoreGive(_mutex);

    _setAnt(false);
    return state;
}

void WioSX1262::startReceive()
{
    _rxFlag = false;
    xSemaphoreTake(_mutex, portMAX_DELAY);
    _radio->startReceive();
    xSemaphoreGive(_mutex);
}
