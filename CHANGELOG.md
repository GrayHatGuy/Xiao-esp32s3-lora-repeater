# Changelog

## v9.0 — 2026-05-25 — Phase 1: Wio-LR1121 cross-band hardware bring-up

The LR1121 (Seeed Wio-LR1121, SKU 113991415) joins the project as Radio 2
in the **MIXED** build profile — the headline next step toward sub-GHz ↔
2.4 GHz cross-band bridging. Specced in [`LR1121-SPEC.md`](LR1121-SPEC.md).

This release lands all the firmware infrastructure for the LR1121 path
plus the verified hardware bring-up on real silicon. **MT reception is
hardware-verified end-to-end on the LR1121**; full MeshCore reception
is the next bench item.

### What's new

- **`LoraRadio` abstract interface** — `begin/available/read/transmit/`
  `startReceive` virtual base. `WioSX1262` and `WioLR1121` both implement
  it; the bridge pipeline holds radios as `LoraRadio *`. SX1262 path is
  hardware-verified unchanged (Phase 0 / v8.1 regression OK).
- **`WioLR1121` wrapper** — `LoraRadio` implementation on RadioLib's
  `LR1121` class. Manual NRESET pulse + extended BUSY-poll with 1 s
  timeout, instrumented with serial logging — empirically the Wio-LR1121
  boot ROM holds BUSY high for ~141 ms after reset (LA-confirmed),
  longer than RadioLib's internal wait. Own IRAM ISR trampolines for
  DIO9 packet-received interrupts.
- **`RADIO_PROFILE` build flag** — three mutually-exclusive profiles
  enforced by `static_assert`:
  - **`RADIO_PROFILE_MIXED`** *(default for v9.0)* — Radio 1 fixed
    SX1262; Radio 2 portal-selectable SX1262/LR1121; both drivers
    linked for hot-swap.
  - **`RADIO_PROFILE_DUAL_SX1262`** — both compile-time SX1262, only
    that driver linked (Phase 0 size profile preserved).
  - **`RADIO_PROFILE_DUAL_LR1121`** — both compile-time LR1121 (Phase 2
    target, code path verified compile-clean).
- **`BridgeConfig` schema v4 → v5** — per-radio `chip` byte (reuses a v4
  pad byte, so layouts are byte-identical). v4 NVS blobs migrate in
  place; `0x00` pad reads as `CHIP_SX1262`, correct for any v4 device.
- **Portal "Radio 2 chip" picker** — MIXED profile only, with hint about
  the 2.4 GHz frequency band. DUAL_* profiles hide the control entirely.
- **Chip-aware frequency validation** — `handleSave()` accepts
  150–960 MHz for SX1262, plus **2400–2500 MHz for LR1121 radios**.
  Region-exempt 13 dBm TX-power ceiling applied on the 2.4 GHz band.
- **`RegionPreset.h` 2.4 GHz extensions** — `modemPresetParams()` gained
  a `wideLora` arg for the Meshtastic 2.4 GHz BWs (812.5 / 406.25 /
  1625 kHz); new `slotFrequency2G4()` + `bandCenter2G4()` helpers.
- **MAC-derived defaults** for node ID, long name, portal SSID — every
  vanilla device uniquely addressable from first boot.
- **RadioLib 6.6.0 → 7.0.0** — the LR11x0 driver in 6.6.0 had a
  chip-detection bug that timed out `GET_VERSION` on the Wio-LR1121; 7.x
  resolves it. The LR11x0 `begin()` API changed in 7.0.0 (added `freq`
  and `power` to the signature, removed the `bool high` flag); the
  `WioLR1121` wrapper is built against the 7.x signature. The SX126x
  API in 7.0.0 is unchanged — Radio 1 (Wio SX1262) works without
  modification.

### Hardware verification

Bench bring-up sequence completed methodically:

- ✅ Chip detection — `Found LR11x0: 0x03 Base FW version: 1.3`
- ✅ Powered cleanly (3.27 V steady at VDD_RF, no brown-out)
- ✅ End-to-end DMM continuity on every signal pad → Xiao GPIO
- ✅ Initial bench-debug ruled out: TCXO voltage (1.6 / 3.0 / 3.3 V), Xiao
  damage from earlier short, breadboard contact, wiring topology
- ✅ Logic analyzer (Hiletgo / PulseView) SPI capture confirmed bus health
- ✅ **LR1121 RX verified at MT LongFast (sync `0x2B`, BW250, SF11)** —
  bridge's own NodeInfo received at -44 dBm / SNR 9.8 dB

### Known limitation (carried into v9.x)

