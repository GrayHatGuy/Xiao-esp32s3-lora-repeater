// UartLink.h
// ---------------------------------------------------------------------------
// Host side of the XIAO <-> T-Lora-Dual co-processor UART link. Owns one
// HardwareSerial, frames/deframes the LinkProtocol wire format, and runs a
// FreeRTOS service task that:
//   - reassembles inbound frames and validates the CRC,
//   - routes MSG_RX packets to the per-radio queue a RemoteRadio registered,
//   - prints MSG_LOG text on the host Serial,
//   - tracks MSG_READY / MSG_PONG link state.
// Outbound frames go through sendFrame(), serialized by a TX mutex so multiple
// RemoteRadio callers (R3/R4) can share the one link safely.
//
// Wired into the bridge in stage C; in stage B this is standalone scaffolding.
// ---------------------------------------------------------------------------

#pragma once

#include <Arduino.h>
#include <HardwareSerial.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>

#include "LoraRadio.h"     // LORA_MAX_PACKET
#include "LinkProtocol.h"

// One received LoRa packet handed from the link task to a RemoteRadio queue.
struct RxPacket {
    uint8_t  data[LORA_MAX_PACKET];
    uint16_t len;
    float    rssi;
    float    snr;
};

class UartLink {
public:
    // R3 + R4 live on the co-processor (local indices 0 and 1).
    static constexpr int MAX_REMOTE_RADIOS = 2;

    explicit UartLink(HardwareSerial &serial);

    // Open the port and spawn the RX service task. rxPin/txPin are the host
    // GPIOs (-1 to use the port defaults).
    void begin(uint32_t baud, int rxPin, int txPin);

    // A RemoteRadio registers the queue it wants MSG_RX packets delivered to,
    // keyed by its co-proc-local index (0/1). Call before begin().
    void registerRxQueue(int localRadio, QueueHandle_t q);

    // Frame + transmit one message. Thread-safe (TX mutex). Returns true if the
    // whole frame was written.
    bool sendFrame(uint8_t type, uint8_t radio, const uint8_t *payload, size_t len);

    bool     isReady()    const { return _ready; }
    uint32_t lastPongMs() const { return _lastPongMs; }

private:
    static void rxTaskTrampoline(void *arg);
    void rxTask();
    void feedByte(uint8_t b);
    void handleFrame(uint8_t type, uint8_t radio, const uint8_t *payload, size_t len);

    HardwareSerial   &_serial;
    SemaphoreHandle_t _txMutex     = nullptr;
    TaskHandle_t      _taskHandle  = nullptr;
    QueueHandle_t     _rxQueues[MAX_REMOTE_RADIOS] = { nullptr, nullptr };
    volatile bool     _ready       = false;
    volatile uint32_t _lastPongMs  = 0;

    // Inbound framing state machine.
    enum class RxState : uint8_t { PRE0, PRE1, HDR, PAYLOAD };
    RxState  _st = RxState::PRE0;
    uint8_t  _hdr[4]   = {0};      // type, radio, len_lo, len_hi
    uint8_t  _hdrCount = 0;
    uint16_t _payLen   = 0;
    uint16_t _payCount = 0;
    uint8_t  _buf[LinkProtocol::MAX_PAYLOAD + LinkProtocol::CRC_LEN] = {0};
};
