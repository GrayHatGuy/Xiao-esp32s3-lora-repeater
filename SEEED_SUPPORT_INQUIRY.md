# Seeed Support Inquiry — Wio-LR1121 RX Path Non-Functional

**To:** sensecap@seeed.cc
**Re:** Wio-LR1121 Module (SKU 113991415, IPEX antenna variant) — sub-GHz
RX produces no `RX_DONE` events; TX confirmed working.
**Hardware:** Two units, ordered on separate occasions, identical behavior.
**Date:** 2026-05-26 (rev 2 — 2026-05-27: full chip-init DOE complete; all UM v2.2 firmware remedies tested and refuted)

---

## Reference documents

This inquiry cites the following Semtech and Seeed reference documents. Both direct PDF and stable product-page links are given for each (use the stable link if the Semtech direct PDF tokens expire).

- **Semtech LR1121 User Manual v2.2** (rev 2.2, Apr 2026, 140 pages — referred to as "UM v2.2" below). Chip-level command spec.
  - Direct PDF: https://semtech.my.salesforce.com/sfc/p/#E0000000JelG/a/RQ00000DClgP/D.pNG5l4FviPI634eCx8GFURZEwDO2ZBA33MpriB_FU
  - Stable product page: https://www.semtech.com/products/wireless-rf/lora-connect/lr1121

- **Semtech LR1121 Datasheet** (rev 2.1, Dec 2023 — referred to as "LR1121 Datasheet v2.1" below). Chip electrical/RF specification.
  - Direct PDF: https://semtech.my.salesforce.com/sfc/p/#E0000000JelG/a/RQ0000093ZiP/RV4Ba6LROsFrFjnAAVK2av5W11RGmCms_3Q2cyKHdDA
  - Stable product page: https://www.semtech.com/products/wireless-rf/lora-connect/lr1121

- **Seeed Wio-LR1121 Module Datasheet v1.0 (2025-07-01)** — referred to as "Module Datasheet" below.
  - Direct PDF: https://files.seeedstudio.com/wiki/Wio-LR1121/Wio-LR1121_Module_Datasheet_v1.0.pdf
  - Stable wiki page: https://wiki.seeedstudio.com/wio_lr1121_module/

- **Seeed Wio-LR1121 KiCad library** (used for the `MCU_DIO5/6/7` test-pad finding documented below).
  - https://github.com/Seeed-Studio/OPL_Kicad_Library/tree/master/Seeed%20Studio%20Wio%20LR1121%20Module%20v0.9

---

## Summary

We have integrated two Seeed Wio-LR1121 modules (SKU 113991415,
firmware-reported `Base FW version: 1.3`) onto a custom Xiao-ESP32-S3
carrier using SPI + GPIO. **Transmit works correctly** — both units
radiate at the configured frequency and power, confirmed by multiple
independent LoRa receivers (Meshtastic + MeshCore devices). **Receive
does not work** — the chip never asserts DIO9 RX_DONE for any over-the-air
packet, even with a transmitting antenna physically touching the module's
SUBG_RF port.

We have exhausted software-side troubleshooting. All RadioLib API calls
return success. We need clarification on the correct RF switch
configuration (DIO5/DIO6/DIO7 truth table) or whether there is an
additional chip-level command we need to issue to enable the RX path on
the Wio-LR1121's integrated front-end.

## Hardware setup

- **Module:** Wio-LR1121 IPEX-antenna variant, SKU 113991415, two units.
- **Antenna:** SMA whip antenna on the SUBG_RF pad (pad 23) via IPEX
  pigtail. Verified continuous DMM continuity from antenna tip through
  IPEX → pad 23. Pad 19 ↔ 20 (GND) verified open (no short).
- **Host MCU:** Seeed Xiao ESP32-S3 (revision v0.1, MAC dc:54:75:d7:ac:1c).
- **Wiring:**
  - LR_NSS → Xiao GPIO5 (D4)
  - LR_SCK → Xiao GPIO7 (shared SPI)
  - LR_MOSI → Xiao GPIO9 (shared SPI)
  - LR_MISO → Xiao GPIO8 (shared SPI)
  - LR_BUSY (pad 8) → Xiao GPIO4 (D3)
  - DIO9 (pad 12) → Xiao GPIO2 (D1) — IRQ
  - RST (pad 14) → Xiao GPIO3 (D2)
  - VDD_RF → 3.3 V (measured 3.27 V steady, no brown-out)
  - GND → common ground
- **Power:** USB-C 5 V → Xiao on-board regulator → 3.3 V rail. No
  brown-out, supply rail clean on scope.

## Software setup

