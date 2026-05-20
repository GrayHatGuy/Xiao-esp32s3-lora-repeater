/**
 * main.cpp
 * ========
 * Dual Wio SX1262 LoRa crossover bridge for XIAO ESP32S3 Sense.
 * Uses the WioSX1262 wrapper class (WioSX1262.h / WioSX1262.cpp).
 *
 * Bridge logic:
 *   Radio1 RX  →  Radio2 TX
 *   Radio2 RX  →  Radio1 TX
 *
 * Project layout:
 *   your_project/
 *   ├── platformio.ini
 *   └── src/
 *       ├── main.cpp        ← this file
 *       ├── WioSX1262.h
 *       └── WioSX1262.cpp
 *
 * All LoRa RF settings (frequency, BW, SF, CR, power …) are
 * defined as preprocessor macros in WioSX1262.h.
 * Override any of them in platformio.ini build_flags, e.g.:
 *   build_flags = -DLORA_FREQUENCY=868.0f -DLORA_TX_POWER=14
 */

#include <Arduino.h>
#include <SPI.h>
#include "WioSX1262.h"
#include "MeshDecoderDebug.h"
#include "MeshEncoderDebug.h"
#include "MeshCoreConfig.h"
#include "MeshtasticConfig.h"
#include "BridgeConfig.h"
#include "CaptivePortal.h"
#include "NodeDB.h"
// Per-radio LoRa settings — fall back to the generic LORA_* defaults
// from WioSX1262.h for any value not defined in platformio.ini.
#ifndef LORA_RADIO1_FREQUENCY
  #define LORA_RADIO1_FREQUENCY    LORA_FREQUENCY
#endif
#ifndef LORA_RADIO1_BANDWIDTH
  #define LORA_RADIO1_BANDWIDTH    LORA_BANDWIDTH
#endif
#ifndef LORA_RADIO1_SPREAD_FACTOR
  #define LORA_RADIO1_SPREAD_FACTOR LORA_SPREAD_FACTOR
#endif
#ifndef LORA_RADIO1_CODING_RATE
  #define LORA_RADIO1_CODING_RATE  LORA_CODING_RATE
#endif
#ifndef LORA_RADIO1_SYNC_WORD
  #define LORA_RADIO1_SYNC_WORD    LORA_SYNC_WORD
#endif
#ifndef LORA_RADIO1_TX_POWER
  #define LORA_RADIO1_TX_POWER     LORA_TX_POWER
#endif
#ifndef LORA_RADIO1_PREAMBLE_LEN
  #define LORA_RADIO1_PREAMBLE_LEN LORA_PREAMBLE_LEN
#endif
#ifndef LORA_RADIO1_TCXO_VOLTAGE
  #define LORA_RADIO1_TCXO_VOLTAGE LORA_TCXO_VOLTAGE
#endif

#ifndef LORA_RADIO2_FREQUENCY
  #define LORA_RADIO2_FREQUENCY    LORA_FREQUENCY
#endif
#ifndef LORA_RADIO2_BANDWIDTH
  #define LORA_RADIO2_BANDWIDTH    LORA_BANDWIDTH
#endif
#ifndef LORA_RADIO2_SPREAD_FACTOR
  #define LORA_RADIO2_SPREAD_FACTOR LORA_SPREAD_FACTOR
#endif
#ifndef LORA_RADIO2_CODING_RATE
  #define LORA_RADIO2_CODING_RATE  LORA_CODING_RATE
#endif
#ifndef LORA_RADIO2_SYNC_WORD
  #define LORA_RADIO2_SYNC_WORD    LORA_SYNC_WORD
#endif
#ifndef LORA_RADIO2_TX_POWER
  #define LORA_RADIO2_TX_POWER     LORA_TX_POWER
#endif
#ifndef LORA_RADIO2_PREAMBLE_LEN
  #define LORA_RADIO2_PREAMBLE_LEN LORA_PREAMBLE_LEN
#endif
#ifndef LORA_RADIO2_TCXO_VOLTAGE
  #define LORA_RADIO2_TCXO_VOLTAGE LORA_TCXO_VOLTAGE
#endif

static const LoraConfig radio1Config = {
    LORA_RADIO1_FREQUENCY,
    LORA_RADIO1_BANDWIDTH,
    LORA_RADIO1_SPREAD_FACTOR,
    LORA_RADIO1_CODING_RATE,
    LORA_RADIO1_SYNC_WORD,
    LORA_RADIO1_TX_POWER,
    LORA_RADIO1_PREAMBLE_LEN,
    LORA_RADIO1_TCXO_VOLTAGE
};

