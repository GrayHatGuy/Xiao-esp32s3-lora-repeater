# LR1121 Spec — Sub-GHz ↔ 2.4 GHz Cross-Band Bridging

Status: **Phase 1 hardware-verified, shipped v9.0 (2026-05-25).**
LR1121 chip detection, init, and RX confirmed on real hardware at MT
LongFast (sync 0x2B, BW250, SF11) — bridge's own NodeInfo received at
-44 dBm SNR 9.8 dB. RadioLib 7.0.0 required (6.6.0 had an LR11x0
chip-detection bug). MC reception on the LR1121's MC RF profile
(BW62.5 / SF7 / sync 0x12) carries forward as a known limitation to
bench-isolate. Phase 2 (DUAL_LR1121) compile-verified, hardware
verification pending.

## Goal

Bridge the SX1262's sub-GHz mesh networks to **2.4 GHz LoRa** (e.g.
Meshtastic's 2.4 GHz preset) by introducing the **Seeed Wio-LR1121** module
as a 2.4 GHz-capable radio. The 2.4 GHz ISM band (2400–2483.5 MHz) is
licence-free worldwide, so a sub-GHz↔2.4 GHz bridge works in any region.

## Hardware target

- **Module:** Seeed **Wio-LR1121 with IPEX4 antenna connector**
  (SKU 113991415 / product 6479). Semtech LR1121, multi-band: sub-GHz
  863–928 MHz **+ 2.4 GHz + 2.1 GHz S-band**, 7.8–500 kHz LoRa BW.
- 24-pin castellated SMT module, 17×12×2.6 mm. **Needs a breakout / adapter
  board** to sit on a breadboard — the bare module is not breadboard-pitch.
- 3.3 V supply (1.8–3.6 V), 3.3 V logic — directly compatible with the
  Xiao ESP32-S3.
- **Integrated TCXO and RF switch** — the module handles antenna switching
  internally; there is **no external ANT_SW pin to wire** (this differs
  from the Wio SX1262 shield).
- Two IPEX4 RF connectors: `SUBG_RF` (pin 23) and `2.4G RF` (pin 2).

## Two-phase rollout

### Phase 1 — one LR1121 replaces the edge-pin SX1262

- Keep **Radio 1** = the B2B Wio SX1262 shield (sub-GHz), untouched.
- Replace **Radio 2** = the edge-pin SX1262 with a **Wio-LR1121**, hand-wired
  to the Xiao's edge pins via breadboard.
- Radio 2 runs on **2.4 GHz**; the bridge becomes a true sub-GHz ↔ 2.4 GHz
  cross-band repeater.
- The LR1121's sub-GHz port is unused in Phase 1 (2.4 GHz antenna only).

### Phase 2 — both radios become LR1121

- Replace the B2B SX1262 with a second LR1121.
- Collapses the design to a single radio family; either radio can then be
  sub-GHz or 2.4 GHz, freely. `WioSX1262` support is retained for existing
  hardware but the reference build is dual-LR1121.

## Phase 1 wiring — Wio-LR1121 → Xiao ESP32-S3

The LR1121 needs SPI + four control lines + power. No ANT_SW. All `GND`
pads tie to the common ground rail.

| LR1121 pin | LR1121 name | Xiao GPIO | Xiao pin | Net |
|-----------:|-------------|-----------|----------|-----|
| 5  | LR_SCK   | GPIO7 | D8  | SPI_SCK  (shared with Radio 1) |
| 6  | LR_MOSI  | GPIO9 | D10 | SPI_MOSI (shared with Radio 1) |
| 7  | LR_MISO  | GPIO8 | D9  | SPI_MISO (shared with Radio 1) |
| 4  | LR_NSS   | GPIO5 | D4  | R2_NSS  (chip select) |
| 12 | DIO9     | GPIO2 | D1  | R2_DIO1 (IRQ / INT line) |
| 14 | RST      | GPIO3 | D2  | R2_RESET |
| 8  | LR_BUSY  | GPIO4 | D3  | R2_BUSY |
| 20, 21 | VDD_RF | 3V3 | 3V3 | +3V3 |
| 1,3,9,10,13,15–19,22,24 | GND | GND | GND | GND |
| 11 | DIO8     | —     | —   | leave unconnected (not needed) |
| 2  | 2.4G RF  | —     | —   | 2.4 GHz IPEX antenna |
| 23 | SUBG_RF  | —     | —   | unused in Phase 1 |

