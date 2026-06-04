# Contest Phase 2 — write-up draft

> **Draft for owner review — not yet posted.** Intended for the Seeed/Meshtastic
> Build-Off issue **`Seeed-Projects/meshtastic-build-off-2026#2`** as the Phase 2
> project description / status update. Review and post manually.

## Phase 2 — 4-Up omnidirectional tri-band repeater (T-Lora-Dual add-on)

**One line:** the dual-SX1262 cross-protocol bridge grows to **four radios** by
bolting on a LilyGO **T-Lora-Dual** (two **LR1121** radios) over a UART link —
a sub-GHz **+ 2.4 GHz** (+ S-band stub) tri-band repeater that relays
Meshtastic / MeshCore / Reticulum across any of its radios under a
portal-configurable routing matrix.

### What it is

Phase 0/1 is a single Xiao ESP32-S3 with two Wio **SX1262** radios (R1/R2)
bridging sub-GHz LoRa protocols. Phase 2 keeps that board as the "brain" and adds
a second board — the **T-Lora-Dual** (ESP32 PICO-D4 + two LR1121) — as radios
**R3/R4**, reached over a framed UART link. The LR1121s bring **2.4 GHz** (and an
S-band stub) alongside the SX1262 sub-GHz radios.

```
   Xiao ESP32-S3 (brain)                 T-Lora-Dual (co-processor)
   ┌───────────────────────┐  UART link ┌──────────────────────────┐
   │ R1 SX1262 ─┐ shared    │  (framed)  │ R3 LR1121 ─┐ shared SPI   │
   │ R2 SX1262 ─┘ SPI       │◄──────────►│ R4 LR1121 ─┘  sub-GHz /   │
   │ config · portal ·      │            │ RadioLib LR1121 · Factory │
   │ NodeDB · routing matrix│            │ RF-switch table · 2.4 GHz │
   └───────────────────────┘            └──────────────────────────┘
```

Each radio's received traffic is repeated out the radios selected in its
**routing matrix** (set per-radio in the captive portal), with the same
cross-protocol translation and loop-prevention markers as Phase 0/1.

### Why the T-Lora-Dual (pivot from the Wio-LR1121)

Phase 1 attempted a Seeed **Wio-LR1121** as the 2.4 GHz radio but hit an
unresolved LR1121 **RX deficit** — it detected every preamble yet completed only
a small, unreliable fraction of packets. The evidence pointed to an
**RSSI/image-calibration or modem-config class issue** (RadioLib `-20
WRONG_MODEM`, RSSI-cal flagged as the top candidate); the on-module **SKY13373
RF switch was swept exhaustively and exonerated**. Rather than keep fighting one
module, Phase 2 pivots to the **T-Lora-Dual**, whose factory firmware demonstrates
working RX+TX — and reuses that factory HAL verbatim.

### What's built (branch `T_LORA_QUAD`)

- **Config** — `BridgeConfig` schema v6: 4 radio slots, each with protocol, full
  RF plan, chip, **band** (sub-GHz / 2.4 GHz / S-band-stub) and **routeMask**;
  forward-migrating from the older 2-radio schemas.
- **UART transport** — one shared `LinkProtocol.h` wire format; host-side
  `UartLink` + `RemoteRadio` so the two remote LR1121s plug into the bridge as
  ordinary radio slots.
- **Bridge core** — generalized from a fixed R1↔R2 crossover to a 4-radio
  routing matrix; multi-task-safe NodeDB.
- **Portal** — all 4 radios configurable, with a band selector for R3/R4 and a
  routing-matrix UI; per-radio 2.4 GHz Meshtastic channel-slot math.
- **Co-processor firmware** — `coproc-tlora-dual/` (ESP32 PICO-D4, RadioLib
  7.7.0) drives the LR1121s with the T-Lora-Dual Factory HAL, speaks the UART
  protocol, applies band-aware power (22 dBm sub-GHz / 13 dBm 2.4 GHz), and stubs
  S-band.

### Status

- ✅ Both firmwares build clean (host `xiao_esp32s3`, co-proc `pico32`).
- ✅ Default behaviour unchanged — R3/R4 ship disabled, so the device still works
  as the proven 2-radio bridge until the new radios are enabled in the portal.
- ⏳ On-air 4-way bridge + 2.4 GHz decode: pending hardware bring-up.
- ⏳ Bench-verify: 2.4 GHz wideLora bandwidth handling on the LR1121 high band.

Design: [`QUAD-SPEC.md`](QUAD-SPEC.md). Changelog: [`CHANGELOG.md`](CHANGELOG.md)
v10.0. Source: branch `T_LORA_QUAD`.
