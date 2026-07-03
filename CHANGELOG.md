# Changelog

## Unreleased — LoRaCam (branch `lora_cam_xiao`, NOT merged to main)

**Status: Phase 1 + Phase 2a proven on silicon 2026-06-28; Phase 3 (always-on web portal) BENCH-PROVEN on
silicon 2026-07-03 — all gating tests pass (`BENCH-PHASE3.md`; branch `lora_cam_xiao`, NOT merged —
owner-gated).** A LoRa-commanded camera built on the v9.0 bridge: a XIAO ESP32-S3 **Sense** (OV2640
+ microSD on the rear B2B 40-pin) + a perimeter-pin **Wio-SX1262** edge radio. Commands ride an encrypted,
sender-whitelisted, replay-protected binary frame on a `PROTO_CUSTOM` radio (sync `0x33`); design of record
[`LORACAM-SPEC.md`](LORACAM-SPEC.md), bench guide [`BENCH-CAMC2.md`](BENCH-CAMC2.md). **Stock repeater builds
stay byte-identical** (`xiao_esp32s3` = 865781 B) — all new code is `#if defined(BRIDGE_ROLE_CAMERA) ||
defined(BRIDGE_CAM_COMMANDER)`.

- **Phase 1 — C2 security core.** New `src/CamC2.{h,cpp}` (frame
  `[ver0xC2][type][senderId:4][recipientId:4][seq:4][ciphertext][cmac:8]`, AES-CTR **encrypt-then-MAC** with an
  8-byte truncated AES-CMAC reusing `LoRaWANCrypto`, keys domain-separated from the per-peer PSK, constant-time
  tag compare, fail-closed boot self-test) + `src/CamC2Config.{h,cpp}` (a `c2auth` NVS per-peer whitelist +
  persist-on-accept fail-closed anti-replay + a reboot-safe block-reserved TX sequence). One guarded hook in
  `ingestAndFanout()` (PROTO_CUSTOM) + `CamC2::begin()` in setup. Offline tool `tools/cam-c2.py`
  (gen/verify/selftest). **Adversarially reviewed sound** (6-lens crypto review). **End-to-end proven:** a signed
  command crosses real LoRa, the cam authenticates + executes it, and ACKs back.
- **Phase 2a — camera capture.** New `src/CameraNode.{h,cpp}` (OV2640 init for the XIAO S3 Sense via
  `esp_camera`) wired into `executeCommand`: a `snap` command captures a real JPEG (proven: 800×600, ~14 KB) and
  the ACK reports it. The example's LED-flash pin (GPIO21 = microSD CS) is deliberately not driven.
- **Phase 3 — always-on web portal (BENCH-PROVEN on silicon 2026-07-03, `BENCH-PHASE3.md`).** New `src/CamPortal.{h,cpp}`
  (a WPA2 SoftAP + a login-gated synchronous `WebServer` on :80) and `src/CamStream.{h,cpp}` (an
  `esp_http_server` on :81 dedicated to the MJPEG stream — its own translation unit because `esp_http_server.h`
  and `WebServer.h` both define `HTTP_GET`). The portal serves: **live video** (MJPEG `<img>` + `/jpg` snapshot),
  **camera control** (snap / record / stop / status via direct `executeCommand`, local-trust — gated by the
  portal login, not the LoRa crypto latch), **config** (reuses the existing captive form verbatim via new
  camera-gated `CaptivePortal::serveConfigForm()/serveConfigSave()` entry points — one source of truth; a save
  reboots), **messaging** (a new signed/encrypted `T_MSG` C2 frame type + an inbound ring; chat with whitelisted
  masters), and **pairing** (manage the `c2auth` peer whitelist) + a **Security** page (change the WPA2 passphrase
  + portal login). New `src/CamPortalConfig.{h,cpp}` stores the AP passphrase + a salted-SHA-256 login in its own
  `camportal` NVS namespace (no `BridgeConfig` schema bump). Cookie-based sessions; the :81 stream is gated by a
  `?sid=` token. `CameraNode` gained a camera mutex so a stream frame and a C2 `snap` can't race on `esp_camera`.
  Pumped from a dedicated FreeRTOS task. Cam env 978 KB (27.9%→29.3%); **stock still 865781 B (do-no-harm)**.
