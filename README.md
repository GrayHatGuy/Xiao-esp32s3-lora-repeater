# Xiao-esp32s3-lora-repeater

Xiao ESP32S3 with dual SX1262 radio SPI cross-band repeater.

<img width="4096" height="3265" alt="PXL_20260507_021829300~2" src="https://github.com/user-attachments/assets/b9e68624-3cb4-46a3-9c2f-4927e6a8fdf2" />

###### *touched by claude but not by epstein*

## Introduction / Background

A bidirectional LoRa mesh bridge running on a single Seeed Xiao ESP32S3 Sense with two Seeed Wio SX1262 shields stacked back-to-back — one mated to the Xiao's edge pins, the other to the 40-pin B2B header. The two radios share one SPI bus through a FreeRTOS mutex and each run in their own task pinned to a separate ESP32-S3 core, so they can transmit and receive in parallel on completely different RF profiles.

Each radio carries its own protocol **and** its own channel. The bridge relays packets received on one radio out the other — *cross-protocol* (MT↔MC) or *same-protocol between two channels* (MC↔MC, MT↔MT — e.g. a private channel bridged to the public one). As of **v8.0** everything — region, per-radio protocol, RF plan (frequency, bandwidth, spreading factor, coding rate, sync word, TX power), channels and identity — is configured through the WiFi captive portal. A single `.bin` flashed with no build flags first-boots straight into the portal, so no PlatformIO build is needed to deploy. The `platformio.ini` `LORA_RADIO*` build flags remain available as optional first-boot defaults for source builds.

Supported today:

- **Meshtastic** (sync `0x2B`) — AES-CTR + a hand-written protobuf walker that lifts `TEXT_MESSAGE_APP` payloads out of the on-air `Data` submessage. `POSITION_APP` and `TELEMETRY_APP` are also decoded and bridged as compact text lines under the same marker (`pos 40.7234,-74.0123 alt 12m`, `bat 87% 4.05V`, `env 22.5C RH 45% 1013hPa`). Defaults to the public LongFast channel; override `BRIDGE_MT_CHANNEL_NAME` / `BRIDGE_MT_PSK_B64` (or use the captive portal) to bridge a private channel — the PSK can be a short key, a 16-byte AES-128 key, or a 32-byte AES-256 key, and the channel hash is auto-derived. Bridged bodies are tagged with the sender's canonical Meshtastic `!`-prefixed hex ID plus the short_name when known: `[MT !3d3a87a3 KN5J] …` (or `[MT !3d3a87a3] …` if no `NODEINFO_APP` has been seen yet for that node). The bridge decodes incoming `NODEINFO_APP` packets to populate a 64-entry, NVS-persistent NodeDB so attribution survives reboots, and emits its own periodic NodeInfo announce so phones surface the bridge as a known sender (`!b16b00b5`, "LoRa Bridge").
- **MeshCore channel** (sync `0x12`) — AES-128-ECB decrypt of `GRP_TXT`, with the 2-byte truncated HMAC-SHA256 verified against the channel key. Defaults to the MeshCore public channel (hash `0x11`); override `BRIDGE_MC_KEY_HEX` / `BRIDGE_MC_CHANNEL_NAME` in `platformio.ini` to bridge a private or custom channel instead — the on-air channel-hash byte is auto-derived from `SHA-256(key)[0]` at boot. Bridged bodies are tagged `[MC] …`.
- **Reticulum / RNode** (sync `0x42`, **stub**) — incoming frames are base64-encoded and bridged into the other mesh as text packets of the form `[rns <seq> <x>/<y>] <base64>`. The bridge auto-fragments across multiple MT/MC packets when a single one wouldn't hold the encoded frame, using a CRC-16 low-byte sequence ID so concurrent fragmented frames don't get mixed up on the receiving side, and pacing between fragments (2000 ms for SF11/BW250 MT, 500 ms for SF7/BW62.5 MC; max 8 fragments per frame, tunable via `BRIDGE_RNS_*` build flags). A proper RNS packet encoder is still TODO; until then, "destination = RNS" is a log-and-drop path with a `No TX 2 RNS:` prefix on serial.

