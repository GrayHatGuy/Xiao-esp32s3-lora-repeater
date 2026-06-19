// RemoteRadio.h
// ---------------------------------------------------------------------------
// A LoraRadio whose hardware lives on the T-Lora-Dual co-processor, reached
// over a UartLink. R3/R4 become two more radio slots indistinguishable to the
// bridge core from the SX1262 radios — same LoraRadio interface.
//
//   begin()        -> CFG_RADIO (+ START_RX) over the link
//   available()    -> packets waiting in this radio's RX queue
//   read()         -> pop one queued packet (filled by UartLink's RX task)
//   transmit()     -> TX frame over the link (fire-and-forget; co-proc auto-RX)
//   startReceive() -> START_RX over the link
//
// `band` is a LinkProtocol::Band (== BridgeConfig::Band value) so the co-proc
// can select the LR1121 sub-GHz vs 2.4 GHz path; S-band is a stub there.
// ---------------------------------------------------------------------------

#pragma once

#include <Arduino.h>
#include "LoraRadio.h"
#include "UartLink.h"

class RemoteRadio : public LoraRadio {
public:
    RemoteRadio(UartLink &link, int localRadio, const char *name,
                const LoraConfig &config, uint8_t band);
    ~RemoteRadio() override;

    bool    begin() override;
    bool    available() override;
    int16_t read(uint8_t *buf, size_t &len, float *rssi, float *snr) override;
    int16_t transmit(const uint8_t *buf, size_t len) override;
    void    startReceive() override;

    // RX-priority CSMA extensions — see LoraRadio.h. CAD happens on the
    // co-processor (the host can't sense a remote radio's channel), so
    // scanChannel() always reports clear; the real gate is txDone(), which
    // tracks the co-proc's MSG_TX_DONE for this radio.
    int16_t scanChannel() override;
    int16_t startTransmit(const uint8_t *buf, size_t len) override;
    bool    txDone() override;
    void    finishTransmit() override;

private:
    static constexpr int      RX_QUEUE_DEPTH      = 8;
    // Upper bound on how long the blocking transmit() fallback waits for the
    // co-proc's MSG_TX_DONE before giving up (matches the co-proc safety
    // timeout). The non-blocking pipeline uses its own scheduler timeout.
    static constexpr uint32_t TX_DONE_TIMEOUT_MS  = 8000;

    UartLink     &_link;
    int           _localRadio;   // co-proc-local index: 0 = R3, 1 = R4
    const char   *_name;
    LoraConfig    _config;
    uint8_t       _band;         // LinkProtocol::Band
    QueueHandle_t _rxQueue = nullptr;
};
