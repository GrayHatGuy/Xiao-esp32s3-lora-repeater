// RemoteRadio.cpp — see RemoteRadio.h.

#include "RemoteRadio.h"
#include <RadioLib.h>     // RADIOLIB_ERR_* status codes
#include <string.h>

RemoteRadio::RemoteRadio(UartLink &link, int localRadio, const char *name,
                         const LoraConfig &config, uint8_t band)
    : _link(link), _localRadio(localRadio), _name(name),
      _config(config), _band(band)
{
    _rxQueue = xQueueCreate(RX_QUEUE_DEPTH, sizeof(RxPacket));
    configASSERT(_rxQueue != nullptr);
    _link.registerRxQueue(localRadio, _rxQueue);
}

RemoteRadio::~RemoteRadio()
{
    if (_rxQueue) vQueueDelete(_rxQueue);
}

bool RemoteRadio::begin()
{
    LinkProtocol::CfgRadio cfg;
    cfg.enable      = 1;
    cfg.band        = _band;
    cfg.frequency   = _config.frequency;
    cfg.bandwidth   = _config.bandwidth;
    cfg.sf          = _config.spreadFactor;
    cfg.cr          = _config.codingRate;
    cfg.syncWord    = _config.syncWord;
    cfg.txPower     = _config.txPower;
    cfg.preambleLen = _config.preambleLen;

    bool ok = _link.sendFrame(LinkProtocol::MSG_CFG_RADIO, (uint8_t)_localRadio,
                              (const uint8_t *)&cfg, sizeof(cfg));
    if (ok)
        _link.sendFrame(LinkProtocol::MSG_START_RX, (uint8_t)_localRadio,
                        nullptr, 0);

    Serial.printf("[%s] remote begin -> %s  %.3f MHz BW%.1f SF%u CR4/%u "
                  "%ddBm sync 0x%02X band%u\n",
                  _name, ok ? "sent" : "LINK FAIL",
                  _config.frequency, _config.bandwidth, _config.spreadFactor,
                  _config.codingRate, _config.txPower, _config.syncWord, _band);
    return ok;
}

bool RemoteRadio::available()
{
    return _rxQueue && uxQueueMessagesWaiting(_rxQueue) > 0;
}

int16_t RemoteRadio::read(uint8_t *buf, size_t &len, float *rssi, float *snr)
{
    RxPacket pkt;
    if (!_rxQueue || xQueueReceive(_rxQueue, &pkt, 0) != pdTRUE) {
        len = 0;
        return RADIOLIB_ERR_UNKNOWN;
    }
    size_t n = (pkt.len < len) ? pkt.len : len;
    memcpy(buf, pkt.data, n);
    len = n;
    if (rssi) *rssi = pkt.rssi;
    if (snr)  *snr  = pkt.snr;
    return RADIOLIB_ERR_NONE;
}

int16_t RemoteRadio::transmit(const uint8_t *buf, size_t len)
{
    // Legacy blocking-style entry point: send the frame and wait (bounded) for
    // the co-proc's MSG_TX_DONE so callers that still use transmit() keep their
    // serialised semantics. The RX-priority pipeline uses the non-blocking trio.
    if (startTransmit(buf, len) != RADIOLIB_ERR_NONE) return RADIOLIB_ERR_UNKNOWN;
    uint32_t t0 = millis();
    while (!txDone() && (millis() - t0) < TX_DONE_TIMEOUT_MS)
        vTaskDelay(pdMS_TO_TICKS(2));
    return txDone() ? _link.txStatus(_localRadio) : RADIOLIB_ERR_TX_TIMEOUT;
}

void RemoteRadio::startReceive()
{
    _link.sendFrame(LinkProtocol::MSG_START_RX, (uint8_t)_localRadio, nullptr, 0);
}

// --- RX-priority CSMA path --------------------------------------------------

int16_t RemoteRadio::scanChannel()
{
    // The host cannot sense a remote radio's RF channel; the co-processor runs
    // its own CAD before each startTransmit(). Always report clear here so the
    // scheduler proceeds to startTransmit(); the co-proc backs off if busy.
    return RADIOLIB_CHANNEL_FREE;
}

int16_t RemoteRadio::startTransmit(const uint8_t *buf, size_t len)
{
    // Arm the backpressure gate BEFORE sending so a fast MSG_TX_DONE can't be
    // missed, then hand the frame to the co-processor.
    _link.armTx(_localRadio);
    bool ok = _link.sendFrame(LinkProtocol::MSG_TX, (uint8_t)_localRadio, buf, len);
    return ok ? RADIOLIB_ERR_NONE : RADIOLIB_ERR_UNKNOWN;
}

bool RemoteRadio::txDone()
{
    // True once the co-proc has reported it finished the on-air TX for us.
    return _link.txDone(_localRadio);
}

void RemoteRadio::finishTransmit()
{
    // Nothing to do: the co-processor auto-returns its radio to RX after TX.
}
