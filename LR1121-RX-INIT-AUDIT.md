# LR1121 RX Initialization Audit

**Date:** 2026-05-26
**Status:** Test plan for Phase 1 RX bring-up
**References:**
- Semtech LR1121 v2.1 datasheet (rev 2.1, Dec 2023)
- RadioLib 7.7.0 `LR11x0.cpp` / `LR1120.cpp` / `LR1121.cpp`
- Project bench evidence: see [`LR1121-SPEC.md`](LR1121-SPEC.md) "Phase 1 status — RX-path block"

## Purpose

Now that the 12-iteration RFSWx switch-table sweep has **conclusively exhausted the switch-table-as-fix hypothesis** (every chip-level RFSWx-capable DIO swept across every meaningful combination, no behavioral change), the diagnostic question shifts from "what's the switch table?" to "**what chip-level RX initialization step is RadioLib's `begin()` not performing?**"

This document audits RadioLib's LR1121 `begin()` against the Semtech v2.1 datasheet to enumerate candidate missing-init-step hypotheses, ranked by likelihood. Each candidate is a single-line firmware change in `WioLR1121.cpp` that can be bench-tested in ~15 minutes.

## RadioLib 7.7.0 LR1121 `begin()` — what IS called

The call chain when `_radio->begin(freq, bw, sf, cr, syncWord, power, preambleLength, tcxoVoltage)` runs:

| Order | Call | What it does |
|---|---|---|
| 1 | `LR1120::begin(cfg)` (LR1121 inherits) | Entry point |
| 2 | → `LR11x0::begin(bw, sf, cr, syncWord, preambleLength)` | Common LR11x0 init |
| 3 | → → `modSetup(LORA)` → `config(LORA)` | **Chip-level init** (see below) |
| 4 | → → `setBandwidth(bw, high)` | `high` arg derived from `freq > 1000.0f` (selects 2.4 GHz path) |
| 5 | → → `setSpreadingFactor(sf)` | LoRa SF |
| 6 | → → `setCodingRate(cr)` | LoRa CR |
| 7 | → → `setSyncWord(syncWord)` | LoRa sync byte |
| 8 | → → `setPreambleLength(preambleLength)` | LoRa preamble |
| 9 | → → `setCRC(2)` | 2-byte CRC |
| 10 | → → `invertIQ(false)` | Standard IQ |
| 11 | → → `setRegulatorLDO()` | **LDO** regulator (not DCDC) |
| 12 | → `setFrequency(freq)` | Triggers `calibrateImageRejection` if freq delta > threshold |
| 13 | → `setOutputPower(power)` | PA config + power setting |

`config(LORA)` (called by `modSetup`) does:

- `setRxTxFallbackMode(STBY_RC)` — chip falls back to RC-standby after RX/TX
- `clearIrqState(ALL)` + `setDioIrqParams(NONE)` — clean IRQ state
- `calibrate(CALIBRATE_ALL)` — **full chip calibration** (RC32k, RC13M, PLL, ADC, image)
- `driveDiosInSleepMode(true)` — keep DIO states valid in sleep
- `setPacketType(LORA)` — modem mode select

## What is NOT called automatically

These chip-level capabilities exist in RadioLib's API but are **not** invoked by `begin()`. Any one of them could be the missing piece.