- **Build envs.** `xiao_loracam` (product), `bench_camc2` (cam) + `bench_camc2_cmdr` (commander) for the
  two-board bench; bench provisioning via `BRIDGE_C2_MY_ID` / `BRIDGE_C2_PEER_ID/_KEY/_PRIMARY` and the portal
  defaults `BRIDGE_PORTAL_USER/_PASS/_AP_PASS`. A do-no-harm `LORA_RADIO{1,2}_DISABLE` build-flag seam disables a
  radio slot (the camera build disables R1, whose B2B pins the camera occupies).
- **Phase-3 hardening (2026-07-03, from the bench-plan red-team + the bench itself).** ① `BridgeConfig` now
  re-applies `LORA_RADIO{1,2}_DISABLE` after every NVS blob load/migration and before every save
  (`applyBuildDisables()`) — a saved config can no longer re-enable R1 onto the camera's B2B pins (proven on
  silicon: saved R1=Meshtastic persisted as `proto=0`). ② The commander gained a `T_MSG` hook (`m` serial key +
  inbound-message print) — messaging proven both directions on air. ③ The :81 stream retries transient null
  frames instead of ending (a frozen `<img>` needed a manual refresh). ④ The portal reloads the stream on
  `visibilitychange` (Android kills the MJPEG connection on screen sleep with no error event). Stock build
  verified byte-identical (865781 B) after all four.
- **Not yet done:** microSD persistence (Phase 2b — save the JPEG + return the real filename over the shared SPI
  bus + `spiMutex`), video record (the `record` command is still a stub), securing first-flash provisioning on a
  product build (today an unconfigured `xiao_loracam` provisions over the OLD open captive portal before the WPA2
  portal exists), and the skipped/deferred bench items (AP-passphrase change, Wave-5 soaks/specials).

## v9.0 — four-radio bridge (2xiao_4sx1262)

**Status: SHIPPED 2026-06-20 (tag `v9.0`).** Two Xiao boards can now be linked over a UART
crossover into one **four-radio** sub-GHz bridge — a *master* (R1/R2 local) plus a *radio
co-processor* (R3/R4) — driven by a per-radio routing matrix. **Fully backwards compatible:**
a single Xiao still runs as the original two-radio Meshtastic ↔ MeshCore bridge with no
changes (Radio 3 / Radio 4 default to disabled, no co-processor, no UART link opened), and
existing 2-radio configs migrate unchanged (`BridgeConfig` schema **v4 → v5**). Builds green:
`xiao_esp32s3` / `xiao_esp32s3_v1_1` (master) + `xiao_coproc_sx1262` / `xiao_coproc_sx1262_v1_1`
(co-processor). Bench: the gating set (0.1–0.3 / A1 / A2 / B1) PASSED on the 2-board rig —
see [`BENCH-v9.0.md`](BENCH-v9.0.md).

- **Four radios over a UART crossover.** A second Xiao runs a lightweight **radio
  co-processor** firmware ([`coproc-xiao-sx1262/`](coproc-xiao-sx1262/)) driving its two SX1262
  as Radio 3 / Radio 4; the master Xiao owns routing, dedup, the captive portal, and the config
  for all four radios and drives R3/R4 over the link. Crossover wiring: master `D6` (GPIO43, TX)
  ↔ co-proc `D7` (GPIO44, RX), master `D7` ↔ co-proc `D6`, GND ↔ GND, 460800 baud — the cable
  does the crossover.
- **Per-radio routing matrix.** A portal "bridge received traffic to" grid picks, per radio,
  which of the others it relays to (`R1→R2/R3/R4`, etc.) — a full four-way mesh, independent
  pairs, or one-way feeds rather than a fixed all-to-all. `BridgeConfig` schema **v4 → v5**
  (4-radio table + per-radio `routeMask`); a clean v2/v3/v4 → v5 migration preserves R1/R2 + the
  R1↔R2 crossover and defaults R3/R4 off.
