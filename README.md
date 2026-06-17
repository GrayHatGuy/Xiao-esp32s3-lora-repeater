# Xiao-esp32s3-lora-repeater

Xiao ESP32S3 with dual SX1262 radio SPI cross-band repeater.

<img width="4096" height="3265" alt="PXL_20260507_021829300~2" src="https://github.com/user-attachments/assets/b9e68624-3cb4-46a3-9c2f-4927e6a8fdf2" />

###### *touched by claude but not by epstein*

## Introduction / Background

A bidirectional LoRa mesh bridge running on a single Seeed Xiao ESP32S3 Sense with two Seeed Wio SX1262 shields stacked back-to-back — one mated to the Xiao's edge pins, the other to the 40-pin B2B header. The two radios share one SPI bus through a FreeRTOS mutex and each run in their own task pinned to a separate ESP32-S3 core, so they can transmit and receive in parallel on completely different RF profiles.

Each radio carries its own protocol **and** its own channel. The bridge relays packets received on one radio out the other — *cross-protocol* (MT↔MC) or *same-protocol between two channels* (MC↔MC, MT↔MT — e.g. a private channel bridged to the public one). Everything — region, per-radio protocol, RF plan (frequency, bandwidth, spreading factor, coding rate, sync word, TX power), channels and identity — is configured through the WiFi captive portal. A single `.bin` flashed with no build flags first-boots straight into the portal, so no PlatformIO build is needed to deploy. The `platformio.ini` `LORA_RADIO*` build flags remain available as optional first-boot defaults for source builds.

Supported today:

- **Meshtastic** (sync `0x2B`) — AES-CTR + a hand-written protobuf walker that lifts `TEXT_MESSAGE_APP` payloads out of the on-air `Data` submessage. `POSITION_APP` and `TELEMETRY_APP` are also decoded and bridged as compact text lines (`pos 40.7234,-74.0123 alt 12m`, `bat 87% 4.05V`, `env 22.5C RH 45% 1013hPa`). Defaults to the public LongFast channel; override `BRIDGE_MT_CHANNEL_NAME` / `BRIDGE_MT_PSK_B64` (or use the captive portal) to bridge a private channel — the PSK can be a short key, a 16-byte AES-128 key, or a 32-byte AES-256 key, and the channel hash is auto-derived. The bridge decodes incoming `NODEINFO_APP` packets into a 64-entry NVS-persistent NodeDB and emits its own periodic NodeInfo so phones surface the bridge as a known sender (`!b16b00b5`, "LoRa Bridge"). It also learns wall-clock time opportunistically from a `POSITION_APP` time field, so bridged MeshCore packets carry a real timestamp.
- **MeshCore channel** (sync `0x12`) — AES-128-ECB decrypt of `GRP_TXT`, with the 2-byte truncated HMAC-SHA256 verified against the channel key. Defaults to the MeshCore public channel (hash `0x11`); override `BRIDGE_MC_KEY_HEX` / `BRIDGE_MC_CHANNEL_NAME` in `platformio.ini` to bridge a private or custom channel instead — the on-air channel-hash byte is auto-derived from `SHA-256(key)[0]` at boot.
- **Reticulum / RNode** (sync `0x42`) — incoming frames are base64-encoded and bridged into the other mesh as text packets of the form `[rns <seq> <x>/<y>] <base64>`. The bridge auto-fragments across multiple MT/MC packets when a single one wouldn't hold the encoded frame, using a CRC-16 low-byte sequence ID so concurrent fragmented frames don't get mixed up on the receiving side. Fragments are paced by the per-radio **airtime throttle** (max 8 fragments per frame, tunable via `BRIDGE_RNS_*`). With both radios set to Reticulum, the bridge **transparently raw-repeats** RNS frames byte-for-byte — a range-extending RNS↔RNS repeater (`BRIDGE_RNS_INPROTO_REPEAT`, on by default). A proper `MT/MC → RNS` packet encoder is still TODO; until then that direction is a log-and-drop path on serial.
- **LoRaWAN** (sync `0x34`, **keyless**) — a localized capture/relay tap, *not* a gateway. The bridge reads only the **cleartext LoRaWAN MAC header** (MType, DevAddr, FCtrl, FCnt, FPort, FRMPayload length; JoinEUI/DevEUI on join-requests) — no keys, no payload decryption, no `FCnt`/MIC synthesis. It can log the header (`evt=RX proto=LW …`), emit a one-line metadata **summary** into your Meshtastic/MeshCore mesh (`BRIDGE_LW_SUMMARY_TO_MESH`), and **transparently raw-repeat** `0x34` frames between two LoRaWAN radios/bridges as a dedup-bounded flood (`BRIDGE_LW_RELAY`) — modeled on the [Tasmota LoRaWAN bridge](https://tasmota.github.io/docs/LoRa-and-LoRaWan-Bridge/). By default `MT/MC → LoRaWAN` is a deliberate log-and-drop (`no-lw-encoder`) — keyless firmware can't inject content. **New in v8.4:** an opt-in **keyed ABP uplink encoder** (`BRIDGE_LW_ENCODE` + ABP session keys) turns that path into a real LoRaWAN encode — it wraps decoded mesh (MT/MC) traffic into a valid LoRaWAN 1.0.x **ABP uplink** (DevAddr + `NwkSKey` + `AppSKey`, AES-CTR FRMPayload, AES-CMAC MIC, monotonic FCnt) and re-emits it over RF for an existing gateway to forward to a ChirpStack LNS. The encoder is **hardware-verified on air** (2026-06-16) — Meshtastic *and* MeshCore frames encode to valid ABP uplinks that decrypt back to the original text with a valid MIC; per-source ABP devices are **captive-portal–configured** with a reboot-safe FCnt, and a raw-LoRa / **Custom** source (the weather-station case) wraps its raw bytes into an uplink too. **The only remaining bench item is ingestion by a live ChirpStack LNS** (Tier C) — see [`BENCH-RESULTS.md`](BENCH-RESULTS.md). Single-channel per radio (no channel hopping, no Class-A RX windows).

### Routing & behavior

Every received packet is decoded once, run through a content-hash loop/dup guard, re-encoded for the *other* radio's protocol, and queued for a non-blocking transmit. The behavior worth calling out:

- **Clean far-side bodies.** The old `[MT !id name]` / `[MC]` / `[rns]` text markers are gone. Loop prevention is now a **TTL content-hash dedup** (FNV-1a over the decoded body + sender id + Meshtastic packet_id, recorded on receive *and* on every emission), which also drops the same packet heard on both radios and never false-drops a user message that happens to start with `[MT`. Folding the packet_id lets a node's genuinely-distinct messages with identical text through each time (new id → new hash) while echoes/replays (same id) still drop.
- **A repeat looks like it came from the original sender, not the bridge.** Cross-protocol **MC→MT** reconstructs the MeshCore sender as a *deterministic virtual Meshtastic node* (id = `FNV-1a("MC|"+name)`) and advertises a synthetic NodeInfo, so a phone shows the message from `Alice @MC`. Cross-protocol **MT→MC** prefixes the body with the Meshtastic sender's name (`Alice@MT: …`, NodeDB short-name or `!hexid`) — the only identity channel MeshCore group text offers. A **same-protocol, same-channel** pair on different frequencies is a *transparent raw repeat*: the original bytes go out unchanged (Meshtastic: hop_limit decremented, `relay_node` set), so the far side sees the original sender natively. The `@MT`/`@MC` origin tags are on by default (`BRIDGE_TAG_ORIGIN_PROTO=0` for bare native-looking names). The whole layer can be disabled with `-DBRIDGE_IDENTITY_PRESERVE=0` (clean-body / bridge-identity behaviour).
- **TX never clobbers RX.** Each radio defaults to receive; an outbound packet goes onto a per-destination, age-bounded, PSRAM-backed queue and is sent by a CAD-gated (listen-before-talk) non-blocking transmit with CSMA backoff and a duty-cycle airtime throttle. So a long SF11 transmit no longer makes the bridge deaf.
- **Structured serial logs.** The runtime log is now one greppable `ts=… evt=… radio=… key=val` line per pipeline event (`RX`/`DEDUP_PASS`/`DROP`/`QUEUE`/`CAD`/`TX_START`/`TX_DONE`/`THROTTLE`/`NODEDB`/`NODEINFO`/`CLOCK`), emitted atomically across both cores.
- **Real timestamps without an RTC (clock-learn).** The bridge has no RTC/NTP, so it *learns* wall-clock time — from inbound MeshCore packet timestamps (**v8.2.1**) and from Meshtastic `POSITION_APP` time (**v8.3**) — and stamps that real Unix time onto bridged MeshCore `GRP_TXT` (surfaced as `mcts=` and a one-time `evt=CLOCK`). Before this, `MT→MC` / `RNS→MC` hardcoded the timestamp to 0 and clients showed *1969*. A cold boot still stamps `0` only until the first timestamped packet calibrates it (inherent without an RTC).
- **Same-protocol transparent repeat spans every protocol.** Two radios on the *same* protocol + channel at *different* frequencies raw-repeat byte-for-byte — a range extender — for Meshtastic, MeshCore, **Reticulum (`RNS↔RNS`, v8.3)** and **LoRaWAN (`LW↔LW`, v8.3)**. The RNS / LoRaWAN repeats are byte-exact (no header mutation — a LoRaWAN MIC would break otherwise), loop-bounded by the shared content-hash dedup + the airtime throttle.
- **LoRaWAN: keyless tap, plus a keyed encoder (v8.4).** A `0x34` radio runs a keyless capture / metadata-summary / `LW↔LW` relay tap (**v8.3**). **v8.4** adds an opt-in **keyed ABP uplink encoder**: with ABP credentials set, `MT/MC → LoRaWAN` becomes a real ABP-uplink encode that RF-re-emits to a gateway → ChirpStack LNS (**hardware-verified on air** — MT *and* MC frames decrypt back, MIC valid; only the live-ChirpStack ingestion bench is open — [`BENCH-RESULTS.md`](BENCH-RESULTS.md)); with no keys it stays the keyless `no-lw-encoder` drop. The two modes are mutually exclusive per radio, selected by the presence of keys — so a stock build is unaffected.

**Everything above is tunable at compile time.** All flags are optional, each documented with its compiled-in default in [`platformio.ini`](platformio.ini); see [Build flags & compile-time configuration](#build-flags--compile-time-configuration) under Instructions for the full catalog.

All crypto runs on the ESP-IDF's built-in mbedTLS — no extra library dependencies beyond `jgromes/RadioLib` (pinned `7.7.0`).

**What's new in each release:** see [`CHANGELOG.md`](CHANGELOG.md) for the full per-version changelog. The latest is the **v8.4 LoRaWAN ABP uplink encoder** ([v8.4 release](https://github.com/GrayHatGuy/Xiao-esp32s3-lora-repeater/releases/tag/v8.4-ABP-lorawan)) — **hardware-verified on air** (Meshtastic *and* MeshCore → valid ABP uplinks, decrypt-verified, MIC-valid; **12/16 bench tests pass**, only live-ChirpStack ingestion remaining — [`BENCH-RESULTS.md`](BENCH-RESULTS.md)), plus a captive-portal **auto-fill of RF defaults on protocol switch** UX fix. Built on the **[v8.3.1 release](https://github.com/GrayHatGuy/Xiao-esp32s3-lora-repeater/releases/tag/v8.3.1)** (Radio-2 V1.0/V1.1 fix). Design + bench docs: [`ABP-LORAWAN-SPEC.md`](ABP-LORAWAN-SPEC.md), [`BENCH-v8.4.md`](BENCH-v8.4.md), [`BENCH-RESULTS.md`](BENCH-RESULTS.md).

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
(Radio 1) mounts on the Xiao's 40-pin board-to-board connector; the edge-pin
SX1262 (Radio 2) mounts on the Xiao's perimeter header. The pin mapping is set in
the firmware (see `src/main.cpp`).

> ⚠️ **The Radio 2 edge module ships in two board revisions (V1.0 / V1.1) with
> different pinouts — you must build the firmware variant that matches yours.**
> Check the silkscreen first: see [Radio 2 module revision](#radio-2-module-revision-v10-vs-v11) below.

The table shows Radio 2 for **both module revisions** — build the env that matches your silkscreen ([how to identify yours](#radio-2-module-revision-v10-vs-v11)). **Bold** marks the four Radio-2 signals that move between V1.0 and V1.1 (NSS, DIO1, BUSY, ANT_SW); the shared SPI bus and RESET are identical on both.

| Signal | Radio 1 (B2B) | Radio 2 — **V1.0** (`xiao_esp32s3`) | Radio 2 — **V1.1** (`xiao_esp32s3_v1_1`) | Notes |
|--------|---------------|-------------------------------------|------------------------------------------|-------|
| SCK        | GPIO7 (D8)  | GPIO7 (D8)     | GPIO7 (D8)     | **shared** SPI bus |
| MOSI       | GPIO9 (D10) | GPIO9 (D10)    | GPIO9 (D10)    | **shared** |
| MISO       | GPIO8 (D9)  | GPIO8 (D9)     | GPIO8 (D9)     | **shared** |
| NSS / CS   | GPIO41      | **GPIO5 (D4)** | **GPIO4 (D3)** | per-radio chip select |
| DIO1 / IRQ | GPIO39      | **GPIO2 (D1)** | **GPIO1 (D0)** | RX-done interrupt |
| RESET      | GPIO42      | GPIO3 (D2)     | GPIO3 (D2)     | per-radio (same on both revs) |
| BUSY       | GPIO40      | **GPIO4 (D3)** | **GPIO2 (D1)** | per-radio |
| ANT_SW     | GPIO38      | **GPIO6 (D5)** | **GPIO5 (D4)** | TX/RX RF switch |
| VCC        | 3V3         | 3V3            | 3V3            | |
| GND        | GND         | GND            | GND            | |

**Key points**

- The two radios **share one SPI bus** (SCK/MOSI/MISO); the firmware serializes
  access with a FreeRTOS mutex.
- Each radio has its own **NSS, DIO1, RESET, BUSY, ANT_SW**, so they run
  independently — one task per ESP32-S3 core.
- Radio 1's pins (GPIO38–42) are exposed **only on the 40-pin B2B connector**.
- The TCXO is internal to each Wio SX1262 module (1.8 V) — not wired to a GPIO.
- **Connect both u.FL antennas before power-on** — transmitting into a missing
  antenna risks the PA.

### Radio 2 module revision (V1.0 vs V1.1)

**Seeed shipped the "Wio-SX1262 for XIAO" edge module with two different
pinouts.** The chip-select (NSS) moved between revisions, so flashing the wrong
variant means **Radio 2 won't be detected** (the bridge keeps running on Radio 1
alone and tells you which env to flash). Identify your module and build the
matching env.

Read the white silkscreen on the **Radio 2** module — both the version string and
the right-hand pin column:

| | **V1.0** | **V1.1** |
|---|---|---|
| Silkscreen | `Wio-SX1262 for XIAO V1.0` | `Wio-SX1262 for XIAO V1.1` |
| Right column (top→bottom) | D0, DIO1, RST, BUSY, **NSS**, RF_SW, D6 | DIO1, BUSY, RST, **NSS**, RF_SW, D5, D6 |
| **NSS** lands on | **D4 / GPIO5** | **D3 / GPIO4** |
| Build env | `xiao_esp32s3` (default) | `xiao_esp32s3_v1_1` |

| V1.0 | V1.1 |
|------|------|
| ![Wio-SX1262 for XIAO V1.0 silkscreen](images/wio-sx1262-v1.0.jpg) | ![Wio-SX1262 for XIAO V1.1 silkscreen](images/wio-sx1262-v1.1.jpg) |

```bash
# V1.0 module (default):
pio run -e xiao_esp32s3      -t upload --upload-port COMx
# V1.1 module:
pio run -e xiao_esp32s3_v1_1 -t upload --upload-port COMx
```

The firmware prints the revision it was built for at boot, so you can confirm from
the serial log:

```
[diag] R2 edge module = V1.0  (NSS=5 DIO1=2 RST=3 BUSY=4 RF_SW=6)
```

## Instructions

> **Fastest path (no toolchain).** **First check your Radio 2 module's silkscreen revision** ([Radio 2 module revision](#radio-2-module-revision-v10-vs-v11) above) and download the **matching** `vanilla-factory` bin from the [latest release](https://github.com/GrayHatGuy/Xiao-esp32s3-lora-repeater/releases/latest): `…-v1.0-vanilla-factory.bin` for a **V1.0** Radio-2 module, `…-v1.1-vanilla-factory.bin` for **V1.1**. Connect both antennas and flash it to offset `0x0` — e.g. `esptool.py --chip esp32s3 write_flash 0x0 <bin>`, or drag it into the [ESP web flasher](https://espressif.github.io/esptool-js/) at address `0x0`. A fresh/erased device first-boots straight into the captive portal, so you can **skip to step 7**. The numbered steps below are for building from source.

1. **Stack the hardware.** Mate the B2B shield (radio 1) on top the Xiao, the edge-pin shield (radio 2) on bottom. Connect antennas to **both** radios before powering on. Correct orientation has all antennas on the same side.
2. **Install [PlatformIO](https://platformio.org/install)** — the VS Code extension is the easiest path.
3. *(Optional — source builds only)* **Pre-seed the radios** in [`platformio.ini`](platformio.ini). As of v8.0 this is no longer required: a `.bin` built with no `LORA_RADIO*` flags first-boots straight into the captive portal where region, protocol and RF are all set. If you do build from source, these flags become the first-boot defaults the portal form pre-fills:
   ```ini
   -DLORA_RADIO1_FREQUENCY=906.875f
   -DLORA_RADIO1_BANDWIDTH=250.0f
   -DLORA_RADIO1_SPREAD_FACTOR=11
   -DLORA_RADIO1_CODING_RATE=5
   -DLORA_RADIO1_TX_POWER=20
   -DLORA_RADIO1_SYNC_WORD=0x2B   ; 0x12 MeshCore, 0x2B Meshtastic, 0x42 Reticulum, 0x34 LoRaWAN
   ```
4. *(Optional)* **Set bridge defaults** in `platformio.ini`. These are the values the bridge uses when its NVS config is empty (i.e. a fresh flash). The captive portal in step 7 lets users override them at runtime without rebuilding. The numeric ID and `!`-prefixed string must encode the same value; string macros are single-quoted so spaces survive shell tokenization:
   ```ini
   -DBRIDGE_MT_NODE_ID=0xB16B00B5u
   '-DBRIDGE_MT_NODE_ID_STR="!b16b00b5"'
   '-DBRIDGE_MT_LONG_NAME="LoRa Bridge"'
   '-DBRIDGE_MT_SHORT_NAME="BR"'
   ```
5. **Pick your Radio 2 variant, then clean + build.** First read the **Radio 2** module silkscreen and choose the matching build env (see [Radio 2 module revision](#radio-2-module-revision-v10-vs-v11) above): a **V1.0** module → `xiao_esp32s3` (default), a **V1.1** module → `xiao_esp32s3_v1_1`. Then `pio run -t clean -e <env> && pio run -e <env>` — the clean is important whenever a header changes. Building the wrong variant won't harm anything (Radio 1 still comes up and the boot log names the env to flash), but Radio 2 won't be detected until the env matches your board. The boot log echoes the active map: `[diag] R2 edge module = V1.0 (NSS=5 …)`.
6. **Upload.** `pio run -t upload` or use the PlatformIO toolbar.
7. **First-boot setup over WiFi.** Open `pio device monitor` at 115200 baud. On a fresh flash the bridge launches an open WiFi AP named `LoRa-Bridge-<XX>` (last byte of the MAC-derived MT node ID, in hex — unique per device). Join that SSID from a phone or laptop — any HTTP request will be DNS-redirected to the single-page config form. As of v8.0 the form covers **everything**: device region, per-radio protocol (Meshtastic / MeshCore / Reticulum / LoRaWAN / Custom / None), modem preset, channel name + key, frequency (Tier 2 channel-slot value pre-filled for Meshtastic, editable), Custom RF plan, identity and the POSITION/TELEMETRY toggles. Hit **Save & reboot** and the bridge restarts into normal mode with the NVS values. To re-enter the form on an already-configured device, reset the board and — within the ~5 s window the serial log announces — either press the **BOOT** button *or* send any character from the serial monitor. (The serial route matters when the BOOT button is physically hidden under the radio shield.)  ***NOTE: if the key press reset fails then erase the device and it will reboot into the active wifi portal config***

   <p align="center"><img src="images/captive-portal.png" alt="Captive-portal configuration form served at LoRa-Bridge-XX @ 192.168.4.1" width="360"></p>
   <p align="center"><em>The captive-portal config form (region, per-radio protocol/RF/channel, identity, toggles).</em></p>

8. **Monitor.** Once the bridge is configured, expect RX summary lines (size / RSSI / SNR), protocol-decoded summaries, bridge re-encode lines, NodeInfo broadcasts, and `loop-drop` messages when relay echoes come back to the bridge.

> **Advanced — v8.4 LoRaWAN ABP uplink encoder (opt-in).** Emitting valid LoRaWAN is *keyed*. Flash the **`xiao_esp32s3_lwabp`** env (or any build with `-DBRIDGE_LW_ENCODE=1`) and configure per-source ABP identities in the captive portal's **"LoRaWAN ABP devices"** section (DevAddr + NwkSKey + AppSKey + FPort, a reboot-safe FCnt, and an optional source tag). The `BRIDGE_LW_ENC_*` build flags are a single-device fallback. Provision the matching ABP device in ChirpStack (MAC 1.0.x, ABP, Class A, ADR off) and paste [`tools/chirpstack-codec.js`](tools/chirpstack-codec.js); then a `0x34` destination radio re-emits your decoded mesh (MT/MC) — or a **Custom** raw-LoRa station's raw bytes — as ABP uplinks for a gateway to forward. The encoder is **hardware-verified on air** (MT/MC → valid ABP, decrypt + MIC checked, [`BENCH-RESULTS.md`](BENCH-RESULTS.md)); **live-ChirpStack ingestion is the one remaining bench item** ([`BENCH-v8.4.md`](BENCH-v8.4.md)). Full design: [`ABP-LORAWAN-SPEC.md`](ABP-LORAWAN-SPEC.md).

### Build flags & compile-time configuration

Every option below is **optional** and lives in [`platformio.ini`](platformio.ini), documented with its compiled-in default (uncomment a `-D` line to override). They are first-boot defaults / behavior tunables only — region, per-radio protocol and RF are normally set in the captive portal at runtime. Grouped by the release that introduced them:

**v8.0 — per-radio RF, identity & channels** *(also the captive-portal defaults)*
- Per radio (`*` = `1` B2B / `2` edge): `LORA_RADIO*_FREQUENCY`, `LORA_RADIO*_BANDWIDTH`, `LORA_RADIO*_SPREAD_FACTOR`, `LORA_RADIO*_CODING_RATE`, `LORA_RADIO*_TX_POWER`, `LORA_RADIO*_SYNC_WORD` (`0x2B` MT · `0x12` MC · `0x42` RNS · `0x34` LoRaWAN).
- Global radio: `LORA_PREAMBLE_LEN` (8), `LORA_CRC` (1), `LORA_TCXO_VOLTAGE` (1.8f), `LORA_MAX_PACKET` (256).
- Identity: `BRIDGE_MT_NODE_ID` (`0xB16B00B5u`) + matching `BRIDGE_MT_NODE_ID_STR` (`"!b16b00b5"`), `BRIDGE_MT_LONG_NAME`, `BRIDGE_MT_SHORT_NAME`.
- Channels: `BRIDGE_MT_CHANNEL_NAME` (LongFast) + `BRIDGE_MT_PSK_B64` (blank = LongFast; decodes to a 1/16/32-byte key); `BRIDGE_MC_CHANNEL_NAME` (public) + `BRIDGE_MC_KEY_HEX` (channel-hash auto-derived from `SHA-256(key)[0]`).
- Per-portnum bridging: `BRIDGE_MT_POSITION` (1), `BRIDGE_MT_TELEMETRY` (1); region default: `BRIDGE_REGION`.

**v8.1 — build-time validation** *(automatic; no flag to set)*
- [`src/LoraConfigCheck.h`](src/LoraConfigCheck.h) rejects an invalid `LORA_RADIO*` set at compile time (`#error` / `static_assert` on sync word, SF/CR/BW/region sanity, TX-power range) — complements the portal's runtime validation of portal-entered values.

**v8.2 — RX-priority routing + source-identity preservation** *(defaults in parens)*
- Loop/dedup: `BRIDGE_DEDUP_TTL_MS` (60000), `BRIDGE_DEDUP_TABLE_SIZE` (512).
- Route queue (PSRAM): `BRIDGE_ROUTE_QUEUE_DEPTH` (64), `BRIDGE_ROUTE_MAX_AGE_MS` (30000).
- TX scheduler / CAD: `BRIDGE_CAD_BACKOFF_MIN_MS` (20), `BRIDGE_CAD_BACKOFF_MAX_MS` (120), `BRIDGE_TX_INFLIGHT_TIMEOUT_MS` (10000).
- Airtime throttle: `BRIDGE_TX_DUTY_PERCENT` (50), `BRIDGE_TX_MIN_GAP_MS` (0).
- Identity preservation: `BRIDGE_IDENTITY_PRESERVE` (1), `BRIDGE_TAG_ORIGIN_PROTO` (1), `BRIDGE_MC_NONAME_VIRTUAL` (0), `BRIDGE_MC_NAME_MAX` (32), `BRIDGE_VIRT_NODES_MAX` (32), `BRIDGE_VIRT_NODEINFO_PERIOD_MS` (900000).
- RNS tunnel: `BRIDGE_RNS_MAX_FRAGS` (8) — fragments paced by the airtime throttle (the old `BRIDGE_RNS_FRAG_DELAY_*` knobs were removed).

**v8.3 — keyless LoRaWAN + RNS↔RNS** *(defaults in parens)*
- LoRaWAN (sync `0x34`, keyless): `BRIDGE_LW_CAPTURE` (1 — `evt=RX proto=LW` header log), `BRIDGE_LW_SUMMARY_TO_MESH` (1 — one-line metadata summary to MT/MC), `BRIDGE_LW_RELAY` (1 — transparent LW↔LW raw repeat / dedup-bounded flood).
- Reticulum: `BRIDGE_RNS_INPROTO_REPEAT` (1 — transparent RNS↔RNS raw repeat; `0` = pre-v8.3 tunnel-only).

**v8.4 — LoRaWAN ABP uplink encoder (keyed; opt-in)** *(defaults in parens)*
- `BRIDGE_LW_ENCODE` (0) — master switch. When `1` **and** both session keys parse, `MT/MC → LoRaWAN` becomes a keyed **ABP uplink encode** (RF re-emit / delivery model B1) instead of the keyless `no-lw-encoder` drop; otherwise the keyless tap is unchanged (do-no-harm). *(Per-source devices + reboot-safe FCnt are also portal-configured; a raw-LoRa/Custom source is handled. On-air ChirpStack ingestion is the remaining bench item.)*
- `BRIDGE_LW_ENC_DEVADDR` (`0x01000001u` — ABP device address; the default uses a private NetID `0x01` prefix), `BRIDGE_LW_ENC_FPORT` (13).
- `BRIDGE_LW_ENC_NWKSKEY` / `BRIDGE_LW_ENC_APPSKEY` (empty) — the 32-hex-char ABP session keys (`NwkSKey` → MIC, `AppSKey` → FRMPayload); the encoder stays **idle until both parse**. Provision the *same* DevAddr + keys in ChirpStack (MAC 1.0.x, ABP, Class A, ADR off, disable-FCnt-validation or persist).
- `BRIDGE_LW_ENC_SELFTEST` (0) — run boot-time known-answer self-tests (RFC 4493 AES-CMAC vectors + the FRMPayload keystream + a frame round-trip) → logs `[lw-selftest] … PASS`.
- *These build flags are the single-device fallback (in-RAM FCnt). For per-source identities + a reboot-safe NVS FCnt, use the captive-portal "LoRaWAN ABP devices" section (env `xiao_esp32s3_lwabp`). See [`ABP-LORAWAN-SPEC.md`](ABP-LORAWAN-SPEC.md).*

**Bench only:** `BRIDGE_BENCH_AUTOSAVE` (set on the `bench_*` envs, incl. the new `bench_lw_enc` ABP-encoder env) makes an erased board boot pre-configured and skip the captive portal — never enable it in a release build.

## Routing & protocol support (current functionality)

The bridge dispatches by **LoRa sync word**: each radio is assigned a protocol in
the portal, and a received packet is decoded once, run through the content-hash
loop/dup guard, re-encoded for the *other* radio's protocol, and queued for a
CAD-gated non-blocking transmit (the v8.2 RX-priority pipeline). Source identity
is preserved/reconstructed across the bridge — see [`V8.2-SPEC.md`](V8.2-SPEC.md).

| Protocol | Sync | RX (decode) | Bridge / TX | Identity across the bridge |
|----------|------|-------------|-------------|----------------------------|
| **Meshtastic (MT)** | `0x2B` | AES-CTR + protobuf walk: `TEXT_MESSAGE_APP`, `POSITION_APP`, `TELEMETRY_APP` → text line; `NODEINFO_APP` → NodeDB (not bridged) | Re-encodes for the destination. **Same channel, different frequency → transparent raw repeat** (original bytes, `hop_limit` decremented) | **MT→MC:** body prefixed `Name@MT:` (NodeDB short-name, else `!hexid`). **MT→MT raw repeat:** original sender preserved natively |
| **MeshCore (MC)** | `0x12` | AES-128-ECB `GRP_TXT` + 2-byte HMAC verify | Re-encodes `GRP_TXT` for the destination channel; same-channel/diff-freq → raw repeat | **MC→MT:** the `"Name: …"` sender becomes a deterministic virtual MT node `FNV-1a("MC|name")` with a synthetic NodeInfo (`Name @MC`); the name is moved into the MT header and stripped from the body |
| **Reticulum (RNS)** | `0x42` | **Not decrypted** — frame treated as opaque bytes (the bridge holds no RNS keys) | **RNS↔RNS:** transparent byte-for-byte raw repeat (range extender, `BRIDGE_RNS_INPROTO_REPEAT`). **RNS→MT/MC:** base64-tunneled as `[rns <seq> <x>/<y>] …` text fragments (CRC-16 seq id, 8-fragment cap, airtime-throttle-paced). **MT/MC→RNS: not implemented** (log-and-drop). See the [Reticulum roadmap](#reticulum--rnode-routing) | n/a — frames opaque; RNS→MT fragments carry the bridge's own MT id |
| **LoRaWAN (LW)** | `0x34` | **Keyless** — cleartext MAC header only (MType / DevAddr / FCtrl / FCnt / FPort / len; JoinEUI/DevEUI on joins); no key, no payload decrypt | **Capture** log + **metadata summary** to MT/MC (`BRIDGE_LW_SUMMARY_TO_MESH`); **LW↔LW transparent raw relay / dedup-bounded flood** (`BRIDGE_LW_RELAY`). **`MT/MC/Custom → LW`: keyed ABP uplink encoder (v8.4, opt-in)** — per-source ABP devices (portal-configured, reboot-safe FCnt) transcode to a valid LoRaWAN ABP uplink (CMAC MIC + AES-CTR FRMPayload + monotonic FCnt); a raw-LoRa **Custom** source sends its raw bytes (weather station). RF-re-emits for a gateway → ChirpStack (**hardware-verified on air**; only live-ChirpStack ingestion pending — [`BENCH-RESULTS.md`](BENCH-RESULTS.md)); default (no keys) stays the keyless `no-lw-encoder` drop | keyless tap: summaries carry the bridge's own MT/MC id. ABP encode: each uplink is sent under a bridge-held ABP device identity (DevAddr) |
| **Custom** | any (portal-entered) | user-chosen sync word; **no built-in decoder** | RF-agnostic dispatch by sync word; a Custom radio with no matching decoder receives + logs but has no protocol-specific re-encode | n/a |
| **None** | — | radio disabled (single-radio / monitor mode) | — | — |

**Loop prevention** is a TTL content-hash dedup over the decoded body + sender id
+ Meshtastic packet_id, recorded on RX *and* on every emission — so echoes and
relayed duplicates drop while genuinely-distinct messages (incl. identical text
with a new packet_id) bridge. All RF settings and channel keys are per-radio in
the captive portal; compile-time defaults and the `BRIDGE_*` tunables live in
[`platformio.ini`](platformio.ini).

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

- [x] ~~**v8.2: RX-priority routing redesign + source-identity preservation.**~~ — **done** (v8.2), bench-verified 2026-06-13; specced in [`V8.2-SPEC.md`](V8.2-SPEC.md). Non-blocking CAD-gated TX + per-destination PSRAM route queues + airtime throttle (a transmit no longer blocks the other radio's RX); content-hash dedup keyed on body + sender id + Meshtastic packet_id (replaces the `[MT]/[MC]/[rns]` text markers → clean far-side bodies); source-identity preservation (MC→MT virtual nodes + synthetic NodeInfo, MT→MC name prefixes, same-channel transparent raw repeat); structured `evt=` serial logging; RadioLib pinned 7.7.0.

- [x] ~~**v8.3: keyless LoRaWAN + carry-overs.**~~ — **done & shipped** (v8.3), bench-validated on hardware 2026-06-14; specced in [`V8.3-SPEC.md`](V8.3-SPEC.md). Keyless LoRaWAN (`0x34`) capture tap + metadata summary to MT/MC + transparent LW↔LW raw relay / dedup-bounded flood (`MT/MC → LoRaWAN` is a `no-lw-encoder` drop); transparent **RNS↔RNS** raw repeat; and Meshtastic `POSITION_APP` clock-learn. `BridgeConfig` schema v4 unchanged.

- [x] ~~**v8.4: LoRaWAN ABP uplink encoder (keyed).**~~ — **shipped** in v8.4 (build-green, crypto-verified, **hardware-verified on air**); specced in [`ABP-LORAWAN-SPEC.md`](ABP-LORAWAN-SPEC.md). A self-contained **RFC 4493 AES-CMAC** (the prebuilt esp32s3 mbedTLS ships CMAC disabled) + `encodeLoRaWANUplink()` mint valid LoRaWAN 1.0.x **ABP uplinks** (AES-CTR FRMPayload, CMAC MIC, monotonic FCnt, ADR off, Unconfirmed). The keyless `no-lw-encoder` drop becomes a keyed transcode → `g_routeQ` **RF re-emit** (delivery model B1) when ABP credentials are configured — opt-in via `BRIDGE_LW_ENCODE` + keys, so a stock build keeps v8.3's keyless behavior. Crypto cross-checked against an independent CMAC (RFC 4493 vectors + a MIC-valid, round-tripping frame). **P2–P4 also done:** per-source ABP devices + a captive-portal section + reboot-safe NVS FCnt (P2), the Custom raw-LoRa **weather-station** path + a sample ChirpStack codec (P3), and a US915 per-TX dwell cap (P4). The **hardware bench is complete** (12/16 — A1–B7 + A3, MT *and* MC verified — [`BENCH-RESULTS.md`](BENCH-RESULTS.md)); **live-ChirpStack (Tier C) ingestion is the one remaining gate** ([`BENCH-v8.4.md`](BENCH-v8.4.md)). v8.4 also added captive-portal **auto-fill of RF defaults on protocol switch** (a protocol change no longer leaves stale freq/BW/SF — the slip that cost a bench session).

**Next, in priority order — protocol routing comes _ahead of_ the Sub-GHz↔2.4 GHz cross-band phase below:**

- [ ] **Reticulum `MT/MC → RNS` encoder + reassembly.** RNS↔RNS repeat and RNS→MT/MC tunneling shipped in v8.3; the remaining half is a real RNS packet encoder (build a valid frame) plus `reassembleReticulumFragment()` so `MT/MC → RNS` works. Current state + work list in [Reticulum / RNode routing](#reticulum--rnode-routing) below.
- [ ] **LoRaWAN ABP *decode* + OTAA + dual-LNS (post-v8.4).** The keyless tap (v8.3) and the full **ABP uplink *encode*** (`MT/MC/Custom → LoRaWAN` — per-source portal devices P2, the Custom weather-station path + codec P3, and a US915 dwell cap P4) landed in **v8.4** (above), pending on-air bench. Remaining: the optional ABP/OTAA **decode** of your own fleet (→ MT/MC) and the dual-LNS crosslink — see [`ABP-LORAWAN-SPEC.md`](ABP-LORAWAN-SPEC.md). Content bridging without keys stays architecturally precluded (analysis in [`V8.2-SPEC.md`](V8.2-SPEC.md) §14).

- [ ] **Sub-GHz ↔ 2.4 GHz LoRa cross-band bridging — Phase 1 ON HOLD pending Seeed clarification.** The current build talks the SX1262's native sub-GHz ranges (902-928 MHz US ISM, 868 MHz EU, etc.). A long-horizon goal is to bridge those to 2.4 GHz LoRa networks (e.g. Meshtastic's 2.4 GHz preset) on the worldwide-licence-free **2.4 GHz ISM band** — the headline feature that makes this milestone worth doing.

  > **⚠️ Phase 1 status (2026-05-27): bench DOE complete — all firmware remedies refuted; Seeed engineering inquiry sent with full evidence package.** A first attempt at this milestone using the **Seeed Wio-LR1121 module (SKU 113991415)** as Radio 2 reached a hard block: TX works end-to-end (other LoRa receivers see the bridge's transmissions), but RX produces zero `RX_DONE` events for any over-the-air traffic — even with a transmitting MT phone's antenna physically pressed against the LR1121's antenna port. We then executed two full Design-of-Experiments (DOE) investigation phases per the **Semtech LR1121 User Manual v2.2** ([direct PDF](https://semtech.my.salesforce.com/sfc/p/#E0000000JelG/a/RQ00000DClgP/D.pNG5l4FviPI634eCx8GFURZEwDO2ZBA33MpriB_FU) · [product page](https://www.semtech.com/products/wireless-rf/lora-connect/lr1121)):
  >
  > - **Phase A — RFSWx switch-table sweep (12 iterations).** All 5 chip-level RFSWx-capable DIOs (DIO5/6/7/8/10 per UM §4.2.1) swept in every meaningful combination. Self-echo RSSI invariant within ~7 dB across all iterations. Zero OTA `RX_DONE`. Seeed's published KiCad library separately confirms DIO5/6/7 are routed as `MCU_DIO5/6/7` host-expansion test pads, **not** used as switch outputs on this module.
  > - **Phase B — Chip-init DOE (4 effective runs).** Tested every UM v2.2-prescribed firmware remedy individually and combined: `SetRssiCalibration` with UM Table 7-21 "600 MHz – 2 GHz" tunes (Run 2), `CalibImage(902, 928)` after `SetTcxoMode` (Run 3), and the kitchen-sink stack with pre-`Standby(STBY_RC)` + RSSI cal + image cal + `SetRxBoostedGainMode(true)` (Run 5). Every command returned `state=0`. **None resolved the RX failure.** Two new pieces of hard evidence emerged: (a) the chip's `GetErrors()` register reads `0x0020 = HF_XOSC_START_ERR` persistent at every POR on every unit, and (b) one `RADIOLIB_ERR_CRC_MISMATCH` event in Run 5 — meaning the RX chain is partially functional but with sensitivity degraded by an estimated 40–50 dB versus LR1121 datasheet spec. Two independently-sourced units behave identically.
  >
  > **All UM v2.2-prescribed firmware remedies have been tested. None resolves the failure.** Remaining hypothesis space: hardware-design issue (matching network, switch insertion loss, LNA isolation) or LR1121 chip firmware errata at base FW 1.3. The full bug report ([`SEEED_SUPPORT_INQUIRY.md`](https://github.com/GrayHatGuy/Xiao-esp32s3-lora-repeater/blob/lr1121-phase1/SEEED_SUPPORT_INQUIRY.md)) with the DOE evidence appended, the design-feedback companion ([`SEEED_RECOMMENDATIONS.md`](https://github.com/GrayHatGuy/Xiao-esp32s3-lora-repeater/blob/lr1121-phase1/SEEED_RECOMMENDATIONS.md)), the full DOE bench plan + results ([`LR1121-RX-INIT-AUDIT.md`](https://github.com/GrayHatGuy/Xiao-esp32s3-lora-repeater/blob/lr1121-phase1/LR1121-RX-INIT-AUDIT.md)), and the locked-in email body ([`SEEED_EMAIL_DRAFT.md`](https://github.com/GrayHatGuy/Xiao-esp32s3-lora-repeater/blob/lr1121-phase1/SEEED_EMAIL_DRAFT.md)) all live on the [`lr1121-phase1`](https://github.com/GrayHatGuy/Xiao-esp32s3-lora-repeater/tree/lr1121-phase1) branch and at the snapshot tag [`lr1121-bringup-2026-05-26`](https://github.com/GrayHatGuy/Xiao-esp32s3-lora-repeater/tree/lr1121-bringup-2026-05-26) for Seeed-correspondence stability. **`main` continues to ship the Phase 0 dual-SX1262 release (now v8.3.1, with v8.4 in progress on `dev-ABP-lorawan`).** Phase 1 will resume on Seeed engineering's reply, or via pivot to an alternate LR1121 carrier (Semtech LR1121DVK1, Ebyte E80-900M22S, or custom).

  **Minimum viable hardware change:** keep the existing Xiao Wio-SX1262 on one of the two slots, and swap *only the other slot* for a radio that can reach 2.4 GHz. The bridge needs exactly one 2.4-GHz-capable side; the SX1262 stays as the sub-GHz endpoint.

  **Prototype target:** the **Seeed Wio LR1121 breadboard** — already on hand, RadioLib-supported, multi-band on one die (sub-GHz **+ 2.4 GHz + S-band 1.9-2.1 GHz**), plus LR-FHSS. For this bridge it sits on the 2.4 GHz slot opposite the existing Xiao Wio SX1262; longer-term a single LR1121 could replace *both* SX1262s if collapsing to one radio family becomes interesting.

  Other Semtech parts to track for later, in roughly increasing capability:
  - **SX1280** — 2.4 GHz only (~2400-2500 MHz). Cheapest and narrowest; useful if a smaller, lower-cost run is ever in scope.
  - **LR22xx series** (e.g. LR2021) — newer multi-band parts: SX1280-class 2.4 GHz, improved sensitivity, BLE-coexistence awareness, expanded LR-FHSS modes. Worth watching as RadioLib support matures.

  **MCU upgrade is conditional, not required.** If a Xiao-compatible 2.4 GHz module turns up (or can be hand-wired onto the edge pins next to the existing B2B Wio shield) the Xiao ESP32S3 Sense stays in service and the form factor barely changes. If the only available SX1280 / LR1121 carriers want full 0.1"-header access, the natural upgrade target is the **ESP32-S3 DevKitC-1 N-R** (e.g. N8R8 — standard ESP32-S3-WROOM-1 dev board with N flash + R PSRAM): ~40 broken-out GPIOs, more flash + PSRAM, and the ability to wire arbitrary off-the-shelf breakouts. `WioSX1262.{h,cpp}` already abstracts pin assignments, SPI bus sharing, and the mutex — switching MCU boards would mean a new `pinout.h` (or per-board `#ifdef` block) and not much else.

  Good news on the firmware side: the protocol decoders and `bridgePacket()` dispatcher in this repo are RF-agnostic — they branch on the LoRa sync word, not the carrier frequency. Once a `WioSX1280` / `WioLR1121` / `WioLR2021` wrapper lands alongside `WioSX1262` (same `LoraConfig` struct, same `available()` / `read()` / `transmit()` surface), the existing bridge pipeline drops straight in with RF parameter changes in `platformio.ini`.

### Reticulum / RNode routing

Prioritized **ahead of** the Sub-GHz↔2.4 GHz cross-band phase above, but it stays
a firmware concern — **no RNS controls will be added to the captive portal.** The
`0x42` sync word is wired into the dispatcher as a third protocol; as of v8.3 the
receive half **and** transparent RNS↔RNS raw repeat are implemented (current state in [Routing & protocol
support](#routing--protocol-support-current-functionality)):

| Direction | Status |
|-----------|--------|
| `RX:RNS → TX:RNS` (both radios Reticulum) | ✅ **v8.3** — transparent byte-for-byte raw repeat (`BRIDGE_RNS_INPROTO_REPEAT`, on by default). |
| `RX:RNS → TX:MT or MC` | ✅ base64 text tunnel — raw bytes re-transmitted as `[rns <seq> <x>/<y>] <base64>` fragments (CRC-16 seq id, 8-fragment cap, airtime-throttle-paced). |
| `RX:MT or MC → TX:RNS` | ❌ log-and-drop — no RNS encoder yet (`encodeReticulum()` returns false). |
| `RX:RNS → human-readable decode` | ❌ opaque base64 only — RNS frame framing isn't parsed. |
| `MT/MC fragment reassembly → RNS TX` | ❌ `reassembleReticulumFragment()` stub present, no logic. |

**To make it bidirectional** (the work behind the three ❌ rows): an **RNS packet
decoder** (parse header byte / IFAC flag / hops / address hashes / context /
ciphertext → structured line in `MeshDecoderDebug.h`), an **RNS packet encoder**
(build a valid frame in `MeshEncoderDebug.h` — a one-line dispatcher wire-in
once it exists), and **fragment reassembly** (fill in `reassembleReticulumFragment()`:
accumulate `[rns <seq> <x>/<y>]` slots keyed on `<seq>`, base64-decode, ~30 s
timeout, emit when complete). Optional **IFAC** HMAC-SHA256 trailer/salt handling
comes along only if talking to an authentication-enabled network.
