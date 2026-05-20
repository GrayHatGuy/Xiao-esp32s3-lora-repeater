# Changelog

## 2026-05-19 — Persistent NodeDB & MT sender attribution

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