- **The master self-heals the link.** It pushes R3/R4 config when it sees the co-processor come
  up and re-pushes automatically on a co-processor reboot; if R3/R4 are *enabled* but no
  co-processor is attached, remote transmits fail fast instead of stalling — a single-board
  build is never slowed.
- **Console logging serialized.** All task-context serial output now goes through one shared
  lock, so the two radio tasks + the link task no longer interleave (garble) mid-line on the USB
  console; the USB TX buffer was enlarged to absorb bursts.
- **Co-processor V1.0 / V1.1 variants.** The co-processor ships in both Radio-2 edge-module
  revisions (`xiao_coproc_sx1262` / `_v1_1`), matching the master's V1.0/V1.1 split.
- **Docs.** README gains the four-radio wiring / parts / build / routing content woven into the
  existing sections; [`CONFIG-USER-MANUAL.md`](CONFIG-USER-MANUAL.md) gains §4.8 (Radio 3/4) +
  §4.9 (routing matrix) + a four-radio example; `platformio.ini` gains a commented "Scenario D"
  four-radio compile-time preset plus bench-config flags (`LORA_RADIO{3,4}_ENABLE`,
  `LORA_RADIO{1..4}_ROUTE_MASK`).

## v8.4.1 — captive-portal UI cleanup + user manual + ChirpStack tooling (UI_UM_config)

**Status: SHIPPED 2026-06-18 (tag `v8.4.1-UI_UM_config`).** A UI / docs / tooling
release — **no protocol, routing, or on-air behaviour changes**. `BridgeConfig` schema
**v4 unchanged** (the new per-radio LoRaWAN region reuses a spare `RadioRf` byte — no NVS
migration); V1.0 and V1.1 builds behave as on v8.4. Builds green: `xiao_esp32s3` /
`xiao_esp32s3_v1_1` / `xiao_esp32s3_lwabp`.

- **Captive-portal look-and-feel cleanup.** Bridge-behaviour toggles moved into the top
  identity frame (with a "Meshtastic only" note); the Meshtastic Channel key shows the
  `AQ==` LongFast default; Meshtastic BW/SF/CR are shown read-only and auto-filled from the
  Modem preset; the MeshCore helper notes the public key starts `8b…`; the Channel name is
  locked to `N/A` for LoRaWAN and defaults to `N/A` (editable) for Custom; a hint warns that
  a custom Meshtastic PSK needs a matching private channel name.
- **LoRaWAN region / channel-slot picker.** A per-radio LoRaWAN **region** selector (US915 /
  AU915 / AS923 / EU868) + a **channel slot** dropdown auto-fills Frequency / SF / Bandwidth
  (CR fixed at 4/5) from RP002-1.0.3 channel plans. The region is **persisted** (reuses a
  spare `RadioRf` byte — no schema bump).
- **Per-device identity from the MAC.** A fresh board now derives its long name
  (`<NodeID> LoRa Bridge`) and short name (`BR<low-byte>`) from the MAC-derived node ID
  instead of a shared build-flag default. The now-residual `BRIDGE_MT_NODE_ID` / `_STR` /
  `_LONG_NAME` / `_SHORT_NAME` flags were removed (they were always overridden by the MAC
  derivation).
- **Smarter protocol-switch autofill.** Choosing a Modem preset updates the Meshtastic
  Channel name (until you enter a custom key, which unlocks the name — same idea on
  MeshCore); selecting **Reticulum** auto-fills the RNode defaults (914.875 MHz / 125 kHz /
  SF8 / CR5); Reticulum BW/SF/CR are now user-editable.
- **New user manual — [`CONFIG-USER-MANUAL.md`](CONFIG-USER-MANUAL.md)** (linked from the
  README): a field-by-field walkthrough of every portal screen with its build flag, a
  detailed LoRaWAN ABP section (Applies-to-source ladder + source tag), three example setups
  (MT↔MC, MT→LoRaWAN, MT public→private), and the full build-flag reference.