- **Toolchain:** PlatformIO, espressif32 @ 6.13.0, arduino-esp32 2.0.17.
- **Driver:** RadioLib 7.7.0 (jgromes/RadioLib).
- **Application:** Custom dual-radio LoRa-mesh bridge. The other radio
  slot (Radio 1) is a Seeed Wio-SX1262 shield on the same Xiao, sharing
  the SPI bus through a FreeRTOS mutex. Radio 1 (SX1262) works perfectly
  on the same carrier with the same firmware — RX and TX both verified.

## What we know works

All of the following are verified by serial-log output (excerpts below):

1. **Chip detection.** RadioLib reports `Found LR11x0: CMD_GET_VERSION =
   0x03, Base FW version: 1.3`.
2. **Manual reset and BUSY-poll bring-up.** BUSY de-asserts at ~141 ms
   post-NRESET rise (logic-analyzer confirmed). LR_BUSY pad behaves as
   the datasheet describes.
3. **`begin()` succeeds** with our LoRa parameters (e.g. 906.875 MHz /
   BW 250 kHz / SF 11 / CR 4/5 / sync 0x2B / +20 dBm / TCXO 3.0 V).
   Return value `RADIOLIB_ERR_NONE`.
4. **`setRfSwitchTable()` accepts the table** (return is void; underlying
   `SetDioAsRfSwitch` SPI command 0x0112 is sent without error).
5. **`startReceive()` returns 0** — every call: post-begin, post-read
   re-arm, post-transmit re-arm.
6. **`getIrqFlags()` immediately after begin returns `0x00000000`** — no
   stale IRQ bits latched.
7. **`transmit(buf, len)` returns 0** and the radiated signal reaches
   external receivers. We confirmed this with three independent MT
   devices (RSSI ~-50 to -75 dBm at 1–2 m distance) and three independent
   MeshCore devices on a separate frequency / channel.
8. **DIO9 IRQ wiring works.** After every `transmit()` call, DIO9 fires
   exactly once with `pktLen=0` — a known LR1121 mode-transition artifact.
   The ISR trampoline, FreeRTOS task wake-up, and SPI readback path are
   all functional.

## What does not work

Across many test sessions:

- **Real OTA RX_DONE events: 1 observed, ever.** That one was the
  bridge's own R1 (SX1262) NodeInfo transmission at +20 dBm 10 cm away,
  picked up at RSSI -63 dBm. Almost certainly received via PCB / supply /
  ground coupling rather than the proper antenna → LNA chain.
- **Distant OTA traffic at -53 to -75 dBm** that Radio 1 (SX1262) on the
  same carrier happily receives on the same frequency / modem / sync —
  R2 (LR1121) detects none of it.
- **Antenna touch-test:** with a transmitting Meshtastic phone antenna
  physically pressed against the LR1121 antenna pigtail, R1 (10 cm away)
  records the phone at -35 to -37 dBm. By geometry, R2's antenna port is
  receiving roughly -10 to -20 dBm. R2 still fires zero DIO9 RX_DONE
  events during the 50+ seconds of contact.

## What we have tried

| Attempt | Result |
|---|---|
| Defensive `standby()` + `startReceive()` after each `readData()` | No change. |
| Explicit `clearIrqFlags(RADIOLIB_LR11X0_IRQ_ALL)` before re-arm | No change. |
| Meshtastic femtofox-style switch table (DIO5/6/7, MODE_RX = 0, 0, 0) | No change. |
| LilyGO T3S3-style switch table (DIO5/6, MODE_RX = 1, 0) | No change. |
| TCXO voltage at 1.6 V, 3.0 V, and 3.3 V | No change at any setting. |
| Second independently-sourced Wio-LR1121 module | Identical behavior. |
| Same-frequency, same-modem test (R1=R2=MT-LongFast 906.875 MHz) | No change. |
| Cross-band test (R1=MT 906.875, R2=MC 910.525) | No change. |
| RadioLib 7.0.0 → 7.7.0 (accumulated LR11x0 fixes) | No change. |
| **Brute-force sweep of all 8 DIO5/6/7 combinations for MODE_RX** | **All 8 fail identically.** See table below. |

### Brute-force MODE_RX DIO sweep — definitive result

We built and flashed 8 separate firmware images, each with a different
`MODE_RX` entry in the LR1121 RF switch table covering every possible
state of DIO5, DIO6, and DIO7. For each, we ran the same bench test
(same-frequency MT, R1=R2=906.875 MHz BW250 SF11 sync 0x2B, MT phone
sending messages at ~30 cm). R1 (SX1262 on the same carrier) consistently
received the phone at -34 to -40 dBm during each test, providing a strong
reference signal R2 should also have detected.

