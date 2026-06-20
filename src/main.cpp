/**
 * main.cpp
 * ========
 * Dual Wio SX1262 cross-protocol LoRa bridge for the XIAO ESP32S3.
 *
 *   R1/R2 = SX1262 on the shared XIAO SPI bus (WioSX1262).
 *
 * Bridge logic (RX-priority pipeline, V8.2-SPEC.md): each radio's RX is
 * repeated to the OTHER radio with the existing cross-protocol translation —
 * the historical R1<->R2 crossover. A received packet is decoded ONCE into a
 * clean body, run through the content-hash loop/dup guard, re-encoded for the
 * destination protocol and PUSHED onto that destination's RouteQueue; the
 * destination radio's task is the only thing that pops, CAD-gates, and
 * transmits — so a slow TX never blocks any radio's RX.
 *
 * Loop prevention no longer uses a prepended "[MT]/[MC]/[rns]" marker: the far
 * side receives the CLEAN body. DedupCache remembers the content hash of every
 * packet received AND every packet emitted, so an echo of our own emission (or
 * a mesh-flood replay, or the same packet heard twice) is dropped by hash.
 *
 * All LoRa RF settings (frequency, BW, SF, CR, power, sync word) are resolved
 * at runtime from BridgeConfig (NVS / captive portal). The platformio.ini
 * LORA_RADIO*_* build flags only seed first-boot defaults.
 */

#include <Arduino.h>
#include <SPI.h>
#include <stdint.h>
#include <stdarg.h>
#include "WioSX1262.h"
#include "MeshDecoderDebug.h"
#include "MeshEncoderDebug.h"
#include "LoRaWANCrypto.h"     // ABP: AES-CMAC + LoRaWAN ABP uplink encoder
#include "LoRaWANConfig.h"     // ABP P2: per-source ABP device table + NVS FCnt
#include "MeshCoreConfig.h"
#include "MeshtasticConfig.h"
#include "BridgeConfig.h"
#include "CaptivePortal.h"
#include "NodeDB.h"
#include "DedupCache.h"        // content-hash loop/dup guard (replaces marker)
#include "RouteQueue.h"        // per-destination outbound queue
#include "VirtualNodeMap.h"    // MC->MT source-identity (virtual MT nodes)
#include "RegionPreset.h"
#include "LoraConfigCheck.h"   // compile-time validation of LORA_RADIO* flags
#include "UartLink.h"          // inter-XIAO UART crossover transport (v8.5)
#include "RemoteRadio.h"       // R3/R4 = SX1262 on the second XIAO over the link
#include "SerialLog.h"         // one serialized console path (anti-garble)
#include <esp_mac.h>

// Build a LoraConfig for one radio (index 0..NUM_RADIOS-1) from the live
// BridgeConfig. For a remote radio (R3/R4) it is carried to the co-processor in
// the CFG_RADIO frame; tcxoVoltage is then a no-op (the co-proc owns its TCXO).
static LoraConfig makeLoraConfig(int radio)
{
    LoraConfig c;
    c.frequency    = BridgeConfig::radioFrequency(radio);
    c.bandwidth    = BridgeConfig::radioBandwidth(radio);
    c.spreadFactor = BridgeConfig::radioSf(radio);
    c.codingRate   = BridgeConfig::radioCr(radio);
    c.syncWord     = BridgeConfig::radioSyncWord(radio);
    c.txPower      = BridgeConfig::radioTxPower(radio);
    c.preambleLen  = LORA_PREAMBLE_LEN;
    c.tcxoVoltage  = LORA_TCXO_VOLTAGE;     // Wio SX1262 module property
    return c;
}

// Derive a unique default Meshtastic node ID from the ESP32 MAC so every
// vanilla device is distinct out of the box (v8 spec §5). Called on first
// boot only; the captive-portal SSID follows automatically since it keys off
// mtNodeId(). The user can still override identity in the portal.
static void deriveMacIdentity()
{
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    uint32_t id = ((uint32_t)mac[2] << 24) | ((uint32_t)mac[3] << 16) |
                  ((uint32_t)mac[4] << 8)  |  (uint32_t)mac[5];
    char idStr[12];
    snprintf(idStr, sizeof(idStr), "!%08lx", (unsigned long)id);
    BridgeConfig::setMtNodeId(id);
    BridgeConfig::setMtNodeIdStr(idStr);

    // Derive the display names from the per-device node ID too (owner request,
    // v8.4.1): long name "<ID-hex> LoRa Bridge", short name "BR" + the ID's low
    // byte — so a fresh board shows e.g. "1DE9DC80 LoRa Bridge" / "BR80" instead
    // of the shared build-flag default. The user can still override both.
    char longName[BridgeConfig::MT_LONG_NAME_MAX + 1];
    snprintf(longName, sizeof(longName), "%08lX LoRa Bridge", (unsigned long)id);
    char shortName[BridgeConfig::MT_SHORT_NAME_MAX + 1];
    snprintf(shortName, sizeof(shortName), "BR%02lX", (unsigned long)(id & 0xFFu));
    BridgeConfig::setMtLongName(longName);
    BridgeConfig::setMtShortName(shortName);

    Serial.printf("[setup] MAC-derived identity: 0x%08lX (%s) \"%s\" / \"%s\"\n",
                  (unsigned long)id, idStr, longName, shortName);
}

// ============================================================
//  Shared SPI bus
//  XIAO ESP32S3 default SPI: SCK=GPIO7(D8), MOSI=GPIO9(D10),
//                             MISO=GPIO8(D9)
// ============================================================
#define SPI_SCK   7   // D8
#define SPI_MOSI  9   // D10
#define SPI_MISO  8   // D9

// Use the board-level SPI object so XIAO's variant pin mapping is
// already in effect.  A separate SPIClass(HSPI) instance re-enters
// spi_bus_initialize with default ESP32-S3 pins until begin() is
// called, introducing a window where both MOSI/MISO are wrong.
#define spi SPI

// ============================================================
//  Radio 1 — Wio SX1262 via 40-pin B2B header
//  GPIOs 38–42 are only accessible via the B2B connector.
// ============================================================
// Radio 1 is always the B2B-connector SX1262 — fixed silicon, never remapped.
#define R1_NSS      41  // SPI chip-select  (B2B / A11)
#define R1_DIO1     39  // IRQ              (B2B)
#define R1_RESET    42  // Reset            (B2B / A12)
#define R1_BUSY     40  // Busy             (B2B)
#define R1_ANT_SW   38  // Antenna switch   (B2B)

// ============================================================
//  Radio 2 — Wio SX1262 for XIAO via perimeter (edge) header pins
//  ----------------------------------------------------------------
//  WARNING: the Seeed "Wio-SX1262 for XIAO" edge module CHANGED its
//  pinout between silkscreen revisions. Read the silkscreen on YOUR
//  Radio-2 module and build the matching variant:
//     V1.0 -> env xiao_esp32s3       (default; NSS on D4 / GPIO5)
//     V1.1 -> env xiao_esp32s3_v1_1  (-DWIO_SX1262_REV=11; NSS on D3 / GPIO4)
//  The module's RIGHT-column silkscreen order (top -> bottom) sockets
//  onto the XIAO digital pins D0..D6, so each signal lands on the XIAO
//  pin at the same position:
//     V1.0: D0,   DIO1, RST,  BUSY, NSS,  RF_SW, D6
//     V1.1: DIO1, BUSY, RST,  NSS,  RF_SW, D5,    D6
//  XIAO ESP32S3 D-pin map: D0=GPIO1 D1=GPIO2 D2=GPIO3 D3=GPIO4 D4=GPIO5 D5=GPIO6
//  Each R2_* is #ifndef-guarded, so a single -DR2_NSS=.. (etc.) can also
//  override one pin from platformio.ini without editing this file.
// ============================================================
#ifndef WIO_SX1262_REV
  #define WIO_SX1262_REV 10          // 10 = V1.0 (default), 11 = V1.1
#endif

#if WIO_SX1262_REV == 11
  // ---- Wio-SX1262 for XIAO  V1.1 ----
  #ifndef R2_NSS
    #define R2_NSS    4   // NSS   chip-select  (D3 / GPIO4)
  #endif
  #ifndef R2_DIO1
    #define R2_DIO1   1   // DIO1  IRQ          (D0 / GPIO1)
  #endif
  #ifndef R2_RESET
    #define R2_RESET  3   // RST   reset        (D2 / GPIO3)
  #endif
  #ifndef R2_BUSY
    #define R2_BUSY   2   // BUSY  busy         (D1 / GPIO2)
  #endif
  #ifndef R2_ANT_SW
    #define R2_ANT_SW 5   // RF_SW antenna sw   (D4 / GPIO5)
  #endif
  #define WIO_SX1262_REV_STR "V1.1"
#else
  // ---- Wio-SX1262 for XIAO  V1.0 (canonical — the original v8.x map) ----
  #ifndef R2_NSS
    #define R2_NSS    5   // NSS   chip-select  (D4 / GPIO5)
  #endif
  #ifndef R2_DIO1
    #define R2_DIO1   2   // DIO1  IRQ          (D1 / GPIO2)
  #endif
  #ifndef R2_RESET
    #define R2_RESET  3   // RST   reset        (D2 / GPIO3)
  #endif
  #ifndef R2_BUSY
    #define R2_BUSY   4   // BUSY  busy         (D3 / GPIO4)
  #endif
  #ifndef R2_ANT_SW
    #define R2_ANT_SW 6   // RF_SW antenna sw   (D5 / GPIO6)
  #endif
  #define WIO_SX1262_REV_STR "V1.0"
#endif

// ============================================================
//  FreeRTOS configuration
// ============================================================
// Radio task stack. 8192 since the NodeDB landed: ingestAndFanout() can nest
// extractMeshtasticNodeInfo (240 B pt + AES ctx ~280 B) under body[256] plus
// enqueueTextForDest's outPkt[256], and the fragmented-RNS path needs similar
// headroom. 8 KB leaves a comfortable margin; the ESP32-S3 has 512 KB SRAM.
#define BRIDGE_TASK_STACK  8192
#define BRIDGE_TASK_PRIO   2       // above idle, below system
#define BRIDGE_POLL_MS     1       // ms between RX polls

// ============================================================
//  Shared SPI mutex — created in setup(), passed to both radios
// ============================================================
SemaphoreHandle_t spiMutex = NULL;

// ============================================================
//  Radio table. R1/R2 are LOCAL (XIAO SPI, WioSX1262); R3/R4 are SX1262 on a
//  SECOND XIAO reached over the UART crossover (RemoteRadio over g_link).
//  Constructed in setup() once the mutex / SPI bus / link are ready. (v8.5)
// ============================================================
static constexpr int NR = 4;

LoraRadio   *g_radio[NR]        = { nullptr, nullptr, nullptr, nullptr };
RadioChannel g_chan[NR];
bool         g_radioEnabled[NR] = { false, false, false, false };
static const char *kTag[NR]     = { "R1", "R2", "R3", "R4" };

// Per-radio routing matrix (v8.5): g_routeMask[src] bit j set => a packet RX'd
// on radio src is bridged to radio j. Loaded from BridgeConfig in setup(); the
// default (R1<->R2) preserves the historical 2-radio crossover behaviour.
static uint8_t g_routeMask[NR]  = { 0, 0, 0, 0 };

// Inter-XIAO UART crossover link to the second board's SX1262 (R3/R4). Opened in
// setup() only when R3 or R4 is enabled, so a single-board build never touches
// Serial1. Pins/baud are build-flag overridable (wire D6/D7 CROSSED to the peer).
#ifndef BRIDGE_LINK_TX_PIN
  #define BRIDGE_LINK_TX_PIN 43        // D6 — to the peer's RX (its D7)
#endif
#ifndef BRIDGE_LINK_RX_PIN
  #define BRIDGE_LINK_RX_PIN 44        // D7 — from the peer's TX (its D6)
#endif
#ifndef BRIDGE_LINK_BAUD
  #define BRIDGE_LINK_BAUD   460800UL
#endif
static UartLink g_link(Serial1);

// ============================================================
//  RX-priority routing state (V8.2-SPEC.md)
//  ----------------------------------------------------------------
//  One outbound queue per destination radio. A source radio's task decodes,
//  de-duplicates and re-encodes a packet, then PUSHES the finished bytes onto
//  the destination's queue and returns to RX. The destination radio's task is
//  the only one that pops + CAD-gates + transmits, one packet at a time, with a
//  non-blocking send. So a slow TX never blocks any radio's RX.
// ============================================================
RouteQueue g_routeQ[NR];