- **ChirpStack tooling.** Importable device-profile templates under
  [`tools/chirpstack/`](tools/chirpstack/) (US915 / AU915 / AS923 / EU868 ABP profiles +
  vendor/device manifests + codec test vectors) and a hardened
  [`tools/chirpstack-codec.js`](tools/chirpstack-codec.js) decoder (real UTF-8, `warnings`/
  `errors`, device-variable-driven source-tag handling).
- **Compile-time preloading.** New per-radio channel build flags
  (`LORA_RADIO{1,2}_CHANNEL_NAME` / `_KEY`) + three commented "scenario" blocks in
  `platformio.ini` you can uncomment to preload the wifi config (MT↔MC, MT→LoRaWAN, MT
  public→private).
- **README dedup.** Restructured + trimmed (8→5 install steps); the per-protocol table
  merged into "Routing & behavior"; the full build-flag catalog migrated to the manual; the
  header + silkscreen images resized.

## v8.4 — LoRaWAN ABP uplink encoder (keyed)

**Status: SHIPPED 2026-06-16 (tag `v8.4-ABP-lorawan`) — build-green, crypto-verified,
and hardware-verified on air.** Meshtastic *and* MeshCore frames encode to valid ABP
uplinks that decrypt back with a valid MIC; **12/16 bench tests pass**, and the only
remaining item is live-ChirpStack (Tier C) ingestion (`BENCH-RESULTS.md`). Design:
`ABP-LORAWAN-SPEC.md`.

- **Keyed LoRaWAN ABP *uplink* encoder.** The v8.3 keyless tap couldn't inject
  content into LoRaWAN (`MT/MC → LoRaWAN` was a `no-lw-encoder` drop). v8.4 adds
  an opt-in **keyed** encoder: when a `0x34` destination radio has ABP
  credentials, the bridge transcodes a decoded MT/MC body into a valid LoRaWAN
  1.0.x **ABP uplink** (DevAddr + `NwkSKey` + `AppSKey`; AES-CTR FRMPayload,
  AES-CMAC MIC, monotonic FCnt, ADR off, Unconfirmed) and re-emits it over RF
  (delivery model **B1**) for an existing gateway to forward to a ChirpStack LNS.
- **New `src/LoRaWANCrypto.h`** — a self-contained **RFC 4493 AES-CMAC** over
  mbedTLS AES (the prebuilt esp32s3 mbedTLS ships `CONFIG_MBEDTLS_CMAC_C` off, so
  `mbedtls_cipher_cmac` won't link) + `encodeUplink()` + a boot self-test
  (`BRIDGE_LW_ENC_SELFTEST`) of the RFC 4493 vectors, the FRMPayload keystream,
  and a frame round-trip. Crypto cross-checked against an independent CMAC.
- **Ships in the standard build (config-gated do-no-harm).** As of v8.4 the encoder
  is compiled into `xiao_esp32s3` (V1.0) **and** `xiao_esp32s3_v1_1` (V1.1) — no
  separate env — but stays **dormant** until you set a radio to LoRaWAN and add an
  ABP device (`BRIDGE_LW_ENCODE` default 1; set 0 to compile it out). A stock MT/MC
  bridge is behaviourally unchanged; the only delta is a boot `[LoRaWANConfig]` /
  `[lw-enc]` line and ~12 KB flash. Build-flag keys (`BRIDGE_LW_ENC_NWKSKEY` /
  `_APPSKEY` / `_DEVADDR` / `_FPORT`) remain the single-device fallback. Bench env:
  `bench_lw_enc`; `xiao_esp32s3_lwabp` now just adds the boot self-test.
- **Per-source ABP devices + portal + persisted FCnt (P2).** A dedicated
  `src/LoRaWANConfig.{h,cpp}` module holds a bounded 4-slot per-source ABP
  identity table in its OWN NVS namespace (`lwabp`) — not a BridgeConfig schema
  bump — with a captive-portal "LoRaWAN ABP devices" section and a reboot-safe
  FCnt (block reservation, one write per 32 uplinks). The encode seam resolves a
  per-source device (else the build-flag fallback). New `xiao_esp32s3_lwabp` env.
- **Universal mapping + Custom weather-station path + codec (P3).** Any decoded
  source maps to an ABP uplink; a raw-LoRa **Custom** source wraps its raw RX
  bytes as the FRMPayload (the canonical weather-station scenario). Optional
  per-device source tag (`[proto][srcId]`) for a multiplexed device.
  `tools/chirpstack-codec.js` is a sample ChirpStack v4 codec.
- **Regional timing (P4).** US915 per-TX **dwell cap** (400 ms ToA) in the TX
  scheduler; EU868 duty via `BRIDGE_TX_DUTY_PERCENT`.
- **Portal UX: auto-fill RF defaults on protocol switch.** Changing a radio's
  Protocol dropdown now fills the new protocol's defaults (MeshCore: public key +
  `910.525`/BW62.5/SF7/CR5; Meshtastic: blank LongFast PSK), fired only on an actual
  change so a value the user typed isn't clobbered. Closes the stale-field trap
  behind a 50 kHz freq slip (`910.575` vs `910.525`) that cost a bench session.
