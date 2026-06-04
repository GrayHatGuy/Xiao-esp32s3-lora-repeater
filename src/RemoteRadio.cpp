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
    // Fire-and-forget: the co-processor auto-returns its radio to RX after TX
    // (mirrors WioSX1262::transmit). MSG_TX_DONE tracking can land later.
    bool ok = _link.sendFrame(LinkProtocol::MSG_TX, (uint8_t)_localRadio, buf, len);
    return ok ? RADIOLIB_ERR_NONE : RADIOLIB_ERR_UNKNOWN;
}

void RemoteRadio::startReceive()
{
    _link.sendFrame(LinkProtocol::MSG_START_RX, (uint8_t)_localRadio, nullptr, 0);
}
