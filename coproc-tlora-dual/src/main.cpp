/**
 * T-Lora-Dual co-processor (Stage E)
 * ==================================
 * Drives the two LR1121 radios on the LilyGO T-Lora-Dual (ESP32 PICO-D4) as
 * R3/R4 for the XIAO quad bridge. Speaks the framed UART wire protocol in
 * ../src/LinkProtocol.h (shared with the host) over a hardware UART.
 *
 * Protocol (host <-> co-proc):
 *   host -> coproc : CFG_RADIO, TX, START_RX, PING, RESET
 *   coproc -> host : RX (rssi/snr + bytes), TX_DONE, READY, LOG, PONG
 *
 * HAL precedence: pin map, RF-switch table and the begin()/config sequence are
 * taken from the LilyGO T-Lora-Dual Factory example (the proven-working
 * reference), NOT from the abandoned Wio-LR1121 driver.
 *
 * TX is blocking (radio.transmit() then startReceive()) — same pattern the host
 * WioSX1262 uses; the host paces TX. RX is IRQ-driven (DIO9 -> flag), drained
 * in loop(). Single-threaded, so no locking is needed.
 */

#include <Arduino.h>
#include <SPI.h>
#include <RadioLib.h>
#include "LinkProtocol.h"

using namespace LinkProtocol;

static const size_t LORA_MAX = 256;   // max on-air LoRa payload

// --- Pins — LilyGO T-Lora-Dual Factory (authoritative HAL) ------------------
#define RADIO_SCK   25
#define RADIO_MISO  33
#define RADIO_MOSI  32
#define R3_CS       27
#define R3_DIO9     37
#define R3_RST      26
#define R3_BUSY     36
#define R4_CS       13
#define R4_DIO9     34
#define R4_RST      21
#define R4_BUSY     39

// --- UART link to the XIAO host (override via build flags) ------------------
#ifndef LINK_UART_NUM
  #define LINK_UART_NUM 1
#endif
#ifndef LINK_TX_PIN
  #define LINK_TX_PIN 22   // -> host RX
#endif
#ifndef LINK_RX_PIN
  #define LINK_RX_PIN 23   // <- host TX
#endif
#ifndef LINK_BAUD
  #define LINK_BAUD 460800
#endif

HardwareSerial LinkSerial(LINK_UART_NUM);

// --- Two LR1121 radios: index 0 = R3 (host-local 0), 1 = R4 (host-local 1) --
LR1121 radio_3 = new Module(R3_CS, R3_DIO9, R3_RST, R3_BUSY);
LR1121 radio_4 = new Module(R4_CS, R4_DIO9, R4_RST, R4_BUSY);
LR1121 *g_radio[2]   = { &radio_3, &radio_4 };
volatile bool g_rxFlag[2] = { false, false };
bool          g_enabled[2] = { false, false };

// --- RF switch table — Factory verbatim (DIO5/6 + HF RX/TX on DIO7/8) -------
static const uint32_t rfSwitchPins[] = {
    RADIOLIB_LR11X0_DIO5, RADIOLIB_LR11X0_DIO6,
    RADIOLIB_LR11X0_DIO7, RADIOLIB_LR11X0_DIO8, RADIOLIB_NC };
static const Module::RfSwitchMode_t rfswitch_table[] = {
    // mode              DIO5, DIO6, DIO_HF_RX, DIO_HF_TX
    {LR11x0::MODE_STBY,  {LOW,  LOW,  LOW,  LOW }},
    {LR11x0::MODE_RX,    {LOW,  HIGH, HIGH, LOW }},
    {LR11x0::MODE_TX,    {HIGH, LOW,  LOW,  LOW }},
    {LR11x0::MODE_TX_HP, {HIGH, LOW,  LOW,  LOW }},
    {LR11x0::MODE_TX_HF, {LOW,  LOW,  LOW,  HIGH}},
    {LR11x0::MODE_GNSS,  {LOW,  LOW,  LOW,  LOW }},
    {LR11x0::MODE_WIFI,  {LOW,  LOW,  LOW,  LOW }},
    END_OF_MODE_TABLE,
};