- MeshCore reception on the LR1121 at MC's standard RF profile
  (910.525 MHz / BW62.5 / SF7 / sync `0x12`) is **not yet verified**
  despite confirmed MT reception working. SX1262 ↔ MC bridging
  unaffected. Next bench session: head-to-head SX1262/LR1121 comparison
  on identical MC RF to isolate whether this is an LR11x0 7.0.0 driver
  issue or a downstream decoder/packet-params gap. Tracked under task #33.

### Migration notes

- **v8.1 → v9.0 NVS**: schema v4 blobs auto-upgrade to v5 on first boot.
  No user action required. Radio 2 chip defaults to SX1262 (preserving
  Phase 0 behaviour); portal "Radio 2 chip" select changes it to LR1121.
- **Build dependency**: `platformio.ini` now pins `RadioLib @ 7.0.0`.
  A `pio pkg uninstall --library "jgromes/RadioLib"` followed by
  `pio pkg install` may be needed if you have a cached 6.6.0.

## v8.1 — 2026-05-21 — Compile-time build-flag validation

A small follow-up to v8.0 — build-time safety for source builds, plus docs.

- **`LoraConfigCheck.h`** — `static_assert` guards that reject an invalid
  `LORA_RADIO*` build-flag default at compile time, so a bad flag fails
  `pio run` with a clear message instead of shipping misbehaving firmware.
  Checks: frequency 150–960 MHz, bandwidth a valid SX1262 value, SF 5–12,
  CR 5–8, TX power −9…22 dBm. Sync word is byte-range only (`0x00–0xFF`),
  not a whitelist, since the v8 Custom protocol path allows any sync word.
  Each guard is `#ifdef`-wrapped, so an unset flag (the vanilla-`.bin` case)
  is skipped. This complements v8.0's portal-side runtime validation —
  build-time checks the compile-time defaults, the portal checks
  portal-entered values.
- **README** — new Wiring section (signal/GPIO table for the stacked-shield
  Xiao ↔ dual SX1262 connections).

No firmware behaviour change for a valid configuration.

## v8.0 — 2026-05-21 — Vanilla firmware: full portal config

The bridge is now configurable **entirely through the WiFi captive portal** —
region, per-radio protocol, RF plan, channels and identity. A single `.bin`
built with no `LORA_RADIO*` build flags first-boots straight into the portal,
so a non-developer can flash and configure without PlatformIO. Build flags, if
present, become first-boot defaults only.

- **`BridgeConfig` schema v3 → v4.** New global `region` and a per-radio
  `RadioRf` struct (`protocol`, `frequency`, `bandwidth`, `sf`, `cr`,
  `syncWord`, `txPower`). `begin()` migrates a v2 or v3 blob forward — region
  and RF fall back to build-flag defaults so an upgraded device keeps running.
- **Runtime RF.** `setup()` builds each radio's `LoraConfig` from
  `BridgeConfig` at boot (`makeLoraConfig()`); the compile-time
  `LORA_RADIO*_*` RF macros and the `WioSX1262.h` `915.0f / SF9 / 0x12`
  fallback chain are gone. Preamble length (8) and TCXO voltage (1.8 V) stay
  compile-time board facts.
- **Per-radio protocol picker** in the portal: Meshtastic / MeshCore /
  Reticulum / Custom / **None**. A None radio is disabled (single-radio
  monitor mode — a debug aid, or a parked slot for a future 2.4 GHz radio).
- **Region support** — global selector for US, EU_868, EU_433, ANZ, CN, JP,
  IN, KR, RU (+ Custom/Other). New `RegionPreset.h` carries the region table,
  the Meshtastic modem-preset BW/SF/CR bundles, and the **Tier 2 channel-slot
  frequency computation** (`djb2` hash + slot formula, transcribed verbatim
  from `meshtastic/firmware` and verified: US + LongFast → 906.875 MHz). The
  computed frequency pre-fills an **editable** field; a manual override is
  flagged inline (`computed: … — overridden`).
- **MAC-derived identity.** The default Meshtastic node ID and portal SSID are
  derived from the ESP32 MAC, so every vanilla device is unique out of the box.
- **Custom RF** path exposes the full plan (frequency, BW, SF, CR, sync word,
  TX power) behind an explicit warning banner. `handleSave()` clamps frequency
  to the SX1262 range and sanity-checks SF/CR/BW/sync; TX power is capped to
  the region's regulatory limit.

This reverses the previous "protocol/RF is config-time only" decision for the
vanilla-bin use case — the MT/MC/RNS presets are vetted; only the Custom path
is dangerous, and it is behind a warning. New file: `RegionPreset.h`.

## v7.0 — 2026-05-20 — Per-radio channels, same-protocol relay