// Per-radio TX-scheduler state (owned by that radio's task).
static bool                g_txBusy[NR]         = { false, false, false, false };
static uint32_t            g_txStartMs[NR]      = { 0, 0, 0, 0 };
static uint32_t            g_txBackoffUntil[NR] = { 0, 0, 0, 0 };
static uint32_t            g_nextTxAllowedMs[NR]= { 0, 0, 0, 0 };  // airtime throttle
static bool                g_txPendingValid[NR] = { false, false, false, false };
static RouteQueue::Entry   g_txPending[NR];

// Route-queue depth (PSRAM-backed) and the max age past which a queued packet
// is dropped instead of delivered (stale mesh text isn't worth airtime).
#ifndef BRIDGE_ROUTE_QUEUE_DEPTH
  #define BRIDGE_ROUTE_QUEUE_DEPTH  64
#endif
#ifndef BRIDGE_ROUTE_MAX_AGE_MS
  #define BRIDGE_ROUTE_MAX_AGE_MS   30000UL
#endif

// CSMA random backoff after a busy CAD, and the upper bound on waiting for a
// non-blocking TX to report done before the scheduler force-recovers.
#ifndef BRIDGE_CAD_BACKOFF_MIN_MS
  #define BRIDGE_CAD_BACKOFF_MIN_MS 20
#endif
#ifndef BRIDGE_CAD_BACKOFF_MAX_MS
  #define BRIDGE_CAD_BACKOFF_MAX_MS 120
#endif
#ifndef BRIDGE_TX_INFLIGHT_TIMEOUT_MS
  #define BRIDGE_TX_INFLIGHT_TIMEOUT_MS 10000UL
#endif

// --- Full-mesh airtime throttle (V8.2-SPEC task #2) ------------------------
// Without a brake a busy mesh would let the bridge hog the channel. After each
// transmit we hold that radio off the air until its own emissions stay under a
// duty-cycle ceiling: a packet of on-air time A makes the next TX wait until
// A*100/DUTY after it started (so the radio is busy A and idle A*(100-DUTY)/
// DUTY). BRIDGE_TX_MIN_GAP_MS is an absolute floor on the post-TX gap.
// DUTY=100 + MIN_GAP=0 disables the throttle.
#ifndef BRIDGE_TX_DUTY_PERCENT
  #define BRIDGE_TX_DUTY_PERCENT  50
#endif
#ifndef BRIDGE_TX_MIN_GAP_MS
  #define BRIDGE_TX_MIN_GAP_MS    0
#endif

// --- Source-identity preservation (V8.2-SPEC.md §5) ------------------------
// Master switch: 1 = a bridged repeat preserves/reconstructs the original
// sender's identity (MC->MT virtual node, MT->MC name prefix); 0 = the clean-
// body / bridge-identity behaviour (the redesign default). The flags below are
// 0/1 literals used in plain `if`s so both code paths always compile and the
// behaviour is a build-flag flip, never a #ifdef'd-out function.
#ifndef BRIDGE_IDENTITY_PRESERVE
  #define BRIDGE_IDENTITY_PRESERVE 1
#endif
// 1 = tag the origin protocol so it is explicit after the repeat: "Alice@MT:"
// on MC, "Alice @MC" in the synthetic MT NodeInfo. 0 = bare name (looks native).
#ifndef BRIDGE_TAG_ORIGIN_PROTO
  #define BRIDGE_TAG_ORIGIN_PROTO 1
#endif
// MeshCore sender with no parseable "<name>: " prefix (Q3): 0 = fall back to
// the bridge's own identity (today's behaviour); 1 = a per-channel catch-all
// virtual node ("MC-<chan>").
#ifndef BRIDGE_MC_NONAME_VIRTUAL
  #define BRIDGE_MC_NONAME_VIRTUAL 0
#endif
// Longest MeshCore sender name we will parse out of a body prefix.
#ifndef BRIDGE_MC_NAME_MAX
  #define BRIDGE_MC_NAME_MAX 32
#endif

// ============================================================
//  Structured serial logging (V8.2-SPEC.md §13)
//  ----------------------------------------------------------------
//  A single greppable "ts=<ms> evt=<TAG> radio=<R1|R2> ..." key=val line per
//  pipeline event, emitted ATOMICALLY under a dedicated mutex so the two
//  core-pinned radio tasks can't interleave mid-line on the USB console (which
//  would shred the machine-parseable format). Kept separate from spiMutex so
//  logging never couples to the SPI critical sections. The heavy hex/protobuf
//  decoder dump (MeshDecoderDebug::print) stays its own multi-line block,
//  anchored by the preceding evt=RX line.
// ============================================================
static void blogf(const char *fmt, ...)
{
    char line[300];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    // Share the one recursive console lock (SerialLog) so an evt= line never
    // interleaves with the multi-line decoder dump or a status print from the
    // other core-pinned task.
    SerialLog::lock();
    Serial.print(line);
    SerialLog::unlock();
}

// Short protocol tag for the proto= field.
static const char *protoTag(uint8_t sync)
{
    switch (sync) {
        case MeshDecoderDebug::SYNC_WORD_MESHTASTIC: return "MT";
        case MeshDecoderDebug::SYNC_WORD_MESHCORE:   return "MC";
        case MeshDecoderDebug::SYNC_WORD_RETICULUM:  return "RNS";
        case MeshDecoderDebug::SYNC_WORD_LORAWAN:    return "LW";
        default:                                     return "?";
    }
}

// Format a node id for a nodeid=/virtualid= field: "!<8hex>" when set, "-" when
// 0 (MeshCore / RNS have no per-sender id, so a bare 0 would mislead). `buf`
// must hold >= 10 bytes; returns buf for use as a %s argument.
static const char *fmtNodeId(uint32_t id, char *buf)
{
    if (id) snprintf(buf, 10, "!%08lx", (unsigned long)id);
    else    { buf[0] = '-'; buf[1] = 0; }
    return buf;
}

// ---- Bridge wall-clock estimate (for outbound MeshCore timestamps) ---------
// The bridge has no RTC/NTP (WiFi only comes up for the portal), so it learns
// the current Unix time opportunistically from any inbound protocol that carries
// a plausible timestamp — MeshCore GRP_TXT ts and Meshtastic POSITION_APP
// (Position.time field 4 / .timestamp field 7) — and stamps that (+ elapsed
// millis) onto outbound MC packets. Otherwise MC clients render the bridged
// message as 1969 (ts=0). volatile: written by either radio task, read by the
// other; display-only, so a rare torn read merely mis-stamps one message's time.
static volatile uint32_t g_clockUnix   = 0;   // last plausible Unix ts learned
static volatile uint32_t g_clockMillis = 0;   // millis() when it was captured

// Learn the wall-clock from a decoded packet timestamp. Ignores implausible
// values (0 / pre-2017) so a clockless sender can't poison the estimate. Logs
// once, when the clock is first calibrated (the useful event); silent after.
// `src` is the protocol tag for the one-time CLOCK log (MC / MT).
static inline void learnClock(uint32_t ts, const char *src) {
    if (ts <= 1500000000u) return;
    bool first = (g_clockUnix == 0);
    g_clockUnix   = ts;
    g_clockMillis = millis();
    if (first)
        blogf("ts=%lu evt=CLOCK src=%s unix=%lu (calibrated — MC TX now timestamped)\n",
              (unsigned long)millis(), src, (unsigned long)ts);
}
static inline void learnClockFromMc(uint32_t mcTs) { learnClock(mcTs, "MC"); }
static inline void learnClockFromMt(uint32_t mtTs) { learnClock(mtTs, "MT"); }

// Best estimate of the current Unix time, or 0 if none learned yet (a freshly
// booted bridge that hasn't yet heard a timestamped MC packet or an MT position
// still stamps 0 until the first calibration).
static uint32_t bridgeNowUnix() {
    uint32_t base = g_clockUnix;
    if (!base) return 0;
    return base + (millis() - g_clockMillis) / 1000u;
}

// Resolve one radio's channel into `out` from its protocol (sync word) and
// its BridgeConfig channel name/key strings.
static void resolveRadioChannel(uint8_t syncWord, const char *chName,
                                const char *chKey, RadioChannel &out)
{
    if (syncWord == MeshDecoderDebug::SYNC_WORD_MESHTASTIC) {
        MeshtasticConfig::resolve(chKey, chName, out);
    } else if (syncWord == MeshDecoderDebug::SYNC_WORD_MESHCORE) {
        MeshCoreConfig::resolve(chKey, chName, out);
    } else {
        // Reticulum or unknown — no channel key material.
        out.protocol    = syncWord;
        out.keyLen      = 0;
        out.channelHash = 0;
        memset(out.key, 0, sizeof(out.key));
        if (!chName) chName = "";
        strncpy(out.name, chName, sizeof(out.name) - 1);
        out.name[sizeof(out.name) - 1] = 0;
        Serial.printf("[radio] protocol 0x%02X — no channel key\n", syncWord);
    }
}

// ============================================================
//  Cross-protocol bridge core (RX-priority pipeline, V8.2-SPEC.md)
//  ----------------------------------------------------------------
//  RX path (ingestAndFanout): decode the packet ONCE into a clean body (+ a
//  Meshtastic srcId), run it through the content-hash loop/dup guard, then
//  re-encode for the destination and PUSH the bytes onto its RouteQueue. No
//  transmit happens here — that is the TX scheduler's job (in radioTask) — so
//  the source radio returns to RX at once.
//
//  Protocols: 0x12 MeshCore (GRP_TXT), 0x2B Meshtastic (text/pos/telemetry),
//  0x42 Reticulum (base64, CRC-16 seq id, fragmented across MT/MC text). RNS
//  fragments are queued like any other packet; the airtime throttle paces them
//  (no inline delay).
// ============================================================

// RNS source path. Override via -D build flags.
#ifndef BRIDGE_RNS_MAX_FRAGS
  #define BRIDGE_RNS_MAX_FRAGS         8
#endif

// In-protocol RNS -> RNS transparent raw repeat (1 = on). When both radios run
// Reticulum, an inbound RNS frame is re-transmitted byte-for-byte on the other
// radio (range-extension / sub-band bridge) instead of being dropped; RNS
// Transport on the end nodes handles hop limits and network-level dedup. Set 0
// to restore the pre-v8.3 behaviour where RNS only ever tunnels to MT/MC.
#ifndef BRIDGE_RNS_INPROTO_REPEAT
  #define BRIDGE_RNS_INPROTO_REPEAT    1
#endif

// LoRaWAN (sync 0x34) keyless feature toggles (V8.3-SPEC §5/§6/§7). v8.3 reads
// only the cleartext PHY header — no key material, no FRMPayload decrypt, no
// inject into LoRaWAN. Override via -D build flags.
#ifndef BRIDGE_LW_CAPTURE
  #define BRIDGE_LW_CAPTURE          1   // log evt=RX proto=LW header metadata
#endif
#ifndef BRIDGE_LW_CAPTURE_HEX
  #define BRIDGE_LW_CAPTURE_HEX      0   // also log evt=LWRAW raw=<full PHYPayload hex>
#endif                                   // (bench "synthetic LNS" sniffer -> tools/lw-verify.py)
#ifndef BRIDGE_LW_SUMMARY_TO_MESH
  #define BRIDGE_LW_SUMMARY_TO_MESH  1   // also emit a metadata summary to MT/MC (LW-Q2)
#endif
#ifndef BRIDGE_LW_RELAY
  #define BRIDGE_LW_RELAY            1   // transparent raw repeat to other 0x34 radios
#endif

// --- ABP P1: LoRaWAN ABP uplink ENCODER (keyed; opt-in) ------------------
// Distinct from the keyless capture/relay above. Minting a valid LoRaWAN uplink
// requires a per-device ABP identity (DevAddr + NwkSKey + AppSKey), a monotonic
// FCnt and a CMAC MIC (ABP-LORAWAN-SPEC §2/§7). OFF by default so a stock build keeps
// v8.3's do-no-harm MT/MC->LW drop. When ON *and* both keys parse, the
// dispatcher transcodes a body into an ABP uplink and queues it for RF re-emit
// (delivery model B1). P1 sources creds from build flags + an in-RAM FCnt;
// ABP P2 replaces this with the schema-v5 per-source store + NVS-persisted
// FCnt (M1). Keys are 32-hex-char strings; empty (default) => encoder idle.
#ifndef BRIDGE_LW_ENCODE
  #define BRIDGE_LW_ENCODE           0
