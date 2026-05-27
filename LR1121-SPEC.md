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
  `high` from `LoraConfig.frequency > 1000 MHz`. TCXO voltage settled at
  3.0 V on the bench. **RF switch config remains unresolved** — see the
  "Phase 1 status — RX-path block" section at the bottom of this doc.
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

---

## Phase 1 status — RX-path block (2026-05-26)

**Status: BLOCKED.** TX path on the Wio-LR1121 works end-to-end and is
verified by other MT/MC devices receiving the bridge's transmissions. RX
path produces zero real `RX_DONE` events even with a transmitting antenna
physically touching the LR1121 antenna port. The block is hardware/RF
below the RadioLib API surface — every software return code is success.

### What is known to work

- Chip detection: `Found LR11x0: CMD_GET_VERSION = 0x03, Base FW version: 1.3`.
- Manual reset + BUSY-poll bring-up: LA-confirmed ~141 ms boot release time.
- `_radio->begin(...)` returns `RADIOLIB_ERR_NONE` with our frequency / BW /
  SF / CR / sync / power / TCXO config.
- `_radio->setRfSwitchTable(...)` accepts the table (LR11x0::setRfSwitchTable
  is void, but the underlying `setDioAsRfSwitch` SPI command 0x0112 is sent
  with no error).
- `_radio->startReceive()` returns `0` every call (boot, post-read re-arm,
  post-transmit re-arm).
- `_radio->transmit(buf, len)` returns `0` and the bridge's relayed messages
  reach external MT and MC devices at expected RSSI.
- `_radio->getIrqFlags()` reads `0x00000000` immediately after begin —
  clean, no stale bits.
- DIO9 IRQ wiring works: the ISR fires reliably on the chip's post-TX
  state-transition event (visible as `[Radio2-Edge] read: pktLen=0` after
  every `[R1->MC bridge] TX OK`).

### What is broken

- DIO9 never fires from an actual RX_DONE event triggered by OTA traffic
  arriving at the antenna.
- Across multiple test sessions (same-freq MT, cross-band MC, antenna-touch
  with an MT phone), `[Radio2-Edge] read: pktLen=N>0` from real OTA traffic
  has been seen exactly **once** — and that one packet was the bridge's own
  R1 NodeInfo TX at +20 dBm 10 cm away, almost certainly coupled in through
  shared PCB / supply / ground rather than the antenna→LNA chain.
- Touch-test evidence (2026-05-26 serial log t=789358 → t=841092):
  - MT phone antenna physically touching R2 antenna.
  - R1 (SX1262, 10 cm farther) hears the phone at **-35 to -37 dBm**.
  - By geometry, R2 antenna port receives ~**-10 to -20 dBm**.
  - During the 50+ seconds of touch contact + active TX, the ISR counter
    on R2 increments only from post-TX spurious fires. Zero real RX.

### What has been ruled out

| Hypothesis | Status |
|---|---|
| Defective single module | Two independently-sourced modules behave identically. |
| Stale IRQ flag latched at boot | `getIrqFlags() post-begin = 0x00000000`. |
| `startReceive()` rejected by chip | Returns 0 every call. |
| Switch table missing | Both Meshtastic femtofox (DIO5/6/7, RX=000) and LilyGO T3S3 production (DIO5/6, RX=100) tables tried. No change. |
| RadioLib `setRfSwitchTable` not issuing chip command | Source review confirms `setDioAsRfSwitch` SPI command 0x0112 is sent. |
| TCXO voltage wrong | Verified at 3.0 V; per-module-datasheet (silent, but 1.6 / 3.0 / 3.3 all tried). |
| TX_DONE bit leaking into DIO9 | Mapped via `setPacketReceivedAction` → RX_DONE only. Post-TX spurious is a one-shot chip transition artifact, not a path issue. |
| Sensitivity floor | Even with -10 dBm at antenna (touch-test), zero RX. |

### Open question — possible RadioLib bit-encoding inconsistency

In `LR11x0::setRfSwitchTable()` (RadioLib 7.7.0 LR11x0.cpp:1284) the
`enable` mask is built with DIO-number bit positions (e.g. DIO5 → bit 5)
while the per-mode `modes[]` masks are built with array-index bit positions
(e.g. pins[0] → bit 0). Whether the chip's `SetDioAsRfSwitch` (cmd 0x0112)
expects both in DIO-number positions or both in array-index positions is
not obvious from the LR1121 reference manual we have access to. If this is
an upstream bug, the Wio-LR1121's switch is being programmed with garbage
per-mode masks regardless of our table values — which would explain why
neither femtofox nor T3S3 tables produced any change. Validation pending
Seeed clarification or a logic-analyzer capture of DIO5/6/7 during a
TX→RX transition.

### What was tried (so the next debug session doesn't repeat work)

1. Defensive `standby() + startReceive()` re-arm inside `read()` — no change.
2. Explicit `clearIrqFlags(RADIOLIB_LR11X0_IRQ_ALL)` before re-arm — no change.
3. ISR counter heartbeat — confirms post-TX ISRs are the only DIO9 events.
4. Per-read `pktLen / state / len` dump — confirms `pktLen=0` on every
   post-TX spurious, never a real packet.
5. Switch table install before `begin()` (RadioLib convention) — no change.
6. Two RF switch tables: femtofox (DIO5/6/7, RX=000) and LilyGO T3S3
   (DIO5/6, RX=100). Both compile and load cleanly. No RX.
7. Cross-band test (R1=MT 906.875, R2=MC 910.525): zero real RX.
8. Same-freq test (R1=R2=MT 906.875): zero real RX, only the one self-echo
   bridge-NodeInfo packet from R1's nearby PA.
