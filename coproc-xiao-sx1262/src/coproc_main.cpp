/**
 * Xiao dual-SX1262 radio CO-PROCESSOR (v8.5 stage E)
 * ==================================================
 * Runs on the SECOND Xiao of a "2xiao_4sx1262" bridge. Drives its two local
 * SX1262 radios as the host's R3/R4, speaking the framed UART wire protocol in
 * ../src/LinkProtocol.h (shared verbatim with the host so the two ends never
 * drift) over the UART crossover.
 *
 * Protocol (host <-> co-proc):
 *   host  -> coproc : CFG_RADIO, TX, START_RX, PING, RESET
 *   coproc -> host  : RX (rssi/snr + bytes), TX_DONE, READY, LOG, PONG
 *
 * The radios are the SAME hardware as the host's R1/R2 (Wio SX1262 on the B2B
 * header + the "Wio-SX1262 for XIAO" edge module), so this reuses the proven
 * WioSX1262 driver (../src/WioSX1262.cpp) — including its DIO2/ANT_SW RF-switch
 * handling and the non-blocking TX trio. A CFG_RADIO re-tunes a radio at runtime
 * via WioSX1262::setConfig() + begin() (no reconstruction, so the static ISR
 * registration is preserved).
 *
 * TX is NON-BLOCKING: each MSG_TX is queued and dispatched one-at-a-time with
 * startTransmit() + the TxDone IRQ, so loop() keeps draining the UART during the
 * on-air time. CAD (scanChannel, listen-before-talk) gates every dispatch. The
 * host gates each MSG_TX on the MSG_TX_DONE we send back, so the queue normally
 * holds at most one job. Single-threaded — no locking needed.
 *
 * Console: native USB-CDC `Serial` is the debug console; the inter-board link is
 * `Serial1` on D6/D7. Define COPROC_PLAINTEXT_DEBUG to also echo LOG text to USB.
 *
 * Build/flash:  pio run -d coproc-xiao-sx1262 -e xiao_coproc_sx1262 -t upload --upload-port COMx
 */

#include <Arduino.h>
#include <SPI.h>
#include "WioSX1262.h"     // reused host driver (LoraRadio.h, RadioLib via -I ../src)
#include "LinkProtocol.h"  // shared wire protocol (single source of truth, -I ../src)

using namespace LinkProtocol;

static const size_t LORA_MAX = LORA_MAX_PACKET;   // 256

// --- Shared SPI bus — XIAO ESP32S3 default (same as the host) ----------------
#define SPI_SCK   7   // D8
#define SPI_MOSI  9   // D10
#define SPI_MISO  8   // D9

// --- Radio pin maps — identical hardware to the host R1/R2 -------------------
// Co-proc radio[0] = host R3 = the B2B-header SX1262 (fixed silicon).
#define CR3_NSS    41
#define CR3_DIO1   39
#define CR3_RESET  42
#define CR3_BUSY   40
#define CR3_ANT_SW 38

// Co-proc radio[1] = host R4 = the "Wio-SX1262 for XIAO" edge module. Its pinout
// changed between silkscreen revisions; pick the matching build env
// (xiao_coproc_sx1262 = V1.0 default, xiao_coproc_sx1262_v1_1 = V1.1).
#ifndef WIO_SX1262_REV
  #define WIO_SX1262_REV 10
#endif
#if WIO_SX1262_REV == 11
  #define CR4_NSS    4   // D3 / GPIO4
  #define CR4_DIO1   1   // D0 / GPIO1
  #define CR4_RESET  3   // D2 / GPIO3
  #define CR4_BUSY   2   // D1 / GPIO2
  #define CR4_ANT_SW 5   // D4 / GPIO5
  #define WIO_SX1262_REV_STR "V1.1"
#else
  #define CR4_NSS    5   // D4 / GPIO5
  #define CR4_DIO1   2   // D1 / GPIO2
  #define CR4_RESET  3   // D2 / GPIO3
  #define CR4_BUSY   4   // D3 / GPIO4
  #define CR4_ANT_SW 6   // D5 / GPIO6
  #define WIO_SX1262_REV_STR "V1.0"
