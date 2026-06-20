// UartLink.cpp — see UartLink.h.

#include "UartLink.h"
#include <string.h>

using namespace LinkProtocol;

UartLink::UartLink(HardwareSerial &serial) : _serial(serial) {}

void UartLink::registerRxQueue(int localRadio, QueueHandle_t q) {
    if (localRadio >= 0 && localRadio < MAX_REMOTE_RADIOS) _rxQueues[localRadio] = q;
}

void UartLink::armTx(int localRadio) {
    if (localRadio >= 0 && localRadio < MAX_REMOTE_RADIOS) _txDone[localRadio] = false;
}

bool UartLink::txDone(int localRadio) const {
    if (localRadio < 0 || localRadio >= MAX_REMOTE_RADIOS) return true;
    return _txDone[localRadio];
}

int16_t UartLink::txStatus(int localRadio) const {
    if (localRadio < 0 || localRadio >= MAX_REMOTE_RADIOS) return 0;
    return _txStatus[localRadio];
}

void UartLink::begin(uint32_t baud, int rxPin, int txPin) {
    _txMutex = xSemaphoreCreateMutex();
    configASSERT(_txMutex != nullptr);
    // Enlarge the RX ring so a burst of inbound frames survives even if the RX
    // service task is briefly behind. The default (~256 B) is smaller than one
    // MAX_FRAME (308 B), so a single oversized frame could otherwise be lost.
    _serial.setRxBufferSize(4096);
    _serial.begin(baud, SERIAL_8N1, (int8_t)rxPin, (int8_t)txPin);
    // Service task on core 0 (the bridge pins its per-radio tasks to 0/1; the
    // link task is I/O-bound and light, so it shares core 0 happily).
    xTaskCreatePinnedToCore(rxTaskTrampoline, "UartLink", 4096, this, 2,
                            &_taskHandle, 0);
    Serial.printf("[UartLink] up @ %lu baud (rx=%d tx=%d)\n",
                  (unsigned long)baud, rxPin, txPin);
}

bool UartLink::sendFrame(uint8_t type, uint8_t radio,
                         const uint8_t *payload, size_t len) {
    uint8_t frame[MAX_FRAME];
    size_t n = buildFrame(frame, sizeof(frame), type, radio, payload, len);
    if (n == 0) return false;
    if (_txMutex) xSemaphoreTake(_txMutex, portMAX_DELAY);
    size_t wrote = _serial.write(frame, n);
    if (_txMutex) xSemaphoreGive(_txMutex);
    return wrote == n;
}

void UartLink::rxTaskTrampoline(void *arg) {
    static_cast<UartLink *>(arg)->rxTask();
}

void UartLink::rxTask() {
    for (;;) {
        while (_serial.available() > 0) {
            feedByte((uint8_t)_serial.read());
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

// Byte-at-a-time framing state machine. Resyncs on any malformed frame.
void UartLink::feedByte(uint8_t b) {
    switch (_st) {
        case RxState::PRE0:
            if (b == PRE0) _st = RxState::PRE1;
            break;

        case RxState::PRE1:
            if (b == PRE1)      { _st = RxState::HDR; _hdrCount = 0; }
            else if (b == PRE0) { /* stay — could be the real start */ }
            else                { _st = RxState::PRE0; }
            break;

        case RxState::HDR:
            _hdr[_hdrCount++] = b;
            if (_hdrCount == 4) {
                _payLen = (uint16_t)_hdr[2] | ((uint16_t)_hdr[3] << 8);
                if (_payLen > MAX_PAYLOAD) { _st = RxState::PRE0; break; }  // bogus
                _payCount = 0;
                _st = RxState::PAYLOAD;   // still need payLen + CRC_LEN bytes
            }
            break;

        case RxState::PAYLOAD:
            _buf[_payCount++] = b;
            if (_payCount == (uint16_t)(_payLen + CRC_LEN)) {
                uint16_t rxCrc = (uint16_t)_buf[_payLen] |
                                 ((uint16_t)_buf[_payLen + 1] << 8);
                // CRC covers type, radio, len_lo, len_hi, then payload.
                static uint8_t vb[4 + MAX_PAYLOAD];   // single rxTask; off-stack
                memcpy(vb, _hdr, 4);
                if (_payLen) memcpy(vb + 4, _buf, _payLen);
                uint16_t calc = crc16(vb, 4 + _payLen);
                if (calc == rxCrc) {
                    handleFrame(_hdr[0], _hdr[1], _buf, _payLen);
                }
                _st = RxState::PRE0;
            }
            break;
    }
}

void UartLink::handleFrame(uint8_t type, uint8_t radio,
                           const uint8_t *payload, size_t len) {
    switch (type) {
        case MSG_RX: {
            if (radio < MAX_REMOTE_RADIOS && _rxQueues[radio] &&
                len >= sizeof(RxHeader)) {
                RxHeader rh;
                memcpy(&rh, payload, sizeof(rh));
                size_t dlen = len - sizeof(RxHeader);
                if (dlen > LORA_MAX_PACKET) dlen = LORA_MAX_PACKET;
                RxPacket pkt;
                memcpy(pkt.data, payload + sizeof(RxHeader), dlen);
                pkt.len  = (uint16_t)dlen;
                pkt.rssi = rh.rssi;
                pkt.snr  = rh.snr;
                xQueueSend(_rxQueues[radio], &pkt, 0);   // drop if full
            }
            break;
        }
        case MSG_LOG:
            Serial.printf("[coproc] %.*s\n", (int)len, (const char *)payload);
            break;
        case MSG_READY:
            _ready = true;
            _readyGen++;   // co-proc (re)booted — host re-pushes CFG on this edge
            Serial.printf("[UartLink] co-processor READY (%u B info)\n",
                          (unsigned)len);
            break;
        case MSG_PONG:
            _lastPongMs = millis();
            break;
        case MSG_TX_DONE:
            // Co-proc finished an on-air TX for this radio: release the
            // backpressure gate so the next queued frame may be sent.
            if (radio < MAX_REMOTE_RADIOS) {
                if (len >= sizeof(int16_t))
                    memcpy((void *)&_txStatus[radio], payload, sizeof(int16_t));
                _txDone[radio] = true;
            }
            break;
        default:
            break;
    }
}