Source-protocol markers double as loop-prevention: when the bridge's own re-transmitted packet bounces back via a relay node, the marker is recognised and the packet is dropped before being bridged a second time.

All crypto runs on the ESP-IDF's built-in mbedTLS — no extra library dependencies beyond `jgromes/RadioLib`.

## Parts Required

| Part | Notes |
|------|-------|
| [Wio SX1262 with Xiao ESP32S3 (B2B 40-pin)](https://www.seeedstudio.com/Wio-SX1262-with-XIAO-ESP32S3-p-5982.html) | Radio 1 (B2B). Kit ships with the Xiao ESP32S3 Sense MCU |
| [Wio SX1262 for Xiao (edge-pin)](https://www.seeedstudio.com/Wio-SX1262-for-XIAO-p-6379.html) | Radio 2, sits on the Xiao's edge-pin header |
| 2 × LoRa antennas tuned for your ISM band | **Don't skip this.** Running an SX1262 at +20 dBm into a missing antenna kills your TX range and risks the PA |
| USB-C cable | Power, programming, serial monitor |

*Some assembly required.*

## Wiring

This build uses **stacked shields** — there is no hand-wiring. The B2B SX1262
mounts on top of the Xiao on the 40-pin board-to-board connector; the edge-pin
SX1262 mounts on top of the Xiao's perimeter header. The pin mapping the firmware (see `src/main.cpp`).

| Signal | Radio 1 (B2B) | Radio 2 (edge) | Notes |
|--------|---------------|----------------|-------|
| SCK    | GPIO7 (D8)    | GPIO7 (D8)     | **shared** SPI bus |
| MOSI   | GPIO9 (D10)   | GPIO9 (D10)    | **shared** |
| MISO   | GPIO8 (D9)    | GPIO8 (D9)     | **shared** |
| NSS / CS | GPIO41      | GPIO5 (D4)     | per-radio chip select |
| DIO1 / IRQ | GPIO39    | GPIO2 (D1)     | RX-done interrupt |
| RESET  | GPIO42        | GPIO3 (D2)     | per-radio |
| BUSY   | GPIO40        | GPIO4 (D3)     | per-radio |
| ANT_SW | GPIO38        | GPIO6 (D5)     | TX/RX RF switch |
| VCC    | 3V3           | 3V3            | |
| GND    | GND           | GND            | |

**Key points**

- The two radios **share one SPI bus** (SCK/MOSI/MISO); the firmware serializes
  access with a FreeRTOS mutex.
- Each radio has its own **NSS, DIO1, RESET, BUSY, ANT_SW**, so they run
  independently — one task per ESP32-S3 core.
- Radio 1's pins (GPIO38–42) are exposed **only on the 40-pin B2B connector**.
- The TCXO is internal to each Wio SX1262 module (1.8 V) — not wired to a GPIO.
- **Connect both u.FL antennas before power-on** — transmitting into a missing
  antenna risks the PA.

## Instructions

1. **Stack the hardware.** Mate the B2B shield (radio 1) on top the Xiao, the edge-pin shield (radio 2) on bottom. Connect antennas to **both** radios before powering on. Correct orientation has all antennas on the same side.
2. **Install [PlatformIO](https://platformio.org/install)** — the VS Code extension is the easiest path.
3. *(Optional — source builds only)* **Pre-seed the radios** in [`platformio.ini`](platformio.ini). As of v8.0 this is no longer required: a `.bin` built with no `LORA_RADIO*` flags first-boots straight into the captive portal where region, protocol and RF are all set. If you do build from source, these flags become the first-boot defaults the portal form pre-fills:
   ```ini
   -DLORA_RADIO1_FREQUENCY=906.875f
   -DLORA_RADIO1_BANDWIDTH=250.0f
   -DLORA_RADIO1_SPREAD_FACTOR=11
   -DLORA_RADIO1_CODING_RATE=5
   -DLORA_RADIO1_TX_POWER=20
   -DLORA_RADIO1_SYNC_WORD=0x2B   ; 0x12 MeshCore, 0x2B Meshtastic, 0x42 Reticulum
   ```
4. *(Optional)* **Set bridge defaults** in `platformio.ini`. These are the values the bridge uses when its NVS config is empty (i.e. a fresh flash). The captive portal in step 7 lets users override them at runtime without rebuilding. The numeric ID and `!`-prefixed string must encode the same value; string macros are single-quoted so spaces survive shell tokenization:
   ```ini
   -DBRIDGE_MT_NODE_ID=0xB16B00B5u
   '-DBRIDGE_MT_NODE_ID_STR="!b16b00b5"'
   '-DBRIDGE_MT_LONG_NAME="LoRa Bridge"'
   '-DBRIDGE_MT_SHORT_NAME="BR"'
   ```
5. **Clean + build.** `pio run -t clean && pio run` — the clean is important whenever a header changes.
6. **Upload.** `pio run -t upload` or use the PlatformIO toolbar.
7. **First-boot setup over WiFi.** Open `pio device monitor` at 115200 baud. On a fresh flash the bridge launches an open WiFi AP named `LoRa-Bridge-<XX>` (last byte of the MAC-derived MT node ID, in hex — unique per device). Join that SSID from a phone or laptop — any HTTP request will be DNS-redirected to the single-page config form. As of v8.0 the form covers **everything**: device region, per-radio protocol (Meshtastic / MeshCore / Reticulum / Custom / None), modem preset, channel name + key, frequency (Tier 2 channel-slot value pre-filled for Meshtastic, editable), Custom RF plan, identity and the POSITION/TELEMETRY toggles. Hit **Save & reboot** and the bridge restarts into normal mode with the NVS values. To re-enter the form on an already-configured device, reset the board and — within the ~5 s window the serial log announces — either press the **BOOT** button *or* send any character from the serial monitor. (The serial route matters when the BOOT button is physically hidden under the radio shield.)  ***NOTE: if the key press reset fails then erase the device and it will reboot into the active wifi portal config***
8. **Monitor.** Once the bridge is configured, expect RX summary lines (size / RSSI / SNR), protocol-decoded summaries, bridge re-encode lines, NodeInfo broadcasts, and `loop-drop` messages when relay echoes come back to the bridge.

## Roadmap

### Other future work

- [x] ~~More Meshtastic portnums bridged: `POSITION_APP` and `TELEMETRY_APP`~~ — **done**; both decoded and re-emitted as text under the existing bridge marker. Individually gated by `BRIDGE_MT_POSITION` / `BRIDGE_MT_TELEMETRY` build flags. `NODEINFO_APP` is decoded into the NodeDB and intentionally not bridged as text.
- [x] ~~MeshCore private-channel support~~ — **done** via `MeshCoreConfig.{h,cpp}`; override `BRIDGE_MC_KEY_HEX` and `BRIDGE_MC_CHANNEL_NAME` in `platformio.ini` to point the bridge at any MC channel. The hash byte is computed automatically from `SHA-256(key)[0]`.
- [x] ~~Persistent NodeDB so `[MT] …` prefixes can be replaced with the actual sender's short name~~ — **done** in `NodeDB.{h,cpp}`; MT→MC bridged messages now carry `[MT !<hexid> <SHORT>] …` attribution learned from `NODEINFO_APP` packets and persisted to NVS.
- [x] ~~**WiFi captive-portal first-boot config.**~~ — **done** via `BridgeConfig.{h,cpp}` + `CaptivePortal.{h,cpp}`. On a fresh flash (or whenever the BOOT button is pressed within ~3 s of reset) the bridge brings up an open AP named `LoRa-Bridge-<XX>` and DNS-redirects all HTTP traffic to a config form for MT identity, MC key + channel name, and the POSITION/TELEMETRY toggles. Saving the form writes the schema-v1 blob to NVS and reboots into bridge mode. Per-radio LoRa params (frequency, BW, SF, CR, TX power, sync word) remain build-flag-only — a misstep there can put the radio out of band.
- [x] ~~**F5: Meshtastic private-channel support.**~~ — **done** via `MeshtasticConfig.{h,cpp}` (the symmetric counterpart to F3). A base64 PSK + channel name from `BridgeConfig` are expanded — empty→LongFast, 1-byte short key, 16-byte AES-128, 32-byte AES-256 — and the on-air channel hash is derived as `XOR-fold(name) ^ XOR-fold(key)`. The five MT decoders + two MT encoders take key length from `MeshtasticConfig::keyLen` so AES-256 channels work. `BridgeConfig` schema bumped to v2 (auto-migrates a v1 blob); captive portal gained a Meshtastic-channel section.

- [x] ~~**v2: per-radio channels — same-protocol relay.**~~ — **done** (v7.0). Each radio slot carries its own channel via the new `RadioChannel` struct (`MeshCoreConfig` / `MeshtasticConfig` are now stateless `resolve()` helpers, not singletons). The bridge relays cross-protocol MT↔MC *and* same-protocol MC↔MC / MT↔MT. Each radio's protocol/RF stays a `platformio.ini` build-flag decision; each radio's channel name + key is portal-editable. `BridgeConfig` schema bumped v2→v3 (auto-migrates). The portal's two channel sections reject identical channels when both radios share a protocol.
- [x] ~~**v8: vanilla firmware — full portal config.**~~ — **done** (v8.0); specced in [`V8-SPEC.md`](V8-SPEC.md). A single distributable `.bin` configured *entirely* through the captive portal — no build flags required. Per-radio protocol picker (Meshtastic / MeshCore / Reticulum / Custom / None), global region selector (US, EU_868, EU_433, ANZ, CN, JP, IN, KR, RU + Custom/Other), Tier 2 Meshtastic channel-slot frequency computation (`RegionPreset.h`) with an editable override, null defaults (a no-flag image first-boots straight into the portal), and MAC-derived identity/SSID. `BridgeConfig` schema v3→v4. `platformio.ini` `LORA_RADIO*` flags are now optional first-boot pre-seeding only.
- [x] ~~**Compile-time validation of build-flag config.**~~ — **done** (v8.1) in [`src/LoraConfigCheck.h`](src/LoraConfigCheck.h). `#error` / `static_assert` guards reject an invalid `LORA_RADIO*` set at build time (sync words, SF/CR/BW/region sanity, TX power range). Complements the v8 portal-side runtime validation — build-time checks defaults, portal checks portal-entered values.

- [ ] **Sub-GHz ↔ 2.4 GHz LoRa cross-band bridging — Phase 1 ON HOLD pending Seeed clarification.** The current build talks the SX1262's native sub-GHz ranges (902-928 MHz US ISM, 868 MHz EU, etc.). A long-horizon goal is to bridge those to 2.4 GHz LoRa networks (e.g. Meshtastic's 2.4 GHz preset) on the worldwide-licence-free **2.4 GHz ISM band** — the headline feature that makes this milestone worth doing.

  > **⚠️ Phase 1 status (2026-05-27): bench DOE complete — all firmware remedies refuted; Seeed engineering inquiry sent with full evidence package.** A first attempt at this milestone using the **Seeed Wio-LR1121 module (SKU 113991415)** as Radio 2 reached a hard block: TX works end-to-end (other LoRa receivers see the bridge's transmissions), but RX produces zero `RX_DONE` events for any over-the-air traffic — even with a transmitting MT phone's antenna physically pressed against the LR1121's antenna port. We then executed two full Design-of-Experiments (DOE) investigation phases per the **Semtech LR1121 User Manual v2.2** ([direct PDF](https://semtech.my.salesforce.com/sfc/p/#E0000000JelG/a/RQ00000DClgP/D.pNG5l4FviPI634eCx8GFURZEwDO2ZBA33MpriB_FU) · [product page](https://www.semtech.com/products/wireless-rf/lora-connect/lr1121)):
  >
  > - **Phase A — RFSWx switch-table sweep (12 iterations).** All 5 chip-level RFSWx-capable DIOs (DIO5/6/7/8/10 per UM §4.2.1) swept in every meaningful combination. Self-echo RSSI invariant within ~7 dB across all iterations. Zero OTA `RX_DONE`. Seeed's published KiCad library separately confirms DIO5/6/7 are routed as `MCU_DIO5/6/7` host-expansion test pads, **not** used as switch outputs on this module.
  > - **Phase B — Chip-init DOE (4 effective runs).** Tested every UM v2.2-prescribed firmware remedy individually and combined: `SetRssiCalibration` with UM Table 7-21 "600 MHz – 2 GHz" tunes (Run 2), `CalibImage(902, 928)` after `SetTcxoMode` (Run 3), and the kitchen-sink stack with pre-`Standby(STBY_RC)` + RSSI cal + image cal + `SetRxBoostedGainMode(true)` (Run 5). Every command returned `state=0`. **None resolved the RX failure.** Two new pieces of hard evidence emerged: (a) the chip's `GetErrors()` register reads `0x0020 = HF_XOSC_START_ERR` persistent at every POR on every unit, and (b) one `RADIOLIB_ERR_CRC_MISMATCH` event in Run 5 — meaning the RX chain is partially functional but with sensitivity degraded by an estimated 40–50 dB versus LR1121 datasheet spec. Two independently-sourced units behave identically.
  >
  > **All UM v2.2-prescribed firmware remedies have been tested. None resolves the failure.** Remaining hypothesis space: hardware-design issue (matching network, switch insertion loss, LNA isolation) or LR1121 chip firmware errata at base FW 1.3. The full bug report ([`SEEED_SUPPORT_INQUIRY.md`](https://github.com/GrayHatGuy/Xiao-esp32s3-lora-repeater/blob/lr1121-phase1/SEEED_SUPPORT_INQUIRY.md)) with the DOE evidence appended, the design-feedback companion ([`SEEED_RECOMMENDATIONS.md`](https://github.com/GrayHatGuy/Xiao-esp32s3-lora-repeater/blob/lr1121-phase1/SEEED_RECOMMENDATIONS.md)), the full DOE bench plan + results ([`LR1121-RX-INIT-AUDIT.md`](https://github.com/GrayHatGuy/Xiao-esp32s3-lora-repeater/blob/lr1121-phase1/LR1121-RX-INIT-AUDIT.md)), and the locked-in email body ([`SEEED_EMAIL_DRAFT.md`](https://github.com/GrayHatGuy/Xiao-esp32s3-lora-repeater/blob/lr1121-phase1/SEEED_EMAIL_DRAFT.md)) all live on the [`lr1121-phase1`](https://github.com/GrayHatGuy/Xiao-esp32s3-lora-repeater/tree/lr1121-phase1) branch and at the snapshot tag [`lr1121-bringup-2026-05-26`](https://github.com/GrayHatGuy/Xiao-esp32s3-lora-repeater/tree/lr1121-bringup-2026-05-26) for Seeed-correspondence stability. **`main` continues to ship the Phase 0 dual-SX1262 release (v8.1).** Phase 1 will resume on Seeed engineering's reply, or via pivot to an alternate LR1121 carrier (Semtech LR1121DVK1, Ebyte E80-900M22S, or custom).

  **Minimum viable hardware change:** keep the existing Xiao Wio-SX1262 on one of the two slots, and swap *only the other slot* for a radio that can reach 2.4 GHz. The bridge needs exactly one 2.4-GHz-capable side; the SX1262 stays as the sub-GHz endpoint.

  **Prototype target:** the **Seeed Wio LR1121 breadboard** — already on hand, RadioLib-supported, multi-band on one die (sub-GHz **+ 2.4 GHz + S-band 1.9-2.1 GHz**), plus LR-FHSS. For this bridge it sits on the 2.4 GHz slot opposite the existing Xiao Wio SX1262; longer-term a single LR1121 could replace *both* SX1262s if collapsing to one radio family becomes interesting.

  Other Semtech parts to track for later, in roughly increasing capability:
  - **SX1280** — 2.4 GHz only (~2400-2500 MHz). Cheapest and narrowest; useful if a smaller, lower-cost run is ever in scope.
  - **LR22xx series** (e.g. LR2021) — newer multi-band parts: SX1280-class 2.4 GHz, improved sensitivity, BLE-coexistence awareness, expanded LR-FHSS modes. Worth watching as RadioLib support matures.

  **MCU upgrade is conditional, not required.** If a Xiao-compatible 2.4 GHz module turns up (or can be hand-wired onto the edge pins next to the existing B2B Wio shield) the Xiao ESP32S3 Sense stays in service and the form factor barely changes. If the only available SX1280 / LR1121 carriers want full 0.1"-header access, the natural upgrade target is the **ESP32-S3 DevKitC-1 N-R** (e.g. N8R8 — standard ESP32-S3-WROOM-1 dev board with N flash + R PSRAM): ~40 broken-out GPIOs, more flash + PSRAM, and the ability to wire arbitrary off-the-shelf breakouts. `WioSX1262.{h,cpp}` already abstracts pin assignments, SPI bus sharing, and the mutex — switching MCU boards would mean a new `pinout.h` (or per-board `#ifdef` block) and not much else.

  Good news on the firmware side: the protocol decoders and `bridgePacket()` dispatcher in this repo are RF-agnostic — they branch on the LoRa sync word, not the carrier frequency. Once a `WioSX1280` / `WioLR1121` / `WioLR2021` wrapper lands alongside `WioSX1262` (same `LoraConfig` struct, same `available()` / `read()` / `transmit()` surface), the existing bridge pipeline drops straight in with RF parameter changes in `platformio.ini`.

### Reticulum / RNode — lowest priority

Deprioritised to the bottom of the roadmap. The `0x42` Reticulum sync word is already wired into the bridge dispatcher as a third protocol, but only the receive half is implemented today. **No RNS controls will be added to the captive portal** — RNS stays a build-flag / firmware concern only.

| Direction | Status |
|-----------|--------|
| `RX:RNS → TX:MT or MC` | ✅ stub. Raw RNS bytes are base64-encoded and re-transmitted as one or more `[rns <seq> <x>/<y>] <base64>` text packets on the destination radio. CRC-16 low-byte sequence ID, per-protocol fragment pacing, 8-fragment cap. |
| `RX:MT or MC → TX:RNS` | ❌ log-only. The decoded body is printed with a `No TX 2 RNS: [MT/MC] …` prefix; nothing is transmitted on the RNS radio. |
| `RX:RNS → human-readable decode` | ❌ base64 dump only — RNS packet framing isn't parsed yet. |
| `MT/MC fragment reassembly → RNS TX` | ❌ stub function present (`reassembleReticulumFragment()` in `MeshDecoderDebug.h`), no logic yet — lands with the RNS encoder. |

Outstanding work to lift the stub:

- **RNS packet decoder.** Parse the RNS LoRa frame: header byte (IFAC flag, hops, header type, propagation/context bits), destination/transport address hashes, context byte, ciphertext. Produce a structured decode line analogous to the Meshtastic/MeshCore ones in `MeshDecoderDebug.h`.
- **RNS packet encoder.** Build a valid outgoing RNS frame in `MeshEncoderDebug.h`: write the header byte, attach the right destination hash, set the context byte for the payload type, and slot the body bytes into the ciphertext region. Wiring it into `bridgePacket()` is then a one-line dispatcher change.
- **Fragment reassembly.** Fill in `reassembleReticulumFragment()` in `MeshDecoderDebug.h`: parse `[rns <seq> <x>/<y>] <base64>` out of the incoming MT/MC body, accumulate slots keyed on `<seq>`, base64-decode each chunk, time out stale entries after ~30 s, and emit the reassembled raw RNS frame once `count == total`. Needed before `MT/MC → RNS` can actually transmit.
- **Optional IFAC support.** If the encoder ever needs to talk on a network with Identify-Fail Authentication enabled, the IFAC HMAC-SHA256 trailer and salt handling come along with it.