#endif

// --- UART link to the host — Serial1 on D6/D7, wired CROSSED ------------------
#ifndef LINK_TX_PIN
  #define LINK_TX_PIN 43      // D6 — to the host's RX (its D7)
#endif
#ifndef LINK_RX_PIN
  #define LINK_RX_PIN 44      // D7 — from the host's TX (its D6)
#endif
#ifndef LINK_BAUD
  #define LINK_BAUD   460800UL
#endif
HardwareSerial &LinkSerial = Serial1;

// --- Two SX1262 radios: index 0 = R3, 1 = R4 --------------------------------
static SemaphoreHandle_t s_spiMutex = nullptr;
static WioSX1262        *g_radio[2]   = { nullptr, nullptr };
static bool              g_enabled[2] = { false, false };

// --- Non-blocking TX state ---------------------------------------------------
static bool     g_txActive[2]  = { false, false };   // a startTransmit() is outstanding
static uint32_t g_txStartMs[2] = { 0, 0 };
static const uint32_t TX_DONE_TIMEOUT_MS = 8000;     // belt-and-suspenders

struct TxJob { uint8_t data[LORA_MAX]; uint16_t len; };
static const int TX_Q_DEPTH = 4;   // host gates 1-in-flight, so this is slack
static TxJob   g_txq[2][TX_Q_DEPTH];
static uint8_t g_txqHead[2]  = { 0, 0 };
static uint8_t g_txqTail[2]  = { 0, 0 };
static uint8_t g_txqCount[2] = { 0, 0 };

static bool txqPush(int i, const uint8_t *d, size_t len) {
    if (len > LORA_MAX) len = LORA_MAX;
    if (g_txqCount[i] >= TX_Q_DEPTH) return false;
    TxJob &j = g_txq[i][g_txqTail[i]];
    memcpy(j.data, d, len);
    j.len = (uint16_t)len;
    g_txqTail[i] = (uint8_t)((g_txqTail[i] + 1) % TX_Q_DEPTH);
    g_txqCount[i]++;
    return true;
}
static TxJob *txqPeek(int i) { return g_txqCount[i] ? &g_txq[i][g_txqHead[i]] : nullptr; }
static void   txqPop(int i)  {
    if (!g_txqCount[i]) return;
    g_txqHead[i] = (uint8_t)((g_txqHead[i] + 1) % TX_Q_DEPTH);
    g_txqCount[i]--;
}

// --- Link TX helpers ---------------------------------------------------------
static void linkSend(uint8_t type, uint8_t radio, const uint8_t *payload, size_t len) {
    uint8_t frame[MAX_FRAME];
    size_t n = buildFrame(frame, sizeof(frame), type, radio, payload, len);
    if (n) LinkSerial.write(frame, n);
}
static void linkLog(const char *s) {
    linkSend(MSG_LOG, RADIO_NONE, (const uint8_t *)s, strlen(s));
#ifdef COPROC_PLAINTEXT_DEBUG
    Serial.println(s);
#endif
}