static const LoraConfig radio2Config = {
    LORA_RADIO2_FREQUENCY,
    LORA_RADIO2_BANDWIDTH,
    LORA_RADIO2_SPREAD_FACTOR,
    LORA_RADIO2_CODING_RATE,
    LORA_RADIO2_SYNC_WORD,
    LORA_RADIO2_TX_POWER,
    LORA_RADIO2_PREAMBLE_LEN,
    LORA_RADIO2_TCXO_VOLTAGE
};

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
//  FreeRTOS configuration
// ============================================================
// Radio task stack. Bumped from 4096 to 8192 once F1 (NodeDB) landed:
// bridgePacket() can nest extractMeshtasticNodeInfo (240 B pt + AES ctx
// ~280 B) under its own body[256]+marked[280]+outPkt[256], plus the
// fragmented-RNS path needs similar headroom. 8 KB leaves a comfortable
// margin; the ESP32-S3 has 512 KB SRAM so it's free real estate.
#define BRIDGE_TASK_STACK  8192
#define BRIDGE_TASK_PRIO   2       // above idle, below system
#define BRIDGE_POLL_MS     1       // ms between RX polls

// ============================================================
//  Shared SPI mutex — created in setup(), passed to both radios
// ============================================================
SemaphoreHandle_t spiMutex = NULL;

// ============================================================
//  Radio objects — constructed as pointers so the mutex exists
//  before the WioSX1262 constructors run.
// ============================================================
WioSX1262 *radio1 = nullptr;
WioSX1262 *radio2 = nullptr;

// ============================================================
//  Per-radio channel context — g_chan[0] = radio1, g_chan[1] = radio2.
//  Resolved once in setup() before the radio tasks start; read-only after.
// ============================================================
RadioChannel g_chan[2];

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
//  Forward declarations
// ============================================================
void radio1Task(void *pvParameters);
void radio2Task(void *pvParameters);

// ============================================================
//  Cross-protocol bridge core
//  ----------------------------------------------------------------
//  One generic per-packet bridge step driven by the two radios' LoRa
//  sync words. Supports three protocols today:
//    0x12 MeshCore   <-> [MC] marker, body = decoded UTF-8 text
//    0x2B Meshtastic <-> [MT] marker, body = decoded UTF-8 text
//    0x42 Reticulum  <-> [rns <seq> <x>/<y>] marker, body = base64 of raw
//                        RNS bytes. Fragmented across multiple MT/MC text
//                        packets when one wouldn't fit.
//
//  Behaviour for text sources (MT/MC):
//    1. Extract the text body.
//    2. Loop check — drop if body already starts with any known bridge
//       marker (the strncmp uses 4 chars so "[rns ..." matches just as
//       "[rns] ..." used to).
//    3. Prepend the source marker.
//    4. dst == RNS  -> log "No TX 2 RNS:" and drop (no RNS encoder yet).
//       dst == MT/MC -> encode and transmit on dstRadio.
//
//  Behaviour for RNS sources: see bridgeFromReticulum() below — base64,
//  CRC-16 sequence ID, fragment loop with per-protocol pacing.
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