```
        XIAO ESP32-S3                         WIO-LR1121  (on breakout)
        ┌───────────────┐                     ┌────────────────────────┐
        │ GPIO7  (D8)  ──┼── SPI_SCK  ─────────┤ 5  LR_SCK              │
        │ GPIO9  (D10) ──┼── SPI_MOSI ─────────┤ 6  LR_MOSI             │
        │ GPIO8  (D9)  ──┼── SPI_MISO ─────────┤ 7  LR_MISO             │
        │ GPIO5  (D4)  ──┼── R2_NSS   ─────────┤ 4  LR_NSS              │
        │ GPIO2  (D1)  ──┼── R2_DIO1  ─────────┤ 12 DIO9 (IRQ)          │
        │ GPIO3  (D2)  ──┼── R2_RESET ─────────┤ 14 RST                 │
        │ GPIO4  (D3)  ──┼── R2_BUSY  ─────────┤ 8  LR_BUSY             │
        │ 3V3          ──┼── +3V3     ─────────┤ 20,21 VDD_RF           │
        │ GND          ──┼── GND  ─────────────┤ 1,3,9,10,13,15-19,     │
        │ GPIO6  (D5)    │   (old ANT_SW —     │ 22,24  GND             │
        │                │    now FREE)        │ 2  2.4G RF ── IPEX ANT │
        └───────────────┘                     │ 11 DIO8  (unconnected) │
        Radio 1 = B2B SX1262 shield            │ 23 SUBG_RF (unused P1) │
        stays mounted under the Xiao           └────────────────────────┘
```

Notes:
- The LR1121 reuses Radio 2's existing GPIO map (NSS/DIO1/RESET/BUSY); the
  old `R2_ANT_SW` pin (GPIO6/D5) becomes free — the LR1121 switches its
  antenna internally.
- DIO9 is the LR1121's generic IRQ line — it maps onto the same GPIO the
  edge-pin SX1262 used for DIO1.
- The two radios still share the one SPI bus + the FreeRTOS mutex.
- Tie **all** LR1121 GND pads to ground; decouple VDD_RF close to the module.

## Firmware components

### 1. `LoraRadio` abstract interface

Introduce an abstract base class — `available()`, `read()`, `transmit()`,
`startReceive()`, `begin()` — with the existing `LoraConfig` struct as the
common config. `WioSX1262` becomes `WioSX1262 : public LoraRadio`. `radio1`
/ `radio2` become `LoraRadio*`. The bridge pipeline is already RF-agnostic
(it branches on the LoRa sync word, not the chip), so `bridgePacket()` and
the radio tasks are unchanged.

### 2. `WioLR1121` wrapper

New `WioLR1121.{h,cpp}` implementing `LoraRadio` on top of RadioLib's
`LR1121` class. Same `available()` / `read()` / `transmit()` surface as
`WioSX1262`. Handles LR1121-specific init: TCXO config, RF-path selection
(the LR1121 needs the band — sub-GHz vs 2.4 GHz — set so it routes the
internal RF switch and PA correctly).

### 3. Radio-chip selection — build profile

A build flag `RADIO_PROFILE` picks one of three profiles:

- **MIXED** (Phase 1 / transition build) — Radio 1 is always SX1262;
  Radio 2 is SX1262 **or** LR1121. The default comes from
  `-DRADIO2_CHIP=SX1262|LR1121`, but the chip is **runtime-selectable in
  the captive portal** — a "Radio 2 chip" picker. The firmware links
  *both* `WioSX1262` and `WioLR1121` drivers and constructs the chosen one
  at boot from `BridgeConfig`. Flash once, swap Radio 2 hardware, repick
  in the portal, no rebuild. This is the bring-up path while only one
  LR1121 is on hand.
- **DUAL_SX1262** — both radios SX1262, **locked at compile time**. The
  portal shows **no chip control**; a `static_assert` enforces the
  homogeneous config. Only the SX1262 driver is linked.
- **DUAL_LR1121** — both radios LR1121, **locked at compile time**. No
  portal chip control; `static_assert`-enforced. Only the LR1121 driver
  is linked.

Rationale: the MIXED build is the transition path — one LR1121 wired,
chip swappable without a rebuild. A finished homogeneous design has no
reason to carry the other driver or expose a picker, so DUAL_* profiles
are compile-time-fixed and the portal hides the control entirely. This
matches the user constraint: chip choice is portal-adjustable *only* in
the mixed one-LR1121 build; the dual-same-chip designs are compile-time
only, no user adjustment.