- **Bench:** hardware-verified on the COM13-DUT / COM6-sniffer rig — **12 PASS /
  4 OPEN** (`BENCH-RESULTS.md`): crypto self-test, dwell cap, stock do-no-harm, MT
  *and* MC → ABP (decrypt + MIC), reboot-safe + DevAddr-keyed FCnt, per-source
  resolve, source-protocol tag, over-cap drop. The only open items are the
  colleague's live **ChirpStack (Tier C, C1–C4)** ingestion. Plan: `BENCH-v8.4.md`.
- **Deferred (not in v8.4):** ABP/OTAA *decode* (own fleet → MT/MC), the dual-LNS
  crosslink, and the MT/MC→RNS encoder.

## v8.3.1 — Radio 2 module-revision (V1.0 / V1.1) support + bring-up hardening

**Status: implemented on `v8.3.1-r2-pin-variants`, build-green (both variants,
Flash 24.6%). V1.1 pin map (`NSS=4 DIO1=1 RST=3 BUSY=2 RF_SW=5`) bench-confirmed on
a physical V1.1 module.**

Fixes a Radio-2 detection failure — `[Radio1-B2B] begin() failed: -2`
(`CHIP_NOT_FOUND`) and a FATAL halt — that appeared on some boards but not others
with seemingly identical hardware. Root cause: Seeed shipped the **"Wio-SX1262 for
XIAO" edge module (Radio 2) with two different pinouts**. The chip-select (NSS) is
on **D4 / GPIO5** on **V1.0** but **D3 / GPIO4** on **V1.1**. The v8.x firmware only
ever shipped the V1.0 map, so on a V1.1 module the real chip-select is left
floating during Radio 1's `begin()`; the un-deselected V1.1 chip then drives the
shared MISO bus and corrupts Radio 1 detection — fatally, since Radio 1 is
mandatory.

- **Per-revision build variants.** Radio-2 pins are selected by `WIO_SX1262_REV`
  (10 = V1.0, default; 11 = V1.1):
  - `xiao_esp32s3` — V1.0 (unchanged; the default release binary).
  - `xiao_esp32s3_v1_1` — V1.1 (`NSS=4 DIO1=1 RST=3 BUSY=2 RF_SW=5`).

  Each `R2_*` pin is `#ifndef`-guarded, so a single `-DR2_NSS=..` (etc.) can also
  override one pin from `platformio.ini`. (Radio 1 is fixed B2B silicon — it stays
  a plain define and is never remapped.)
- **Bring-up hardening — a Radio-2 fault can no longer take down Radio 1.** Radio 2
  is held in hardware reset (`RST` is `GPIO3` on *both* revisions) for the whole of
  Radio 1's probe + `begin()`, then released and brought up afterward. A wrong
  variant (or any R2 fault) now leaves Radio 1 running single-radio with a message
  naming the silkscreen check and the env to flash — it no longer halts.