#endif
#ifndef BRIDGE_LW_ENC_SELFTEST
  #define BRIDGE_LW_ENC_SELFTEST     0   // boot-time RFC4493/keystream/frame KATs
#endif
#ifndef BRIDGE_LW_ENC_DEVADDR
  #define BRIDGE_LW_ENC_DEVADDR      0x01000001u   // private NetID (0x01) prefix
#endif
#ifndef BRIDGE_LW_ENC_FPORT
  #define BRIDGE_LW_ENC_FPORT        13            // Custom/weather (ABP-LORAWAN-SPEC §3)
#endif
#ifndef BRIDGE_LW_ENC_NWKSKEY
  #define BRIDGE_LW_ENC_NWKSKEY      ""
#endif
#ifndef BRIDGE_LW_ENC_APPSKEY
  #define BRIDGE_LW_ENC_APPSKEY      ""
#endif

// Per-fragment raw-byte budgets, derived from:
//   max on-air packet sizes: MC = 184 B, MT = 200 B (conservative).
//   prefix overhead "[rns AA X/Y] " = 13 chars.
//   base64 chunk length rounded down to a multiple of 4 so each fragment
//   decodes cleanly without cross-fragment padding tricks.
#define BRIDGE_RNS_RAW_PER_FRAG_MC    117
#define BRIDGE_RNS_RAW_PER_FRAG_MT    123

// Fragment an RNS frame for one destination protocol and ENQUEUE each fragment
// on that destination's RouteQueue (the scheduler paces + CAD-gates the sends).
static void enqueueReticulumForDest(const RadioChannel &dstChan, int destIdx,
                                    const char *srcTag,
                                    const uint8_t *buf, size_t len)
{
    if (dstChan.protocol == MeshDecoderDebug::SYNC_WORD_RETICULUM) return;
    if (len == 0) return;

    // srcId we stamp into the destination encoding — folded into the dedup hash
    // so an echo of our own fragment is recognised as a loop.
    uint32_t dstSrcId = (dstChan.protocol == MeshDecoderDebug::SYNC_WORD_MESHTASTIC)
                        ? BridgeConfig::mtNodeId() : 0;

    // Pick destination-specific fragment size.
    size_t   rawPerFrag = 0;
    const char *dstName = "?";
    if (dstChan.protocol == MeshDecoderDebug::SYNC_WORD_MESHTASTIC) {
        rawPerFrag = BRIDGE_RNS_RAW_PER_FRAG_MT;
        dstName    = "MT";
    } else if (dstChan.protocol == MeshDecoderDebug::SYNC_WORD_MESHCORE) {
        rawPerFrag = BRIDGE_RNS_RAW_PER_FRAG_MC;
        dstName    = "MC";
    } else {
        blogf("ts=%lu evt=DROP radio=%s dst=%s drop=bad-proto what=rns\n",
              (unsigned long)millis(), srcTag, kTag[destIdx]);
        return;
    }

    // Fragment count + bound check
    size_t totalFrags = (len + rawPerFrag - 1) / rawPerFrag;
    if (totalFrags > BRIDGE_RNS_MAX_FRAGS) {
        blogf("ts=%lu evt=DROP radio=%s dst=%s drop=frag-overflow len=%u frags=%u max=%u\n",
              (unsigned long)millis(), srcTag, kTag[destIdx], (unsigned)len,
              (unsigned)totalFrags, (unsigned)BRIDGE_RNS_MAX_FRAGS);
        return;
    }

    // Low byte of CRC-16/CCITT over the raw RNS frame is the sequence ID
    // shared by all fragments of this frame.
    uint8_t seq = (uint8_t)(MeshDecoderDebug::crc16_ccitt(buf, len) & 0xFF);

    for (size_t idx = 0; idx < totalFrags; idx++) {
        size_t rawStart = idx * rawPerFrag;
        size_t rawLen   = (idx + 1 == totalFrags) ? (len - rawStart) : rawPerFrag;

        // base64 this slice (per-fragment, so no cross-fragment padding)
        unsigned char b64chunk[200];
        size_t b64Len = 0;
        int b64rc = mbedtls_base64_encode(b64chunk, sizeof(b64chunk), &b64Len,
                                           buf + rawStart, rawLen);
        if (b64rc != 0) {
            blogf("ts=%lu evt=DROP radio=%s dst=%s drop=b64-fail frag=%u/%u rc=%d\n",
                  (unsigned long)millis(), srcTag, kTag[destIdx],
                  (unsigned)(idx + 1), (unsigned)totalFrags, b64rc);
            return;
        }

        // Build the marked body and encode for the destination protocol
        char marked[240];
        snprintf(marked, sizeof(marked), "[rns %02X %u/%u] %.*s",
                 seq, (unsigned)(idx + 1), (unsigned)totalFrags,
                 (int)b64Len, (const char *)b64chunk);

        uint8_t  outPkt[256];
        size_t   outLen  = 0;
        bool     encoded = false;
        uint32_t emitPid = 0;   // MT pid we stamp (0 for MC); folded per §15.1
        if (dstChan.protocol == MeshDecoderDebug::SYNC_WORD_MESHTASTIC) {
            encoded = MeshEncoderDebug::encodeMeshtasticText(
                          dstChan, BridgeConfig::mtNodeId(),
                          marked, outPkt, sizeof(outPkt), outLen, &emitPid);
        } else {
            encoded = MeshEncoderDebug::encodeMeshCoreGrpTxt(
                          dstChan, marked, /*ts=*/bridgeNowUnix(), outPkt, sizeof(outPkt), outLen);
        }
        if (!encoded) {
            blogf("ts=%lu evt=DROP radio=%s dst=%s drop=encode-fail frag=%u/%u\n",
                  (unsigned long)millis(), srcTag, kTag[destIdx],
                  (unsigned)(idx + 1), (unsigned)totalFrags);
            return;
        }

        // Remember this emission (fragment body + stamped src + MT pid) so its
        // echo is dropped as a loop, then queue it. Inter-fragment pacing is
        // provided by the destination's airtime throttle, not an inline delay.
        DedupCache::record(
            DedupCache::hash((const uint8_t *)marked, strlen(marked), dstSrcId, emitPid));
        if (g_routeQ[destIdx].push(outPkt, outLen)) {
            blogf("ts=%lu evt=QUEUE radio=%s dst=%s dstproto=%s frag=%u/%u seq=%02x "
                  "len=%u qdepth=%u msg=\"%s\"\n",
                  (unsigned long)millis(), srcTag, kTag[destIdx], dstName,
                  (unsigned)(idx + 1), (unsigned)totalFrags, seq,
                  (unsigned)outLen, (unsigned)g_routeQ[destIdx].count(), marked);
        } else {
            blogf("ts=%lu evt=DROP radio=%s dst=%s drop=queue-full frag=%u/%u\n",
                  (unsigned long)millis(), srcTag, kTag[destIdx],
                  (unsigned)(idx + 1), (unsigned)totalFrags);
        }
    }
}

// Parse a leading "<name>: " from a MeshCore group-text body (the MeshCore
// convention — the sender name rides inside the body since GRP_TXT has no
// per-sender id field). On success copies the name into nameOut and returns a
// pointer to the message text after ": "; returns nullptr if there is no
// plausible name prefix. Best-effort: the convention is not guaranteed on the
// wire, so a missing/oversized/non-printable prefix simply falls back to
// no-attribution. (Assumption flagged for bench — V8.2-SPEC §5.3.)
static const char *parseMcSenderName(const char *body, char *nameOut, size_t cap)
{
    if (!body || !nameOut || cap < 2) return nullptr;
    nameOut[0] = 0;
    size_t maxScan = (cap - 1 < (size_t)BRIDGE_MC_NAME_MAX)
                         ? (cap - 1) : (size_t)BRIDGE_MC_NAME_MAX;
    for (size_t i = 0; i < maxScan; i++) {
        char c = body[i];
        if (c == 0) break;
        if (c == ':' && body[i + 1] == ' ') {
            if (i == 0) return nullptr;             // empty name
            memcpy(nameOut, body, i);
            nameOut[i] = 0;
            return body + i + 2;                    // message text after ": "
        }
        if (i == 0 && c == ' ')   return nullptr;   // leading space — not a name
        if (c < 0x20 || c > 0x7E) return nullptr;   // non-printable — not a name
    }
    return nullptr;
}

// Build + enqueue a synthetic Meshtastic NodeInfo for a virtual node on the MT
// destination, so an MT client recognises a bridged MeshCore sender as its own
// distinct node ("Alice @MC") rather than attributing the text to the bridge
// (V8.2-SPEC §5.3). Not dedup-recorded: a NodeInfo echo is absorbed by the
// NodeInfo-upsert path in ingestAndFanout, not re-bridged, so it can't loop.
static void enqueueVirtualNodeInfo(const RadioChannel &dstChan, int destIdx,
                                   uint32_t vid, const char *name)
{
    char idStr[12];
    snprintf(idStr, sizeof(idStr), "!%08lx", (unsigned long)vid);

    char longName[NodeDB::MAX_LONG_NAME + 1];
    if (BRIDGE_TAG_ORIGIN_PROTO)
        snprintf(longName, sizeof(longName), "%.32s @MC", name);
    else
        snprintf(longName, sizeof(longName), "%.39s", name);

    char shortName[NodeDB::MAX_SHORT_NAME + 1];
    snprintf(shortName, sizeof(shortName), "%.4s", name);

    uint8_t pkt[256];
    size_t  pktLen = 0;
    if (MeshEncoderDebug::encodeMeshtasticNodeInfo(
            dstChan, vid, idStr, longName, shortName, pkt, sizeof(pkt), pktLen)) {
        if (g_routeQ[destIdx].push(pkt, pktLen))
            blogf("ts=%lu evt=NODEINFO radio=%s op=virtual selfid=%s long=\"%s\"\n",
                  (unsigned long)millis(), kTag[destIdx], idStr, longName);
    } else {
        blogf("ts=%lu evt=DROP radio=%s drop=encode-fail what=virt-nodeinfo selfid=%s\n",
              (unsigned long)millis(), kTag[destIdx], idStr);
    }
}

// Encode `body` for one destination protocol and ENQUEUE it on that
// destination's RouteQueue (its scheduler CAD-gates + sends). Records the
// emission's content hash so its echo is recognised as a loop — the job the
// prepended marker used to do, now done by DedupCache with a CLEAN far-side
// body. The source-identity layer (V8.2-SPEC §5) hooks in here for the two
// cross-protocol directions:
//   MC -> MT: mint a deterministic virtual MT node from the MC sender name,
//             advertise its NodeInfo, stamp it as the packet src, strip the
//             name from the body (identity moves into the MT header).
//   MT -> MC: prefix the body with the MT sender name ("Alice@MT: ...") since
//             MeshCore has no header identity field.
// Same-protocol relays keep the bridge identity here (the raw-repeat /
// trans-crypt paths that preserve the exact origin land in a later commit).

#if BRIDGE_LW_ENCODE
// ABP P1: resolve build-flag ABP credentials once into a usable form.
// `ready` is false (=> keep the keyless do-no-harm drop) unless BOTH session
// keys parse as 32 hex chars. P2 supersedes this with a schema-v5 per-source
// credential store + an NVS-persisted FCnt (the M1 device model).
struct LwEncCreds {
    bool     ready;
    uint32_t devAddr;
    uint8_t  fport;
    uint8_t  nwkSKey[16];
    uint8_t  appSKey[16];
};

