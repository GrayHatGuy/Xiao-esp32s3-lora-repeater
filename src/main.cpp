/**
 * main.cpp
 * ========
 * Quad-radio cross-protocol LoRa bridge for the XIAO ESP32S3.
 *
 *   R1/R2 = SX1262 (or LR1121) on the XIAO SPI bus (WioSX1262 / WioLR1121).
 *   R3/R4 = LR1121 on the LilyGO T-Lora-Dual co-processor, reached over a
 *           framed UART link (RemoteRadio + UartLink + LinkProtocol).
 *
 * Bridge logic: each radio's RX is repeated to every radio selected in that
 * radio's routeMask (the portal-configurable routing matrix), with the
 * existing cross-protocol translation. Default routeMask reproduces the
 * historical R1<->R2 crossover; R3/R4 are disabled until configured.
 *
 * All LoRa RF settings (frequency, BW, SF, CR, power, sync word, band) are
 * resolved at runtime from BridgeConfig (NVS / captive portal). The
 * platformio.ini LORA_RADIO*_* build flags only seed first-boot defaults.
 */

#include <Arduino.h>
#include <SPI.h>
#include <stdint.h>
#include "WioSX1262.h"
#include "WioLR1121.h"
#include "RemoteRadio.h"
#include "UartLink.h"
#include "RadioProfile.h"
#include "MeshDecoderDebug.h"
#include "MeshEncoderDebug.h"
#include "MeshCoreConfig.h"
#include "MeshtasticConfig.h"
#include "BridgeConfig.h"
#include "CaptivePortal.h"
#include "NodeDB.h"
#include "DedupCache.h"        // content-hash loop/dup guard (replaces marker)
#include "RouteQueue.h"        // per-destination outbound queue
#include "RegionPreset.h"
#include "LoraConfigCheck.h"   // compile-time validation of LORA_RADIO* flags
#include <esp_mac.h>

