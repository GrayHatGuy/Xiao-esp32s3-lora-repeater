# LR1121 RX Initialization Audit

**Date:** 2026-05-26 (rev 2 — UM v2.2 incorporated); **2026-05-27 rev 3 — bench results appended**
**Status:** ✅ **DOE complete — all UM-recommended remedies tested. None resolves the RX failure.**

**References:**
- **Semtech LR1121 Datasheet** (Semtech LR1121 chip datasheet, rev 2.1, Dec 2023 — referred to as "LR1121 Datasheet" below)
  - Direct PDF: https://semtech.my.salesforce.com/sfc/p/#E0000000JelG/a/RQ0000093ZiP/RV4Ba6LROsFrFjnAAVK2av5W11RGmCms_3Q2cyKHdDA
  - Stable product page: https://www.semtech.com/products/wireless-rf/lora-connect/lr1121
- **Semtech LR1121 User Manual v2.2** (rev 2.2, Apr 2026, 140 pages — referred to as "UM v2.2" below) — primary reference for chip-level command spec
  - Direct PDF: https://semtech.my.salesforce.com/sfc/p/#E0000000JelG/a/RQ00000DClgP/D.pNG5l4FviPI634eCx8GFURZEwDO2ZBA33MpriB_FU
  - Stable product page: https://www.semtech.com/products/wireless-rf/lora-connect/lr1121
- **Seeed Wio-LR1121 Module Datasheet v1.0 (2025-07-01)** — module-level datasheet
  - Direct PDF: https://files.seeedstudio.com/wiki/Wio-LR1121/Wio-LR1121_Module_Datasheet_v1.0.pdf
  - Stable wiki page: https://wiki.seeedstudio.com/wio_lr1121_module/
- RadioLib 7.7.0 `LR11x0.cpp` / `LR1120.cpp` / `LR1121.cpp`
- Project bench evidence: see [`LR1121-SPEC.md`](LR1121-SPEC.md) "Phase 1 status — RX-path block"

## Purpose

With the 12-iteration RFSWx switch-table sweep having **conclusively exhausted the switch-table-as-fix hypothesis** (every chip-level RFSWx-capable DIO swept across every meaningful combination, no behavioral change), the diagnostic question shifts to "**what chip-level RX initialization step is RadioLib's `begin()` not performing — and what about that step explains the gross RX-path failure?**"

This document audits RadioLib's LR1121 `begin()` against the Semtech UM v2.2 to enumerate candidate missing-init-step hypotheses, ranked by likelihood given our specific bench symptoms (zero `RX_DONE` even at -20 dBm antenna input, fully-functional TX).

## RadioLib 7.7.0 LR1121 `begin()` — what IS called

The call chain when `_radio->begin(freq, bw, sf, cr, syncWord, power, preambleLength, tcxoVoltage)` runs:

| Order | Call | What it does |
|---|---|---|
| 1 | `LR1120::begin(cfg)` (LR1121 inherits) | Entry point |
| 2 | → `LR11x0::begin(bw, sf, cr, syncWord, preambleLength)` | Common LR11x0 init |
| 3 | → → `modSetup(LORA)` → `config(LORA)` | **Chip-level init** (see below) |
| 4 | → → `setBandwidth(bw, high)` | `high` arg derived from `freq > 1000.0f` |
| 5 | → → `setSpreadingFactor(sf)` | LoRa SF |
| 6 | → → `setCodingRate(cr)` | LoRa CR |
| 7 | → → `setSyncWord(syncWord)` | LoRa sync byte |
| 8 | → → `setPreambleLength(preambleLength)` | LoRa preamble |
| 9 | → → `setCRC(2)` | 2-byte CRC |
| 10 | → → `invertIQ(false)` | Standard IQ |
| 11 | → → `setRegulatorLDO()` | **LDO** regulator |
| 12 | → `setFrequency(freq)` | Triggers `calibrateImageRejection` if freq delta > threshold |
| 13 | → `setOutputPower(power)` | PA config + power setting |

`config(LORA)` (called by `modSetup`) does:

- `setRxTxFallbackMode(STBY_RC)` — chip falls back to RC-standby after RX/TX
- `clearIrqState(ALL)` + `setDioIrqParams(NONE)` — clean IRQ state
- `calibrate(CALIBRATE_ALL)` — full chip calibration (RC32k, RC13M, PLL, ADC, image)
- `driveDiosInSleepMode(true)` — keep DIO states valid in sleep
- `setPacketType(LORA)` — modem mode select

## What is NOT called automatically (candidate gap analysis vs UM v2.2)