The bridge is no longer limited to cross-protocol MT↔MC. Each radio slot now
carries its own channel, so it can also relay **same-protocol between two
channels** — MC↔MC or MT↔MT (private↔public, private↔private).

- New `RadioChannel` struct (`protocol`, `key`, `keyLen`, `channelHash`,
  `name`) — two of them, one per radio slot, resolved at boot. Replaces the
  `MeshCoreConfig` / `MeshtasticConfig` singletons; those modules are now
  stateless `resolve()` helpers.
- Every decoder and encoder takes a `const RadioChannel&`; `bridgePacket()`
  decodes with the RX radio's channel and encodes with the TX radio's.
- `BridgeConfig` schema v2→v3: the protocol-specific channel fields become
  per-radio (`radio1/2ChannelName` + `radio1/2ChannelKey`). A v2 NVS blob is
  migrated forward — its MT/MC channels map onto whichever radio runs that
  protocol.
- Captive portal: the two protocol-specific channel sections become a
  *Radio 1 channel* / *Radio 2 channel* pair, each labelled by that radio's
  build-flag protocol. The form rejects a config where both radios run the
  same protocol with identical channels (a channel relayed to itself is a
  feedback loop). No RNS controls in the portal.

Radio protocol/RF stays a build-flag (`LORA_RADIO*`) decision — config-time
only, never portal-editable.

## v6.2.1 — 2026-05-20 — Build reproducibility + GPIO init fix

- **Pinned the PlatformIO platform.** `platformio.ini` now requires `espressif32 @ 6.13.0` (arduino-esp32 2.0.17) — the core the bridge is verified against. An unpinned `platform = espressif32` let a fresh checkout pull a newer core whose GPIO/SPI behaviour differs, producing spurious diagnostics on other people's builds.
- **Fixed a GPIO init bug.** The `busyWait` pre-flight diagnostic in `setup()` read the SX1262 BUSY pins via `digitalRead()` without first calling `pinMode(..., INPUT)`. Newer arduino-esp32 cores logged `[E] __digitalRead(): IO N is not set as GPIO` for each read. Harmless to radio detection, but now correct.

Neither change alters bridge behaviour — this is a build-reproducibility and diagnostic-cleanliness patch.

## v6.2 — 2026-05-20 — Remove deprecated raw RX hex dump

The raw incoming-packet hex dump (`[R1 RX] data: FF FF FF FF …`) is removed from both radio tasks. It was an early-development debugging aid — pure `Serial.printf` with no functional role — and is now deprecated. The bridge's decode → re-encode → transmit path is unaffected; the `extract*()` decoders are untouched.

Per-packet serial output keeps the RX summary line (size / RSSI / SNR), the `[R1 decoded]` protocol summary, and the bridge re-encode / TX lines.

## v6.1 — 2026-05-19 — F5: Meshtastic private-channel support

The Meshtastic side is no longer pinned to the LongFast public channel. New `MeshtasticConfig.{h,cpp}` mirrors `MeshCoreConfig`: it reads a base64 PSK + channel name from `BridgeConfig`, runs Meshtastic's key expansion, and derives the on-air channel-hash byte.

- **PSK expansion.** The base64 PSK decodes to 0 bytes (LongFast default), 1 byte (short-key index — expanded against `defaultpsk` with the last byte bumped by `index-1`), 16 bytes (AES-128) or 32 bytes (AES-256).
- **AES-256.** The five MT decoders and two MT encoders now take the key length from `MeshtasticConfig::keyLen` (128/256) instead of a hard-coded 128, so 32-byte PSK channels decrypt correctly.
- **Channel hash.** Computed as `XOR-fold(name) ^ XOR-fold(expanded key)` and used as the decoder gate, replacing the literal `0x08`.

`BridgeConfig` schema bumped v1 → v2 with two new fields (`mtChannelName`, `mtPskBase64`); a v1 NVS blob from a v6.0 build is migrated forward automatically (existing settings preserved, MT channel defaults to LongFast). The captive portal gains a "Meshtastic channel" section with name + PSK inputs, and new `BRIDGE_MT_CHANNEL_NAME` / `BRIDGE_MT_PSK_B64` build-flag defaults.

Also: the captive-portal recovery trigger now accepts **any serial-monitor character** during the post-boot window, not just the BOOT button — the button is unreachable when the radio shield is mounted over it. The window was widened 3 s → 5 s.

## v6.0 — 2026-05-19 — Captive-portal config, POSITION/TELEMETRY bridging, configurable MC channel

This release makes the bridge field-configurable without a rebuild and widens the Meshtastic traffic it understands. It bundles features F2, F3 and F4.