static int lwHexNibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool lwParseKey16(const char *s, uint8_t out[16]) {
    if (!s || strlen(s) != 32) return false;
    for (int i = 0; i < 16; ++i) {
        int hi = lwHexNibble(s[2 * i]);
        int lo = lwHexNibble(s[2 * i + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return true;
}

static const LwEncCreds &lwEncCreds() {
    static LwEncCreds c;
    static bool init = false;
    if (!init) {
        init      = true;
        c.devAddr = (uint32_t)(BRIDGE_LW_ENC_DEVADDR);
        c.fport   = (uint8_t)(BRIDGE_LW_ENC_FPORT);
        bool kn   = lwParseKey16(BRIDGE_LW_ENC_NWKSKEY, c.nwkSKey);
        bool ka   = lwParseKey16(BRIDGE_LW_ENC_APPSKEY, c.appSKey);
        c.ready   = kn && ka;
    }
    return c;
}

// ABP crypto self-test gate (#2): default true so a build WITHOUT the boot
// self-test behaves unchanged; when BRIDGE_LW_ENC_SELFTEST runs, setup() latches
// its result here and a FAIL stops the encoder from emitting frames whose wrong
// MIC the LNS would silently drop.
static bool g_lwCryptoOk = true;

// Encode `payload` as a LoRaWAN ABP uplink under the per-source ABP device
// (LoRaWANConfig) or the P1 build-flag fallback, and queue it for RF re-emit
// (delivery model B1). Shared by the decoded-text path (enqueueTextForDest) and
// the Custom raw-LoRa path (ingestAndFanout, P3). When the matched device sets
// FLAG_TAG_SRC, a [proto:1][srcId:4 LE] header is prepended so a multiplexed
// ChirpStack codec can recover the origin. Returns true if an uplink was
// attempted (credentials present), false if no ABP identity matched.
static bool enqueueAbpUplink(const RadioChannel &srcChan, uint32_t srcId,
                             int destIdx, const char *srcTag,
                             const uint8_t *payload, size_t payloadLen,
                             const char *logMsg)
{
    uint32_t       devAddr = 0, fcnt = 0;
    const uint8_t *nwkKey  = nullptr, *appKey = nullptr;
    uint8_t        fport   = 0;
    const char    *credSrc = "?";
    bool           tagSrc  = false;

    int devIdx = -1;
    const LoRaWANConfig::Device *dev =
        LoRaWANConfig::resolve(srcChan.protocol, srcId, devIdx);
    if (dev) {
        devAddr = dev->devAddr; nwkKey = dev->nwkSKey; appKey = dev->appSKey;
        fport   = dev->fport;   fcnt   = LoRaWANConfig::nextFcnt(devIdx);
        credSrc = "nvs";
        tagSrc  = (dev->flags & LoRaWANConfig::FLAG_TAG_SRC) != 0;
    } else {
        const LwEncCreds &lw = lwEncCreds();
        if (lw.ready) {
            devAddr = lw.devAddr; nwkKey = lw.nwkSKey; appKey = lw.appSKey;
            // (#4) reboot-safe, DevAddr-keyed FCnt (was an in-RAM ++ that reset to
            // 0 every boot → replays). FCNT_INVALID handled below.
            fport   = lw.fport;   fcnt   = LoRaWANConfig::nextFcntForDevAddr(lw.devAddr);
            credSrc = "flag";
        }
    }
    if (!nwkKey) return false;

    // (#2) refuse to emit if the on-device crypto self-test failed — a wrong-MIC
    // frame is silently dropped by the LNS, so stop it here instead.
    if (!g_lwCryptoOk) {
        blogf("ts=%lu evt=DROP radio=%s dst=%s drop=lw-selftest-fail msg=\"%s\"\n",
              (unsigned long)millis(), srcTag, kTag[destIdx], logMsg ? logMsg : "");
        return true;
    }
    // (#3) a non-durable FCnt would risk a reboot replay — drop rather than emit.
    if (fcnt == LoRaWANConfig::FCNT_INVALID) {
        blogf("ts=%lu evt=DROP radio=%s dst=%s drop=lw-fcnt-fail msg=\"%s\"\n",
              (unsigned long)millis(), srcTag, kTag[destIdx], logMsg ? logMsg : "");
        return true;
    }

    // FRMPayload = optional [proto][srcId LE] source tag, then the payload.
    uint8_t frm[242];                                  // LoRaWAN app-payload cap
    size_t  flen = 0;
    if (tagSrc) {
        // (#5) stamp the small BridgeConfig::Protocol enum (1=MT/2=MC/3=RNS/
        // 4=Custom/5=LoRaWAN), NOT the raw LoRa sync word, so the ChirpStack
        // codec (tools/chirpstack-codec.js) decodes the source proto correctly.
        frm[flen++] = LoRaWANConfig::protoOf(srcChan.protocol);
        frm[flen++] = (uint8_t)(srcId);       frm[flen++] = (uint8_t)(srcId >> 8);
        frm[flen++] = (uint8_t)(srcId >> 16); frm[flen++] = (uint8_t)(srcId >> 24);
    }
    // (#6) drop an over-cap payload cleanly instead of silently truncating it
    // into a mangled FRMPayload the codec would mis-decode.
    if (payloadLen > sizeof(frm) - flen) {
        blogf("ts=%lu evt=DROP radio=%s dst=%s drop=lw-payload-overflow "
              "len=%u cap=%u msg=\"%s\"\n",
              (unsigned long)millis(), srcTag, kTag[destIdx], (unsigned)payloadLen,
              (unsigned)(sizeof(frm) - flen), logMsg ? logMsg : "");
        return true;
    }
    memcpy(frm + flen, payload, payloadLen);
    flen += payloadLen;

    uint8_t lwPkt[256];
    size_t  n = LoRaWANCrypto::encodeUplink(devAddr, nwkKey, appKey, fcnt, fport,
                                            frm, flen, /*confirmed=*/false,
                                            lwPkt, sizeof(lwPkt));
    if (n == 0) {
        blogf("ts=%lu evt=DROP radio=%s dst=%s drop=lw-encode-fail msg=\"%s\"\n",
              (unsigned long)millis(), srcTag, kTag[destIdx], logMsg ? logMsg : "");
        return true;
    }
    // Loop-safety: remember our own emission (mirror the MT/MC dedup path).
    DedupCache::record(DedupCache::hash(lwPkt, n, devAddr, fcnt));
    if (g_routeQ[destIdx].push(lwPkt, n)) {
        blogf("ts=%lu evt=QUEUE radio=%s dst=%s dstproto=LW len=%u devaddr=%08lx "
              "fcnt=%lu fport=%u cred=%s tag=%d qdepth=%u msg=\"%s\"\n",
              (unsigned long)millis(), srcTag, kTag[destIdx], (unsigned)n,
              (unsigned long)devAddr, (unsigned long)fcnt, (unsigned)fport,
              credSrc, tagSrc ? 1 : 0, (unsigned)g_routeQ[destIdx].count(),
              logMsg ? logMsg : "");
    } else {
        blogf("ts=%lu evt=DROP radio=%s dst=%s drop=queue-full\n",
              (unsigned long)millis(), srcTag, kTag[destIdx]);
    }
    return true;
}
#endif  // BRIDGE_LW_ENCODE

static void enqueueTextForDest(const RadioChannel &srcChan, uint32_t srcId,
                               const RadioChannel &dstChan, int destIdx,
                               const char *srcTag, const char *body)
{
    // Destination is Reticulum — log and drop. No RNS encoder yet.
    if (dstChan.protocol == MeshDecoderDebug::SYNC_WORD_RETICULUM) {
        blogf("ts=%lu evt=DROP radio=%s dst=%s drop=no-rns-encoder msg=\"%s\"\n",
              (unsigned long)millis(), srcTag, kTag[destIdx], body);
        return;
    }

    // Destination is LoRaWAN. v8.3 was keyless (always dropped). ABP P1: when
    // the ABP encoder is built in (BRIDGE_LW_ENCODE) AND credentials parse,
    // transcode the body into a LoRaWAN ABP uplink and queue it for RF re-emit
    // (delivery model B1); otherwise keep v8.3's do-no-harm drop (keyless mode /
    // no keys). Injecting content needs the device keys + monotonic FCnt + MIC.
    if (dstChan.protocol == MeshDecoderDebug::SYNC_WORD_LORAWAN) {
#if BRIDGE_LW_ENCODE
        // ABP encode (P1/P2): transcode the decoded body into an ABP uplink under
        // the matching per-source device (else the build-flag fallback). Returns
        // true when an identity matched; otherwise fall through to the keyless drop.
        if (enqueueAbpUplink(srcChan, srcId, destIdx, srcTag,
                             (const uint8_t *)body, strlen(body), body))
            return;
#endif  // BRIDGE_LW_ENCODE
        blogf("ts=%lu evt=DROP radio=%s dst=%s drop=no-lw-encoder msg=\"%s\"\n",
              (unsigned long)millis(), srcTag, kTag[destIdx], body);
        return;
    }

    const uint8_t srcProto = srcChan.protocol;
    char          xbody[256];                 // scratch for an attributed body
    const char   *outBody  = body;            // what we actually encode
    uint8_t       outPkt[256];
    size_t        outLen   = 0;
    bool          encoded  = false;
    const char   *dstName  = "?";
    uint32_t      dstSrcId = 0;               // src stamped into the re-encode
    uint32_t      emitPid  = 0;               // MT pid we stamp (0 for MC); §15.1
    uint32_t      mcStampTs = 0;              // Unix ts stamped on MC encodes (ts-fix)

    if (dstChan.protocol == MeshDecoderDebug::SYNC_WORD_MESHTASTIC) {
        dstName  = "MT";
        dstSrcId = BridgeConfig::mtNodeId();  // default: the bridge's identity

        if (BRIDGE_IDENTITY_PRESERVE &&
            srcProto == MeshDecoderDebug::SYNC_WORD_MESHCORE) {
            // MC -> MT: reconstruct the sender as a virtual MT node.
            char        name[NodeDB::MAX_LONG_NAME + 1];
            const char *rest = parseMcSenderName(body, name, sizeof(name));
            if (rest) {
                uint32_t vid = VirtualNodeMap::idForLabel(name, BridgeConfig::mtNodeId());
                if (VirtualNodeMap::nodeInfoDue(vid))
                    enqueueVirtualNodeInfo(dstChan, destIdx, vid, name);
                dstSrcId = vid;
                outBody  = rest;              // name moved into the MT header
            } else if (BRIDGE_MC_NONAME_VIRTUAL) {
                // No parseable name -> one catch-all virtual node per channel.
                char label[40];
                snprintf(label, sizeof(label), "MC-%s",
                         srcChan.name[0] ? srcChan.name : "chan");
                uint32_t vid = VirtualNodeMap::idForLabel(label, BridgeConfig::mtNodeId());
                if (VirtualNodeMap::nodeInfoDue(vid))
                    enqueueVirtualNodeInfo(dstChan, destIdx, vid, label);
                dstSrcId = vid;               // body unchanged (no name to strip)
            }
            // else fall back to the bridge identity + full body (today's path)
        }

        encoded = MeshEncoderDebug::encodeMeshtasticText(
                      dstChan, dstSrcId, outBody, outPkt, sizeof(outPkt), outLen,
                      &emitPid);

    } else if (dstChan.protocol == MeshDecoderDebug::SYNC_WORD_MESHCORE) {
        dstName  = "MC";
        dstSrcId = 0;   // MeshCore GRP_TXT has no stable per-sender id

        if (BRIDGE_IDENTITY_PRESERVE &&
            srcProto == MeshDecoderDebug::SYNC_WORD_MESHTASTIC) {
            // MT -> MC: prefix the MT sender name (the only identity channel MC
            // offers). NodeDB short_name if known, else the recoverable !hexid.
            char sname[NodeDB::MAX_SHORT_NAME + 1];
            char who[24];
            if (NodeDB::lookupShortName(srcId, sname, sizeof(sname)) && sname[0])
                snprintf(who, sizeof(who), "%s", sname);
            else
                snprintf(who, sizeof(who), "!%08lx", (unsigned long)srcId);
            if (BRIDGE_TAG_ORIGIN_PROTO)
                snprintf(xbody, sizeof(xbody), "%s@MT: %s", who, body);
            else
                snprintf(xbody, sizeof(xbody), "%s: %s", who, body);
            outBody = xbody;
        }

        mcStampTs = bridgeNowUnix();          // learned wall-clock (0 if uncalibrated)
        encoded = MeshEncoderDebug::encodeMeshCoreGrpTxt(
                      dstChan, outBody, /*ts=*/mcStampTs, outPkt, sizeof(outPkt), outLen);

    } else {
        blogf("ts=%lu evt=DROP radio=%s dst=%s drop=bad-proto\n",
              (unsigned long)millis(), srcTag, kTag[destIdx]);
        return;
    }

    if (!encoded) {
        blogf("ts=%lu evt=DROP radio=%s dst=%s drop=encode-fail\n",
              (unsigned long)millis(), srcTag, kTag[destIdx]);
        return;
    }

    // Remember our own emission (body + stamped src + the MT pid we just stamped)
    // so the echo is dropped as a loop, then queue it for the dest's scheduler.
    // emitPid is 0 for MeshCore (no per-message id), so MC matching is unchanged.
    uint32_t hout = DedupCache::hash((const uint8_t *)outBody, strlen(outBody),
                                     dstSrcId, emitPid);
    DedupCache::record(hout);
    char vbuf[10];
    if (g_routeQ[destIdx].push(outPkt, outLen)) {
        blogf("ts=%lu evt=QUEUE radio=%s dst=%s dstproto=%s len=%u virtualid=%s "
              "hout=0x%08lx qdepth=%u qdropped=%lu mcts=%lu msg=\"%s\"\n",
              (unsigned long)millis(), srcTag, kTag[destIdx], dstName,
              (unsigned)outLen, fmtNodeId(dstSrcId, vbuf),
              (unsigned long)hout,
              (unsigned)g_routeQ[destIdx].count(),
              (unsigned long)g_routeQ[destIdx].dropped(),
              (unsigned long)mcStampTs, outBody);
    } else {
        blogf("ts=%lu evt=DROP radio=%s dst=%s drop=queue-full\n",
              (unsigned long)millis(), srcTag, kTag[destIdx]);
    }
}

// Two radios carry the SAME channel when they share protocol, channel hash, and
// key material — so a packet's ciphertext from one is valid verbatim on the
// other. That is the precondition for a transparent raw repeat (V8.2-SPEC §5.1).
static bool sameChannel(const RadioChannel &a, const RadioChannel &b)
{
    if (a.protocol != b.protocol)     return false;
    if (a.channelHash != b.channelHash) return false;
    if (a.keyLen != b.keyLen)         return false;
    return memcmp(a.key, b.key, a.keyLen) == 0;
}

// Transparent raw repeat (V8.2-SPEC §5.1): the destination radio is on the SAME
// channel as the source (different frequency), so we re-transmit the ORIGINAL
// bytes — the far side sees the ORIGINAL sender natively, in full fidelity
// (text, position, telemetry), not a bridge re-encode. For Meshtastic we only
// touch the mutable relay fields: decrement hop_limit (drop at 0) and set
// relay_node to our own low byte; src/packet_id/ciphertext are untouched, so
// native (src,packet_id) dedup keeps working. For MeshCore the bytes go out
// unchanged (path-append left to a later version — Q5).
//
// Loop-safe with NO extra dedup record: the ingest guard already recorded
// hash(body, srcId) in the SHARED DedupCache, and a raw repeat preserves both
// body and srcId, so the echo — heard on either radio — matches and is dropped.
static void rawRepeatForDest(const RadioChannel &dstChan, int destIdx,
                             const char *srcTag, uint32_t srcId,
                             const uint8_t *buf, size_t len)
{
    if (len == 0 || len > LORA_MAX_PACKET) return;
    uint8_t pkt[LORA_MAX_PACKET];
    memcpy(pkt, buf, len);

    if (dstChan.protocol == MeshDecoderDebug::SYNC_WORD_MESHTASTIC) {
        if (len < 16) return;
        if (srcId == BridgeConfig::mtNodeId()) return;   // never repeat our own
        uint8_t hops = pkt[12] & 0x07;                   // flags[2:0] = hop_limit
        if (hops == 0) {
            blogf("ts=%lu evt=DROP radio=%s dst=%s drop=hop0\n",
                  (unsigned long)millis(), srcTag, kTag[destIdx]);
            return;
        }
        pkt[12] = (uint8_t)((pkt[12] & ~0x07) | (hops - 1));   // decrement hops
        pkt[15] = (uint8_t)(BridgeConfig::mtNodeId() & 0xFF);  // relay_node = us
    }
    // MeshCore: bytes unchanged.

    char vbuf[10];
    if (g_routeQ[destIdx].push(pkt, len)) {
        blogf("ts=%lu evt=QUEUE radio=%s dst=%s mode=raw len=%u virtualid=%s "
              "qdepth=%u qdropped=%lu\n",
              (unsigned long)millis(), srcTag, kTag[destIdx], (unsigned)len,
              fmtNodeId(srcId, vbuf), (unsigned)g_routeQ[destIdx].count(),
              (unsigned long)g_routeQ[destIdx].dropped());
    } else {
        blogf("ts=%lu evt=DROP radio=%s dst=%s drop=queue-full mode=raw\n",
              (unsigned long)millis(), srcTag, kTag[destIdx]);
    }
}

// Decode a received packet ONCE, run the loop/dup guard, and fan it out to every
// (other) enabled destination's queue. No transmit happens here — the
// destinations' schedulers do that — so the source radio returns to RX.
static void ingestAndFanout(int srcIdx, const uint8_t *buf, size_t len)
{
    const RadioChannel &srcChan = g_chan[srcIdx];
    const char         *srcTag  = kTag[srcIdx];

    // RNS source: dedup the raw frame, then route to each destination — a
    // transparent raw repeat for an in-protocol RNS destination, or the base64
    // fragment tunnel for an MT/MC destination.
    if (srcChan.protocol == MeshDecoderDebug::SYNC_WORD_RETICULUM) {
        if (DedupCache::seenAndRecord(DedupCache::hash(buf, len, 0))) {
            blogf("ts=%lu evt=DROP radio=%s proto=RNS drop=rns-dup\n",
                  (unsigned long)millis(), srcTag);
            return;
        }
        for (int j = 0; j < NR; j++) {
            if (j == srcIdx) continue;
            if (!g_radioEnabled[j] || !g_radio[j]) continue;
            if (!(g_routeMask[srcIdx] & (1u << j))) continue;   // routing matrix
            // In-protocol RNS -> RNS is a transparent raw repeat (V8.2-SPEC §5.1
            // same-channel model, extended to Reticulum): re-transmit the
            // PHYPayload byte-for-byte so the far side sees the original frame
            // natively and RNS Transport handles hops/dedup at the network
            // layer. Loop-safe with no extra record — the raw-frame dedup above
            // already catches our own echo on the way back in. The cross-protocol
            // RNS -> MT/MC path stays the base64 fragment tunnel.
            if (BRIDGE_RNS_INPROTO_REPEAT &&
                g_chan[j].protocol == MeshDecoderDebug::SYNC_WORD_RETICULUM)
                rawRepeatForDest(g_chan[j], j, srcTag, /*srcId=*/0, buf, len);
            else
                enqueueReticulumForDest(g_chan[j], j, srcTag, buf, len);
        }
        return;
    }

    // LoRaWAN source (sync 0x34, keyless): capture the cleartext header, optionally
    // summarize it to the MT/MC meshes, and transparently relay the raw frame to any
    // other LoRaWAN radio. No payload decrypt (no keys); MT/MC -> LoRaWAN has no
    // keyless inject (see enqueueTextForDest's 0x34 dest drop). Loop-safe: the
    // raw-frame dedup recorded here drops a relayed echo, and enqueueTextForDest
    // records each summary emission so its echo is dropped too. V8.3-SPEC §5/§6/§7.
    if (srcChan.protocol == MeshDecoderDebug::SYNC_WORD_LORAWAN) {
        if (DedupCache::seenAndRecord(DedupCache::hash(buf, len, 0))) {
            blogf("ts=%lu evt=DROP radio=%s proto=LW drop=lw-dup\n",
                  (unsigned long)millis(), srcTag);
            return;
        }
        MeshDecoderDebug::LoRaWANMeta lw;
        bool parsed = MeshDecoderDebug::extractLoRaWANMeta(buf, len, lw);
#if BRIDGE_LW_CAPTURE
        if (parsed)
            blogf("ts=%lu evt=RX radio=%s proto=LW mtype=%s devaddr=0x%08lx "
                  "fcnt=%u fport=%d len=%u\n",
                  (unsigned long)millis(), srcTag,
                  MeshDecoderDebug::loraWanMtypeName(lw.mtype),
                  (unsigned long)lw.devAddr, (unsigned)lw.fcnt,
                  lw.hasFport ? (int)lw.fport : -1, (unsigned)len);
        else
            blogf("ts=%lu evt=RX radio=%s proto=LW parse=fail len=%u\n",
                  (unsigned long)millis(), srcTag, (unsigned)len);
#endif
#if BRIDGE_LW_CAPTURE_HEX
        // Full PHYPayload hex for an off-box "synthetic LNS" verify (MIC + decrypt
        // via tools/lw-verify.py) without a real ChirpStack. Bench sniffer only.
        {
            char hx[2 * 256 + 1];
            size_t hn = (len < 256) ? len : 256;
            for (size_t i = 0; i < hn; i++) snprintf(hx + 2 * i, 3, "%02x", buf[i]);
            hx[2 * hn] = '\0';
            blogf("ts=%lu evt=LWRAW radio=%s len=%u raw=%s\n",
                  (unsigned long)millis(), srcTag, (unsigned)len, hx);
        }
#endif
#if BRIDGE_LW_SUMMARY_TO_MESH
        if (parsed) {
            char sum[160];
            if (lw.isJoinReq)
                snprintf(sum, sizeof(sum), "LoRaWAN JoinReq DevEUI %08lx%08lx",
                         (unsigned long)(lw.devEui >> 32),
                         (unsigned long)(lw.devEui & 0xFFFFFFFFul));
            else if (lw.isData)
                snprintf(sum, sizeof(sum),
                         "LoRaWAN %s DevAddr 0x%08lx FCnt %u FPort %d len %u",
                         MeshDecoderDebug::loraWanMtypeName(lw.mtype),
                         (unsigned long)lw.devAddr, (unsigned)lw.fcnt,
                         lw.hasFport ? (int)lw.fport : -1, (unsigned)len);
            else
                snprintf(sum, sizeof(sum), "LoRaWAN %s len %u",
                         MeshDecoderDebug::loraWanMtypeName(lw.mtype), (unsigned)len);
            for (int j = 0; j < NR; j++) {
                if (j == srcIdx) continue;
                if (!g_radioEnabled[j] || !g_radio[j]) continue;
                if (!(g_routeMask[srcIdx] & (1u << j))) continue;   // routing matrix
                uint8_t dp = g_chan[j].protocol;
                if (dp == MeshDecoderDebug::SYNC_WORD_MESHTASTIC ||
                    dp == MeshDecoderDebug::SYNC_WORD_MESHCORE)
                    enqueueTextForDest(srcChan, /*srcId=*/0, g_chan[j], j, srcTag, sum);
            }
        }
#endif
#if BRIDGE_LW_RELAY
        for (int j = 0; j < NR; j++) {
            if (j == srcIdx) continue;
            if (!g_radioEnabled[j] || !g_radio[j]) continue;
            if (!(g_routeMask[srcIdx] & (1u << j))) continue;   // routing matrix
            if (g_chan[j].protocol == MeshDecoderDebug::SYNC_WORD_LORAWAN)
                rawRepeatForDest(g_chan[j], j, srcTag, /*srcId=*/0, buf, len);
        }
#endif
        return;
    }

#if BRIDGE_LW_ENCODE
    // Custom raw-LoRa source (e.g. a proprietary weather station): no built-in
    // decoder, but if a destination radio is a keyed LoRaWAN ABP encoder, wrap
    // the RAW received bytes as an ABP uplink FRMPayload (P3 weather-station
    // path). A ChirpStack payload codec decodes the station-specific format.
    if (BridgeConfig::radioProtocol(srcIdx) == BridgeConfig::PROTO_CUSTOM) {
        if (DedupCache::seenAndRecord(DedupCache::hash(buf, len, 0))) {
            blogf("ts=%lu evt=DROP radio=%s proto=Custom drop=loop-dup\n",
                  (unsigned long)millis(), srcTag);
            return;
        }
        bool any = false;
        for (int j = 0; j < NR; j++) {
            if (j == srcIdx) continue;
            if (!g_radioEnabled[j] || !g_radio[j]) continue;
            if (!(g_routeMask[srcIdx] & (1u << j))) continue;   // routing matrix
            if (g_chan[j].protocol == MeshDecoderDebug::SYNC_WORD_LORAWAN)
                any |= enqueueAbpUplink(srcChan, /*srcId=*/0, j, srcTag, buf, len, "custom-raw");
        }
        if (!any)
            blogf("ts=%lu evt=DROP radio=%s proto=Custom drop=no-lw-abp-dest\n",
                  (unsigned long)millis(), srcTag);
        return;
    }
#endif

    // MT NodeInfo packets feed the NodeDB and are NOT bridged as text — they'd
    // be constant noise on the destination mesh. Try this before the text-body
    // extract so we don't walk the Data protobuf twice.
    if (srcChan.protocol == MeshDecoderDebug::SYNC_WORD_MESHTASTIC) {
        uint32_t niNodeId = 0;
        char     niShort[NodeDB::MAX_SHORT_NAME + 1] = {0};
        char     niLong [NodeDB::MAX_LONG_NAME  + 1] = {0};
        if (MeshDecoderDebug::extractMeshtasticNodeInfo(
                buf, len, srcChan, niNodeId,
                niShort, sizeof(niShort),
                niLong,  sizeof(niLong))) {
            // Skip our own NodeInfo bouncing back via a relay (would needlessly
            // rewrite our own ID to NVS on every echo).
            if (niNodeId == BridgeConfig::mtNodeId()) {
                blogf("ts=%lu evt=DROP radio=%s proto=MT drop=self-echo ni_id=!%08lx\n",
                      (unsigned long)millis(), srcTag, (unsigned long)niNodeId);
                return;
            }
            NodeDB::upsert(niNodeId, niShort, niLong);
            blogf("ts=%lu evt=NODEDB radio=%s op=upsert ni_id=!%08lx ni_short=\"%s\" ni_long=\"%s\"\n",
                  (unsigned long)millis(), srcTag,
                  (unsigned long)niNodeId, niShort, niLong);
            return;     // not a text packet — don't bridge
        }
    }

    // Decode the body ONCE. For MT we try POSITION_APP and TELEMETRY_APP first;
    // if either yields structured data we format it as a compact text line and
    // reuse the text path below. TEXT_MESSAGE_APP is the final fallback. For MC
    // we lift the GRP_TXT body directly. We also capture the Meshtastic src so
    // identical text from different senders isn't false-dropped by the guard.
    uint32_t srcId = 0;
    uint32_t pid   = 0;   // MT packet_id, folded into the dedup hash (§15.1)
    char body[256];
    bool decoded = false;

    if (srcChan.protocol == MeshDecoderDebug::SYNC_WORD_MESHTASTIC) {
        if (len >= 8)
            srcId = (uint32_t)buf[4]  | ((uint32_t)buf[5]  << 8)
                  | ((uint32_t)buf[6] << 16) | ((uint32_t)buf[7] << 24);
        if (len >= 12)
            pid   = (uint32_t)buf[8]  | ((uint32_t)buf[9]  << 8)
                  | ((uint32_t)buf[10] << 16) | ((uint32_t)buf[11] << 24);
        if (!decoded && BridgeConfig::positionEnabled()) {
            MeshDecoderDebug::MeshtasticPositionInfo pos;
            if (MeshDecoderDebug::extractMeshtasticPosition(buf, len, srcChan, pos)) {
                // Opportunistically calibrate the bridge wall-clock from the
                // POSITION time field — closes the cold-boot window where MT->MC
                // stamps 1969 until the first timestamped MeshCore packet arrives.
                if (pos.hasTime) learnClockFromMt(pos.timeUnix);
                int n = snprintf(body, sizeof(body), "pos");
                if (pos.hasLat && pos.hasLon) {
                    n += snprintf(body + n, sizeof(body) - n, " %.4f,%.4f",
                                  (double)pos.latI / 1e7,
                                  (double)pos.lonI / 1e7);
                }
                if (pos.hasAlt) {
                    n += snprintf(body + n, sizeof(body) - n, " alt %dm",
                                  (int)pos.altM);
                }
                decoded = true;
            }
        }
        if (!decoded && BridgeConfig::telemetryEnabled()) {
            MeshDecoderDebug::MeshtasticTelemetryInfo tel;
            if (MeshDecoderDebug::extractMeshtasticTelemetry(buf, len, srcChan, tel)) {
                using TKind = MeshDecoderDebug::MeshtasticTelemetryInfo::Kind;
                if (tel.kind == TKind::DEVICE) {
                    int n = snprintf(body, sizeof(body), "bat");
                    if (tel.hasBatteryPct)
                        n += snprintf(body + n, sizeof(body) - n,
                                      " %.0f%%", (double)tel.batteryPct);
                    if (tel.hasVoltage)
                        n += snprintf(body + n, sizeof(body) - n,
                                      " %.2fV", (double)tel.voltage);
                    decoded = true;
                } else if (tel.kind == TKind::ENVIRONMENT) {
                    int n = snprintf(body, sizeof(body), "env");
                    if (tel.hasTempC)
                        n += snprintf(body + n, sizeof(body) - n,
                                      " %.1fC", (double)tel.tempC);
                    if (tel.hasRhPct)
                        n += snprintf(body + n, sizeof(body) - n,
                                      " RH %.0f%%", (double)tel.rhPct);
                    if (tel.hasPressureHpa)
                        n += snprintf(body + n, sizeof(body) - n,
                                      " %.0fhPa", (double)tel.pressureHpa);
                    decoded = true;
                }
            }
        }
        if (!decoded) {
            decoded = MeshDecoderDebug::extractMeshtasticBody(
                buf, len, srcChan, body, sizeof(body));
        }
    } else if (srcChan.protocol == MeshDecoderDebug::SYNC_WORD_MESHCORE) {
        uint32_t mcTs = 0;
        decoded = MeshDecoderDebug::extractMeshCoreBody(
            buf, len, srcChan, body, sizeof(body), &mcTs);
        if (decoded) learnClockFromMc(mcTs);   // learn wall-clock for MC TX ts
    } else {
        return;
    }
    if (!decoded) return;

    // Loop/dup guard on the CLEAN body (replaces the old marker check). A hit
    // means a mesh-flood replay, the same packet heard on a second radio, or an
    // echo of one of our own emissions — drop it. See DedupCache.h.
    char idbuf[10];
    uint32_t hin = DedupCache::hash((const uint8_t *)body, strlen(body), srcId, pid);
    if (DedupCache::seenAndRecord(hin)) {
        blogf("ts=%lu evt=DROP radio=%s proto=%s drop=loop-dup nodeid=%s hin=0x%08lx msg=\"%s\"\n",
              (unsigned long)millis(), srcTag, protoTag(srcChan.protocol),
              fmtNodeId(srcId, idbuf), (unsigned long)hin, body);
        return;
    }
    blogf("ts=%lu evt=DEDUP_PASS radio=%s proto=%s nodeid=%s hin=0x%08lx msg=\"%s\"\n",
          (unsigned long)millis(), srcTag, protoTag(srcChan.protocol),
          fmtNodeId(srcId, idbuf), (unsigned long)hin, body);

    // Fan out to every (other) enabled destination. Per destination:
    //   - SAME channel (same protocol+hash+key, different frequency) -> a
    //     transparent RAW repeat that preserves the original sender natively
    //     (V8.2-SPEC §5.1). Covers the text/position/telemetry decoded above;
    //     NodeInfo is consumed for NodeDB earlier and not raw-repeated in v8.2.
    //   - otherwise -> the identity-aware cross-protocol / re-encode path
    //     (virtual node, name prefix, or bridge identity) in enqueueTextForDest.
    for (int j = 0; j < NR; j++) {
        if (j == srcIdx) continue;
        if (!g_radioEnabled[j] || !g_radio[j]) continue;
        if (!(g_routeMask[srcIdx] & (1u << j))) continue;   // routing matrix
        if (BRIDGE_IDENTITY_PRESERVE && sameChannel(srcChan, g_chan[j]))
            rawRepeatForDest(g_chan[j], j, srcTag, srcId, buf, len);
        else
            enqueueTextForDest(srcChan, srcId, g_chan[j], j, srcTag, body);
    }
}

// Estimate LoRa time-on-air in ms for a packet of `payloadLen` bytes at the
// given modulation (Semtech AN1200.13 / SX1276 datasheet §4.1.1.7). Assumes
// explicit header + CRC on. Low-data-rate optimisation follows RadioLib's rule
// (enabled when the symbol time reaches 16 ms). Used by the airtime throttle.
static uint32_t estimateAirtimeMs(size_t payloadLen, uint8_t sf, float bwKhz,
                                  uint8_t crDenom, uint16_t preambleLen)
{
    if (sf < 5)  sf = 5;
    if (sf > 12) sf = 12;
    if (bwKhz <= 0.0f) bwKhz = 125.0f;
    int cr = (int)crDenom - 4;            // denom 5..8 -> 1..4
    if (cr < 1) cr = 1;
    if (cr > 4) cr = 4;

    float tSym = (float)(1UL << sf) / bwKhz;          // symbol time, ms
    int   de   = (tSym >= 16.0f) ? 1 : 0;             // low-data-rate optimise

    float num = 8.0f * (float)payloadLen - 4.0f * sf + 28.0f + 16.0f /*CRC*/;
    float den = 4.0f * (float)(sf - 2 * de);
    int   payloadSymb = 8;
    if (num > 0.0f && den > 0.0f) {
        float b = num / den;
        int blocks = (int)b;
        if ((float)blocks < b) blocks++;              // ceil
        payloadSymb += blocks * (cr + 4);
    }
    float toa = ((float)preambleLen + 4.25f) * tSym + (float)payloadSymb * tSym;
    return (uint32_t)(toa + 0.999f);                  // round up to whole ms
}

// (P4) Per-TX dwell-time limit (ms) for a region's band, or 0 when the region is
// governed by duty cycle instead (enforced by BRIDGE_TX_DUTY_PERCENT). The FCC
// US915 LoRaWAN band caps each transmission at 400 ms time-on-air; EU868 has no
// per-TX dwell limit (it's a ~1%/sub-band duty cycle).
static uint32_t regionDwellMs(uint8_t region) {
    switch (region) {
        case BridgeConfig::REGION_US: return 400;
        default:                      return 0;
    }
}

// ============================================================
//  Generic per-radio FreeRTOS task — the RX-priority loop (V8.2-SPEC.md).
//  Each iteration, in priority order:
//    (A) service an in-flight non-blocking TX (wait for done / safety timeout);
//    (B) drain RX — read fast, re-arm RX, then decode + dedup + fan out (cheap);
//    (C) enqueue periodic MT NodeInfo onto our own queue;
//    (D) TX scheduler — pop ONE queued packet, CAD (listen-before-talk), and
//        start a non-blocking send.
//  Because the on-air wait happens between iterations (in (A)) rather than
//  inside transmit(), a slow TX on this radio never blocks any radio's RX.
// ============================================================
void radioTask(void *pvParameters)
{
    const int   i    = (int)(intptr_t)pvParameters;
    LoraRadio  *self = g_radio[i];
    const char *tag  = kTag[i];
    uint8_t     buf[LORA_MAX_PACKET];

    const bool isMT = (g_chan[i].protocol == MeshDecoderDebug::SYNC_WORD_MESHTASTIC);
    uint32_t   nextNodeInfoMs = millis() + 10000;

    for (;;) {
        // --- (A) service an in-flight non-blocking TX --------------------
        if (g_txBusy[i]) {
            bool done     = self->txDone();
            bool timedOut = (millis() - g_txStartMs[i]) > BRIDGE_TX_INFLIGHT_TIMEOUT_MS;
            if (done || timedOut) {
                self->finishTransmit();          // cleanup + return to RX
                g_txBusy[i] = false;
                blogf("ts=%lu evt=TX_DONE radio=%s result=%s\n",
                      (unsigned long)millis(), tag,
                      (timedOut && !done) ? "timeout-recovered" : "done");
            }
            vTaskDelay(pdMS_TO_TICKS(BRIDGE_POLL_MS));
            continue;   // radio is mid-TX (deaf) — nothing else to do
        }

        // --- (B) RX drain: read fast, re-arm RX, THEN decode + fan out ---
        if (self->available()) {
            size_t len  = sizeof(buf);
            float  rssi = 0.0f, snr = 0.0f;
            int16_t state = self->read(buf, len, &rssi, &snr);
            if (state == RADIOLIB_ERR_NONE && len > 0) {
                self->startReceive();            // back to listening BEFORE decode
                blogf("ts=%lu evt=RX radio=%s proto=%s len=%u rssi=%.1f snr=%.1f\n",
                      (unsigned long)millis(), tag, protoTag(g_chan[i].protocol),
                      (unsigned)len, rssi, snr);
                // The decoder emits a multi-line block via raw Serial; bracket
                // it so the other task's output can't split it mid-block. (The
                // lock is released before ingestAndFanout, which logs its own
                // atomic evt= lines and can run long.)
                SerialLog::lock();
                MeshDecoderDebug::print(buf, len, g_chan[i], tag);
                SerialLog::unlock();
                ingestAndFanout(i, buf, len);
            } else if (state != RADIOLIB_ERR_NONE) {
                blogf("ts=%lu evt=DROP radio=%s drop=rx-error rc=%d\n",
                      (unsigned long)millis(), tag, state);
                self->startReceive();
            }
        }

        // --- (C) periodic MT NodeInfo -> our own queue (CAD-gated like any TX)
        if (isMT && (int32_t)(millis() - nextNodeInfoMs) >= 0) {
            uint8_t niPkt[256];
            size_t  niLen = 0;
            if (MeshEncoderDebug::encodeMeshtasticNodeInfo(
                    g_chan[i], BridgeConfig::mtNodeId(),
                    BridgeConfig::mtNodeIdStr(), BridgeConfig::mtLongName(),
                    BridgeConfig::mtShortName(), niPkt, sizeof(niPkt), niLen)) {
                if (g_routeQ[i].push(niPkt, niLen))
                    blogf("ts=%lu evt=NODEINFO radio=%s op=mint selfid=%s len=%u\n",
                          (unsigned long)millis(), tag,
                          BridgeConfig::mtNodeIdStr(), (unsigned)niLen);
            } else {
                blogf("ts=%lu evt=DROP radio=%s drop=encode-fail what=nodeinfo\n",
                      (unsigned long)millis(), tag);
            }
            nextNodeInfoMs = millis() + 300000;   // every 5 min
        }

        // --- (D) TX scheduler: CAD-gated non-blocking send of one queued pkt
        if (!g_txPendingValid[i]) {
            if (g_routeQ[i].pop(g_txPending[i])) g_txPendingValid[i] = true;
        }
        // Expire a LATCHED packet that aged past max-age while CAD kept it
        // waiting on a jammed channel. RouteQueue::pop() only prunes at pop time,
        // so a packet popped just under the limit could otherwise be sent stale —
        // "stale mesh text is not worth airtime" (RouteQueue.h policy). Unsigned
        // age compare, matching pop(); NOT the signed deadline idiom used for the
        // backoff/throttle windows below.
        if (g_txPendingValid[i] && BRIDGE_ROUTE_MAX_AGE_MS &&
            (uint32_t)(millis() - g_txPending[i].enqueueMs) > (uint32_t)BRIDGE_ROUTE_MAX_AGE_MS) {
            blogf("ts=%lu evt=DROP radio=%s drop=latch-stale age=%lu\n",
                  (unsigned long)millis(), tag,
                  (unsigned long)(millis() - g_txPending[i].enqueueMs));
            g_txPendingValid[i] = false;
        }
        // Keep RX strictly first: skip TX while a packet is waiting to be read,
        // while inside a CSMA backoff window, or while the airtime throttle is
        // holding this radio off the air.
        if (g_txPendingValid[i] && !self->available() &&
            (int32_t)(millis() - g_txBackoffUntil[i])  >= 0 &&
            (int32_t)(millis() - g_nextTxAllowedMs[i]) >= 0) {
            int16_t cad = self->scanChannel();
            if (cad == RADIOLIB_LORA_DETECTED) {
                // Busy: random CSMA backoff, stay in RX, keep the pending job.
                uint32_t bo = (uint32_t)random(BRIDGE_CAD_BACKOFF_MIN_MS,
                                               BRIDGE_CAD_BACKOFF_MAX_MS + 1);
                g_txBackoffUntil[i] = millis() + bo;
                self->startReceive();
                blogf("ts=%lu evt=CAD radio=%s cad=busy backoff=%lu\n",
                      (unsigned long)millis(), tag, (unsigned long)bo);
            } else {
                size_t  txLen = g_txPending[i].len;
                // (P4) US915 per-TX dwell cap: drop a LoRaWAN transmission whose
                // time-on-air would exceed the regional dwell limit (FCC 400 ms)
                // rather than emit an out-of-spec uplink. Other protocols and
                // duty-cycle regions (regionDwellMs == 0) are unaffected.
                uint32_t dwellLim = regionDwellMs(BridgeConfig::region());
                bool dwellBlocked = false;
                if (dwellLim &&
                    g_chan[i].protocol == MeshDecoderDebug::SYNC_WORD_LORAWAN) {
                    uint32_t toa = estimateAirtimeMs(
                        txLen, BridgeConfig::radioSf(i),
                        BridgeConfig::radioBandwidth(i),
                        BridgeConfig::radioCr(i), LORA_PREAMBLE_LEN);
                    if (toa > dwellLim) {
                        blogf("ts=%lu evt=DROP radio=%s drop=dwell toa=%lu limit=%lu len=%u\n",
                              (unsigned long)millis(), tag, (unsigned long)toa,
                              (unsigned long)dwellLim, (unsigned)txLen);
                        g_txPendingValid[i] = false;
                        self->startReceive();
                        dwellBlocked = true;
                    }
                }
                int16_t txs = dwellBlocked
                    ? (int16_t)RADIOLIB_ERR_NONE
                    : self->startTransmit(g_txPending[i].data, txLen);
                if (!dwellBlocked && txs == RADIOLIB_ERR_NONE) {
                    g_txBusy[i]         = true;
                    g_txStartMs[i]      = millis();
                    g_txPendingValid[i] = false;
                    blogf("ts=%lu evt=TX_START radio=%s cad=clear len=%u rc=0\n",
                          (unsigned long)millis(), tag, (unsigned)txLen);
                    // Airtime throttle: hold this radio off the air until its
                    // own duty cycle is back under the ceiling.
                    uint32_t air = estimateAirtimeMs(
                        txLen, BridgeConfig::radioSf(i),
                        BridgeConfig::radioBandwidth(i),
                        BridgeConfig::radioCr(i), LORA_PREAMBLE_LEN);
                    // DUTY=0 means "no duty cap" (just the min-gap floor); guard
                    // the division either way.
                    uint32_t gap = (BRIDGE_TX_DUTY_PERCENT > 0)
                        ? (air * 100u) / (uint32_t)BRIDGE_TX_DUTY_PERCENT
                        : air;
                    if (gap < air + BRIDGE_TX_MIN_GAP_MS)
                        gap = air + BRIDGE_TX_MIN_GAP_MS;
                    g_nextTxAllowedMs[i] = g_txStartMs[i] + gap;
                    blogf("ts=%lu evt=THROTTLE radio=%s air=%lu gap=%lu nexttx=%lu\n",
                          (unsigned long)millis(), tag, (unsigned long)air,
                          (unsigned long)gap, (unsigned long)g_nextTxAllowedMs[i]);
                } else if (!dwellBlocked) {
                    blogf("ts=%lu evt=DROP radio=%s drop=tx-startfail rc=%d\n",
                          (unsigned long)millis(), tag, txs);
                    g_txPendingValid[i] = false;
                    self->startReceive();
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(BRIDGE_POLL_MS));
    }
}

// ============================================================
//  setup()
// ============================================================
void setup()
{
    // Enlarge the USB-CDC TX ring so a burst of output (e.g. the co-proc-reboot
    // recovery, or both radios decoding at once) is buffered instead of dropped.
    // Must be set BEFORE begin(), mirroring UartLink's RX-side setRxBufferSize.
    Serial.setTxBufferSize(4096);
    Serial.begin(115200);
    while (!Serial && millis() < 3000);
    Serial.println("\n=== XIAO ESP32S3 Dual SX1262 Cross-Protocol Bridge (v8.2) ===");

#if BRIDGE_LW_ENC_SELFTEST
    // ABP P1/#2: prove the ABP encoder crypto on-device before it ever emits. A
    // FAIL latches g_lwCryptoOk=false so enqueueAbpUplink refuses to emit (a
    // wrong-MIC frame would otherwise be silently dropped at the LNS).
    bool lwSelfOk = LoRaWANCrypto::selfTest();
  #if BRIDGE_LW_ENCODE
    g_lwCryptoOk = lwSelfOk;
  #endif
    (void)lwSelfOk;
#endif

    // One recursive lock serialises ALL console output (blogf evt= lines, the
    // per-packet decoder dump, and the link/status prints) across the two
    // core-pinned radio tasks + the link task. Created first so every later log
    // is atomic.
    SerialLog::begin();

    // Bridge configuration: NVS first, build-flag defaults otherwise.
    BridgeConfig::begin();

#if BRIDGE_LW_ENCODE
    // ABP P2: load the per-source LoRaWAN ABP device table + persisted FCnts.
    LoRaWANConfig::begin();
    {
        const LwEncCreds &lw = lwEncCreds();
        // Pre-flight echo: confirm the active ABP identity from serial BEFORE
        // sending traffic (avoids a wasted test run on a mis-set board). Keys are
        // never printed — only DevAddr/FPort/ready + whether any NVS device exists.
        Serial.printf("[lw-enc] ABP encoder ON — build-flag creds ready=%d "
                      "DevAddr=0x%08lX FPort=%u; NVS devices configured=%d\n",
                      lw.ready ? 1 : 0, (unsigned long)lw.devAddr, (unsigned)lw.fport,
                      LoRaWANConfig::anyConfigured() ? 1 : 0);
        // (#4) warn if the build-flag fallback identity collides with a provisioned
        // device — two RAM counters on one DevAddr can issue a replayed FCnt.
        if (lw.ready && LoRaWANConfig::hasDevAddr(lw.devAddr))
            Serial.printf("[lw-warn] build-flag DevAddr 0x%08lx also configured as a "
                          "device — use distinct DevAddrs to avoid an FCnt collision\n",
                          (unsigned long)lw.devAddr);
    }
#endif

    // First boot (nothing saved): seed a unique MAC-derived identity so the
    // captive-portal form pre-fills with a per-device node ID + SSID rather
    // than a shared build-flag default. Portal save persists it.
    if (!BridgeConfig::isConfigured())
        deriveMacIdentity();

#ifdef BRIDGE_BENCH_AUTOSAVE
    // Bench-only (NEVER in a release build): persist the build-flag defaults on
    // first boot so an erased board comes straight up as a configured bridge and
    // SKIPS the captive portal entirely. The 5 s BOOT/serial window below still
    // lets you reach the portal on demand.
    if (!BridgeConfig::isConfigured()) {
        BridgeConfig::save();
        Serial.println("[setup] BRIDGE_BENCH_AUTOSAVE: persisted build-flag config — skipping portal");
    }
#endif

    BridgeConfig::debugDump();

    // Captive portal trigger:
    //   - First-flash path: NVS has no saved config yet (isConfigured()==false).
    //   - Recovery path:    within a short window after boot, either the BOOT
    //                       button (GPIO0, active-LOW) is pressed, OR any byte
    //                       arrives on the serial monitor. The serial route
    //                       exists because on this hardware stack the BOOT
    //                       button is physically hidden under the radio shield.
    // The portal call is blocking — it ESP.restart()s once the form saves —
    // so the radio init below it never runs while the portal is up.
    {
        pinMode(0, INPUT_PULLUP);
        if (!BridgeConfig::isConfigured()) {
            Serial.println("[setup] no config in NVS — entering captive portal");
            CaptivePortal::begin();   // never returns
        }
        const uint32_t windowMs  = 5000;
        const uint32_t windowEnd = millis() + windowMs;
        bool trigger = false;
        Serial.printf("[setup] press BOOT — or send any character over serial — "
                      "within %lu s to enter the config portal...\n",
                      (unsigned long)(windowMs / 1000));
        while (millis() < windowEnd) {
            if (digitalRead(0) == LOW) {
                Serial.println("[setup] BOOT pressed");
                trigger = true; break;
            }
            if (Serial.available() > 0) {
                Serial.println("[setup] serial input received");
                trigger = true; break;
            }
            delay(20);
        }
        if (trigger) {
            while (Serial.available()) Serial.read();   // drain pending input
            Serial.println("[setup] entering captive portal");
            CaptivePortal::begin();   // never returns
        }
        Serial.println("[setup] proceeding to bridge mode");
    }

    // Per-radio enable (protocol != None).
    for (int i = 0; i < NR; i++) {
        g_radioEnabled[i] = (BridgeConfig::radioProtocol(i) != BridgeConfig::PROTO_NONE);
        if (!g_radioEnabled[i])
            Serial.printf("[setup] Radio%d protocol = None — disabled\n", i + 1);
    }

    // Resolve each enabled radio's channel into g_chan[] before any RX, and load
    // the per-radio routing matrix (v8.5). Generalized to all NUM_RADIOS radios.
    for (int i = 0; i < NR; i++) {
        if (g_radioEnabled[i])
            resolveRadioChannel(BridgeConfig::radioSyncWord(i),
                                BridgeConfig::radioChannelName(i),
                                BridgeConfig::radioChannelKey(i), g_chan[i]);
        g_routeMask[i] = BridgeConfig::radioRouteMask(i);
    }

    // NodeDB is populated from received NodeInfo. With clean far-side bodies
    // (V8.2-SPEC §3.1) it is currently consulted only by the identity layer;
    // see the next commit. Kept persistent across boots.
    NodeDB::begin();
    NodeDB::debugDump();

    // RX-priority pipeline state: the content-hash loop/dup guard and one
    // PSRAM-backed outbound queue per enabled radio. Seed the RNG used for CSMA
    // backoff from the hardware RNG so co-located bridges don't back off in
    // lock-step.
    DedupCache::begin();
    VirtualNodeMap::begin();   // MC->MT source-identity (V8.2-SPEC §5.3)
    randomSeed(esp_random());
    for (int i = 0; i < NR; i++) {
        if (g_radioEnabled[i])
            g_routeQ[i].begin(BRIDGE_ROUTE_QUEUE_DEPTH, BRIDGE_ROUTE_MAX_AGE_MS, kTag[i]);
    }

    // Start shared SPI bus with explicit XIAO ESP32S3 pin mapping
    spi.begin(SPI_SCK, SPI_MISO, SPI_MOSI);

    // Mutex must exist before any radio object is constructed
    spiMutex = xSemaphoreCreateMutex();
    configASSERT(spiMutex != NULL);

    // Park BOTH chip-selects HIGH before ANY radio is constructed or probed —
    // including a DISABLED slot. The WioSX1262 ctor deasserts its own CS, but it
    // only runs for ENABLED slots; a disabled radio would otherwise leave its CS
    // floating, and that chip can then drive the shared MISO during the other
    // radio's begin(), corrupting detection. Observed on the bench: setting
    // Radio 2 = None made Radio 1 begin() fail with CHIP_NOT_FOUND (MISO read
    // 0x2A then 0x00). Deasserting here is safe and idempotent with the ctor.
    pinMode(R1_NSS, OUTPUT); digitalWrite(R1_NSS, HIGH);
    pinMode(R2_NSS, OUTPUT); digitalWrite(R2_NSS, HIGH);

    // Echo the compiled-in Radio-2 edge-module pin map so a serial log always
    // self-identifies which board revision this firmware was built for. If
    // Radio-2 fails to detect, this line is the first thing to check against
    // the module silkscreen (V1.0 vs V1.1 — see README "Wiring").
    Serial.printf("[diag] R2 edge module = %s  (NSS=%d DIO1=%d RST=%d BUSY=%d RF_SW=%d)\n",
                  WIO_SX1262_REV_STR, R2_NSS, R2_DIO1, R2_RESET, R2_BUSY, R2_ANT_SW);

    // Construct radio objects now the mutex and SPI bus are ready. RF comes
    // from BridgeConfig at runtime (makeLoraConfig). A disabled slot stays null.
    if (g_radioEnabled[0])
        g_radio[0] = new WioSX1262(R1_NSS, R1_DIO1, R1_RESET, R1_BUSY,
                                   R1_ANT_SW, spi, spiMutex, "Radio1-B2B",
                                   makeLoraConfig(0));
    if (g_radioEnabled[1])
        g_radio[1] = new WioSX1262(R2_NSS, R2_DIO1, R2_RESET, R2_BUSY,
                                   R2_ANT_SW, spi, spiMutex, "Radio2-Edge",
                                   makeLoraConfig(1));

    // R3/R4 — SX1262 on the SECOND XIAO, reached over the UART crossover. They
    // are RemoteRadio (no local SPI): construct them first (each registers its
    // RX queue with g_link), then open the link so its RX service task can
    // deliver inbound packets to those queues. The link is opened only when R3
    // or R4 is enabled, so a single-board build never touches Serial1. (v8.5)
    const bool linkNeeded = g_radioEnabled[2] || g_radioEnabled[3];
    if (g_radioEnabled[2])
        g_radio[2] = new RemoteRadio(g_link, /*co-proc R1*/ 0, kTag[2],
                                     makeLoraConfig(2));
    if (g_radioEnabled[3])
        g_radio[3] = new RemoteRadio(g_link, /*co-proc R2*/ 1, kTag[3],
                                     makeLoraConfig(3));
    if (linkNeeded)
        g_link.begin(BRIDGE_LINK_BAUD, BRIDGE_LINK_RX_PIN, BRIDGE_LINK_TX_PIN);

    // HARDENING: hold Radio 2 in hardware RESET (NRESET low) for the WHOLE of
    // Radio 1's probe + begin(). A chip held in reset tri-states its SPI pins,
    // so it physically cannot drive the shared MISO and corrupt R1 detection —
    // regardless of whether the R2 pin map matches the physical module (e.g. a
    // V1.1 module flashed with the V1.0 variant, which floats R2's real CS). RST
    // is GPIO3 (D2) on BOTH the V1.0 and V1.1 edge modules, so this assert holds
    // either revision in reset. R2 is released + brought up only AFTER R1 is up.
    if (g_radioEnabled[1]) { pinMode(R2_RESET, OUTPUT); digitalWrite(R2_RESET, LOW); }

    // Allow B2B power rail and SX1262 TCXO to settle before first SPI access.
    delay(150);

    // Pre-flight (enabled radios only): pulse RESET and watch BUSY drain LOW.
    // BUSY stuck HIGH 50 ms after reset => module absent/unpowered (wiring).
    auto busyWait = [](int resetPin, int busyPin, const char *label) {
        pinMode(resetPin, OUTPUT);
        pinMode(busyPin,  INPUT);   // configure before digitalRead(), else newer
                                    // arduino-esp32 cores log "IO N is not GPIO"
        digitalWrite(resetPin, LOW);
        delay(2);
        digitalWrite(resetPin, HIGH);
        uint32_t t0 = millis();
        while (digitalRead(busyPin) && millis() - t0 < 50);
        Serial.printf("[diag] %s  BUSY after reset = %d  (%lu ms)  %s\n",
                      label, digitalRead(busyPin), millis() - t0,
                      digitalRead(busyPin) ? "STUCK-HIGH -> module absent/unpowered!" : "OK");
    };
    // Only Radio 1 is pre-flighted here. Radio 2 stays held in reset (asserted
    // above) so it cannot touch the shared bus while R1 is detected; it is
    // reset/checked below, after R1 is confirmed up.
    if (g_radioEnabled[0]) busyWait(R1_RESET, R1_BUSY, "R1");

    // Raw SPI probe on R1 (SX1262 GetStatus 0xC0 + version reg) — bypasses
    // RadioLib so we can see what MISO carries. 0x00/0xFF = MISO open;
    // 0x20-0x2E = chip alive.
    if (g_radioEnabled[0]) {
        SPI.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));
        digitalWrite(R1_NSS, LOW);
        delayMicroseconds(2);
        SPI.transfer(0xC0);                     // GetStatus opcode
        uint8_t r1Status = SPI.transfer(0x00);  // read status byte
        digitalWrite(R1_NSS, HIGH);
        SPI.endTransaction();
        Serial.printf("[diag] R1 raw SPI GetStatus = 0x%02X  "
                      "(0x00/0xFF = MISO open; 0x20-0x2E = chip alive)\n", r1Status);

        uint8_t ver[6] = {};
        SPI.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));
        digitalWrite(R1_NSS, LOW);
        delayMicroseconds(2);
        SPI.transfer(0x1D);   // ReadRegister opcode
        SPI.transfer(0x03);   // address MSB
        SPI.transfer(0x20);   // address LSB
        SPI.transfer(0x00);   // NOP transition byte (required before data)
        for (int n = 0; n < 6; n++) { ver[n] = SPI.transfer(0x00); }
        digitalWrite(R1_NSS, HIGH);
        SPI.endTransaction();
        Serial.printf("[diag] R1 reg 0x0320 raw: %02X %02X %02X %02X %02X %02X  = '%.6s'\n",
                      ver[0], ver[1], ver[2], ver[3], ver[4], ver[5], (char*)ver);
    }

    // --- Radio 1 (B2B) — Radio 2 is still held in hardware reset, so a
    // misconfigured/contending R2 can no longer make this fail. If R1 still
    // fails here it is a genuine R1 module/wiring fault, so it stays fatal.
    if (g_radioEnabled[0] && g_radio[0] && !g_radio[0]->begin()) {
        Serial.println("\nFATAL: Radio1 (B2B) init failed. Check the B2B module/wiring. Halting.");
        while (true) { vTaskDelay(pdMS_TO_TICKS(1000)); }
    }

    // --- Radio 2 (edge) — release it from reset now that R1 is up, pre-flight,
    // then begin(). R2 failing is NON-fatal: disable the slot and run single-
    // radio. The most common cause is a module-revision / pin mismatch, so point
    // the operator straight at the silkscreen + the matching build variant.
    if (g_radioEnabled[1] && g_radio[1]) {
        busyWait(R2_RESET, R2_BUSY, "R2 release");   // pulse RESET high, drain BUSY
        if (!g_radio[1]->begin()) {
            Serial.printf(
                "\n[WARN] Radio2 (edge) not detected — running single-radio (R1 only).\n"
                "       This build targets module revision %s. If your Radio-2 module\n"
                "       silkscreen reads a DIFFERENT revision, flash the matching variant:\n"
                "         V1.0 -> env xiao_esp32s3   |   V1.1 -> env xiao_esp32s3_v1_1\n"
                "       (See README \"Wiring\" to identify your revision.)\n",
                WIO_SX1262_REV_STR);
            g_radioEnabled[1] = false;
        }
    }

    // R3/R4 (remote SX1262 on the second XIAO): push their RF config to the
    // co-processor over the now-open link. RemoteRadio::begin() sends
    // CFG_RADIO + START_RX; a "false" return only means the UART write failed —
    // co-processor liveness is tracked via MSG_READY/PONG, not gated here, so a
    // missing peer just leaves R3/R4 silent (non-fatal). (v8.5)
    for (int i = 2; i < NR; i++) {
        if (g_radioEnabled[i] && g_radio[i] && !g_radio[i]->begin())
            Serial.printf("[WARN] Radio%d (remote) link write failed — check the "
                          "UART crossover to the second XIAO.\n", i + 1);
    }

    // Start enabled radios listening
    for (int i = 0; i < NR; i++)
        if (g_radioEnabled[i] && g_radio[i]) g_radio[i]->startReceive();

    Serial.println("\nBridge active.\n");

    // Spawn one task per enabled radio, distributed across the two cores.
    int spawned = 0;
    for (int i = 0; i < NR; i++) {
        if (!g_radioEnabled[i] || !g_radio[i]) continue;
        char taskName[8];
        snprintf(taskName, sizeof(taskName), "R%d_task", i + 1);
        xTaskCreatePinnedToCore(radioTask, taskName,
                                BRIDGE_TASK_STACK, (void *)(intptr_t)i,
                                BRIDGE_TASK_PRIO, NULL, i % 2);
        spawned++;
    }
    if (spawned == 0)
        Serial.println("[setup] WARNING: no radios enabled — bridge idle.");
}

// ============================================================
//  loop() — bridge runs entirely in FreeRTOS tasks
// ============================================================
void loop()
{
    // Self-heal the co-processor link. The co-proc sends MSG_READY at every boot
    // and then sits idle until it receives CFG_RADIO. If it resets (brown-out,
    // re-flash, reseated cable) after we already configured it at startup, it
    // would otherwise stay silent until the HOST is rebooted. So whenever we see
    // a NEW READY (the generation count moved since we last configured it),
    // re-push each remote radio's CFG_RADIO + START_RX. The very first READY
    // after our own boot also lands here, which harmlessly re-sends config and
    // closes the boot-time race where setup() may have sent CFG before the
    // co-proc was listening. On a single-board build the link is never opened, so
    // readyGen() stays 0 and this is a no-op (do-no-harm). (v8.5)
    static uint32_t s_lastReadyGen = 0;
    uint32_t gen = g_link.readyGen();
    if (gen != s_lastReadyGen) {
        s_lastReadyGen = gen;
        int n = 0;
        for (int i = 2; i < NR; i++) {
            if (g_radioEnabled[i] && g_radio[i] && g_radio[i]->begin()) n++;
        }
        SerialLog::logf("[link] co-proc READY gen=%lu -> re-pushed config to %d "
                        "remote radio(s)\n", (unsigned long)gen, n);
    }

    vTaskDelay(pdMS_TO_TICKS(1000));
}