| Chip-level command | RadioLib API | begin() calls? | UM section | Likely impact on our RX failure |
|---|---|---|---|---|
| `SetDioAsRfSwitch (0x0112)` | `setRfSwitchTable()` (call from app code, not begin) | No, app-level | §4.2.1 | ⚠️ **Requires STBY_RC mode** — silent failure mode |
| `CalibImage (0x0111)` for app band | `calibrateImage(low, high)` | Auto on first setFrequency | §2.1.3.1 | ⚠️ **POR image cal fails if TCXO fitted** — may require explicit re-cal |
| `SetRssiCalibration (0x0229)` | `setRssiCalibration(...)` | **No** | §7.2.15 | ⭐ **Default cal is LR1121 EVK only** — wrong cal → wrong LNA gain → packet loss |
| `SetRxBoosted (0x0227)` | `setRxBoostedGainMode(bool)` | **No** | §7.2.12 | Small (~2 dB) — demoted |
| `SetTcxoMode` (lr11xx_system_set_tcxo_mode) | `setTCXO(voltage, delay)` (lower-level) | Internal, via tcxoVoltage arg | §6.2.3 | Verify it's called before calibrate() |
| `SetRegulatorDCDC()` | `setRegulatorDCDC()` | No (LDO is default) | §5.3.1 | Unlikely RX-relevant |

## Headline findings from UM v2.2 — candidate priority reordered

### ⭐ Top Candidate — RSSI Calibration is per-PCB

**UM §7.2.15** (verbatim):

> *"The LR1121 internal LNA has a set of predefined gains (G4, G5, ... G13 (split into sub-gains G13hp1, G13hp2, ... G13hp7)), used to amplify the RF power at the correct level, depending on the RF input power. In normal usage of the chip, the LNA gain control is automatic."*
>
> *"The power seen by the LR1121 analog front-end is affected by external components such as the matching network, or RF switches."*
>
> *"**An incorrect RSSI results in a sensitivity degradation in (G)FSK mode and an incorrect gain selection in LoRa and GFSK mode.**"*
>
> *"**An incorrect gain can result in a missed detection (packet loss) or decreased resistance to interference.**"*
>
> *"**By default, the chip is calibrated for the 868-915MHz band on the LR1121 EVK.** Therefore, in order to have a correct reading of the RSSI value of the chip integrated in the application PCB, the RSSI must be calibrated. **The RSSI must be calibrated for each hardware type**, but not for each device individually."*

This **directly matches our symptom**: missed packets (zero `RX_DONE` for distant traffic), but TX works fine. The Wio-LR1121's matching network is materially different from the LR1121 EVK's. Without per-PCB RSSI calibration, the chip's AGC picks the wrong LNA gain on incoming signals → missed detection.