- **Self-identifying boot log.** Boot prints
  `[diag] R2 edge module = V1.0  (NSS=5 DIO1=2 RST=3 BUSY=4 RF_SW=6)`, so any
  serial log shows which revision the firmware was built for.
- **Docs.** `README.md` "Wiring" gains a "Radio 2 module revision" section with
  silkscreen photos and an identification table.
- **Do-no-harm note.** On a correctly-matched board the only behavioral change is
  serial-log *ordering*: the Radio-2 reset diagnostic now prints as
  `[diag] R2 release  BUSY after reset = …` **after** `[Radio1-B2B] ready` (R2 is
  reset after R1 is up). Radio operation is unchanged.

## v8.3 — LoRaWAN keyless bridge + Reticulum repeat + POSITION clock

**Status: implemented on `v8.3-dev`, build-green; awaiting owner bench — not yet
tagged or released.**

- **POSITION clock-learn.** The bridge now also learns wall-clock from a
  Meshtastic `POSITION_APP` time field (`Position.time` field 4, `.timestamp`
  field 7 as fallback), closing the cold-boot window where `MT→MC` bridged
  messages stamped 1969 until the first timestamped MeshCore packet arrived.
- **Transparent in-protocol Reticulum repeat (`RNS → RNS`).** When both radios
  run Reticulum, an inbound RNS frame is now re-transmitted byte-for-byte on the
  other radio (a transparent range-extension repeater) instead of being dropped;
  RNS Transport handles hop limits and dedup at the network layer. Gated by
  `BRIDGE_RNS_INPROTO_REPEAT` (default 1). `RNS → MT/MC` tunnelling and the
  `MT/MC → RNS` drop are unchanged.
- **LoRaWAN (sync `0x34`) — keyless.** A new `LoRaWAN` per-radio protocol in the
  captive portal:
  - **Metadata capture tap** — decodes the cleartext LoRaWAN MAC header (MType,
    DevAddr, FCtrl, FCnt, FPort, FRMPayload length; JoinEUI/DevEUI for
    join-requests) and logs `evt=RX proto=LW`. No `FRMPayload` decrypt (no keys).
  - **Metadata summary to mesh** — emits a one-line summary onto the MT/MC meshes
    (`BRIDGE_LW_SUMMARY_TO_MESH`, default on).
  - **Transparent LW↔LW relay / dedup-bounded flood "mesh"** — re-transmits the
    raw `0x34` frame to other LoRaWAN radios (`BRIDGE_LW_RELAY`), loop-bounded by
    the shared content-hash dedup. There is no in-band hop counter (mutating the
    frame would break its MIC); flood is bounded by dedup + the airtime throttle.
    Single-channel per radio (no LoRaWAN channel hopping) — a localized bridge,
    like the Tasmota model.
  - `MT/MC → LoRaWAN` is a deliberate log-and-drop (`no-lw-encoder`): keyless
    firmware cannot inject content into LoRaWAN.
  - **Deferred to v8.3+:** ABP/OTAA key-based decode of one's own devices
    (→ MT/MC) and `MT/MC → LoRaWAN` encode.

## v8.2.1 — MeshCore timestamp fix

**Patch — bench-verified 2026-06-13.** `MT→MC` (and `RNS→MC`) bridged messages
hardcoded the MeshCore `GRP_TXT` Unix timestamp to 0, so MeshCore clients showed
them as 1969-12-31. (`MC→MT` was unaffected — Meshtastic clients timestamp on
receipt.)

The bridge has no RTC/NTP, so it now **learns wall-clock time from inbound
MeshCore packets** (which carry a Unix `ts`) and stamps that estimate (+ elapsed
uptime) onto outbound MeshCore packets. Verified on air: an inbound MC packet
(`ts=1781399207`) calibrated the clock and the next `MT→MC` bridge stamped the
real current time (`mcts=1781399220`) instead of 0.

- `extractMeshCoreBody()` gains an optional `tsOut`; `learnClockFromMc()` +
  `bridgeNowUnix()` added in `main.cpp`; the stamped value is surfaced as
  `mcts=` on the `QUEUE` log line, with a one-time `evt=CLOCK` when first
  calibrated.