| `BRUTEFORCE_RX_DIOMASK` | D5 D6 D7 | R2 self-echo RSSI | R2 self-echo SNR | Real OTA RX events |
|---|---|---|---|---|
| 0 | LOW LOW LOW | -56 dBm | 9.8 dB | 0 |
| 1 (LilyGO T3S3 production) | HIGH LOW LOW | -54 dBm | 10.0 dB | 0 |
| 2 | LOW HIGH LOW | -56 dBm | 11.0 dB | 0 |
| 3 | HIGH HIGH LOW | -55 dBm | 10.5 dB | 0 |
| 4 | LOW LOW HIGH | -53 dBm | 11.2 dB | 0 |
| 5 | HIGH LOW HIGH | -52 dBm | 11.5 dB | 0 |
| 6 | LOW HIGH HIGH | -52 dBm | 11.2 dB | 0 |
| 7 | HIGH HIGH HIGH | -53 dBm | 10.8 dB | 0 |

The "self-echo" column is the bridge's own R1 NodeInfo TX (R1 transmits
at +20 dBm 10 cm from R2's antenna at module boot). The fact that this
self-echo RSSI is **within 4 dB across all 8 states** (-52 to -56 dBm)
strongly suggests this signal is reaching the LR1121 via PCB substrate /
supply rail / ground coupling, **not** through the
antenna→switch→LNA chain. The chain through the antenna appears to be
electrically disconnected (or its routing is independent of the
DIO5/6/7 state).

### Extended sweep — DIO5/6/7 + DIO8 (RFSW3) + DIO10 (RFSW4)

After confirming via the **Semtech LR1121 v2.1 datasheet** (rev 2.1,
Dec 2023, §4.5.1 and Table 4-1) that the chip supports up to 5 RFSWx
outputs, with the chip-fixed mapping (per Table 4-1) being:

  - **RFSW0 = chip pin 20 = DIO5**
  - **RFSW1 = chip pin 19 = DIO6**
  - **RFSW2 = chip pin 11 = DIO7**
  - **RFSW3 = chip pin 10 = DIO8**
  - **RFSW4 = chip pin 8 = DIO10** (alternate function of 32k_N; available
    when no 32 kHz crystal is used, as on the Wio-LR1121 with integrated TCXO)
  - DIO9 (chip pin 9) is **IRQ only** — no RFSWx alternate
  - DIO11 (chip pin 7) is **NC** (no alternate function) when used in
    DIO mode rather than as 32k_P

…with all RFSWx outputs defaulting to **High-Z** state until
`SetDioAsRfSwitch` (cmd 0x0112) is called.

RadioLib 7.7.0's LR11x0 driver exposes macros
`RADIOLIB_LR11X0_DIO5/6/7/8/10` for `setRfSwitchTable()` — these
correctly cover all 5 chip-level RFSWx-capable DIOs per Table 4-1.
There is no missing-pin gap in RadioLib's RFSWx coverage.

The brute-force flag was extended from 3-bit (DIO5/6/7) to 5-bit
(DIO5/6/7/8/10), exhaustively exercising all 5 chip-level RFSWx
outputs in additional targeted combinations. DIO10 is particularly
interesting because the Wio-LR1121's integrated TCXO frees it from
its 32 kHz crystal role, making it directly available as RFSW4.

| `DIOMASK` | D5 D6 D7 D8 D10 | Self-echo RSSI | Real OTA RX |
|---|---|---|---|
| 8  | 0 0 0 1 0 (DIO8 alone — RFSW3) | -49 dBm | 0 |
| 16 | 0 0 0 0 1 (DIO10 alone — RFSW4) | (none in window) | 0 |
| 24 | 0 0 0 1 1 (DIO8 + DIO10 — top hypothesis) | -50 dBm | 0 |
| 31 | 1 1 1 1 1 (all five HIGH — catch-all) | -50 dBm | 0 |

**All four extended combinations fail identically to the 3-DIO sweep.**
Self-echo RSSI variance across the full **12-iteration sweep**
(8 × 3-bit + 4 × 5-bit) is **-49 to -56 dBm — about 7 dB**, pure noise
level, completely independent of any RFSWx-capable DIO state.

This rules out **every RFSWx switch configuration that RadioLib
`setRfSwitchTable()` can apply.** (DIO11 is also an RFSW4 alternate
per the LR1121 datasheet but is not in RadioLib's
`RADIOLIB_LR11X0_DIOx(0..4)` mapping, so it cannot be reached via
the standard API.)

No real distant OTA traffic is ever decoded by R2,
even when the same RF is being received cleanly by R1 at -35 dBm.

## KiCad PCB-layout finding — DIO5/6/7 are exposed as MCU expansion, not switch controls