void IRAM_ATTR rx0Isr() { g_rxFlag[0] = true; }
void IRAM_ATTR rx1Isr() { g_rxFlag[1] = true; }

// --- Link TX helpers -------------------------------------------------------
static void linkSend(uint8_t type, uint8_t radio, const uint8_t *payload, size_t len) {
    uint8_t frame[MAX_FRAME];
    size_t n = buildFrame(frame, sizeof(frame), type, radio, payload, len);
    if (n) LinkSerial.write(frame, n);
}
static void linkLog(const char *s) {
    linkSend(MSG_LOG, RADIO_NONE, (const uint8_t *)s, strlen(s));
    Serial.println(s);
}

// --- Configure one LR1121 from a CfgRadio (Factory radio_config sequence) ---
// Returns RADIOLIB_ERR_NONE on success. On any setter error it logs and leaves
// the radio disabled — never halts (a co-processor must stay responsive).
static int16_t applyCfg(int i, const CfgRadio &c) {
    if (c.band == BAND_SBAND) {
        linkLog("S-band requested — disabled stub; radio left off");
        g_enabled[i] = false;
        g_radio[i]->standby();
        return RADIOLIB_ERR_UNKNOWN;
    }
    LR1121 &r = *g_radio[i];
    int16_t st;
    if ((st = r.setFrequency(c.frequency))   != RADIOLIB_ERR_NONE) { linkLog("setFrequency failed");   return st; }
    // NOTE: 2.4 GHz wideLora BW (406.25/812.5/1625) acceptance depends on
    // RadioLib's LR11x0 high-band handling; if this rejects a 2.4 GHz BW,
    // bench-verify whether a high-band begin() variant is needed.
    if ((st = r.setBandwidth(c.bandwidth))   != RADIOLIB_ERR_NONE) { linkLog("setBandwidth failed (2.4G wideLora? bench-verify)"); return st; }
    if ((st = r.setSpreadingFactor(c.sf))    != RADIOLIB_ERR_NONE) { linkLog("setSpreadingFactor failed"); return st; }
    if ((st = r.setCodingRate(c.cr))         != RADIOLIB_ERR_NONE) { linkLog("setCodingRate failed");  return st; }
    // Band-aware power ceiling: LR1121 HF PA (2.4 GHz) tops out ~13 dBm; the
    // sub-GHz HP PA reaches 22 dBm. Clamp the host's requested power.
    int8_t ceiling = (c.frequency >= 1900.0f) ? 13 : 22;
    int8_t pwr = c.txPower;
    if (pwr > ceiling) pwr = ceiling;
    if (pwr < -17)     pwr = -17;
    if ((st = r.setOutputPower(pwr))         != RADIOLIB_ERR_NONE) { linkLog("setOutputPower failed"); return st; }
    if ((st = r.setPreambleLength(c.preambleLen)) != RADIOLIB_ERR_NONE) { linkLog("setPreambleLength failed"); return st; }
    if ((st = r.setSyncWord(c.syncWord))     != RADIOLIB_ERR_NONE) { linkLog("setSyncWord failed");   return st; }
    r.invertIQ(false);
    r.setTCXO(3.3);                 // per Factory (T-Lora-Dual)
    r.startReceive();
    g_enabled[i] = true;

    char m[96];
    snprintf(m, sizeof(m), "R%d cfg ok: %.3f MHz BW%.1f SF%u CR4/%u %ddBm sync0x%02X band%u",
             i + 3, c.frequency, c.bandwidth, (unsigned)c.sf, (unsigned)c.cr,
             (int)pwr, (unsigned)c.syncWord, (unsigned)c.band);
    linkLog(m);
    return RADIOLIB_ERR_NONE;
}