// Build a LoraConfig for one radio (index 0..NUM_RADIOS-1) from the live
// BridgeConfig. TCXO voltage is chip-dependent (SX1262 vs LR1121 module).
static LoraConfig makeLoraConfig(int radio, uint8_t chip)
{
    LoraConfig c;
    c.frequency    = BridgeConfig::radioFrequency(radio);
    c.bandwidth    = BridgeConfig::radioBandwidth(radio);
    c.spreadFactor = BridgeConfig::radioSf(radio);
    c.codingRate   = BridgeConfig::radioCr(radio);
    c.syncWord     = BridgeConfig::radioSyncWord(radio);
    c.txPower      = BridgeConfig::radioTxPower(radio);
    c.preambleLen  = LORA_PREAMBLE_LEN;
    c.tcxoVoltage  = (chip == BridgeConfig::CHIP_LR1121)
                         ? LR1121_TCXO_VOLTAGE : LORA_TCXO_VOLTAGE;
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
    Serial.printf("[setup] MAC-derived identity: 0x%08lX (%s)\n",
                  (unsigned long)id, idStr);
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
#define R1_NSS      41  // SPI chip-select  (B2B / A11)
#define R1_DIO1     39  // IRQ              (B2B)
#define R1_RESET    42  // Reset            (B2B / A12)
#define R1_BUSY     40  // Busy             (B2B)
#define R1_ANT_SW   38  // Antenna switch   (B2B)

// ============================================================
//  Radio 2 — Wio SX1262 via perimeter (edge) header pins
//  Pin order on module left side (top → bottom): D0, DIO1, RST, BUSY, NSS, RF_SW
//  XIAO ESP32S3: D0=GPIO1, D1=GPIO2, D2=GPIO3, D3=GPIO4, D4=GPIO5, D5=GPIO6
// ============================================================
#define R2_NSS      5   // SPI chip-select  (D4 / GPIO5)
#define R2_DIO1     2   // IRQ              (D1 / GPIO2)
#define R2_RESET    3   // Reset            (D2 / GPIO3)
#define R2_BUSY     4   // Busy             (D3 / GPIO4)
#define R2_ANT_SW   6   // RF switch        (D5 / GPIO6)

// ============================================================
//  R3/R4 UART co-processor link (T-Lora-Dual). Build-flag overridable.
//  XIAO UART1 on D6/D7 (GPIO43/44) — the only clean free pads, since
//  `Serial` is USB-CDC. VERIFY against your physical wiring.
// ============================================================
#ifndef BRIDGE_LINK_TX_PIN
  #define BRIDGE_LINK_TX_PIN  43   // D6 -> co-proc RX
#endif
#ifndef BRIDGE_LINK_RX_PIN
  #define BRIDGE_LINK_RX_PIN  44   // D7 <- co-proc TX
#endif
#ifndef BRIDGE_LINK_BAUD
  #define BRIDGE_LINK_BAUD    460800
#endif

// ============================================================
//  FreeRTOS configuration
// ============================================================
// Radio task stack. Bumped from 4096 to 8192 once F1 (NodeDB) landed:
// ingestAndFanout() can nest extractMeshtasticNodeInfo (240 B pt + AES ctx
// ~280 B) under body[256] plus enqueueTextForDest's outPkt[256], and the
// fragmented-RNS path needs similar headroom. 8 KB leaves a comfortable
// margin; the ESP32-S3 has 512 KB SRAM so it's free real estate.
#define BRIDGE_TASK_STACK  8192
#define BRIDGE_TASK_PRIO   2       // above idle, below system
#define BRIDGE_POLL_MS     1       // ms between RX polls

// ============================================================
//  Shared SPI mutex — created in setup(), passed to the local radios
// ============================================================
SemaphoreHandle_t spiMutex = NULL;

// ============================================================
//  Radio table. 0/1 = local (XIAO SPI), 2/3 = remote (T-Lora-Dual UART).
//  Constructed in setup() once the mutex / SPI bus / link are ready.
// ============================================================
static constexpr int NR = BridgeConfig::NUM_RADIOS;   // 4

LoraRadio   *g_radio[NR]       = { nullptr, nullptr, nullptr, nullptr };
RadioChannel g_chan[NR];
bool         g_radioEnabled[NR] = { false, false, false, false };
uint8_t      g_routeMask[NR]    = { 0, 0, 0, 0 };
static const char *kTag[NR]     = { "R1", "R2", "R3", "R4" };

// ============================================================
//  RX-priority routing state (ROUTING-REDESIGN.md)
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
  #define BRIDGE_TX_INFLIGHT_TIMEOUT_MS 10000UL   // > co-proc 8 s TX_DONE timeout
#endif

// --- Full-mesh airtime throttle (ROUTING-REDESIGN task #2) ------------------
// Fanning one received message out to up to 3 destinations can triple the
// bridge's own airtime; without a brake a busy mesh would let the bridge hog
// the channel. After each transmit we hold that radio off the air until its
// own emissions stay under a duty-cycle ceiling: a packet of on-air time A
// makes the next TX wait until A*100/DUTY after it started (so the radio is
// busy A and idle A*(100-DUTY)/DUTY). BRIDGE_TX_MIN_GAP_MS is an absolute floor
// on the post-TX gap. DUTY=100 + MIN_GAP=0 disables the throttle.
#ifndef BRIDGE_TX_DUTY_PERCENT
  #define BRIDGE_TX_DUTY_PERCENT  50
#endif
#ifndef BRIDGE_TX_MIN_GAP_MS
  #define BRIDGE_TX_MIN_GAP_MS    0
#endif

// One UART link shared by the two remote radios (R3/R4). Only opened in
// setup() if at least one remote radio is enabled, so a pure R1/R2 build
// never touches the link GPIOs or spawns the link task.
HardwareSerial g_linkSerial(1);   // UART1
UartLink       g_link(g_linkSerial);

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
//  Cross-protocol bridge core (RX-priority pipeline, ROUTING-REDESIGN.md)
//  ----------------------------------------------------------------
//  RX path (ingestAndFanout): decode the packet ONCE into a clean body (+ a
//  Meshtastic srcId), run it through the content-hash loop/dup guard, then
//  re-encode for each routed destination and PUSH the bytes onto that
//  destination's RouteQueue. No transmit happens here — that is the TX
//  scheduler's job (in radioTask) — so the source radio returns to RX at once.
//
//  Loop prevention no longer uses a prepended "[MT]/[MC]/[rns]" marker: the far
//  side now receives the CLEAN body. Instead, DedupCache remembers the content
//  hash of every packet received AND every packet emitted, so an echo of our
//  own emission (or a mesh-flood replay, or the same packet heard twice) is
//  dropped by hash. See DedupCache.h for why this is loop-safe, including for
//  same-channel cross-band MT/MC twins.
//
//  Protocols: 0x12 MeshCore (GRP_TXT), 0x2B Meshtastic (text/pos/telemetry),
//  0x42 Reticulum (base64, CRC-16 seq id, fragmented across MT/MC text). RNS
//  fragments are queued like any other packet; the airtime throttle paces them
//  (no inline delay).
// ============================================================

// Tuning knobs for the RNS source path. Override any of these via
// -D build flags in platformio.ini.
#ifndef BRIDGE_RNS_MAX_FRAGS
  #define BRIDGE_RNS_MAX_FRAGS         8
#endif
#ifndef BRIDGE_RNS_FRAG_DELAY_MT_MS
  #define BRIDGE_RNS_FRAG_DELAY_MT_MS  2000   // SF11/BW250 — slow airtime
#endif
#ifndef BRIDGE_RNS_FRAG_DELAY_MC_MS
  #define BRIDGE_RNS_FRAG_DELAY_MC_MS  500    // SF7/BW62.5  — fast airtime
#endif

// Per-fragment raw-byte budgets, derived from:
//   max on-air packet sizes: MC = 184 B, MT = 200 B (conservative).
//   prefix overhead "[rns AA X/Y] " = 13 chars.
//   base64 chunk length rounded down to a multiple of 4 so each fragment
//   decodes cleanly without cross-fragment padding tricks.
//
//   MC: max body 170 -> 157 b64 -> 156 b64 -> 117 raw bytes / fragment
//   MT: max body 179 -> 166 b64 -> 164 b64 -> 123 raw bytes / fragment
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
        Serial.printf("[%8lu ms][%s->RNS-src bridge] unknown dst protocol 0x%02X\n",
                      millis(), srcTag, dstChan.protocol);
        return;
    }

    // Fragment count + bound check
    size_t totalFrags = (len + rawPerFrag - 1) / rawPerFrag;
    if (totalFrags > BRIDGE_RNS_MAX_FRAGS) {
        Serial.printf("[%8lu ms][%s->%s bridge] RNS %u B needs %u frags "
                      "(max %u) — drop\n",
                      millis(), srcTag, dstName, (unsigned)len,
                      (unsigned)totalFrags, (unsigned)BRIDGE_RNS_MAX_FRAGS);
        return;
    }

    // Low byte of CRC-16/CCITT over the raw RNS frame is the sequence ID
    // shared by all fragments of this frame.
    uint8_t seq = (uint8_t)(MeshDecoderDebug::crc16_ccitt(buf, len) & 0xFF);

    Serial.printf("[%8lu ms][%s->%s bridge] RNS %u B -> %u frag(s), seq=%02X\n",
                  millis(), srcTag, dstName, (unsigned)len,
                  (unsigned)totalFrags, seq);

    for (size_t idx = 0; idx < totalFrags; idx++) {
        size_t rawStart = idx * rawPerFrag;
        size_t rawLen   = (idx + 1 == totalFrags) ? (len - rawStart) : rawPerFrag;

        // base64 this slice (per-fragment, so no cross-fragment padding)
        unsigned char b64chunk[200];
        size_t b64Len = 0;
        int b64rc = mbedtls_base64_encode(b64chunk, sizeof(b64chunk), &b64Len,
                                           buf + rawStart, rawLen);
        if (b64rc != 0) {
            Serial.printf("[%8lu ms][%s->%s bridge] frag %u/%u base64 fail %d\n",
                          millis(), srcTag, dstName,
                          (unsigned)(idx + 1), (unsigned)totalFrags, b64rc);
            return;
        }

        // Build the marked body and encode for the destination protocol
        char marked[240];
        snprintf(marked, sizeof(marked), "[rns %02X %u/%u] %.*s",
                 seq, (unsigned)(idx + 1), (unsigned)totalFrags,
                 (int)b64Len, (const char *)b64chunk);

        uint8_t outPkt[256];
        size_t  outLen  = 0;
        bool    encoded = false;
        if (dstChan.protocol == MeshDecoderDebug::SYNC_WORD_MESHTASTIC) {
            encoded = MeshEncoderDebug::encodeMeshtasticText(
                          dstChan, BridgeConfig::mtNodeId(),
                          marked, outPkt, sizeof(outPkt), outLen);
        } else {
            encoded = MeshEncoderDebug::encodeMeshCoreGrpTxt(
                          dstChan, marked, /*ts=*/0, outPkt, sizeof(outPkt), outLen);
        }
        if (!encoded) {
            Serial.printf("[%8lu ms][%s->%s bridge] frag %u/%u encode failed\n",
                          millis(), srcTag, dstName,
                          (unsigned)(idx + 1), (unsigned)totalFrags);
            return;
        }

        // Remember this emission (clean fragment body) so its echo is dropped
        // as a loop, then queue it. Inter-fragment pacing is now provided by
        // the destination's airtime throttle, not an inline delay.
        DedupCache::record(
            DedupCache::hash((const uint8_t *)marked, strlen(marked), dstSrcId));
        if (g_routeQ[destIdx].push(outPkt, outLen)) {
            Serial.printf("[%8lu ms][%s->%s queued] frag %u/%u (%u B): \"%s\"\n",
                          millis(), srcTag, dstName,
                          (unsigned)(idx + 1), (unsigned)totalFrags,
                          (unsigned)outLen, marked);
        } else {
            Serial.printf("[%8lu ms][%s->%s bridge] frag %u/%u queue push failed\n",
                          millis(), srcTag, dstName,
                          (unsigned)(idx + 1), (unsigned)totalFrags);
        }
    }
}

