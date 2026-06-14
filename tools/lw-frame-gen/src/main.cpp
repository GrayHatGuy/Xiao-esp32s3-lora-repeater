// lw-frame-gen — LoRaWAN (sync 0x34) test-frame generator for the v8.3 bench.
// -----------------------------------------------------------------------------
// Transmits the canonical hand-built LoRaWAN frames from BENCH-v8.3.md so the
// v8.3 LoRaWAN capture / summary / relay / flood features can be exercised
// WITHOUT a real LoRaWAN gateway or end-device. The bridge only reads the
// cleartext MAC header, so an unencrypted hand-built frame is sufficient.
//
// Hardware: a Xiao ESP32-S3 + Wio-SX1262 (one of the bridges, temporarily).
// Transmits on Radio 1 (the B2B-header radio) — same pins/SPI/TCXO/RF-switch as
// the bridge, so the on-air signal is identical to what a bridge would relay.
//
// Frame bytes are decode-verified against extractLoRaWANMeta() (MeshDecoderDebug.h).
// -----------------------------------------------------------------------------
#include <Arduino.h>
#include <RadioLib.h>

// ---- RF profile (overridable via -D in platformio.ini) ----------------------
#ifndef GEN_FREQ_MHZ
  #define GEN_FREQ_MHZ  904.6f
#endif
#ifndef GEN_BW_KHZ
  #define GEN_BW_KHZ    125.0f
#endif
#ifndef GEN_SF
  #define GEN_SF        7
#endif
#ifndef GEN_CR
  #define GEN_CR        5
#endif
#ifndef GEN_TXP
  #define GEN_TXP       20
#endif
#ifndef GEN_AUTO_MS
  #define GEN_AUTO_MS   0          // 0 = manual (serial/button); >0 = auto-cycle
#endif

// ---- Xiao ESP32-S3 + Wio-SX1262 Radio 1 (B2B) pins — match the bridge -------
#define PIN_SCK     7
#define PIN_MISO    8
#define PIN_MOSI    9
#define PIN_NSS     41
#define PIN_DIO1    39
#define PIN_RST     42
#define PIN_BUSY    40
#define PIN_ANT_SW  38           // external antenna switch: HIGH = TX path
#define PIN_BOOT    0            // BOOT button (active low)
// Radio 2 shares this SPI bus. Its chip-select MUST be parked HIGH before we
// probe Radio 1, or Radio 2 drives MISO and Radio 1 reads as not-found (rc=-2).
#define PIN_R2_NSS  5            // Radio 2 chip-select (edge header) — park HIGH

// Same Module wiring + SPI settings the bridge uses (WioSX1262.cpp).
SX1262 radio = new Module(PIN_NSS, PIN_DIO1, PIN_RST, PIN_BUSY, SPI,
                          SPISettings(1000000, MSBFIRST, SPI_MODE0));

// ---- Canonical bench frames (verified vs extractLoRaWANMeta) ----------------
static const uint8_t FRAME_U[]  = {0x40,0x8A,0x1F,0x01,0x26,0x00,0x01,0x00,0x02,0xAA,0xBB,0x11,0x22,0x33,0x44};
static const uint8_t FRAME_U2[] = {0x40,0x8A,0x1F,0x01,0x26,0x00,0x02,0x00,0x02,0xAA,0xBB,0x11,0x22,0x33,0x44};
static const uint8_t FRAME_D[]  = {0x80,0x8A,0x1F,0x01,0x26,0x00,0x07,0x00,0x99,0x88,0x77,0x66};
static const uint8_t FRAME_J[]  = {0x00,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x3C,0x2B,0x1A,0x00,0x0B,0xA3,0x04,0x00,0x55,0x66,0xDE,0xAD,0xBE,0xEF};
static const uint8_t FRAME_C[]  = {0x40,0x8A,0x1F,0x01,0x26,0x00,0x01,0x00,0x02,0x99,0x88};

struct Frame { const char *name; const uint8_t *bytes; size_t len; const char *expect; };
static const Frame FRAMES[] = {
  {"FRAME-U  (UnconfDataUp)", FRAME_U,  sizeof(FRAME_U),  "proto=LW mtype=UnconfDataUp devaddr=0x26011f8a fcnt=1 fport=2"},
  {"FRAME-U2 (UnconfDataUp)", FRAME_U2, sizeof(FRAME_U2), "proto=LW mtype=UnconfDataUp devaddr=0x26011f8a fcnt=2 fport=2 (distinct hash)"},
  {"FRAME-D  (ConfDataUp)",   FRAME_D,  sizeof(FRAME_D),  "proto=LW mtype=ConfDataUp devaddr=0x26011f8a fcnt=7 fport=-1"},
  {"FRAME-J  (JoinRequest)",  FRAME_J,  sizeof(FRAME_J),  "proto=LW mtype=JoinRequest; summary DevEUI 0004a30b001a2b3c"},
  {"FRAME-C  (short->fail)",  FRAME_C,  sizeof(FRAME_C),  "proto=LW parse=fail (len 11 < 12)"},
};
static const int NFRAMES = sizeof(FRAMES) / sizeof(FRAMES[0]);
static int g_idx = 0;

