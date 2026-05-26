// WioLR1121.cpp — see WioLR1121.h for design notes.
//
// Build-time flags (set in platformio.ini build_flags):
//
//   -DLR1121_DEBUG
//      Enable verbose Phase-B diagnostic prints — per-read pktLen/state,
//      getIrqFlags after begin(), transmit() return codes, startReceive()
//      return codes, re-arm warnings. ON by default while the RX block is
//      under investigation. Flip OFF (delete the flag) for quiet production
//      builds once the Wio-LR1121 RX path is resolved.
//
//   -DLR1121_BRUTEFORCE_RX_DIOMASK=<0..7>
//      Override the MODE_RX entry of the LR1121 RF switch table with an
//      explicit DIO5/DIO6/DIO7 bitmask. Encoding:
//          bit 0 → DIO5,  bit 1 → DIO6,  bit 2 → DIO7
//      Examples:
//          0 = 000 (femtofox config — passive RX)
//          1 = 001 (DIO5 HIGH — LilyGO T3S3 production)
//          2 = 010 (DIO6 HIGH)
//          3 = 011, 4 = 100, 5 = 101, 6 = 110, 7 = 111
//      Use this to walk through all 8 combinations on the bench if neither
//      femtofox nor T3S3 tables produce RX_DONE events on a Wio-LR1121.
//      Default: 1 (T3S3-style, DIO5 HIGH).

#include "WioLR1121.h"

#ifndef LR1121_BRUTEFORCE_RX_DIOMASK
  #define LR1121_BRUTEFORCE_RX_DIOMASK 1   // T3S3-style: DIO5 HIGH for RX
#endif

// Static ISR trampolines — supports up to 2 simultaneous instances.
// Each ISR just sets a flag; SPI reads happen later in the polling task.
static WioLR1121 *_inst[2]  = {};
static uint8_t    _instCount = 0;

static void IRAM_ATTR _isr0() {
    if (_inst[0]) { _inst[0]->_rxFlag = true; _inst[0]->_isrCount++; }
}
static void IRAM_ATTR _isr1() {
    if (_inst[1]) { _inst[1]->_rxFlag = true; _inst[1]->_isrCount++; }
}

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

    // ----- Install the LR1121 RF switch truth table BEFORE begin() -------
    // The Wio-LR1121 module's DIO5/6/7 are not broken out on module pads —
    // they drive the internal RF switch. RadioLib's setRfSwitchTable() must
    // be called before begin() so internal mode transitions use it.
    //
    // MODE_RX is the variable under test (see LR1121_BRUTEFORCE_RX_DIOMASK
    // build flag). Other modes use the LilyGO T3S3 production reference:
    //   - STBY:  all LOW
    //   - TX:    DIO6 HIGH (low power PA)
    //   - TX_HP: DIO6 HIGH (high power PA, same as low)
    //   - TX_HF: all LOW (2.4 GHz, unused on this module)
    //   - GNSS:  all LOW (not exercised)
    //   - WIFI:  all LOW (not exercised)
    static const uint32_t rfswitch_pins[Module::RFSWITCH_MAX_PINS] = {
        RADIOLIB_LR11X0_DIO5, RADIOLIB_LR11X0_DIO6, RADIOLIB_LR11X0_DIO7,
        RADIOLIB_NC, RADIOLIB_NC
    };
    // Decode the brute-force MODE_RX bitmask into per-DIO values.
    constexpr uint8_t RX_D5 = (LR1121_BRUTEFORCE_RX_DIOMASK >> 0) & 1;
    constexpr uint8_t RX_D6 = (LR1121_BRUTEFORCE_RX_DIOMASK >> 1) & 1;
    constexpr uint8_t RX_D7 = (LR1121_BRUTEFORCE_RX_DIOMASK >> 2) & 1;
    static const Module::RfSwitchMode_t rfswitch_table[] = {
        // mode                        DIO5    DIO6    DIO7    -  -
        { LR11x0::MODE_STBY,         { 0,      0,      0,      0, 0 } },
        { LR11x0::MODE_RX,           { RX_D5,  RX_D6,  RX_D7,  0, 0 } },
        { LR11x0::MODE_TX,           { 0,      1,      0,      0, 0 } },
        { LR11x0::MODE_TX_HP,        { 0,      1,      0,      0, 0 } },
        { LR11x0::MODE_TX_HF,        { 0,      0,      0,      0, 0 } },
        { LR11x0::MODE_GNSS,         { 0,      0,      0,      0, 0 } },
        { LR11x0::MODE_WIFI,         { 0,      0,      0,      0, 0 } },
        { LR11x0::MODE_END_OF_TABLE, {} },
    };
    Serial.printf("[%s] installing RF switch table: MODE_RX = "
                  "D5=%u D6=%u D7=%u (BRUTEFORCE_RX_DIOMASK=%u)\n",
                  _name, RX_D5, RX_D6, RX_D7,
                  (unsigned)LR1121_BRUTEFORCE_RX_DIOMASK);
    _radio->setRfSwitchTable(rfswitch_pins, rfswitch_table);

    // RadioLib 7.x LR11x0 begin() signature (via LR1120 parent class):
    //   begin(freq, bw, sf, cr, syncWord, power, preambleLength, tcxoVoltage)
    // setFrequency() inside begin() accepts 150-960 MHz, 1900-2200 MHz, and
    // 2400-2500 MHz directly. TX power and frequency are applied inside
    // begin(); no separate setFrequency()/setOutputPower() calls are needed
    // afterwards.
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