// Encode `body` for one destination protocol and ENQUEUE it on that
// destination's RouteQueue (its scheduler CAD-gates + sends). Also records the
// emission's content hash so its echo is recognised as a loop — the job the
// prepended marker used to do, now done by DedupCache with a CLEAN far-side body.
static void enqueueTextForDest(const RadioChannel &dstChan, int destIdx,
                               const char *srcTag, const char *body)
{
    // Destination is Reticulum — log and drop. No RNS encoder yet.
    if (dstChan.protocol == MeshDecoderDebug::SYNC_WORD_RETICULUM) {
        Serial.printf("[%8lu ms][%s->RNS bridge] No TX 2 RNS: %s\n",
                      millis(), srcTag, body);
        return;
    }

    uint8_t     outPkt[256];
    size_t      outLen   = 0;
    bool        encoded  = false;
    const char *dstName  = "?";
    uint32_t    dstSrcId = 0;   // the src we stamp into the re-encoded packet
    switch (dstChan.protocol) {
        case MeshDecoderDebug::SYNC_WORD_MESHTASTIC:
            encoded  = MeshEncoderDebug::encodeMeshtasticText(
                           dstChan, BridgeConfig::mtNodeId(),
                           body, outPkt, sizeof(outPkt), outLen);
            dstName  = "MT";
            dstSrcId = BridgeConfig::mtNodeId();
            break;
        case MeshDecoderDebug::SYNC_WORD_MESHCORE:
            encoded  = MeshEncoderDebug::encodeMeshCoreGrpTxt(
                           dstChan, body, /*ts=*/0, outPkt, sizeof(outPkt), outLen);
            dstName  = "MC";
            dstSrcId = 0;   // MeshCore GRP_TXT has no stable per-sender id
            break;
        default:
            Serial.printf("[%8lu ms][%s bridge] unknown dst protocol 0x%02X — drop\n",
                          millis(), srcTag, dstChan.protocol);
            return;
    }
    if (!encoded) {
        Serial.printf("[%8lu ms][%s->%s bridge] encode failed (body too long?)\n",
                      millis(), srcTag, dstName);
        return;
    }

    // Remember our own emission (clean body + stamped src) so the echo is
    // dropped as a loop, then queue it for the destination's TX scheduler.
    DedupCache::record(
        DedupCache::hash((const uint8_t *)body, strlen(body), dstSrcId));
    if (g_routeQ[destIdx].push(outPkt, outLen)) {
        Serial.printf("[%8lu ms][%s->%s queued] %u B: \"%s\"\n",
                      millis(), srcTag, dstName, (unsigned)outLen, body);
    } else {
        Serial.printf("[%8lu ms][%s->%s bridge] queue push failed\n",
                      millis(), srcTag, dstName);
    }
}