// --- Configure one radio from a CfgRadio (re-tune in place) ------------------
// Returns RADIOLIB_ERR_NONE on success. On error it logs and leaves the radio
// disabled — never halts (a co-processor must stay responsive).
static int16_t applyCfg(int i, const CfgRadio &c) {
    if (!g_radio[i]) { linkLog("CFG for an absent radio"); return RADIOLIB_ERR_UNKNOWN; }
    // SX1262 is sub-GHz only.
    LoraConfig cfg;
    cfg.frequency    = c.frequency;
    cfg.bandwidth    = c.bandwidth;
    cfg.spreadFactor = c.sf;
    cfg.codingRate   = c.cr;
    cfg.syncWord     = c.syncWord;
    cfg.txPower      = c.txPower;
    cfg.preambleLen  = c.preambleLen ? c.preambleLen : LORA_PREAMBLE_LEN;
    cfg.tcxoVoltage  = LORA_TCXO_VOLTAGE;   // Wio SX1262 module TCXO (1.8 V)

    g_radio[i]->setConfig(cfg);
    g_txActive[i] = false;
    if (!g_radio[i]->begin()) {             // re-tune; ISR stays registered
        g_enabled[i] = false;
        linkLog("radio begin() failed during CFG");
        return RADIOLIB_ERR_UNKNOWN;
    }
    g_radio[i]->startReceive();
    g_enabled[i] = true;

    char m[112];
    snprintf(m, sizeof(m), "R%d cfg ok: %.3f MHz BW%.1f SF%u CR4/%u %ddBm sync0x%02X",
             i + 3, c.frequency, c.bandwidth, (unsigned)c.sf, (unsigned)c.cr,
             (int)c.txPower, (unsigned)c.syncWord);
    linkLog(m);
    return RADIOLIB_ERR_NONE;
}

// --- Host frame dispatch -----------------------------------------------------
static void handleHostFrame(uint8_t type, uint8_t radio, const uint8_t *payload, size_t len) {
    switch (type) {
        case MSG_CFG_RADIO:
            if (radio < 2 && len >= sizeof(CfgRadio)) {
                CfgRadio c;
                memcpy(&c, payload, sizeof(c));
                if (c.enable) {
                    applyCfg(radio, c);
                } else if (g_radio[radio]) {
                    g_enabled[radio] = false;
                    g_txActive[radio] = false;
                    g_radio[radio]->startReceive();   // idle-listen on last plan
                }
            }
            break;
        case MSG_TX: {
            // Enqueue only — dispatched non-blocking in loop() after CAD. EVERY
            // MSG_TX must be answered by exactly one MSG_TX_DONE or the host's
            // gate wedges: an accepted frame gets it on completion in loop(); a
            // rejected frame (radio disabled / queue full) is answered here so
            // the host releases the gate immediately.
            if (radio >= 2) break;
            int16_t st = RADIOLIB_ERR_NONE;
            if (!g_enabled[radio]) {
                st = RADIOLIB_ERR_WRONG_MODEM;
                linkLog("MSG_TX for a radio that is not enabled — rejected");
            } else if (!txqPush(radio, payload, len)) {
                st = RADIOLIB_ERR_TX_TIMEOUT;
                linkLog("TX queue full — frame dropped");
            }
            if (st != RADIOLIB_ERR_NONE)
                linkSend(MSG_TX_DONE, radio, (const uint8_t *)&st, sizeof(st));
            break;
        }
        case MSG_START_RX:
            // Never re-arm RX mid-TX: that would abort the transmit and the
            // TxDone IRQ would never fire. The TX path returns to RX itself.
            if (radio < 2 && g_enabled[radio] && g_radio[radio] && !g_txActive[radio])
                g_radio[radio]->startReceive();
            break;
        case MSG_PING: {
            uint32_t up = millis();
            linkSend(MSG_PONG, RADIO_NONE, (const uint8_t *)&up, sizeof(up));
            break;
        }
        case MSG_RESET:
            ESP.restart();
            break;
        default:
            break;
    }
}

// --- UART frame parser (mirror of the host UartLink state machine) -----------
static uint8_t  s_hdr[4];
static uint8_t  s_hdrCount = 0;
static uint16_t s_payLen   = 0;
static uint16_t s_payCount = 0;
static uint8_t  s_buf[MAX_PAYLOAD + CRC_LEN];
enum class PSt : uint8_t { PRE0, PRE1, HDR, PAYLOAD };
static PSt s_st = PSt::PRE0;

#ifdef COPROC_PLAINTEXT_DEBUG
static uint32_t g_linkRxBytes = 0;   // total bytes seen on the link RX pin (diag)
static uint32_t g_lastDiagMs  = 0;
#endif