| RadioLib method | What it does | Default state | Candidate? |
|---|---|---|---|
| `setRxBoostedGainMode(true)` | Enable boosted-gain LNA mode | **OFF** (normal gain) | **⭐⭐⭐ TOP CANDIDATE** |
| `setRegulatorDCDC()` | Switch from LDO to DCDC regulator | LDO active | Low |
| `setTCXO(voltage, delay)` (lower-level) | Explicit TCXO config | `tcxoVoltage` passed to begin() | Mid |
| `setLrFhssConfig(bw, cr)` | LR-FHSS modem | Standard LoRa | Low (we're not using LR-FHSS) |
| `setRfSwitchTable(pins, modes)` | Per-mode DIO switch states | High-Z | **Ruled out** (12-iter sweep) |
| `calibrateImageRejection(low, high)` | Force band-specific image calibration | Auto on freq change | Low (already called by setFrequency) |
| `forceLDRO(bool)` | Force Low Data Rate Optimization | Auto-detected | Low |

## The headline finding — RxBoosted

**Semtech v2.1 datasheet quotes the chip's LoRa sensitivity specs only with `RxBoosted = 1` enabled.** Direct citations:

- §3 (Table 3-9 footnotes, line 557): *"defined in 50Ω load, RxBoosted = 1 for LoRa and FSK, differential use of the LNAs (receiver gain levels are referenced in the device's...)"*
- Sensitivity table RXSL specs (US LongFast-equivalent at 125 kHz / SF11/SF12): all qualified `RxBoosted = 1`
- 2.4 GHz HF-band sensitivity (RXSLHF series): all qualified `RxBoosted = 1`
- v2.1 changelog (line 98): *"Added RxBoosted = 1 to RXSLHF1-6 in Table 3-9"* — indicating the qualifier was added/refined in this datasheet revision

The Wio-LR1121 module datasheet quotes sensitivity numbers (-126 dBm @ SF7, -140 dBm @ SF12) on its product page **without specifying RxBoosted state**. Per the chip-level spec, those numbers are only achieved with RxBoosted = 1.

**RadioLib's `begin()` does not call `setRxBoostedGainMode(true)`.** The chip defaults to RxBoosted = 0 (normal gain).

### What this could explain

If RxBoosted = 0 mode has sensitivity ~15-20 dB worse than RxBoosted = 1 (a typical penalty for non-boosted LNA gain on this class of receiver — Semtech doesn't publish exact non-boosted sensitivity for the LR1121, but the SX126x family shows ~6 dB penalty and the LR1121 LNA chain is more complex), then the chip would:

- Hear the bridge's own +20 dBm R1 NodeInfo at 10 cm (≈ -55 dBm at antenna) ✓ — far above the degraded sensitivity floor
- **Not hear** distant OTA traffic at -53 to -75 dBm ✗ — below the degraded floor
- Not hear MC devices at 1-2 ft (estimated -30 to -50 dBm) — depends on exact non-boosted floor
- Antenna-touch test at ~-20 dBm at antenna — should be received... unless the antenna chain has additional loss putting it below the floor

This **partially explains** our observation. The antenna-touch case where R2 still doesn't fire DIO9 is concerning if RxBoosted is the only issue — it suggests there might be a second factor in play. But enabling RxBoosted is the cheapest single experiment that could shift the picture, and it's literally one line of code.

## Test plan — candidate sweep

Each candidate is a one-line addition to `WioLR1121::begin()` AFTER `_radio->begin()` returns success. Bench-test sequentially with the established same-freq MT config (R1 = R2 = 906.875 MHz / BW250 / SF11 / sync 0x2B). 15-minute time-box per candidate.

### Candidate A — `setRxBoostedGainMode(true)` ★ TOP

```cpp
// After _radio->begin() returns success in WioLR1121::begin():
int16_t boostState = _radio->setRxBoostedGainMode(true);
Serial.printf("[%s] setRxBoostedGainMode(true) = %d\n", _name, boostState);
```

**Pass criteria:** `[R2 RX]` lines appear with `pktLen > 0` for real OTA traffic from MT phone.
**Expected if hypothesis correct:** RX comes alive at normal distance with normal RSSI values (-50 to -90 dBm range).

### Candidate B — Try `tcxoVoltage = 1.8 V`

Meshtastic's femtofox config uses 1.8 V. We've tested 1.6 / 3.0 / 3.3 — never tried the femtofox value.

```cpp
// In LoraRadio.h or BridgeConfig defaults — change LR1121_TCXO_VOLTAGE:
constexpr float LR1121_TCXO_VOLTAGE = 1.8f;
```

**Pass criteria:** Same as Candidate A.

### Candidate C — `setRegulatorDCDC()` instead of LDO

```cpp
int16_t regState = _radio->setRegulatorDCDC();
Serial.printf("[%s] setRegulatorDCDC() = %d\n", _name, regState);
```

**Pass criteria:** Same. Unlikely to matter for RX but trivial to test.

### Candidate D — Explicit `calibrateImageRejection` for sub-GHz US band

```cpp
// After setFrequency in begin() — or after begin() returns from our wrapper:
int16_t calState = _radio->calibrateImage(902.0f, 928.0f);
Serial.printf("[%s] calibrateImage(902, 928) = %d\n", _name, calState);
```

**Pass criteria:** Same. RadioLib already triggers this on freq change but maybe not at boot, or maybe with a too-narrow band.

### Candidate E — A+B+C+D stacked

If individual candidates fail, try all four at once as a kitchen-sink test. If THAT fails, we're firmly in hardware-fault or chip-firmware-bug territory.

## What we are NOT testing (and why)

- **`setRfSwitchTable()` variations**: 12-iteration sweep exhausted this. Skip.
- **`setLrFhssConfig()`**: We're using standard LoRa, not LR-FHSS. Not applicable.
- **GNSS / WiFi init**: Different modem mode, irrelevant to LoRa RX.
- **Raw SPI `SetDioAsRfSwitch` with custom encoding**: RadioLib's API correctly covers all 5 chip-level RFSWx slots (DIO5/6/7/8/10) per v2.1 Table 4-1; no upstream gap.

## Result tracking

| Candidate | DIOMASK (if applicable) | RX_DONE on real OTA? | Notes |
|---|---|---|---|
| A — RxBoosted = true | — | TBD | |
| B — TCXO 1.8 V | — | TBD | |
| C — Regulator DCDC | — | TBD | |
| D — calibrateImage(902, 928) | — | TBD | |
| E — A+B+C+D stacked | — | TBD | |

## Decision tree after the sweep

- **Any candidate passes** → implement that init step in `WioLR1121::begin()` permanently. Follow up with Seeed inquiry to confirm the chip-firmware default behavior is what we observed, and recommend they document the required init for the Wio-LR1121 product page. Phase 1 unblocks → v9.1.
- **All candidates fail** → hardware-fault or chip-firmware-1.3 bug. Continue waiting for Seeed reply. The full audit + sweep results become supplementary evidence for the inquiry.

## Estimated effort

| Step | Time |
|---|---|
| Apply Candidate A → flash → bench-test → log | ~15 min |
| Repeat for B, C, D | ~45 min total |
| Stacked test E if all four individuals fail | ~15 min |
| Doc updates with results | ~15 min |
| **Total time-box** | **~1.5 hours** |

If Candidate A passes (the highest-prior hypothesis), this resolves in 15 minutes.

---

**Outcome of this audit:** the bench experiment now has a structured 5-candidate test plan with clear pass criteria, citations, and a decision tree. Priority 2 (the bench experiment) reduces to "walk down the table." No ad-hoc guessing.
