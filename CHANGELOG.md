# Changelog

## v6.0 — 2026-05-19 — Captive-portal config, POSITION/TELEMETRY bridging, configurable MC channel

This release makes the bridge field-configurable without a rebuild and widens the Meshtastic traffic it understands. It bundles features F2, F3 and F4.

**WiFi captive portal (F4).** On a fresh flash — or any time the **BOOT** button is pressed within ~3 s of reset — the bridge comes up as an open WiFi AP named `LoRa-Bridge-<XX>` (last byte of the MT node ID in hex) and DNS-redirects all HTTP traffic to a single-page config form. The form exposes the eight settings users actually re-tune: Meshtastic identity (node ID numeric + `!hex` string, long name, short name), MeshCore channel (32-char hex key + display name), and the POSITION/TELEMETRY bridge toggles. Saving writes a schema-v1 blob to NVS and reboots into bridge mode. New `BridgeConfig.{h,cpp}` is the single source of truth; the `BRIDGE_MT_*` / `BRIDGE_MC_*` build flags become the defaults a fresh device starts from, so existing `platformio.ini` configs behave identically until the portal saves something new.

**POSITION + TELEMETRY bridging (F2).** `POSITION_APP` and `TELEMETRY_APP` are decoded from incoming MT packets and re-emitted to MeshCore as compact text under the existing `[MT !<hexid> <SHORT>]` marker — `pos 40.7234,-74.0123 alt 12m`, `bat 87% 4.05V`, `env 22.5C RH 45% 1013hPa`. Each portnum is individually toggleable from the portal or via build flag.

**Configurable MeshCore channel (F3).** The hard-coded public-channel key moved into a new `MeshCoreConfig` module; the bridge can now point at any MC channel via config, with the on-air channel hash auto-derived from `SHA-256(key)[0]`.

Per-radio LoRa parameters (frequency, BW, SF, CR, TX power, sync word) remain build-flag-only — out-of-band misconfiguration is too easy to expose in a v1 form.

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