static void feedByte(uint8_t b) {
    switch (s_st) {
        case PSt::PRE0:
            if (b == PRE0) s_st = PSt::PRE1;
            break;
        case PSt::PRE1:
            if (b == PRE1)      { s_st = PSt::HDR; s_hdrCount = 0; }
            else if (b != PRE0) { s_st = PSt::PRE0; }
            break;
        case PSt::HDR:
            s_hdr[s_hdrCount++] = b;
            if (s_hdrCount == 4) {
                s_payLen = (uint16_t)s_hdr[2] | ((uint16_t)s_hdr[3] << 8);
                if (s_payLen > MAX_PAYLOAD) { s_st = PSt::PRE0; break; }
                s_payCount = 0;
                s_st = PSt::PAYLOAD;
            }
            break;
        case PSt::PAYLOAD:
            s_buf[s_payCount++] = b;
            if (s_payCount == (uint16_t)(s_payLen + CRC_LEN)) {
                uint16_t rxCrc = (uint16_t)s_buf[s_payLen] |
                                 ((uint16_t)s_buf[s_payLen + 1] << 8);
                static uint8_t vb[4 + MAX_PAYLOAD];   // single-threaded; off-stack
                memcpy(vb, s_hdr, 4);
                if (s_payLen) memcpy(vb + 4, s_buf, s_payLen);
                if (crc16(vb, 4 + s_payLen) == rxCrc)
                    handleHostFrame(s_hdr[0], s_hdr[1], s_buf, s_payLen);
                s_st = PSt::PRE0;
            }
            break;
    }
}

static LoraConfig defaultConfig() {
    // Valid sub-GHz placeholder so begin() succeeds before the first CFG_RADIO;
    // the host re-tunes each radio immediately on connect.
    LoraConfig c;
    c.frequency = 906.875f; c.bandwidth = 250.0f; c.spreadFactor = 11;
    c.codingRate = 5; c.syncWord = 0x2B; c.txPower = 20;
    c.preambleLen = LORA_PREAMBLE_LEN; c.tcxoVoltage = LORA_TCXO_VOLTAGE;
    return c;
}

void setup() {
    Serial.begin(115200);
    LinkSerial.setRxBufferSize(4096);   // host fire-and-forwards TX while we are on-air
    LinkSerial.begin(LINK_BAUD, SERIAL_8N1, LINK_RX_PIN, LINK_TX_PIN);

    // Park both chip-selects HIGH before constructing/probing either radio so an
    // idle chip can't drive the shared MISO during the other's begin().
    pinMode(CR3_NSS, OUTPUT); digitalWrite(CR3_NSS, HIGH);
    pinMode(CR4_NSS, OUTPUT); digitalWrite(CR4_NSS, HIGH);
    SPI.begin(SPI_SCK, SPI_MISO, SPI_MOSI);

    s_spiMutex = xSemaphoreCreateMutex();
    configASSERT(s_spiMutex != nullptr);

    LoraConfig dc = defaultConfig();
    g_radio[0] = new WioSX1262(CR3_NSS, CR3_DIO1, CR3_RESET, CR3_BUSY, CR3_ANT_SW,
                               SPI, s_spiMutex, "CoProc-R3-B2B", dc);
    g_radio[1] = new WioSX1262(CR4_NSS, CR4_DIO1, CR4_RESET, CR4_BUSY, CR4_ANT_SW,
                               SPI, s_spiMutex, "CoProc-R4-Edge", dc);

    // Hold R4 (edge) in reset while R3 (B2B) is brought up, so a contending/
    // mis-mapped edge module cannot corrupt R3 detection on the shared bus.
    pinMode(CR4_RESET, OUTPUT); digitalWrite(CR4_RESET, LOW);
    delay(150);

    bool ok3 = g_radio[0]->begin();
    if (ok3) { g_radio[0]->startReceive(); g_enabled[0] = true; }
    else linkLog("R3 (B2B) begin() failed at boot");

    digitalWrite(CR4_RESET, HIGH);   // release R4 now that R3 is up
    delay(10);
    bool ok4 = g_radio[1]->begin();
    if (ok4) { g_radio[1]->startReceive(); g_enabled[1] = true; }
    else linkLog("R4 (edge) begin() failed at boot — check module revision/wiring");

    uint8_t ready[2] = { 1 /*fw*/, 2 /*radio count*/ };
    linkSend(MSG_READY, RADIO_NONE, ready, sizeof(ready));

    char m[96];
    snprintf(m, sizeof(m),
             "Xiao SX1262 co-processor ready (R3=%s R4=%s %s) — idle until CFG_RADIO",
             ok3 ? "up" : "down", ok4 ? "up" : "down", WIO_SX1262_REV_STR);
    linkLog(m);
}