- **Known:** until the bridge hears its first timestamped MC packet after boot
  it still stamps 0 (then self-calibrates within seconds on a live mesh) —
  inherent without an RTC. A future option is to also learn time from Meshtastic
  `POSITION_APP` (which carries a Unix `time` field).

## v8.2 — RX-priority routing + source-identity preservation

**Status: bench-verified on hardware 2026-06-13 (branch `v8.2-router-backport`);
ready to tag.** Backports the RX-priority routing redesign from
the unreleased `T_LORA_QUAD_ROUTE` line onto the shipping 2-radio dual-SX1262
bridge, and adds a source-identity-preservation layer. No LR1121/co-processor
code; `BridgeConfig` stays schema v4 (no NVS migration — an upgraded v8.1 device
keeps its config). Full design + bench plan in `V8.2-SPEC.md`.

- **RX-priority pipeline (no more TX-blocks-RX).** Each radio defaults to
  receive; a received packet is decoded once, de-duplicated, re-encoded for the
  destination and pushed onto a per-destination, age-bounded, PSRAM-backed
  `RouteQueue`. The destination radio's task pops it, does **CAD**
  (listen-before-talk), and sends it with a **non-blocking** transmit, CSMA
  backoff and a duty-cycle **airtime throttle**. A long SF11 transmit no longer
  makes the bridge deaf. New: `LoraRadio` interface, `WioSX1262`
  `scanChannel`/`startTransmit`/`txDone`/`finishTransmit`, `DedupCache`,
  `RouteQueue`.
- **Content-hash loop prevention (clean far-side bodies).** The `[MT]/[MC]/[rns]`
  text markers are removed. Loops/duplicates are dropped by a TTL-windowed
  FNV-1a hash of the decoded body + sender id **+ Meshtastic packet_id**,
  recorded on receive and on every emission — which also drops a packet heard on
  both radios and never false-drops a user message starting with `[MT`. Folding
  the packet_id means a node's genuinely-distinct messages with identical text
  bridge each time (new id → new hash) while echoes/replays (same id) still drop.
- **Source-identity preservation.** A bridged repeat carries/reconstructs the
  original sender instead of the bridge:
  - **MC→MT**: the MeshCore sender name (`"<name>: …"`) becomes a deterministic
    virtual Meshtastic node (`FNV-1a("MC|"+name)`) with a synthetic NodeInfo, so
    a phone shows the message from `Alice @MC`.
  - **MT→MC**: the body is prefixed with the Meshtastic sender's name
    (`Alice@MT: …`, NodeDB short-name or `!hexid`).
  - **Same protocol, same channel, different frequency**: a transparent **raw
    repeat** — original bytes unchanged (Meshtastic: hop_limit decremented,
    `relay_node` set), so the far side sees the original sender natively.
  - Flags: `BRIDGE_IDENTITY_PRESERVE` (1), `BRIDGE_TAG_ORIGIN_PROTO` (1,
    `@MT`/`@MC` tags), `BRIDGE_MC_NONAME_VIRTUAL` (0).
  - Deferred: MT→MT re-encrypt across *different* channels (trans-crypt);
    NodeInfo is consumed for NodeDB, not raw-repeated; LoRaWAN (sync `0x34`) is a
    separate future version (capture-only — see `V8.2-SPEC.md` §14).
- **`NodeDB`** is now mutex-guarded with a copy-based `lookupShortName`, since
  more than one task reads it.
- **Structured serial logging.** One greppable `ts=… evt=… radio=… key=val` line
  per pipeline event, emitted atomically across both cores via a dedicated log
  mutex.
- **`RadioLib` pinned `7.7.0`** (the version the TX/CAD path was developed and
  bench-validated against; the LR11x0 RX deficit on 7.7.0 does not affect the
  SX126x path).
- **Captive portal**: the same-protocol self-bridge guard is now freq-aware — it
  rejects a routed pair only when protocol, channel name+key **and** frequency
  all match, so a same-channel cross-frequency relay is configurable.

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