Reviewing the Wio-LR1121 KiCad library that Seeed publishes at
[Seeed-Studio/OPL_Kicad_Library](https://github.com/Seeed-Studio/OPL_Kicad_Library/tree/master/Seeed%20Studio%20Wio%20LR1121%20Module%20v0.9),
we noticed the module's PCB layout routes the LR1121 chip's **DIO5, DIO6,
and DIO7 pins** (Semtech UM Table 4-1: chip pins 20, 19, 11 respectively)
out to **bottom-side test pads** that are named **`MCU_DIO5`, `MCU_DIO6`,
and `MCU_DIO7`** in the design files.

The `MCU_` net-name prefix is consistent throughout the module's schematic
to denote **signals routed to the host MCU** for general-purpose use —
contrasted with `LR1121_DIO8` / `LR1121_DIO9_INT` / `LR1121_BUSY`
(LR1121-specific function pins) and `RFI_P/N` / `LP_LF` / `HP_LF` / `RFIO_HF`
(RF-functional pins) which use their own conventions.

This strongly suggests **DIO5/6/7 are not used by the module's internal RF
switch** — they are deliberately freed up by Seeed as additional host-MCU
GPIO lines via bottom-side test pads. This is consistent with our
12-iteration brute-force sweep finding: self-echo RSSI varied only ±3.5 dB
across all 8 DIO5/6/7 states and all 4 added DIO8/DIO10 states, indicating
the antenna routing is **independent of any RFSWx-capable DIO that
RadioLib can drive**.

Implications:

- The Wio-LR1121's internal RF switch (if there is one) must be controlled
  by **DIO11** (the one RFSWx-capable DIO not reachable through RadioLib's
  `RADIOLIB_LR11X0_DIOx(0..4)` mapping), **or**
- The module uses a **passive RF combining network** (diplexer / matching)
  rather than an active switch IC, with the chip's internal LNA/PA path
  selection handling mode transitions — in which case `SetDioAsRfSwitch`
  is **moot for this module** and our RX failure has a different root cause
  entirely (chip-firmware bug, undocumented init step, hardware fault).

We do not have access to the module's bottom-layer schematic detail to
distinguish these cases; only Seeed engineering does.

## Serial-log excerpt — the decisive touch-test

```
[Radio2-Edge] BUSY released after 141 ms (post 50ms boot margin)
[Radio2-Edge] installing RF switch table: MODE_RX = D5=1 D6=0 D7=0
[Radio2-Edge] ready — 906.875 MHz  BW 250.0 kHz  SF11  CR4/5  20 dBm  sync 0x2B  sub-GHz
[Radio2-Edge] getIrqFlags() post-begin = 0x00000000
[Radio2-Edge] startReceive() = 0

[789358 ms][R1 RX] 47 bytes  RSSI -36.0 dBm  SNR 6.8 dB     <-- MT phone @ touch range
[795313 ms][R1 RX] 47 bytes  RSSI -36.0 dBm  SNR 6.8 dB
[805034 ms][R1 RX] 49 bytes  RSSI -37.0 dBm  SNR 6.0 dB
[817596 ms][R1 RX] 65 bytes  RSSI -35.0 dBm  SNR 6.5 dB
[823489 ms][R1 RX] 65 bytes  RSSI -36.0 dBm  SNR 6.5 dB
[828260 ms][R1 RX] 65 bytes  RSSI -36.0 dBm  SNR 6.5 dB
[829099 ms][R1 RX] 49 bytes  RSSI -36.0 dBm  SNR 6.5 dB
[841092 ms][R1 RX] 49 bytes  RSSI -35.0 dBm  SNR 6.8 dB

[796037 ms][R2 HB] isr=22 (+1/5s) rxFlag=0
[801037 ms][R2 HB] isr=23 (+1/5s) rxFlag=0       <-- counter only climbs
[811037 ms][R2 HB] isr=25 (+2/5s) rxFlag=0           from post-TX spurious
[831037 ms][R2 HB] isr=27 (+2/5s) rxFlag=0           ISRs; pktLen=0 every
[846037 ms][R2 HB] isr=29 (+2/5s) rxFlag=0           time on readback.
```

R1 (SX1262) receives the touching phone at -35 dBm. R2 (LR1121, antenna
under the phone) sees the same RF energy at roughly -10 to -20 dBm. R2
reports zero `RX_DONE` for the duration of the test.

## Phase B — Chip initialization DOE (added 2026-05-27)

Following the 12-iteration RFSWx sweep (above), we ran a second structured
Design-of-Experiments phase covering every chip-level RX initialization
remedy that UM v2.2 prescribes. All four runs were executed on the same
bench setup (R1 = SX1262 LongFast 906.875 MHz, R2 = LR1121 LongFast2
906.875 MHz, same modem params, Meshtastic phone within ~1 m for OTA
traffic). Each run was controlled by a single `LR1121_RX_AUDIT_RUN` build
flag (0..5) compiled into `WioLR1121::begin()`. The full bench plan with
serial-log excerpts is in
[`LR1121-RX-INIT-AUDIT.md`](LR1121-RX-INIT-AUDIT.md).

### DOE results table

| Run | Treatments applied | Chip return codes | OTA `RX_DONE` | Outcome |
|---|---|---|---|---|
| **0** | None (baseline + `GetErrors()` always-on diagnostic) | — | **0** | **`errors=0x0020` = `HF_XOSC_START_ERR` persistent at POR**; zero real OTA RX |
| **2** | `SetRssiCalibration` using UM v2.2 Table 7-21 "600 MHz – 2 GHz" reference tunes, gain offset = 0 | `setRssiCalibration = 0` | **0** | Cal accepted; self-echo RSSI shifted +4 dB; zero real OTA RX |
| **3** | `CalibImage(902, 928)` after `SetTcxoMode` | `calibrateImageRejection = 0` | **0** | Cal accepted; zero real OTA RX |
| **5** | Combined: pre-`Standby(STBY_RC)` + RSSI cal + image cal + `SetRxBoostedGainMode(true)` | All four `= 0` | **0** | All accepted; persistent `errors=0x0020` unchanged; **one `RADIOLIB_ERR_CRC_MISMATCH` event observed** (signal of life) |

Runs 1 (pre-standby alone) and 4 (RxBoosted alone) were folded into Run 5
after Run 0's `errors=0x0020` was confirmed as `HF_XOSC_START_ERR`
(bit 5), **not** the `CMD_FAIL` (which lives in a different status register)
predicted by the Run 1 rationale — and after the ~2 dB benefit from
RxBoosted was confirmed insufficient alone against a sensitivity gap of
tens of dB. Both effects are present in Run 5.

### New evidence from Phase B

**1. `errors=0x0020` = `HF_XOSC_START_ERR` persistent at every POR.**
Per UM v2.2 Errors register bitfield, bit 5 (mask `0x0020`) is
`HF_XOSC_START_ERR`. The Wio-LR1121 has an integrated TCXO. Per
UM v2.2 §2.1.3, automatic POR calibration fails on TCXO-fitted chips.
The HF crystal start error appears to be a downstream symptom of the
same root cause.

- The bit is set on **every boot** of **every unit** tested.
- Pre-standby (Run 5) does **not** clear it — it is sticky from POR;
  pre-standby only changes chip mode. Would require explicit
  `ClearErrors` (UM v2.2 §7.2.11) to clear.
- Explicit `CalibImage(902, 928)` after `SetTcxoMode` (Runs 3 and 5)
  does **not** clear it and does **not** recover RX.

This is the first hard evidence we have observed of any non-zero state
in the chip's own error register during our investigation.

**2. Signal of life — `RADIOLIB_ERR_CRC_MISMATCH` event in Run 5.**

```
[148963 ms] [Radio2-Edge] read: pktLen=52 state=-7 len=0
[148963 ms] [R2 RX] ERROR -7
```

RadioLib error `-7` is `RADIOLIB_ERR_CRC_MISMATCH`. The LR1121 detected
a preamble and header **strongly enough to attempt CRC validation**,
but the payload failed CRC. This event occurred during Run 5 (with all
four treatments active), not during Runs 0 / 2 / 3, and only once in
roughly 5 minutes of OTA-strength activity from a phone within ~1 m of
the antenna.

**Interpretation:** the RX chain is **partially functional but with
severely degraded sensitivity** — estimated **40–50 dB above LR1121
datasheet spec** (LR1121 Datasheet v2.1 §6.3, sensitivity table for
sub-GHz LoRa BW 250 / SF 11). This is not a complete RX wall. It is
consistent with either an LNA-gain / matching-network mismatch
(firmware-side already exhausted) or a hardware-side issue (matching
network, switch insertion loss, LNA isolation, RF trace impedance).

**3. Self-echo RSSI across all runs (control reading).**

| Run | Self-echo RSSI (R1 NodeInfo TX at +20 dBm, ~10 cm from R2 antenna) |
|---|---|
| 0 (baseline) | –46 dBm |
| 2 (RSSI cal applied) | –42 dBm (+4 dB from cal table change) |
| 3 (image cal applied) | –46 dBm (back to baseline) |
| 5 (kitchen-sink) | –45 dBm |

The 4 dB shift on Run 2 confirms `SetRssiCalibration` took effect at
the AGC level. It still did not recover OTA-strength RX. **RSSI
calibration is therefore not the bottleneck.**

### Updated refuted-hypothesis list

| # | Hypothesis | Refuted by |
|---|---|---|
| 1 | Single defective unit | Two units, identical behavior |
| 2 | RF switch table mis-set | 12-iter sweep + Run 5 |
| 3 | Stale IRQ at boot | `getIrqFlags()` = `0x00000000` |
| 4 | `startReceive()` rejected | Returns 0 every call |
| 5 | Sensitivity floor (lab-grade noise) | Zero RX at antenna touch (–20 dBm at port) |
| 6 | Wrong RSSI calibration (LR EVK default) | Run 2 |
| 7 | POR image cal failure on TCXO chips | Run 3 |
| 8 | RX gain mode | Run 4 (in Run 5) |
| 9 | Switch table install outside STBY_RC | Run 1 (in Run 5) |
| 10 | Combined firmware remedies (UM v2.2) | Run 5 |

**Provisional conclusion:** the LR1121's antenna → integrated-switch →
LNA → demodulator chain is electrically intact, but RX sensitivity is
degraded by tens of dB. All firmware remedies prescribed by UM v2.2
have been tested. None resolves the failure. Remaining hypothesis
space is **hardware-design issue** (matching network, switch insertion
loss, LNA isolation, RF trace impedance) or **LR1121 chip firmware
errata** at base FW version 1.3.

## Questions for Seeed engineering

**Status legend (added 2026-05-28 after David Du's reply — see [`SEEED_EMAIL_DRAFT.md`](SEEED_EMAIL_DRAFT.md) "Inbound replies received" for the full verbatim response):**

- ✅ **ANSWERED** — Seeed engineering provided a definitive answer; question is closed.
- 🟡 **PARTIALLY ANSWERED** — Reply addressed the question implicitly or partially; remaining detail re-asked in the 2026-05-28 follow-up at [`SEEED_EMAIL_REPLY_2026-05-28.md`](SEEED_EMAIL_REPLY_2026-05-28.md).
- ⏳ **OPEN** — No reply yet; re-asked in the 2026-05-28 follow-up.

| # | Topic | Status |
|---|---|---|
| 1 | Where IS the RF switch and how is it controlled | ✅ **ANSWERED 2026-05-28** |
| 2 | Recommended `SetRssiCalibration` values for Wio-LR1121 PCB | ⏳ **OPEN** (re-asked) |
| 3 | Additional chip-level command sequence for sub-GHz RX | 🟡 **PARTIALLY ANSWERED** (no additional command implied; re-confirmation requested) |
| 4 | Customer-report history / firmware-update availability | ⏳ **OPEN** (re-asked as base FW 1.3 update path question) |
| 5 | Are DIO5/6/7 wired to the on-module RF switch | ✅ **ANSWERED 2026-05-28** (DIO5/6 wired as V1/V2 of SKY13373-460LF; DIO7 not used by the switch) |
| 6 | Lot-number commonality across the two test units | ⏳ **OPEN** |

---

### 1. ✅ ANSWERED 2026-05-28 — Where IS the Wio-LR1121's RF switch, and how is it controlled?

**Seeed reply (David Du, Sensecap Support):** the on-module RF switch is a **Skyworks SKY13373-460LF SP3T antenna switch** (Skyworks document 310060742; datasheet attached to David's reply, copy saved at [`docs/datasheets/310060742_SKYWORKS_SKY13373-460LF_Datasheet.pdf`](docs/datasheets/310060742_SKYWORKS_SKY13373-460LF_Datasheet.pdf)). Internal wiring per David: **switch pin 4 (V1) is wired to LR1121 DIO5, switch pin 5 (V2) is wired to LR1121 DIO6.** The authoritative truth table is:

| V1 (DIO5) | V2 (DIO6) | Switch state |
|---|---|---|
| 0 | 0 | Shutdown (with 20 µs entry/recovery timing — see SKY13373 datasheet) |
| 1 | 0 | RFI_P_LF & RFI_N_LF (**RX**) |
| 0 | 1 | RFO_HP_LF (**TX high-power**) |
| 1 | 1 | RFO_LP_LF (**TX low-power**) |

Our pre-reply 12-iteration RFSWx sweep had already been driving DIO5=1, DIO6=0 (the confirmed RX state) under `LR1121_BRUTEFORCE_RX_DIOMASK=1`, so the switch was correctly configured during all earlier bench runs. The sweep evidence against the switch-as-fix hypothesis stands; David's reply confirms it independently. **The switch hypothesis is officially closed.** See bench evidence at [`LR1121-RX-INIT-AUDIT.md` § Run 7 — Authoritative Seeed reply received](LR1121-RX-INIT-AUDIT.md#run-7--authoritative-seeed-reply-received-2026-05-28).

**Original question text retained below for the inquiry record:**

   

   Per the **Semtech LR1121 v2.1 datasheet** (rev 2.1, Dec 2023, §4.5.1
   and Table 4-1), the chip supports up to 5 RFSWx outputs with the
   chip-fixed pin assignment **RFSW0=DIO5, RFSW1=DIO6, RFSW2=DIO7,
   RFSW3=DIO8, RFSW4=DIO10** (DIO9 is IRQ-only; DIO11 has no DIO-mode
   alternate function), all defaulting to **High-Z** until
   `SetDioAsRfSwitch` (cmd 0x0112) is called.

   RadioLib 7.7.0's `RADIOLIB_LR11X0_DIOx(0..4)` macros correctly
   cover all 5 chip-level RFSWx-capable DIOs. We have now bench-tested
   all 5 in 12 combinations covering every meaningful switch-state
   permutation — and **every combination produced identical failure
   with the self-echo RSSI invariant within ~7 dB (noise level)**.
   Combined with the KiCad layout finding above (DIO5/6/7 routed as
   `MCU_DIO*` host-expansion pads, deliberately freed for user GPIO
   precisely because the module doesn't use them as switch outputs),
   this exhausts the entire chip-level RFSWx software-control
   possibility space. The module's antenna routing is **conclusively
   independent of any RFSWx software state** the chip can be programmed
   into via `setRfSwitchTable()`.

   Given that finding, the diagnostic question has changed from "what's
   the truth table?" to "**is there an internal RF switch on the
   Wio-LR1121 that needs software configuration at all, and if so, what
   controls it?**" Specifically:

   - **(a) Does the module have an active switch controlled by *any*
     non-RFSWx DIO?** Per Semtech v2.1 Table 4-1 the chip's RFSWx
     outputs are only on DIO5/6/7/8/10, all of which we have swept
     exhaustively. If the module's switch is somehow driven by a
     non-RFSWx DIO (e.g. one of DIO1-DIO4 used as a general GPIO
     output that the host MCU is expected to toggle from outside the
     chip's RFSWx logic), we have no way to discover that from the
     published documentation. This would be unusual but not
     impossible — please confirm.

   - **(b) Or does the module use a passive RF combining network**
     (diplexer / matching circuit) rather than an active switch IC,
     with the chip's internal LNA/PA path-selection logic handling mode
     transitions? In that case `SetDioAsRfSwitch` would be **moot** for
     this module and our RX failure has a different root cause entirely
     — an undocumented init step, a chip-firmware bug, or a hardware
     fault in the RF path.

   - **(c) Or is there a hardware-level enable / mode-select signal we
     should be driving** from the host MCU that we're missing entirely?

   **A definitive answer on which of (a)/(b)/(c) applies — plus, if (a),
   the truth table for DIO11 across the LR1121 operating modes (STBY /
   sub-GHz RX / sub-GHz TX low-power / sub-GHz TX high-power / 2.4 GHz
   TX / 2.4 GHz RX) — is the most important single piece of information
   we can receive from Seeed engineering.** It determines whether the
   solution is a firmware patch (RadioLib needs DIO11 support added),
   an alternate driver path (raw SPI `SetDioAsRfSwitch` issued manually
   with DIO11 in the bitmask), or a fundamentally different RX-bring-up
   procedure.

### 2. ⏳ OPEN (re-asked in 2026-05-28 follow-up) — Recommended `SetRssiCalibration` (cmd 0x0229) values for the Wio-LR1121's PCB

**What are the recommended `SetRssiCalibration` (cmd 0x0229) values
   for the Wio-LR1121's PCB?** Per Semtech LR1121 User Manual v2.2
   §7.2.15, the LR1121's automatic LNA gain control uses RSSI to pick
   a gain level, and "*An incorrect gain can result in a missed
   detection (packet loss) or decreased resistance to interference.*"
   The UM further states: "*By default, the chip is calibrated for
   the 868-915MHz band on the LR1121 EVK... **The RSSI must be
   calibrated for each hardware type.***"

   The Wio-LR1121's matching network and front-end are different from
   the LR1121 EVK reference, so the default calibration is incorrect
   for our PCB. Without per-PCB calibration, the AGC picks the wrong
   LNA gain, causing the missed-packet symptom we observe — this is a
   strong candidate root cause and directly matches our bench
   observation. **Can Seeed provide the Gain Tune values (G4..G13,
   G13hp1..hp7) and Gain Offset that the Wio-LR1121's hardware design
   requires, ideally measured on a production unit per the procedure
   in UM §7.2.15?** This would let LR1121-on-Wio-LR1121 firmware ship
   with the correct calibration baked in.

### 3. 🟡 PARTIALLY ANSWERED 2026-05-28 — Additional chip-level command sequence for sub-GHz RX

David's reply confirmed the SKY13373 switch-routing question definitively but did not explicitly state whether any *additional* chip-level command sequence is required beyond `SetDioAsRfSwitch` + `SetRssiCalibration` + `SetRx`. By implication (no other command was mentioned as missing) the answer appears to be "no, nothing else is needed at the chip level." The 2026-05-28 follow-up re-asks this implicitly through question 3 ("HF_XOSC_START_ERR expected behaviour") which targets the same chip-init-completeness concern. **Original question text:**

**Is there a chip-level command sequence required to activate the
   sub-GHz RX path** beyond `SetDioAsRfSwitch` (0x0112) + `SetRssiCalibration`
   (0x0229) + `SetRx` (0x0209)? Some Semtech reference designs require
   additional steps such as setting `SetLnaConfig` or selecting the
   RFI input — our reading of the LR1121 UM v2.2 suggests these
   should be defaults, but our bench evidence (12 RFSWx combinations
   all failing identically) indicates either an undocumented init
   step is needed, or something in the chip's RX path is not engaging.

### 4. ⏳ OPEN (re-asked in 2026-05-28 follow-up) — Customer-report history and firmware update path

**Has Seeed seen this symptom on the Wio-LR1121 in customer reports?**
   If so, is there a known workaround, errata, or recommended firmware
   pattern? Our modules report `Base FW version: 1.3` via `GET_VERSION`
   (cmd 0x0303). Per UM v2.2 §2.3.1, "*Shipping versions of the LR1121
   after production test is rev 01.01. It is advised to update the
   firmware with the latest firmware.*" — has firmware been updated
   beyond 01.03 since our modules shipped, and if so does it address
   any known RX-path issue?

### 5. ✅ ANSWERED 2026-05-28 — Are DIO5/6/7 wired to the on-module RF switch

**Seeed reply:** **DIO5 and DIO6 are wired** to the SKY13373-460LF switch (as V1 and V2 respectively — see question 1 above for the full truth table). **DIO7 is not used by the on-module RF switch** on this module variant. This is consistent with the published Seeed KiCad library showing DIO5/6/7 exposed as `MCU_DIO*` test pads — DIO7 is genuinely free for host-MCU use; DIO5/6 are used internally by the switch despite also being exposed on bottom-side pads. **Original question text:**

**Can you confirm whether DIO5/6/7 (or any other internal DIOs) are
   in fact wired to the on-module RF switch on the Wio-LR1121?** The
   datasheet pinout confirms DIO5/6/7 are exposed as `MCU_DIO5/6/7`
   on bottom-side test pads (per Seeed's published KiCad library),
   suggesting they are not the switch-control pins. UM v2.2 §4.2.1
   confirms RFSW0..RFSW4 map to DIO5/6/7/8/10 at the chip level — but
   the *module's* internal antenna routing (between the chip's RF pins
   and the module's SUBG_RF / 2.4G_RF pads) is opaque to us. Is there
   an active switch IC, or is the front-end passive combining?

### 6. ⏳ OPEN — Production-lot commonality across the two test units

**Is the second Wio-LR1121 module we ordered likely from the same
   production lot as the first?** If you can confirm or deny this, it
   helps us decide whether to order a third unit from a separate
   distributor (different lot likely) or pivot to a different LR1121
   carrier design.

## What we are not asking

- This is **not** an RMA request — the modules are functioning electrically
  (chip responds to commands, TX radiates, BUSY behaves correctly). We
  suspect either a documentation gap or a non-obvious chip-level config
  requirement.
- This is **not** a complaint — the Wio-LR1121 is a compact and capable
  module, and we want to use it in production. We need the missing piece
  of configuration knowledge to make RX work.

## Contact

GrayHatGuy — `grayhatguyllc@protonmail.com`
Project repository: <https://github.com/GrayHatGuy/Xiao-esp32s3-lora-repeater>
Relevant source files (for engineering review if useful):
- `src/WioLR1121.cpp` — module driver wrapper.
- `LR1121-SPEC.md` — full Phase 1 status with the bench-test detail.
- `platformio.ini` — `LR1121_BRUTEFORCE_RX_DIOMASK` build flag we are
  using to walk through all 8 possible DIO5/6/7 MODE_RX combinations.

We are happy to share fuller serial logs, scope captures, or run any
additional bench tests Seeed engineering suggests.

Thank you for your time.
