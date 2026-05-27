# Seeed Support Inquiry — Wio-LR1121 RX Path Non-Functional

**To:** sensecap@seeed.cc
**Re:** Wio-LR1121 Module (SKU 113991415, IPEX antenna variant) — sub-GHz
RX produces no `RX_DONE` events; TX confirmed working.
**Hardware:** Two units, ordered on separate occasions, identical behavior.
**Date:** 2026-05-26

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

After confirming via the Semtech LR1121 user manual §4.5.2 that the
chip supports up to 5 RFSWx outputs (RFSW0=DIO5, RFSW1=DIO6,
RFSW2=DIO7, RFSW3=DIO8, RFSW4=DIO10), the brute-force flag was
extended from 3-bit to 5-bit and four additional targeted
combinations were tested. DIO10 is particularly interesting because
the Wio-LR1121's integrated TCXO frees it from the 32 kHz crystal
role, making it a plausible internal RF-switch candidate.

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

## Questions for Seeed engineering

1. **What is the correct RF switch truth table for the Wio-LR1121's
   integrated front-end?** Specifically, what state should DIO5, DIO6,
   DIO7 (and DIO8 if applicable) be driven to for each of the LR1121
   operating modes — STBY, sub-GHz RX, sub-GHz TX (low power), sub-GHz TX
   (high power), 2.4 GHz TX, 2.4 GHz RX? The Wio-LR1121 Module Datasheet
   (v1.0, 2025-07-01) does not publish this table, and the KiCad library
   on the Seeed Studio OPL repository only contains the part footprint,
   not the module's internal schematic.

2. **Is there a chip-level command sequence required to activate the
   sub-GHz RX path** beyond `SetDioAsRfSwitch` (0x0112) + `SetRx`
   (0x0209)? Some Semtech reference designs require additional steps such
   as setting `SetLnaConfig` or selecting the RFI input — our reading of
   the LR1121 user manual suggests these should be defaults, but our
   bench evidence indicates otherwise.

3. **Has Seeed seen this symptom on the Wio-LR1121 in customer reports?**
   If so, is there a known workaround, errata, or recommended firmware
   pattern?

4. **Can you confirm whether DIO5, DIO6, and DIO7 are in fact wired to
   the on-module RF switch on the Wio-LR1121?** The datasheet pinout
   confirms DIO5/6/7 are not exposed on module pads (i.e. they are
   reserved for internal use), but does not state which pins drive which
   switch input — or even confirm that DIO5/6/7 are the switch-control
   pins (vs, say, DIO8 + an internal pull network).

5. **Is the second Wio-LR1121 module we ordered likely from the same
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

GrayHatGuy — `jrussell328@gmail.com`
Project repository: <https://github.com/GrayHatGuy/Xiao-esp32s3-lora-repeater>
Relevant source files (for engineering review if useful):
- `src/WioLR1121.cpp` — module driver wrapper.
- `LR1121-SPEC.md` — full Phase 1 status with the bench-test detail.
- `platformio.ini` — `LR1121_BRUTEFORCE_RX_DIOMASK` build flag we are
  using to walk through all 8 possible DIO5/6/7 MODE_RX combinations.

We are happy to share fuller serial logs, scope captures, or run any
additional bench tests Seeed engineering suggests.

Thank you for your time.