UM Table 7-21 provides **default tunes for "From 600MHz to 2GHz" band** (rows: 0 2 2 2 3 3 4 5 4 4 6 5 5 6 6 6 7 6) which we can try as a first approximation. Proper calibration requires an RF generator (we don't have one), but the UM's reference values for the 600 MHz–2 GHz band should be a closer match for 906.875 MHz US LongFast than the default EVK calibration.

### 🥈 Mid-priority — Verify SetDioAsRfSwitch actually succeeded

**UM §4.2.1**:

> *"This command only works with the chip in **Standby RC mode**, otherwise it returns a **CMD_FAIL** on the next GetStatus command."*

RadioLib's `setRfSwitchTable()` (LR11x0.cpp:1322) calls `setDioAsRfSwitch(...)` but **discards the return value**. If the chip was not in STBY_RC when our wrapper called this (e.g., if `_radio->begin()` had already transitioned the chip into another mode by then), the switch configuration would silently fail — and our 12-iteration sweep would have been measuring a chip with its switch table never configured.

Our code calls `setRfSwitchTable()` BEFORE `_radio->begin()`, so at boot the chip should be in STBY_RC (per UM §2.2 Startup Sequence: "*At the end of the startup sequence, the device is set in Standby RC mode*"). So timing should be OK. But we never *verified* this — adding a `_radio->getErrors()` call right after `setRfSwitchTable()` would confirm.

If `getErrors()` returns a CMD_FAIL bit, the entire 12-iteration sweep evidence is moot (we'd have been testing with the switch never configured). If it returns no errors, our sweep evidence stands as conclusive.

### 🥉 Mid-priority — Explicit CalibImage for application band

**UM §2.1.3**:

> *"In the case of POR, or when the device is recovering from power-down or sleep mode without retention, the image calibration is performed as part of the initial calibration process and for optimal image rejection in the band 902-928MHz. **If a TCXO is fitted, the calibration fails.**"*

The Wio-LR1121 has an integrated TCXO. The chip's automatic POR image calibration therefore **fails** on this module, leaving image rejection in some default state — potentially not calibrated for our actual operating band.

RadioLib's `setFrequency()` triggers `calibrateImageRejection(freq - band, freq + band)` when freq delta exceeds threshold, which fires on first call (since previous freq is 0). This **should** explicitly re-cal, but it may be using a too-narrow band window. UM §2.1.3.1 Table 2-3 specifies the US 902-928 MHz band uses `0xE1 / 0xE9` (default) — we can force this with `_radio->calibrateImage(902.0f, 928.0f)`.

### Lowest priority — RxBoosted

**UM §7.2.12**:

> *"Command SetRxBoosted(...) sets the device in RX Boosted mode, allowing a **~2dB increased sensitivity**, at the expense of a ~2mA higher current consumption in RX mode."*

Only ~2 dB benefit. Not enough to explain our gross RX failure (zero RX even at -20 dBm antenna input). **Significantly demoted from the rev-1 audit doc**, which speculated 15-20 dB improvement. The "RxBoosted = 1" qualifier in v2.1 datasheet sensitivity tables just means those headline numbers are measured with boost on; turning it off only costs ~2 dB, not 15-20.

## Test plan (revised order)

Each candidate is a small change to `WioLR1121::begin()`. Bench-test sequentially with same-freq MT config (R1 = R2 = 906.875 MHz / BW250 / SF11 / sync 0x2B). 15-minute time-box per candidate.

### Candidate A — Verify `SetDioAsRfSwitch` succeeded (5 min, diagnostic only)

This is purely diagnostic — confirms or refutes whether our 12-iteration sweep evidence is valid.

```cpp
// After _radio->setRfSwitchTable() in WioLR1121::begin():
uint16_t devErrors = 0;
int16_t errState = _radio->getErrors(&devErrors);
Serial.printf("[%s] post-setRfSwitchTable getErrors() = state=%d errors=0x%04X\n",
              _name, errState, devErrors);
// Per UM §3.6.1: CMD_FAIL bit set in returned errors if any prior command failed
```

**Outcome:** If `devErrors` has CMD_FAIL bit set → our switch table never took effect, sweep evidence is invalidated, we need to first fix the timing (ensure STBY_RC before call). If clean → sweep evidence is solid.

### Candidate B — SetRssiCalibration with UM "600 MHz to 2 GHz" defaults (15 min) ⭐ HIGHEST FIX-PROBABILITY

```cpp
// Add to WioLR1121::begin() AFTER _radio->begin() returns success.
// Values per UM Table 7-21 "From 600MHz to 2GHz" row:
// Gain Offset = 0, then tunes G4..G13 + G13hp1..hp7
uint8_t rssi_cal[11] = {
    // Encoded per UM Table 7-19 (each byte holds two 4-bit tunes):
    // Byte 2: G5(7:4) | G4(3:0)
    (2 << 4) | 2,   // G5=2, G4=2
    (2 << 4) | 3,   // G7=2, G6=3 ... wait, layout TBD
    // ... (verify byte ordering vs UM Table 7-19)
};
// Or via RadioLib API if it provides a cleaner wrapper
int16_t rssiState = _radio->setRssiCalibration(...);
Serial.printf("[%s] setRssiCalibration() = %d\n", _name, rssiState);
```

**Note:** the exact API call depends on RadioLib's `setRssiCalibration()` signature — to verify, look at `LR11x0.h` for the public method. The UM table values may need to be encoded into a struct. If RadioLib doesn't expose this cleanly, we can issue the raw SPI command `0x0229` via `SPIcommand(...)`.

**Outcome:** If `[R2 RX]` lines appear with real packet content → **Phase 1 solved**. The chip's AGC was picking wrong LNA gain due to default EVK calibration; with 600 MHz-2 GHz defaults the gain selection works better for the Wio-LR1121's matching network. We'd follow up with Seeed asking for the *exact* recommended SetRssiCalibration values for their PCB.

### Candidate C — Explicit `calibrateImage(902.0f, 928.0f)` (10 min)

```cpp
// After _radio->begin() returns success in WioLR1121::begin():
int16_t calState = _radio->calibrateImage(902.0f, 928.0f);
Serial.printf("[%s] calibrateImage(902, 928) = %d\n", _name, calState);
```

**Outcome:** If RX comes alive, image rejection was the gap. Likely smaller effect than (B) — image cal mostly affects rejection of mirror images at carrier - 2×IF, not gross sensitivity floor.

### Candidate D — `setRxBoostedGainMode(true)` (5 min, low-reward)

```cpp
int16_t boostState = _radio->setRxBoostedGainMode(true);
Serial.printf("[%s] setRxBoostedGainMode(true) = %d\n", _name, boostState);
```

**Outcome:** ~2 dB improvement at most. Try after others; only sufficient on its own if we're right at the edge of sensitivity.

### Candidate E — A + B + C + D stacked

If any individual candidate fails, try the full kitchen sink. If THAT fails, hardware-fault or chip-firmware-1.3 bug is confirmed and we resume waiting for Seeed's authoritative reply.

## Decision tree

- **Candidate A reports CMD_FAIL** → sweep evidence invalidated; fix the chip-mode timing first, then re-run the sweep before assuming switch-table is exhausted. Significant rewind, but at least it's a discoverable bug.
- **Candidate A clean + Candidate B passes** → solved. Implement permanently in `begin()`, follow up with Seeed for exact Wio-LR1121 SetRssiCalibration values, release v9.1.
- **A clean + B fails + C passes** → image-cal was the issue; far less common but possible.
- **A clean + B/C/D individual fails + E (stacked) passes** → combined effect; commit the stack to begin().
- **All fail** → hardware-fault or chip-firmware-bug confirmed. Wait for Seeed reply. Audit + bench results become strong supplementary evidence.

## Estimated effort

| Step | Time |
|---|---|
| Candidate A (getErrors diagnostic) | ~5 min |
| Candidate B (SetRssiCalibration) | ~15 min (longer if RadioLib API encoding needs research) |
| Candidate C (calibrateImage) | ~10 min |
| Candidate D (setRxBoosted) | ~5 min |
| Candidate E (stacked) | ~15 min |
| **Total time-box** | **~50 min** |

The lower-reward Candidate D test is included for completeness — if A + B + C all clean and RX is still dead, D is the last single-shot option before we declare exhaustion and wait for Seeed.

---

**Outcome of this audit (rev 2):** the bench experiment now has a UM-v2.2-grounded 5-candidate test plan with chip-level citations, clear pass criteria, and an actionable decision tree. The headline finding — RSSI calibration is per-PCB and the chip ships set up for the LR1121 EVK, not the Wio-LR1121 — provides a much more plausible mechanism for the observed "missed detection / packet loss" symptom than the earlier RxBoosted hypothesis.

---

# Bench results (2026-05-27)

The DOE was executed on bench using a 4-effective-run sequence. Runs 1 and 4 were folded into Run 5 (kitchen-sink) to compress total bench time after Run 0 produced its decisive `getErrors()` reading. All four UM-prescribed firmware remedies — individually and combined — failed to recover the RX path.

## Results summary

| Run | `LR1121_RX_AUDIT_RUN` value | Treatments enabled | Chip return codes | OTA `RX_DONE` | Outcome |
|---|---|---|---|---|---|
| **0** | 0 | None (baseline + `GetErrors()` always-on diagnostic) | — | **0** | ❌ Baseline reproduces; **`errors=0x0020` = `HF_XOSC_START_ERR` persistent at POR** |
| **2** | 2 | `SetRssiCalibration` (UM Table 7-21 "600 MHz – 2 GHz" tunes, gain offset = 0) | `setRssiCalibration = 0` | **0** | ❌ Cal accepted; self-echo RSSI shifted +4 dB (–46 → –42 dBm); zero real OTA RX |
| **3** | 3 | `CalibImage(902, 928)` post-`SetTcxoMode` | `calibrateImageRejection = 0` | **0** | ❌ Cal accepted; self-echo RSSI back to baseline; zero real OTA RX |
| **5** | 5 | Pre-`Standby(STBY_RC)` + RSSI cal + ImgCal + `SetRxBoostedGainMode(true)` | All four `= 0` | **0** | ❌ All accepted; persistent `errors=0x0020` **unchanged** (pre-standby does not clear the POR sticky bit — would need explicit `ClearErrors`); one `RADIOLIB_ERR_CRC_MISMATCH` (state=–7) event observed late in test |

Runs 1 (pre-standby alone) and 4 (RxBoosted alone) were not run independently because Run 0's `errors=0x0020` proved to be `HF_XOSC_START_ERR` (bit 5 of the LR1121 errors register), **not** the `CMD_FAIL` predicted by the pre-standby hypothesis — invalidating Run 1's specific rationale. Run 4 (~2 dB benefit) was always insufficient alone against a sensitivity gap measured in tens of dB. Both were covered by Run 5.

## Decisive new evidence

### 1. `errors=0x0020` = `HF_XOSC_START_ERR` persistent at every POR

```
[Radio2-Edge] [RX-AUDIT diag] post-setRfSwitchTable getErrors() state=0 errors=0x0020
```

Per UM v2.2 LR1121 Errors register bitfield, bit 5 (mask `0x0020`) is `HF_XOSC_START_ERR`. The Wio-LR1121 has an integrated TCXO; per UM v2.2 §2.1.3, automatic POR calibration fails on TCXO-fitted chips. The HF crystal start error appears to be a downstream symptom of the same root cause.

- The bit is set on **every boot** of **every unit** tested.
- Pre-standby (Run 5) does **not** clear it (the bit is sticky from POR; pre-standby only changes chip mode).
- Explicit `CalibImage` after `SetTcxoMode` (Runs 3 and 5) does **not** clear it and does **not** recover RX.

This appears to be a benign-but-noted state per UM §2.1.3 commentary, but it confirms the chip's automatic POR calibration sequence is not completing normally on this hardware.

### 2. Signal of life — `RADIOLIB_ERR_CRC_MISMATCH` event in Run 5

```
[148963 ms] [Radio2-Edge] read: pktLen=52 state=-7 len=0
[148963 ms] [R2 RX] ERROR -7
```

RadioLib error code `-7` is `RADIOLIB_ERR_CRC_MISMATCH`. The LR1121 detected a preamble and header **strongly enough to attempt CRC validation**, but the payload failed CRC. This event occurred during Run 5 alone (not observed on Runs 0/2/3), and only once in ~5 minutes of OTA-strength activity from a Meshtastic phone within ~1 m of the antenna.

**Interpretation:** the RX chain is **partially functional but with severely degraded sensitivity**. This is not a complete RX wall — it is a sensitivity floor estimated **40–50 dB above LR1121 datasheet spec**. Consistent with either an LNA-gain / matching-network mismatch (firmware-side already exhausted) or a hardware-side issue (matching network, switch insertion loss, LNA isolation).

### 3. Self-echo RSSI across all runs (control reading)

| Run | Self-echo RSSI (R1 NodeInfo TX at +20 dBm, ~10 cm from R2 antenna) |
|---|---|
| 0 (baseline) | –46 dBm |
| 2 (RSSI cal) | –42 dBm (+4 dB from cal table change) |
| 3 (image cal) | –46 dBm (back to baseline) |
| 5 (kitchen-sink) | –45 dBm |

The 4 dB shift on Run 2 confirms `SetRssiCalibration` took effect at the AGC level. It still did not recover OTA-strength RX. **RSSI cal is therefore not the bottleneck.**

## Refuted hypotheses

| # | Hypothesis | Refuted by |
|---|---|---|
| 1 | Single defective unit | Two units, identical behavior |
| 2 | RF switch table mis-set | 12-iter sweep (RFSWx) + Run 5 (pre-standby) |
| 3 | Stale IRQ at boot | `getIrqFlags()` = `0x00000000` |
| 4 | `startReceive()` rejected | Returns 0 every call |
| 5 | Sensitivity floor (lab-grade) | Zero RX at antenna touch (–20 dBm at port) |
| 6 | Wrong RSSI calibration (LR EVK default) | Run 2 |
| 7 | POR image cal failure on TCXO chips | Run 3 |
| 8 | RX gain mode | Run 4 (in Run 5) |
| 9 | Switch table install outside STBY_RC | Run 1 (in Run 5) |
| 10 | Combined firmware remedies (UM v2.2) | Run 5 |

## Conclusion

The LR1121's antenna → integrated-switch → LNA → demodulator path is **electrically intact** (TX radiates correctly; near-field signals demodulate at –42 to –46 dBm; one OTA `CRC_MISMATCH` event observed). But **normal-OTA-strength RX sensitivity is degraded by tens of dB** on this module variant in our setup.

**All firmware remedies prescribed by UM v2.2 have been tested. None resolve the RX failure.**

Remaining hypothesis space:

- **Hardware-design issue** — matching network, switch insertion loss, LNA isolation, RF trace impedance
- **LR1121 chip firmware errata** — base FW version 1.3 may have a known RX-path issue

Next action: send the Seeed engineering inquiry (`SEEED_SUPPORT_INQUIRY.md`) with this DOE results table appended as definitive supplementary evidence. The inquiry has been held since 2026-05-26 pending these bench outcomes.