void loop() {
    // Drain inbound host frames first — keeps running while a radio is on-air.
    while (LinkSerial.available() > 0) {
        uint8_t b = (uint8_t)LinkSerial.read();
#ifdef COPROC_PLAINTEXT_DEBUG
        g_linkRxBytes++;
        Serial.printf("%02X ", b);   // raw hex of every link RX byte (diag)
#endif
        feedByte(b);
    }
#ifdef COPROC_PLAINTEXT_DEBUG
    if ((uint32_t)(millis() - g_lastDiagMs) > 3000) {
        g_lastDiagMs = millis();
        Serial.printf("[diag] link RX bytes total=%lu\n", (unsigned long)g_linkRxBytes);
    }
#endif

    for (int i = 0; i < 2; i++) {
        WioSX1262 *r = g_radio[i];
        if (!r) continue;

        // (1) TX in flight: wait for the TxDone IRQ (or the safety timeout).
        if (g_txActive[i]) {
            bool done     = r->txDone();
            bool timedOut = (uint32_t)(millis() - g_txStartMs[i]) > TX_DONE_TIMEOUT_MS;
            if (done || timedOut) {
                r->finishTransmit();              // completes TX + returns to RX
                g_txActive[i] = false;
                int16_t st = timedOut && !done ? RADIOLIB_ERR_TX_TIMEOUT
                                               : RADIOLIB_ERR_NONE;
                linkSend(MSG_TX_DONE, (uint8_t)i, (const uint8_t *)&st, sizeof(st));
            }
            continue;   // nothing else for this radio until TX settles
        }

        // (2) RX: a received packet -> forward to the host.
        if (g_enabled[i] && r->available()) {
            uint8_t data[LORA_MAX];
            size_t  len = sizeof(data);
            float   rssi = 0, snr = 0;
            int16_t st = r->read(data, len, &rssi, &snr);
            if (st == RADIOLIB_ERR_NONE && len) {
                uint8_t pl[sizeof(RxHeader) + LORA_MAX];
                RxHeader h; h.rssi = rssi; h.snr = snr;
                memcpy(pl, &h, sizeof(h));
                memcpy(pl + sizeof(h), data, len);
                linkSend(MSG_RX, (uint8_t)i, pl, sizeof(h) + len);
            } else if (st != RADIOLIB_ERR_NONE) {
                char m[80];
                snprintf(m, sizeof(m), "R%d RX read err=%d rssi=%.1f snr=%.1f",
                         i + 3, (int)st, rssi, snr);
                linkLog(m);
            }
            r->startReceive();   // re-arm
            continue;
        }

        // (3) TX dispatch: CAD (listen-before-talk) then non-blocking TX.
        TxJob *job = g_enabled[i] ? txqPeek(i) : nullptr;
        if (job) {
            int16_t cad = r->scanChannel();
            if (cad == RADIOLIB_LORA_DETECTED) {
                r->startReceive();   // busy: stay in RX, retry next loop (CSMA)
            } else {
                int16_t st = r->startTransmit(job->data, job->len);
                if (st == RADIOLIB_ERR_NONE) {
                    g_txActive[i]  = true;
                    g_txStartMs[i] = millis();
                    txqPop(i);
                } else {
                    txqPop(i);
                    r->startReceive();
                    linkSend(MSG_TX_DONE, (uint8_t)i, (const uint8_t *)&st, sizeof(st));
                }
            }
        }
    }
}