static void bridgeFromReticulum(const RadioChannel &dstChan, WioSX1262 *dstRadio,
                                const char *srcTag,
                                const uint8_t *buf, size_t len)
{
    if (dstChan.protocol == MeshDecoderDebug::SYNC_WORD_RETICULUM) return;
    if (len == 0) return;

    // Pick destination-specific fragment size and pacing
    size_t   rawPerFrag = 0;
    uint32_t delayMs    = 0;
    const char *dstName = "?";
    if (dstChan.protocol == MeshDecoderDebug::SYNC_WORD_MESHTASTIC) {
        rawPerFrag = BRIDGE_RNS_RAW_PER_FRAG_MT;
        delayMs    = BRIDGE_RNS_FRAG_DELAY_MT_MS;
        dstName    = "MT";
    } else if (dstChan.protocol == MeshDecoderDebug::SYNC_WORD_MESHCORE) {
        rawPerFrag = BRIDGE_RNS_RAW_PER_FRAG_MC;
        delayMs    = BRIDGE_RNS_FRAG_DELAY_MC_MS;
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

        Serial.printf("[%8lu ms][%s->%s bridge] frag %u/%u (%u B): \"%s\"\n",
                      millis(), srcTag, dstName,
                      (unsigned)(idx + 1), (unsigned)totalFrags,
                      (unsigned)outLen, marked);

        int16_t txState = dstRadio->transmit(outPkt, outLen);
        if (txState != RADIOLIB_ERR_NONE) {
            Serial.printf("[%8lu ms][%s->%s bridge] frag %u/%u TX ERROR %d\n",
                          millis(), srcTag, dstName,
                          (unsigned)(idx + 1), (unsigned)totalFrags, txState);
            // Continue with remaining fragments so the receiver at least sees
            // partial reassembly state; could return here instead.
        }

        // Inter-fragment pacing — give the destination mesh time to relay
        // fragment N before we step on it with N+1.
        if (idx + 1 < totalFrags) {
            vTaskDelay(pdMS_TO_TICKS(delayMs));
        }
    }
}

// Build the bridge prefix for a text-source packet. For MT source, surfaces
// the sender's canonical 32-bit ID in Meshtastic's "!<8 lowercase hex>" form
// and appends the short_name from NodeDB when known:
//   "[MT !3d3a87a3 KN5J]"  — id + name (NodeDB hit)
//   "[MT !3d3a87a3]"       — id only (no NodeInfo seen yet for this node)
//   "[MT]"                 — defensive fallback if buf < 8 B
// MC packets already carry the sender's name inline in the body, so the MC
// marker stays bare.
static void buildTextSrcMarker(uint8_t srcSync,
                               const uint8_t *buf, size_t len,
                               char *out, size_t outCap)
{
    if (!out || outCap == 0) return;
    if (srcSync == MeshDecoderDebug::SYNC_WORD_MESHTASTIC) {
        if (len >= 8) {
            uint32_t srcId = (uint32_t)buf[4]  | ((uint32_t)buf[5]  << 8)
                           | ((uint32_t)buf[6] << 16) | ((uint32_t)buf[7] << 24);
            const char *shortName = NodeDB::lookupShortName(srcId);
            if (shortName && shortName[0]) {
                snprintf(out, outCap, "[MT !%08lx %s]",
                         (unsigned long)srcId, shortName);
            } else {
                snprintf(out, outCap, "[MT !%08lx]",
                         (unsigned long)srcId);
            }
            return;
        }
        snprintf(out, outCap, "[MT]");
        return;
    }
    if (srcSync == MeshDecoderDebug::SYNC_WORD_MESHCORE) {
        snprintf(out, outCap, "[MC]");
        return;
    }
    snprintf(out, outCap, "[?]");
}