### 4. 2.4 GHz in the config model

- **Radio 2 chip — `BridgeConfig` (MIXED profile only).** Schema bumps
  v4 → v5 with a per-radio `chip` field (`CHIP_SX1262` / `CHIP_LR1121`).
  In the MIXED profile the portal shows a "Radio 2 chip" `<select>`;
  `setup()` constructs `radio2` from the stored value. In DUAL_* profiles
  the field is ignored, fixed by the profile, and the portal omits the
  control. v4 blobs migrate forward (chip defaults to the build profile).
- **Region exemption** — already in the v8 design: a 2.4 GHz radio resolves
  its frequency from the 2.4 GHz band regardless of the global region.
- **Portal frequency validation** — `handleSave()` currently clamps to the
  SX1262 150–960 MHz range. For an LR1121 slot the valid range becomes the
  union: 150–960 MHz **or** 2400–2500 MHz. The portal learns the slot's
  chip type from the `RADIOn_CHIP` build flag (surfaced through
  `BridgeConfig`).
- **2.4 GHz modem presets** — Meshtastic's 2.4 GHz presets use the
  *wideLora* parameter set (e.g. LongFast → 812.5 kHz BW). `RegionPreset.h`
  currently emits only the non-wideLora bundle; add the wideLora variants
  and a 2.4 GHz "region" row (a fixed 2400–2483.5 MHz band) so Tier 2 slot
  computation works on 2.4 GHz too.

### 5. TX power

LR1121 2.4 GHz output tops out at ~11 dBm (vs SX1262 sub-GHz +20 dBm). The
TX-power cap logic gains a 2.4 GHz ceiling.

## Build staging

- **A** — `LoraRadio` abstract interface; refactor `WioSX1262` to implement
  it; `radio1/2` become `LoraRadio*`. No behaviour change, dual-SX1262
  still builds and runs.
- **B** — `WioLR1121` wrapper on RadioLib `LR1121`; `RADIO_PROFILE` build
  flag (MIXED / DUAL_SX1262 / DUAL_LR1121); `BridgeConfig` schema v4 → v5
  (per-radio `chip`); `setup()` constructs each radio per profile (MIXED
  reads radio2 chip from NVS, DUAL_* are `static_assert`-fixed).
- **C** — portal: "Radio 2 chip" picker shown only in the MIXED profile;
  2.4 GHz config model — wideLora modem presets + 2.4 GHz band row in
  `RegionPreset.h`; portal frequency validation widened for an LR1121
  Radio 2; 2.4 GHz TX-power ceiling.
- **D** — bench bring-up: Phase 1 hardware, verify LR1121 RX/TX on 2.4 GHz,
  verify sub-GHz↔2.4 GHz bridging end to end.
- **E** — docs (README wiring + roadmap, CHANGELOG); release.

## Open questions — confirm before code

- **(a)** ~~RadioLib `LR1121` API.~~ **RESOLVED** — `LR1121` derives
  `LR1120`→`LR11x0`. `begin(bw, sf, cr, syncWord, preambleLength,
  tcxoVoltage, bool high)` — no frequency arg; `high=true` for >1 GHz
  (2.4 GHz). Frequency is set separately via `setFrequency(float)` (valid
  150–960 / 1900–2200 / 2400–2500 MHz, auto image calibration).
  `setOutputPower(int8_t)` −9…22 dBm. The `WioLR1121` wrapper derives
  `high` from `LoraConfig.frequency > 1000 MHz`. Two bench items remain:
  the module's exact TCXO voltage (datasheet silent — assume 1.6 V,
  verify) and the RF-switch table config (`setRfSwitchTable` /
  `setDioAsRfSwitch` — the LR1121 drives the on-module switch).
- **(b)** ~~Breakout board for the castellated Wio-LR1121?~~ **CONFIRMED**
  — a 1:1 breakout/carrier board is used, so the wiring table maps the
  module pads directly to the carrier pins.
- **(c)** MIXED profile links both radio drivers — confirm the combined
  flash/RAM cost is acceptable (current build is ~24 % flash, so there is
  ample headroom; this is just a sanity check at integration).

## Out of scope

- 2.1 GHz S-band — the LR1121 supports it, but it is not bridged.
- LR-FHSS modes.
- SX1280 / LR2021 — tracked separately in the README roadmap.