// --- Host frame dispatch ---------------------------------------------------
static void handleHostFrame(uint8_t type, uint8_t radio, const uint8_t *payload, size_t len) {
    switch (type) {
        case MSG_CFG_RADIO:
            if (radio < 2 && len >= sizeof(CfgRadio)) {
                CfgRadio c;
                memcpy(&c, payload, sizeof(c));
                if (c.enable) {
                    applyCfg(radio, c);
                } else {
                    g_enabled[radio] = false;
                    g_radio[radio]->standby();
                }
            }
            break;
        case MSG_TX:
            if (radio < 2 && g_enabled[radio]) {
                int16_t st = g_radio[radio]->transmit(const_cast<uint8_t *>(payload), len);
                g_radio[radio]->startReceive();   // auto-return to RX
                linkSend(MSG_TX_DONE, radio, (const uint8_t *)&st, sizeof(st));
            }
            break;
        case MSG_START_RX:
            if (radio < 2 && g_enabled[radio]) g_radio[radio]->startReceive();
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

// --- UART frame parser (mirror of the host UartLink state machine) ----------
static uint8_t  s_hdr[4];
static uint8_t  s_hdrCount = 0;
static uint16_t s_payLen   = 0;
static uint16_t s_payCount = 0;
static uint8_t  s_buf[MAX_PAYLOAD + CRC_LEN];
enum class PSt : uint8_t { PRE0, PRE1, HDR, PAYLOAD };
static PSt s_st = PSt::PRE0;

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
                uint8_t vb[4 + MAX_PAYLOAD];
                memcpy(vb, s_hdr, 4);
                if (s_payLen) memcpy(vb + 4, s_buf, s_payLen);
                if (crc16(vb, 4 + s_payLen) == rxCrc)
                    handleHostFrame(s_hdr[0], s_hdr[1], s_buf, s_payLen);
                s_st = PSt::PRE0;
            }
            break;
    }
}

void setup() {
    Serial.begin(115200, SERIAL_8N1, 3, 1);   // local USB debug (Factory pins)
    LinkSerial.begin(LINK_BAUD, SERIAL_8N1, LINK_RX_PIN, LINK_TX_PIN);

    pinMode(R3_CS, OUTPUT); digitalWrite(R3_CS, HIGH);
    pinMode(R4_CS, OUTPUT); digitalWrite(R4_CS, HIGH);
    SPI.begin(RADIO_SCK, RADIO_MISO, RADIO_MOSI, R3_CS);
    delay(1000);

    for (int i = 0; i < 2; i++) {
        int16_t st = g_radio[i]->begin();          // Factory: no-arg begin()
        LR11x0VersionInfo_t info;
        g_radio[i]->getVersionInfo(&info);
        g_radio[i]->setRfSwitchTable(rfSwitchPins, rfswitch_table);
        g_radio[i]->setPacketReceivedAction(i == 0 ? rx0Isr : rx1Isr);
        char m[80];
        snprintf(m, sizeof(m), "R%d(LR1121) begin=%d hw=%02X dev=%02X fw=%02X",
                 i + 3, (int)st, info.hardware, info.device, info.fwMajor);
        linkLog(m);
    }

    uint8_t ready[2] = { 1 /*fw version*/, 2 /*radio count*/ };
    linkSend(MSG_READY, RADIO_NONE, ready, sizeof(ready));
    linkLog("T-Lora-Dual co-processor ready (R3/R4 idle until CFG_RADIO)");
}

void loop() {
    // Drain inbound host frames.
    while (LinkSerial.available() > 0) feedByte((uint8_t)LinkSerial.read());

    // Drain received LoRa packets (IRQ-flagged) and forward to the host.
    for (int i = 0; i < 2; i++) {
        if (!g_rxFlag[i]) continue;
        g_rxFlag[i] = false;
        if (!g_enabled[i]) { g_radio[i]->startReceive(); continue; }

        size_t plen = g_radio[i]->getPacketLength();
        if (plen == 0) { g_radio[i]->startReceive(); continue; }
        if (plen > LORA_MAX) plen = LORA_MAX;

        uint8_t data[LORA_MAX];
        int16_t st = g_radio[i]->readData(data, plen);
        if (st == RADIOLIB_ERR_NONE) {
            uint8_t pl[sizeof(RxHeader) + LORA_MAX];
            RxHeader h;
            h.rssi = g_radio[i]->getRSSI();
            h.snr  = g_radio[i]->getSNR();
            memcpy(pl, &h, sizeof(h));
            memcpy(pl + sizeof(h), data, plen);
            linkSend(MSG_RX, (uint8_t)i, pl, sizeof(h) + plen);
        }
        g_radio[i]->startReceive();   // re-arm
    }
}