#ifdef LR1121_DEBUG
    // Dump the chip's IRQ flags right after begin() so we can see what (if
    // any) bits are pending before we ever ask it to RX. Latched bits here
    // might mask future RX_DONE events.
    uint32_t irqAfterBegin = _radio->getIrqFlags();
    Serial.printf("[%s] getIrqFlags() post-begin = 0x%08lX\n",
                  _name, (unsigned long)irqAfterBegin);
#endif

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

#ifdef LR1121_DEBUG
    // Dump every read attempt so we can see why some ISR fires don't
    // translate into an [R2 RX] line.
    Serial.printf("[%s] read: pktLen=%u state=%d len=%u\n",
                  _name, (unsigned)pktLen, (int)state, (unsigned)len);
#endif

    if (state == RADIOLIB_ERR_NONE) {
        if (rssi) *rssi = _radio->getRSSI();
        if (snr)  *snr  = _radio->getSNR();
    }

    // Defensive re-arm: guarantee return to continuous RX before releasing the
    // mutex. RadioLib's readData() already calls clearIrqState() internally,
    // but bench-observed behavior on the Wio-LR1121 showed that without this
    // belt-and-braces sequence the chip stops receiving after one packet.
    int16_t clrSt = _radio->clearIrqFlags(RADIOLIB_LR11X0_IRQ_ALL);
    int16_t sbSt  = _radio->standby();
    int16_t rxSt  = _radio->startReceive();
#ifdef LR1121_DEBUG
    if (clrSt != RADIOLIB_ERR_NONE || sbSt != RADIOLIB_ERR_NONE ||
        rxSt != RADIOLIB_ERR_NONE) {
        Serial.printf("[%s] re-arm warn: clr=%d sb=%d rx=%d\n",
                      _name, clrSt, sbSt, rxSt);
    }
#else
    (void)clrSt; (void)sbSt; (void)rxSt;
#endif

    xSemaphoreGive(_mutex);
    return state;
}

int16_t WioLR1121::transmit(const uint8_t *buf, size_t len)
{
    xSemaphoreTake(_mutex, portMAX_DELAY);
    int16_t txSt = _radio->transmit(const_cast<uint8_t *>(buf), len);
    // Auto-return to RX so the bridge keeps listening without the caller
    // having to explicitly call startReceive() on the transmitting radio.
    _rxFlag = false;
    int16_t rxSt = _radio->startReceive();
#ifdef LR1121_DEBUG
    Serial.printf("[%s] transmit(%u B) tx=%d post-rx=%d\n",
                  _name, (unsigned)len, (int)txSt, (int)rxSt);
#else
    (void)rxSt;
#endif
    xSemaphoreGive(_mutex);
    return txSt;
}

void WioLR1121::startReceive()
{
    _rxFlag = false;
    xSemaphoreTake(_mutex, portMAX_DELAY);
    int16_t st = _radio->startReceive();
#ifdef LR1121_DEBUG
    Serial.printf("[%s] startReceive() = %d\n", _name, (int)st);
#else
    (void)st;
#endif
    xSemaphoreGive(_mutex);
}