**WiFi captive portal (F4).** On a fresh flash — or any time the **BOOT** button is pressed within ~3 s of reset — the bridge comes up as an open WiFi AP named `LoRa-Bridge-<XX>` (last byte of the MT node ID in hex) and DNS-redirects all HTTP traffic to a single-page config form. The form exposes the eight settings users actually re-tune: Meshtastic identity (node ID numeric + `!hex` string, long name, short name), MeshCore channel (32-char hex key + display name), and the POSITION/TELEMETRY bridge toggles. Saving writes a schema-v1 blob to NVS and reboots into bridge mode. New `BridgeConfig.{h,cpp}` is the single source of truth; the `BRIDGE_MT_*` / `BRIDGE_MC_*` build flags become the defaults a fresh device starts from, so existing `platformio.ini` configs behave identically until the portal saves something new.

**POSITION + TELEMETRY bridging (F2).** `POSITION_APP` and `TELEMETRY_APP` are decoded from incoming MT packets and re-emitted to MeshCore as compact text under the existing `[MT !<hexid> <SHORT>]` marker — `pos 40.7234,-74.0123 alt 12m`, `bat 87% 4.05V`, `env 22.5C RH 45% 1013hPa`. Each portnum is individually toggleable from the portal or via build flag.

**Configurable MeshCore channel (F3).** The hard-coded public-channel key moved into a new `MeshCoreConfig` module; the bridge can now point at any MC channel via config, with the on-air channel hash auto-derived from `SHA-256(key)[0]`.

Per-radio LoRa parameters (frequency, BW, SF, CR, TX power, sync word) remain build-flag-only — out-of-band misconfiguration is too easy to expose in a v1 form.

### Changes since v5.0

`v5.0..v6.0` — commit `04c0c5a`. 12 files changed, +1122 / -62.

```
 CHANGELOG.md           |  14 ++         changelog
 README.md              |  35 ++         instructions + roadmap (F5, portal v2, 2.4 GHz)
 platformio.ini         |  13 ++         BRIDGE_MC_* / BRIDGE_MT_* flag examples
 src/BridgeConfig.h     |  64 ++         new — NVS-backed config interface
 src/BridgeConfig.cpp   | 178 ++         new — schema-v1 blob load/save/defaults
 src/CaptivePortal.h    |  29 ++         new — portal interface
 src/CaptivePortal.cpp  | 276 ++         new — WiFi AP + DNS + HTTP config form
 src/MeshCoreConfig.h   |  33 ++         new — MC channel module interface
 src/MeshCoreConfig.cpp |  68 ++         new — key/hash from BridgeConfig
 src/MeshDecoderDebug.h | 312 ++         POSITION/TELEMETRY decoders, MC config refs
 src/MeshEncoderDebug.h |  37 ++         encoders take srcNodeId param
 src/main.cpp           | 125 ++         portal trigger, runtime portnum gates, wiring
```

New files: `BridgeConfig.{h,cpp}`, `CaptivePortal.{h,cpp}`, `MeshCoreConfig.{h,cpp}`.

## v5.0 — 2026-05-19 — Persistent NodeDB & MT sender attribution

This release adds end-to-end sender attribution for MT→MC bridged text, plus reliability fixes shaken out during integration testing.

**Bridge attribution.** MT→MC text now carries the sender's canonical Meshtastic `!<8 lowercase hex>` ID in the bridge marker — `[MT !3d3a87a3] body` from any new MT node, upgrading to `[MT !3d3a87a3 KN5J] body` once the bridge has heard a `NODEINFO_APP` packet from that node.

**NodeDB.** New `NodeDB.{h,cpp}` module: 64-entry, NVS-persistent `(nodeId → short_name, long_name)` table populated by a new `extractMeshtasticNodeInfo()` decoder in `MeshDecoderDebug.h`. LRU eviction when full. Attribution survives reboot.

**Reliability fixes:**

- Radio-task stacks raised 4 KB → 8 KB; `saveToNvs()` scratch buffer (~3.3 KB) moved off-stack into BSS. Fixes hard-reset on every received NodeInfo.
- Bridge drops upserts of its own NodeInfo bouncing back via relays.
- NodeDB opens NVS namespace in RW mode on first boot to silence the cold-boot `nvs_open failed: NOT_FOUND` log.

Received MT NodeInfo packets feed the DB and are not bridged as text (would just be noise on the destination mesh).

## 2026-05-18 — RNS fragmentation

RNS bridging now uses base64 + auto-fragmentation across MT/MC packets with `[rns <seq> <x>/<y>]` markers, CRC-16 sequence IDs, per-protocol fragment pacing, and 8-fragment cap.