static void sendFrame(int i) {
  const Frame &f = FRAMES[i];
  digitalWrite(PIN_ANT_SW, HIGH);                     // TX path
  int st = radio.transmit((uint8_t *)f.bytes, f.len);
  digitalWrite(PIN_ANT_SW, LOW);
  Serial.printf("[gen] TX %s  %u B  rc=%d  -> DUT should log: %s\n",
                f.name, (unsigned)f.len, st, f.expect);
  if (st != RADIOLIB_ERR_NONE)
    Serial.printf("[gen] WARNING: transmit rc=%d (non-zero = TX error)\n", st);
}

void setup() {
  Serial.begin(115200);
  delay(300);
  pinMode(PIN_ANT_SW, OUTPUT); digitalWrite(PIN_ANT_SW, LOW);
  pinMode(PIN_BOOT, INPUT_PULLUP);
  // Park Radio 2's CS HIGH so it stays off the shared SPI bus while we probe
  // Radio 1 (mirrors the bridge's WioSX1262 constructor; without it, begin()
  // returns rc=-2 RADIOLIB_ERR_CHIP_NOT_FOUND).
  pinMode(PIN_R2_NSS, OUTPUT); digitalWrite(PIN_R2_NSS, HIGH);
  SPI.begin(PIN_SCK, PIN_MISO, PIN_MOSI, PIN_NSS);

  Serial.println("\n=== lw-frame-gen — LoRaWAN 0x34 test-frame generator ===");
  Serial.printf("[gen] RF: %.3f MHz  BW %.1f kHz  SF%d  CR4/%d  %d dBm  sync 0x34  preamble 8  TCXO 1.8V\n",
                (float)GEN_FREQ_MHZ, (float)GEN_BW_KHZ, (int)GEN_SF, (int)GEN_CR, (int)GEN_TXP);
  Serial.println("[gen] ^ MUST match the DUT's LoRaWAN radio exactly or it will not demodulate.");

  int st = radio.begin((float)GEN_FREQ_MHZ, (float)GEN_BW_KHZ, (uint8_t)GEN_SF,
                       (uint8_t)GEN_CR, 0x34, (int8_t)GEN_TXP, 8, 1.8f);
  if (st != RADIOLIB_ERR_NONE) {
    Serial.printf("[gen] radio.begin() FAILED rc=%d — halting. Check the Wio-SX1262 on the B2B header.\n", st);
    while (true) delay(1000);
  }
  radio.setDio2AsRfSwitch(true);                      // Wio module internal RF switch

  Serial.println("[gen] radio ready. Commands (type in this monitor):");
  Serial.println("        1=FRAME-U  2=FRAME-U2  3=FRAME-D  4=FRAME-J  5=FRAME-C");
  Serial.println("        n = next in cycle   (the BOOT button also sends next)");
  Serial.println("        LW-LOOP/dedup: send the same number twice within 60 s -> DUT logs capture then drop=lw-dup");
#if GEN_AUTO_MS
  Serial.printf("[gen] AUTO mode ON: sending the next frame every %d ms.\n", (int)GEN_AUTO_MS);
#endif
}

void loop() {
  // Serial command: specific frame (1-5) or next (n).
  if (Serial.available()) {
    char c = (char)Serial.read();
    if (c >= '1' && c <= '5')      { g_idx = c - '1'; sendFrame(g_idx); }
    else if (c == 'n' || c == 'N') { g_idx = (g_idx + 1) % NFRAMES; sendFrame(g_idx); }
  }
  // BOOT button -> next in cycle (falling-edge, simple debounce).
  static bool prev = HIGH;
  bool now = digitalRead(PIN_BOOT);
  if (prev == HIGH && now == LOW) { g_idx = (g_idx + 1) % NFRAMES; sendFrame(g_idx); delay(50); }
  prev = now;
#if GEN_AUTO_MS
  static uint32_t last = 0;
  if (millis() - last >= (uint32_t)GEN_AUTO_MS) { last = millis(); g_idx = (g_idx + 1) % NFRAMES; sendFrame(g_idx); }
#endif
  delay(5);
}
