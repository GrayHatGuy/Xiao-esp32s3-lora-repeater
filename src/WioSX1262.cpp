#include "WioSX1262.h"

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
                     const char *name)
    : _mutex(mutex), _antSw(antSw), _name(name)
{
    _mod   = new Module(nss, dio1, reset, busy, spi);
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
        LORA_FREQUENCY,
        LORA_BANDWIDTH,
        LORA_SPREAD_FACTOR,
        LORA_CODING_RATE,
        LORA_SYNC_WORD,
        LORA_TX_POWER,
        LORA_PREAMBLE_LEN,
        LORA_TCXO_VOLTAGE
    );

    if (state != RADIOLIB_ERR_NONE) {
        Serial.printf("[%s] begin() failed: %d\n", _name, state);
        xSemaphoreGive(_mutex);
        return false;
    }

    Serial.printf("[%s] ready — %.3f MHz  BW %.1f kHz  SF%d  CR4/%d  %d dBm  sync 0x%02X\n",
                  _name,
                  LORA_FREQUENCY, LORA_BANDWIDTH,
                  LORA_SPREAD_FACTOR, LORA_CODING_RATE,
                  LORA_TX_POWER, LORA_SYNC_WORD);

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
