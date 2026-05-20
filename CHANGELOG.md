# Changelog

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