9. Antenna touch-test: zero real RX even at -10 dBm antenna input.

### Brute-force MODE_RX DIO sweep — RESULTS (2026-05-26, bench run)

`platformio.ini` exposes `LR1121_BRUTEFORCE_RX_DIOMASK` (bits 0/1/2 →
DIO5/6/7). Built and flashed all 8 values; for each, ran a 2-minute
bench test with the same-frequency MT config (R1=R2=MT-LongFast,
906.875 MHz, BW250/SF11/CR5, sync 0x2B), sending 2–3 MT messages from a
nearby phone (R1 received the phone at -34 to -40 dBm throughout each
test). Result for every combination:

| DIOMASK | D5 D6 D7 | Self-echo RSSI | SNR | Real OTA RX |
|---|---|---|---|---|
| 0 | 0 0 0 | -56 dBm | 9.8 dB | 0 |
| 1 | 1 0 0 (T3S3) | -54 dBm | 10.0 dB | 0 |
| 2 | 0 1 0 | -56 dBm | 11.0 dB | 0 |
| 3 | 1 1 0 | -55 dBm | 10.5 dB | 0 |
| 4 | 0 0 1 | -53 dBm | 11.2 dB | 0 |
| 5 | 1 0 1 | -52 dBm | 11.5 dB | 0 |
| 6 | 0 1 1 | -52 dBm | 11.2 dB | 0 |
| 7 | 1 1 1 | -53 dBm | 10.8 dB | 0 |

**All 8 combinations fail identically.** Every run produced exactly one
RX event — the bridge's own R1 NodeInfo at +20 dBm 10 cm away, picked
up at -52 to -56 dBm (4 dB variance across all 8 states, i.e. noise).
No real distant OTA traffic ever reached the demodulator on R2, despite
R1 (SX1262, same carrier) hearing the same RF at -34 to -40 dBm.

### Extended sweep — DIO5/6/7 + DIO8 (RFSW3) + DIO10 (RFSW4)

After confirming the LR1121 chip supports **up to 5 RFSWx outputs** per
Semtech user manual §4.5.2 (RFSW0=DIO5, RFSW1=DIO6, RFSW2=DIO7,
RFSW3=DIO8, RFSW4=DIO10), the brute-force flag was extended from
3-bit to 5-bit and four additional targeted combinations were tested.
DIO10 is particularly interesting because it serves dual purpose on
the chip (32 kHz crystal alt **or** RFSW4 alt), and the Wio-LR1121's
integrated TCXO frees DIO10 from the 32 kHz crystal role — making it
a plausible RF-switch candidate.

| `DIOMASK` | D5 D6 D7 D8 D10 | Self-echo RSSI | Real OTA RX |
|---|---|---|---|
| 8  | 0 0 0 1 0 (DIO8 / RFSW3 alone) | -49 dBm | 0 |
| 16 | 0 0 0 0 1 (DIO10 / RFSW4 alone) | (no self-echo captured) | 0 |
| 24 | 0 0 0 1 1 (DIO8 + DIO10) | -50 dBm | 0 |
| 31 | 1 1 1 1 1 (all 5 HIGH) | -50 dBm | 0 |

**All four extended combinations fail identically to the 3-DIO sweep.**
Self-echo RSSI variance across the full 12-iteration sweep is **-49 to
-56 dBm — about 7 dB**, pure noise, **completely independent of any
RFSWx-capable DIO state.**

(DIO11 is also an RFSW4 alternate per the LR1121 datasheet, but
RadioLib 7.7.0's LR11x0 driver does not map DIO11 in its
`RADIOLIB_LR11X0_DIOx(0..4)` set — only DIO5/6/7/8/10 are reachable
via `setRfSwitchTable()`. DIO11 cannot be tested via this software
path.)

### Final interpretation

The Wio-LR1121's antenna → switch → LNA → demodulator chain is
**non-functional independent of any RFSWx programming RadioLib can
apply.** The signal reaching the demodulator (only ever the bridge's
own R1 NodeInfo at extreme near-field) couples in through PCB
substrate / supply rail / ground, **not** through the antenna path.

**This conclusively rules out switch-table-as-fix.** Any software
resolution must come from either:

- A chip-level init step Semtech / Seeed has not documented (e.g. an
  undocumented LNA-enable command, a `SetRxBoosted` quirk, or a
  configuration that has to precede `SetDioAsRfSwitch`);
- A RadioLib LR11x0 driver bug (specifically the bit-encoding
  inconsistency in `setRfSwitchTable()` flagged earlier);
- A chip-firmware-1.3-specific issue that newer firmware fixes; or
- A hardware-level fault that no software can resolve.

The Seeed support inquiry asks for definitive guidance from Seeed
engineering on which of these is the case.

### Path forward

- **Short-term:** ship Phase 0 (dual SX1262) as v9.0.x with the LR1121
  driver tree intact behind the MIXED build profile and the Phase 1 RX
  limitation documented in the changelog. Production users keep Phase 0.
- **Mid-term:** file a Seeed support inquiry (see `SEEED_SUPPORT_INQUIRY.md`)
  with all the evidence above. Wait on their reply for the authoritative
  Wio-LR1121 RF switch wiring / RX activation procedure.
- **Long-term:** if Seeed confirms a module-level RF design issue, pivot
  Phase 2 to an alternate LR1121 carrier — Semtech reference design, Ebyte
  E80-900M22S, or a custom carrier with an external switch IC we control
  directly.