// Decode a received packet ONCE, run the loop/dup guard, and fan the clean body
// out to every routed destination's queue. No transmit happens here — the
// destinations' schedulers do that — so the source radio returns to RX at once.
static void ingestAndFanout(int srcIdx, const uint8_t *buf, size_t len)
{
    const RadioChannel &srcChan = g_chan[srcIdx];
    const char         *srcTag  = kTag[srcIdx];
    const uint8_t       mask    = g_routeMask[srcIdx];

    // RNS source: dedup the raw frame, then fragment to each routed destination.
    if (srcChan.protocol == MeshDecoderDebug::SYNC_WORD_RETICULUM) {
        if (DedupCache::seenAndRecord(DedupCache::hash(buf, len, 0))) {
            Serial.printf("[%8lu ms][%s bridge] loop-drop (RNS dup)\n",
                          millis(), srcTag);
            return;
        }
        for (int j = 0; j < NR; j++) {
            if (j == srcIdx) continue;
            if (!(mask & (uint8_t)(1u << j))) continue;
            if (!g_radioEnabled[j] || !g_radio[j]) continue;
            enqueueReticulumForDest(g_chan[j], j, srcTag, buf, len);
        }
        return;
    }

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
                Serial.printf("[%8lu ms][%s NodeDB] self-echo NodeInfo dropped (!%08lX)\n",
                              millis(), srcTag, (unsigned long)niNodeId);
                return;
            }
            NodeDB::upsert(niNodeId, niShort, niLong);
            Serial.printf("[%8lu ms][%s NodeDB] upsert !%08lX short=\"%s\" long=\"%s\"\n",
                          millis(), srcTag,
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
    char body[256];
    bool decoded = false;

    if (srcChan.protocol == MeshDecoderDebug::SYNC_WORD_MESHTASTIC) {
        if (len >= 8)
            srcId = (uint32_t)buf[4]  | ((uint32_t)buf[5]  << 8)
                  | ((uint32_t)buf[6] << 16) | ((uint32_t)buf[7] << 24);
        if (!decoded && BridgeConfig::positionEnabled()) {
            MeshDecoderDebug::MeshtasticPositionInfo pos;
            if (MeshDecoderDebug::extractMeshtasticPosition(buf, len, srcChan, pos)) {
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
        decoded = MeshDecoderDebug::extractMeshCoreBody(
            buf, len, srcChan, body, sizeof(body));
    } else {
        return;
    }
    if (!decoded) return;

    // Loop/dup guard on the CLEAN body (replaces the old marker check). A hit
    // means a mesh-flood replay, the same packet heard on a second radio, or an
    // echo of one of our own emissions — drop it. See DedupCache.h.
    if (DedupCache::seenAndRecord(
            DedupCache::hash((const uint8_t *)body, strlen(body), srcId))) {
        Serial.printf("[%8lu ms][%s bridge] loop-drop (dup): \"%s\"\n",
                      millis(), srcTag, body);
        return;
    }

    // Fan the CLEAN body out to every routed, enabled destination's queue.
    for (int j = 0; j < NR; j++) {
        if (j == srcIdx) continue;
        if (!(mask & (uint8_t)(1u << j))) continue;
        if (!g_radioEnabled[j] || !g_radio[j]) continue;
        enqueueTextForDest(g_chan[j], j, srcTag, body);
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

// ============================================================
//  Generic per-radio FreeRTOS task — the RX-priority loop (ROUTING-REDESIGN).
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
                if (timedOut && !done)
                    Serial.printf("[%8lu ms][%s TX] done-timeout — recovered\n",
                                  millis(), tag);
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
                Serial.printf("[%8lu ms][%s RX] %u bytes  RSSI %.1f dBm  SNR %.1f dB\n",
                              millis(), tag, (unsigned)len, rssi, snr);
                MeshDecoderDebug::print(buf, len, g_chan[i], tag);
                ingestAndFanout(i, buf, len);
            } else if (state != RADIOLIB_ERR_NONE) {
                Serial.printf("[%8lu ms][%s RX] ERROR %d\n", millis(), tag, state);
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
                    Serial.printf("[%8lu ms][%s NodeInfo] queued %u B id=%s\n",
                                  millis(), tag, (unsigned)niLen,
                                  BridgeConfig::mtNodeIdStr());
            } else {
                Serial.printf("[%8lu ms][%s NodeInfo] encode failed\n", millis(), tag);
            }
            nextNodeInfoMs = millis() + 300000;   // every 5 min
        }

        // --- (D) TX scheduler: CAD-gated non-blocking send of one queued pkt
        if (!g_txPendingValid[i]) {
            if (g_routeQ[i].pop(g_txPending[i])) g_txPendingValid[i] = true;
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
                g_txBackoffUntil[i] = millis() +
                    (uint32_t)random(BRIDGE_CAD_BACKOFF_MIN_MS,
                                     BRIDGE_CAD_BACKOFF_MAX_MS + 1);
                self->startReceive();
            } else {
                size_t  txLen = g_txPending[i].len;
                int16_t txs = self->startTransmit(g_txPending[i].data, txLen);
                if (txs == RADIOLIB_ERR_NONE) {
                    g_txBusy[i]         = true;
                    g_txStartMs[i]      = millis();
                    g_txPendingValid[i] = false;
                    // Airtime throttle: hold this radio off the air until its
                    // own duty cycle is back under the ceiling.
                    uint32_t air = estimateAirtimeMs(
                        txLen, BridgeConfig::radioSf(i),
                        BridgeConfig::radioBandwidth(i),
                        BridgeConfig::radioCr(i), LORA_PREAMBLE_LEN);
                    uint32_t gap = (air * 100u) / (uint32_t)BRIDGE_TX_DUTY_PERCENT;
                    if (gap < air + BRIDGE_TX_MIN_GAP_MS)
                        gap = air + BRIDGE_TX_MIN_GAP_MS;
                    g_nextTxAllowedMs[i] = g_txStartMs[i] + gap;
                } else {
                    Serial.printf("[%8lu ms][%s TX] startTransmit failed %d — dropped\n",
                                  millis(), tag, txs);
                    g_txPendingValid[i] = false;
                    self->startReceive();
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(BRIDGE_POLL_MS));
    }
}

// Construct the radio wrapper for a LOCAL slot (R1/R2), picking SX1262 vs
// LR1121 by chip. The LR1121 module has an integrated RF switch, so antSw is
// unused for it. Remote slots (R3/R4) use RemoteRadio, built directly.
static LoraRadio *makeRadio(uint8_t chip, int nss, int irqDio, int reset,
                            int busy, int antSw, const char *name,
                            const LoraConfig &cfg)
{
    if (chip == BridgeConfig::CHIP_LR1121)
        return new WioLR1121(nss, irqDio, reset, busy, spi, spiMutex, name, cfg);
    return new WioSX1262(nss, irqDio, reset, busy, antSw, spi, spiMutex, name, cfg);
}

// ============================================================
//  setup()
// ============================================================
void setup()
{
    Serial.begin(115200);
    while (!Serial && millis() < 3000);
    Serial.println("\n=== XIAO ESP32S3 Quad-Radio Cross-Protocol Bridge ===");

    // Bridge configuration: NVS first, build-flag defaults otherwise.
    BridgeConfig::begin();

    // First boot (nothing saved): seed a unique MAC-derived identity so the
    // captive-portal form pre-fills with a per-device node ID + SSID rather
    // than a shared build-flag default. Portal save persists it.
    if (!BridgeConfig::isConfigured())
        deriveMacIdentity();

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

    // Per-radio enable (protocol != None) + cached routing matrix.
    for (int i = 0; i < NR; i++) {
        g_radioEnabled[i] = (BridgeConfig::radioProtocol(i) != BridgeConfig::PROTO_NONE);
        g_routeMask[i]    = BridgeConfig::radioRouteMask(i);
        if (!g_radioEnabled[i])
            Serial.printf("[setup] Radio%d protocol = None — disabled\n", i + 1);
    }

    // Resolve each enabled radio's channel into g_chan[] before any RX.
    for (int i = 0; i < NR; i++) {
        if (g_radioEnabled[i])
            resolveRadioChannel(BridgeConfig::radioSyncWord(i),
                                BridgeConfig::radioChannelName(i),
                                BridgeConfig::radioChannelKey(i), g_chan[i]);
    }

    // NodeDB is still populated from received NodeInfo (kept for future use),
    // but the cross-protocol attribution prefix it fed was removed with the
    // text marker — bridged bodies are now clean (ROUTING-REDESIGN §3.1).
    NodeDB::begin();
    NodeDB::debugDump();

    // RX-priority pipeline state: the content-hash loop/dup guard and one
    // PSRAM-backed outbound queue per enabled (destination-capable) radio. Seed
    // the RNG used for CSMA backoff from the hardware RNG so co-located bridges
    // don't back off in lock-step.
    DedupCache::begin();
    randomSeed(esp_random());
    for (int i = 0; i < NR; i++) {
        if (g_radioEnabled[i])
            g_routeQ[i].begin(BRIDGE_ROUTE_QUEUE_DEPTH, BRIDGE_ROUTE_MAX_AGE_MS, kTag[i]);
    }

    // Start shared SPI bus with explicit XIAO ESP32S3 pin mapping
    spi.begin(SPI_SCK, SPI_MISO, SPI_MOSI);

    // Mutex must exist before any local radio object is constructed
    spiMutex = xSemaphoreCreateMutex();
    configASSERT(spiMutex != NULL);

    // R1/R2 chip resolve. DUAL_* profiles fix it at compile time; the MIXED
    // profile fixes Radio 1 = SX1262 and reads Radio 2 from BridgeConfig.
    uint8_t chip0, chip1;
#if defined(RADIO_PROFILE_DUAL_LR1121)
    chip0 = chip1 = BridgeConfig::CHIP_LR1121;
#elif defined(RADIO_PROFILE_DUAL_SX1262)
    chip0 = chip1 = BridgeConfig::CHIP_SX1262;
#else  // MIXED
    chip0 = BridgeConfig::CHIP_SX1262;
    chip1 = BridgeConfig::radioChip(1);
#endif

    // Construct LOCAL radios (R1/R2) on the XIAO SPI bus.
    if (g_radioEnabled[0])
        g_radio[0] = makeRadio(chip0, R1_NSS, R1_DIO1, R1_RESET, R1_BUSY,
                               R1_ANT_SW, "Radio1-B2B", makeLoraConfig(0, chip0));
    if (g_radioEnabled[1])
        g_radio[1] = makeRadio(chip1, R2_NSS, R2_DIO1, R2_RESET, R2_BUSY,
                               R2_ANT_SW, "Radio2-Edge", makeLoraConfig(1, chip1));

    // Allow B2B power rail and SX1262 TCXO to settle before first SPI access.
    delay(150);

    // Pre-flight (local radios only): pulse RESET and watch BUSY drain LOW.
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
    if (g_radioEnabled[0]) busyWait(R1_RESET, R1_BUSY, "R1");
    if (g_radioEnabled[1]) busyWait(R2_RESET, R2_BUSY, "R2");

    // Raw SPI probe on R1 (SX1262 GetStatus 0xC0 + version reg) — bypasses
    // RadioLib so we can see what MISO carries. 0x00/0xFF = MISO open;
    // 0x20-0x2E = chip alive. Only meaningful for a local SX1262 R1.
    if (g_radioEnabled[0] && chip0 == BridgeConfig::CHIP_SX1262) {
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

    // Construct REMOTE radios (R3/R4) on the T-Lora-Dual co-processor. They
    // register their RX queues in the ctor, so build them BEFORE the link
    // service task starts. The UART link is only opened when a remote radio is
    // enabled — a pure R1/R2 build never touches the link GPIOs.
    if (g_radioEnabled[2])
        g_radio[2] = new RemoteRadio(g_link, /*localRadio=*/0, "Radio3-LR",
                                     makeLoraConfig(2, BridgeConfig::CHIP_LR1121),
                                     BridgeConfig::radioBand(2));
    if (g_radioEnabled[3])
        g_radio[3] = new RemoteRadio(g_link, /*localRadio=*/1, "Radio4-LR",
                                     makeLoraConfig(3, BridgeConfig::CHIP_LR1121),
                                     BridgeConfig::radioBand(3));
    if (g_radioEnabled[2] || g_radioEnabled[3])
        g_link.begin(BRIDGE_LINK_BAUD, BRIDGE_LINK_RX_PIN, BRIDGE_LINK_TX_PIN);

    // Initialise every enabled radio. Local R1 failing is fatal (wiring);
    // any other radio failing just disables that slot so the rest keep going.
    for (int i = 0; i < NR; i++) {
        if (!g_radioEnabled[i] || !g_radio[i]) continue;
        if (g_radio[i]->begin()) continue;
        if (i == 0) {
            Serial.println("\nFATAL: Radio1 init failed. Check wiring. Halting.");
            while (true) { vTaskDelay(pdMS_TO_TICKS(1000)); }
        }
        Serial.printf("\n[WARN] Radio%d init failed — disabling that slot.\n", i + 1);
        g_radioEnabled[i] = false;
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
    vTaskDelay(pdMS_TO_TICKS(1000));
}