static void bridgePacket(const RadioChannel &srcChan, const RadioChannel &dstChan,
                         WioSX1262 *dstRadio, const char *srcTag,
                         const uint8_t *buf, size_t len)
{
    // RNS source uses its own fragmenting path
    if (srcChan.protocol == MeshDecoderDebug::SYNC_WORD_RETICULUM) {
        bridgeFromReticulum(dstChan, dstRadio, srcTag, buf, len);
        return;
    }

    // MT NodeInfo packets feed the NodeDB and are NOT bridged as text —
    // they'd be constant noise on the destination mesh and the destination
    // doesn't have a use for them. Try this before the text-body extract
    // so we don't waste a Data-protobuf walk twice.
    if (srcChan.protocol == MeshDecoderDebug::SYNC_WORD_MESHTASTIC) {
        uint32_t niNodeId = 0;
        char     niShort[NodeDB::MAX_SHORT_NAME + 1] = {0};
        char     niLong [NodeDB::MAX_LONG_NAME  + 1] = {0};
        if (MeshDecoderDebug::extractMeshtasticNodeInfo(
                buf, len, srcChan, niNodeId,
                niShort, sizeof(niShort),
                niLong,  sizeof(niLong))) {
            // Skip our own NodeInfo bouncing back via a relay. Without this
            // guard every echo would trigger an NVS write of our own ID.
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

    // Text source (MT or MC): single-packet bridge.
    // For MT we try POSITION_APP and TELEMETRY_APP first; if either yields
    // structured data we format it as a compact text line and reuse the
    // standard text-bridge pipeline below. TEXT_MESSAGE_APP is the final
    // fallback. For MC we just lift the GRP_TXT body directly.
    char body[256];
    bool decoded = false;

    if (srcChan.protocol == MeshDecoderDebug::SYNC_WORD_MESHTASTIC) {
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

    // Loop check — drop anything already carrying a bridge marker.
    // We use 3-char "[MT" and "[MC" prefixes so the check survives the
    // upcoming "[MT <SHORT>]" form when NodeDB attribution lands. The "[rns"
    // 4-char prefix already covers both legacy and fragmented RNS markers.
    if (strncmp(body, "[MT",  3) == 0 ||
        strncmp(body, "[MC",  3) == 0 ||
        strncmp(body, "[rns", 4) == 0) {
        Serial.printf("[%8lu ms][%s bridge] loop-drop: \"%s\"\n",
                      millis(), srcTag, body);
        return;
    }

    char srcMarker[32];   // fits "[MT !12345678 ABCDEFGH]" (23 chars) + room
    buildTextSrcMarker(srcChan.protocol, buf, len, srcMarker, sizeof(srcMarker));
    char marked[280];
    snprintf(marked, sizeof(marked), "%s %s", srcMarker, body);

    // Destination is Reticulum — log and drop. No RNS encoder yet.
    if (dstChan.protocol == MeshDecoderDebug::SYNC_WORD_RETICULUM) {
        Serial.printf("[%8lu ms][%s->RNS bridge] No TX 2 RNS: %s\n",
                      millis(), srcTag, marked);
        return;
    }

    uint8_t outPkt[256];
    size_t  outLen  = 0;
    bool    encoded = false;
    const char *dstName = "?";
    switch (dstChan.protocol) {
        case MeshDecoderDebug::SYNC_WORD_MESHTASTIC:
            encoded = MeshEncoderDebug::encodeMeshtasticText(
                          dstChan, BridgeConfig::mtNodeId(),
                          marked, outPkt, sizeof(outPkt), outLen);
            dstName = "MT";
            break;
        case MeshDecoderDebug::SYNC_WORD_MESHCORE:
            encoded = MeshEncoderDebug::encodeMeshCoreGrpTxt(
                          dstChan, marked, /*ts=*/0, outPkt, sizeof(outPkt), outLen);
            dstName = "MC";
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

    Serial.printf("[%8lu ms][%s->%s bridge] re-encoded %u B: \"%s\"\n",
                  millis(), srcTag, dstName, (unsigned)outLen, marked);
    int16_t txState = dstRadio->transmit(outPkt, outLen);
    if (txState == RADIOLIB_ERR_NONE) {
        Serial.printf("[%8lu ms][%s->%s bridge] TX OK\n",
                      millis(), srcTag, dstName);
    } else {
        Serial.printf("[%8lu ms][%s->%s bridge] TX ERROR %d\n",
                      millis(), srcTag, dstName, txState);
    }
}

// ============================================================
//  FreeRTOS task: Radio1 RX → Radio2 TX
// ============================================================
void radio1Task(void *pvParameters)
{
    uint8_t buf[LORA_MAX_PACKET];

    // First Meshtastic NodeInfo goes out ~10 s after task start, then every
    // 5 min. Without this, Meshtastic clients tend to hide text messages from
    // a node they've never seen NodeInfo for, so the bridge appears silent.
    // The guard on the sync word keeps the broadcast silent if R1 is ever
    // reconfigured to a non-Meshtastic protocol (it's a compile-time check,
    // so unused code drops out of the binary).
    uint32_t nextNodeInfoMs = millis() + 10000;

    for (;;) {
        if (LORA_RADIO1_SYNC_WORD == MeshDecoderDebug::SYNC_WORD_MESHTASTIC &&
            (int32_t)(millis() - nextNodeInfoMs) >= 0) {
            uint8_t niPkt[256];
            size_t  niLen = 0;
            if (MeshEncoderDebug::encodeMeshtasticNodeInfo(
                    g_chan[0],
                    BridgeConfig::mtNodeId(),
                    BridgeConfig::mtNodeIdStr(),
                    BridgeConfig::mtLongName(),
                    BridgeConfig::mtShortName(),
                    niPkt, sizeof(niPkt), niLen)) {
                Serial.printf("[%8lu ms][R1 NodeInfo TX] %u B id=%s name=\"%s\"\n",
                              millis(), (unsigned)niLen,
                              BridgeConfig::mtNodeIdStr(),
                              BridgeConfig::mtLongName());
                int16_t txState = radio1->transmit(niPkt, niLen);
                if (txState != RADIOLIB_ERR_NONE) {
                    Serial.printf("[%8lu ms][R1 NodeInfo TX] ERROR %d\n",
                                  millis(), txState);
                }
                radio1->startReceive();
            } else {
                Serial.printf("[%8lu ms][R1 NodeInfo TX] encode failed\n",
                              millis());
            }
            nextNodeInfoMs = millis() + 300000;   // every 5 min
        }

        if (radio1->available()) {
            size_t len  = sizeof(buf);
            float  rssi = 0.0f;
            float  snr  = 0.0f;

            int16_t state = radio1->read(buf, len, &rssi, &snr);

            if (state == RADIOLIB_ERR_NONE && len > 0) {
                Serial.printf("[%8lu ms][R1 RX] %u bytes  RSSI %.1f dBm  SNR %.1f dB\n",
                              millis(), (unsigned)len, rssi, snr);

                MeshDecoderDebug::print(buf, len, g_chan[0], "R1");

                bridgePacket(g_chan[0], g_chan[1], radio2, "R1", buf, len);

                // RadioLib exits RX mode on packet receipt;
                // restore it before polling again.
                radio1->startReceive();

            } else if (state != RADIOLIB_ERR_NONE) {
                Serial.printf("[%8lu ms][R1 RX] ERROR %d\n", millis(), state);
                radio1->startReceive();
            }
        }

        vTaskDelay(pdMS_TO_TICKS(BRIDGE_POLL_MS));
    }
}

// ============================================================
//  FreeRTOS task: Radio2 RX → Radio1 TX
// ============================================================
void radio2Task(void *pvParameters)
{
    uint8_t buf[LORA_MAX_PACKET];

    for (;;) {
        if (radio2->available()) {
            size_t len  = sizeof(buf);
            float  rssi = 0.0f;
            float  snr  = 0.0f;

            int16_t state = radio2->read(buf, len, &rssi, &snr);

            if (state == RADIOLIB_ERR_NONE && len > 0) {
                Serial.printf("[%8lu ms][R2 RX] %u bytes  RSSI %.1f dBm  SNR %.1f dB\n",
                              millis(), (unsigned)len, rssi, snr);

                MeshDecoderDebug::print(buf, len, g_chan[1], "R2");

                bridgePacket(g_chan[1], g_chan[0], radio1, "R2", buf, len);

                radio2->startReceive();

            } else if (state != RADIOLIB_ERR_NONE) {
                Serial.printf("[%8lu ms][R2 RX] ERROR %d\n", millis(), state);
                radio2->startReceive();
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
    Serial.begin(115200);
    while (!Serial && millis() < 3000);
    Serial.println("\n=== XIAO ESP32S3 Dual SX1262 Crossover Bridge ===");

    // Bridge configuration: NVS first, build-flag defaults otherwise.
    BridgeConfig::begin();
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

    // Resolve each radio's channel into g_chan[] before any RX. Each radio's
    // protocol is its build-flag sync word; its channel name/key come from
    // BridgeConfig (build-flag defaults or the captive-portal-saved values).
    resolveRadioChannel((uint8_t)LORA_RADIO1_SYNC_WORD,
                        BridgeConfig::radio1ChannelName(),
                        BridgeConfig::radio1ChannelKey(), g_chan[0]);
    resolveRadioChannel((uint8_t)LORA_RADIO2_SYNC_WORD,
                        BridgeConfig::radio2ChannelName(),
                        BridgeConfig::radio2ChannelKey(), g_chan[1]);

    // Load persisted NodeDB before the radio tasks start so the very first
    // bridged MT packet can already carry a [MT !<hexid> <SHORT>] attribution
    // if the sender was seen in a previous boot.
    NodeDB::begin();
    NodeDB::debugDump();

    // Start shared SPI bus with explicit XIAO ESP32S3 pin mapping
    spi.begin(SPI_SCK, SPI_MISO, SPI_MOSI);

    // Mutex must exist before any WioSX1262 object is constructed
    spiMutex = xSemaphoreCreateMutex();
    configASSERT(spiMutex != NULL);

    // Construct radio objects now the mutex and SPI bus are ready
    radio1 = new WioSX1262(R1_NSS, R1_DIO1, R1_RESET, R1_BUSY,
                            R1_ANT_SW, spi, spiMutex, "Radio1-B2B", radio1Config);

    radio2 = new WioSX1262(R2_NSS, R2_DIO1, R2_RESET, R2_BUSY,
                            R2_ANT_SW, spi, spiMutex, "Radio2-Edge", radio2Config);

    // Allow B2B power rail and SX1262 TCXO to settle before first SPI access.
    delay(150);

    // Pre-flight: manually pulse each RESET and watch BUSY drain to LOW.
    // If BUSY stays HIGH for 50 ms after reset that radio's module is absent
    // or unpowered — wiring problem, not a software bug.
    auto busyWait = [](int resetPin, int busyPin, const char *label) {
        pinMode(resetPin, OUTPUT);
        pinMode(busyPin,  INPUT);   // must configure before digitalRead(),
                                    // else newer arduino-esp32 cores log
                                    // "IO N is not set as GPIO"
        digitalWrite(resetPin, LOW);
        delay(2);
        digitalWrite(resetPin, HIGH);
        uint32_t t0 = millis();
        while (digitalRead(busyPin) && millis() - t0 < 50);
        Serial.printf("[diag] %s  BUSY after reset = %d  (%lu ms)  %s\n",
                      label, digitalRead(busyPin), millis() - t0,
                      digitalRead(busyPin) ? "STUCK-HIGH -> module absent/unpowered!" : "OK");
    };
    busyWait(R1_RESET, R1_BUSY, "R1");
    busyWait(R2_RESET, R2_BUSY, "R2");

    // Raw SPI probe on R1: send SX1262 GetStatus opcode (0xC0) and read
    // the response byte.  This bypasses RadioLib entirely so we can see
    // what MISO actually carries.
    //   0x00 or 0xFF → MISO not connected to this chip
    //   0x20..0x2E   → valid chip status byte (SPI path works)
    {
        SPI.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));
        digitalWrite(R1_NSS, LOW);
        delayMicroseconds(2);
        SPI.transfer(0xC0);           // GetStatus opcode
        uint8_t r1Status = SPI.transfer(0x00);  // read status byte
        digitalWrite(R1_NSS, HIGH);
        SPI.endTransaction();
        Serial.printf("[diag] R1 raw SPI GetStatus = 0x%02X  "
                      "(0x00/0xFF = MISO open; 0x20-0x2E = chip alive)\n", r1Status);
    }

    // Raw ReadRegister 0x0320 — read 6 bytes of the version string.
    // RadioLib's findChip() compares this to "SX1261".
    // Format: opcode 0x1D + addr 0x0320 + 1 NOP + data bytes.
    {
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
        Serial.printf("[diag] R1 reg 0x0320 raw: "
                      "%02X %02X %02X %02X %02X %02X  = '%.6s'\n",
                      ver[0], ver[1], ver[2], ver[3], ver[4], ver[5],
                      (char*)ver);
        Serial.printf("[diag] RadioLib expects '%.6s' at 0x0320\n", "SX1261");
    }

    // Initialise — applies all LORA_* settings from WioSX1262.h
    bool r1ok = radio1->begin();
    bool r2ok = radio2->begin();

    if (!r1ok) {
        Serial.println("\nFATAL: Radio1 init failed. Check wiring. Halting.");
        while (true) { vTaskDelay(pdMS_TO_TICKS(1000)); }
    }
    if (!r2ok) {
        Serial.println("\n[WARN] Radio2 init failed — running Radio1 only.\n");
    }

    // Start both radios listening
    radio1->startReceive();
    radio2->startReceive();

    Serial.println("\nBridge active — both radios listening.\n");

    // Spawn one FreeRTOS task per radio, pinned to separate cores
    xTaskCreatePinnedToCore(
        radio1Task, "R1_task",
        BRIDGE_TASK_STACK, NULL,
        BRIDGE_TASK_PRIO,  NULL,
        0   // core 0
    );

    xTaskCreatePinnedToCore(
        radio2Task, "R2_task",
        BRIDGE_TASK_STACK, NULL,
        BRIDGE_TASK_PRIO,  NULL,
        1   // core 1
    );
}

// ============================================================
//  loop() — bridge runs entirely in FreeRTOS tasks
// ============================================================
void loop()
{
    vTaskDelay(pdMS_TO_TICKS(1000));
}
