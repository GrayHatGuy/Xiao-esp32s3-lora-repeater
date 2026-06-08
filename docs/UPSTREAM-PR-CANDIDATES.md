# Upstream Bugs & PR Candidates

Notes on bugs discovered in upstream projects during this project's bench testing that warrant a PR or issue report when time permits. Tracked here so they aren't lost.

---

## Meshtastic firmware — Region change does not re-derive bandwidth from preset

**Status:** Discovered 2026-05-30 during Wio-LR1121 Phase 1 bring-up bench session. Workaround applied locally. **Not yet reported upstream.**

**Affected:** Meshtastic firmware `2.7.15.567b8ea` on LilyGO TLORA_T3_S3 (T3S3 LR1121 variant). Likely affects all hardware platforms — bug is in shared LoRa config logic.

**Repo:** https://github.com/meshtastic/firmware

### Symptom

When changing `lora.region` via the Meshtastic CLI (e.g., US → LORA_24 or vice versa) while `lora.usePreset = true` and `lora.modemPreset` is set, the firmware does **not** re-derive `lora.bandwidth` from the new region's preset definition.

Example: T3S3 starts on US region with LongFast → bandwidth = 250 (= 250 kHz). User runs:

```
meshtastic --port COM5 --set lora.region 4   # LORA_24
```

After reboot, T3S3 reports:
```
lora.region: LORA_24
lora.usePreset: true
lora.modemPreset: LONG_FAST
lora.bandwidth: 250          # ← WRONG, should be 800 (= 812.5 kHz per 2.4G LongFast preset)
lora.spreadFactor: 11
lora.codingRate: 5
```

The mismatched bandwidth is silently used at TX/RX. Any companion device configured per the *expected* 2.4 GHz LongFast preset (BW 812.5 kHz) will fail to demodulate the T3S3's packets because BW mismatch (250 vs 812.5) prevents LoRa correlator lock.

### Real-world failure mode

If the user does NOT manually inspect `lora.bandwidth` after a region change, they see:
- T3S3 successfully boots and TXes at "2.4 GHz LongFast"
- Companion radio at the documented 2.4 GHz LongFast params (812.5 kHz) reports zero RX
- User chases firmware/RF issues for hours when the actual problem is upstream Meshtastic dropping the BW update.

### Workaround (verified working)

Four-step manual sequence:

```
1. meshtastic --port COM5 --set lora.usePreset false
2. meshtastic --port COM5 --set lora.bandwidth <target enum>
     # e.g. 800 for 2.4 GHz LongFast (= 812.5 kHz)
3. Reboot device (CLI reboots automatically on save, or trigger one)
4. meshtastic --port COM5 --set lora.usePreset true
```

Step 1 disables the preset (so manual BW takes effect). Step 2 writes the correct BW. Step 4 re-enables preset mode — the firmware then honors the correctly-set BW.

### Suggested fix (high-level)

In the region/preset config handler (probably `src/mesh/RadioInterface.cpp` or `src/configuration.h` in the Meshtastic firmware repo):

- When `lora.region` changes while `lora.usePreset = true`, the firmware should:
  - Look up the region's preset table for the current `modemPreset`
  - Overwrite `lora.bandwidth`, `lora.spreadFactor`, `lora.codingRate` with the new region's preset values
  - Persist to NVS

Currently it appears to update region but skip BW/SF/CR re-derivation.

### Suggested PR title

"Fix: re-derive bandwidth/SF/CR from preset on region change"

### Reproduction steps for the issue report

1. Flash Meshtastic firmware ≥ 2.7.15 on a T3S3 (or other LR1121-equipped board)
2. Set US region with LongFast preset (default for US)
3. `meshtastic --port <port> --get lora` — confirm `bandwidth: 250`
4. `meshtastic --port <port> --set lora.region 4` (LORA_24)
5. Wait for reboot
6. `meshtastic --port <port> --get lora` — **bug:** `bandwidth: 250` (still), should be `800` to match 2.4G LongFast preset

### Local impact

Documented for the project's own benefit. Once the bench fully validates 2.4 GHz, file the upstream issue with reproduction steps + suggested fix. Until upstream is patched, anyone using a T3S3 as a known-good 2.4 GHz reference radio must follow the manual workaround sequence above first. (The original 2.4 GHz bench procedure lives on the [`lr1121-phase1`](https://github.com/GrayHatGuy/Xiao-esp32s3-lora-repeater/tree/lr1121-phase1) branch.)

---

## RadioLib — LR1121 sub-GHz LoRa RX detects preamble but never completes (regression candidate)

**Status:** Discovered 2026-06-07 (Core1121 + Seeed Wio-LR1121 bench). **CONDITIONAL / PENDING CONFIRMATION** — do NOT file until the two controls below confirm it. Owner reviews/submits all outbound (drafts only).

**Affected:** `jgromes/RadioLib` **7.7.0**, LR11x0/LR1121 driver, on ESP32-S3 (Arduino). Suspected regression vs **7.4.0** (or a longstanding LR1121-RX defect across versions).

**Repo:** https://github.com/jgromes/RadioLib

### Symptom
An LR1121 in continuous LoRa RX **detects the preamble** (IRQ `0x10` PREAMBLE_DETECTED latches) but **never advances to header/RX_DONE** (`0x40/0x50/0x08` never set, `isr` stays 0) — even on strong packets (−52 dBm, ~67 dB above the floor). A co-located SX1262 (RadioLib `SX126x`) on the identical channel/antenna receives every packet. Reproduced on **two different LR1121 boards** (WaveShare Core1121 + Seeed Wio-LR1121).

### What was ruled OUT (so the report is airtight)
Every config knob was matched against Semtech's official `lr11xx_driver` demo (which RXes fine on the same Core1121) and bench-eliminated: **sync word** (both `0x12`, byte-identical), **RF-switch table** (identical DIO5/DIO6 map), **IQ/header/CRC/LDRO** (all match; the LR1121 even *completes* at BW250 on the same settings), **TCXO recalibration** (`setTCXO`+`calibrate(ALL)` warm — cleared HF_XOSC_START_ERR, RX still dead), **carrier-frequency offset** (~+50 kHz correction reaches preamble only), and the **RSSI/AGC gain-tune table** RadioLib never programs (added via `setRssiCalibration` — no change). So the remaining variable is RadioLib's LR11x0 RX path itself.

### Confirming controls (run before filing — gives the bisect)
1. **OEM Semtech `lr11xx_driver` on the same Core1121** (repo env `core1121_oem_rx`): if it `RX_DONE`s where RadioLib only reaches preamble → RadioLib's LR1121 path is the bug.
2. **RadioLib 7.4.0** on a T-Lora-Dual (dual LR1121), OEM example untouched: works → **7.7.0 regression**, then `git bisect` RadioLib 7.4.0→7.7.0 to the offending commit; fails too → driver-level defect across versions.

### Suggested action
- If 7.4.0 works and 7.7.0 doesn't: **file an issue** with the bisect result + minimal repro (LR1121 continuous RX of a standard SX126x LoRa packet at SF7/BW62.5, sync 0x12), then a **fix PR** reverting/correcting the regressing change in `src/modules/LR11x0/`.
- Note also (independent of this bug) that `LR11x0::getFrequencyError()` is a **stub returning 0** (`LR11x0.cpp:1203`, `// TODO implement this`) — a small separate PR candidate.

### Reproduction steps (for the issue)
1. LR1121 board on Arduino-ESP32; `LR1121::begin()` → continuous `startReceive()` at **910.525 MHz / BW 62.5 / SF7 / CR4-5 / sync 0x12, explicit header, CRC on**.
2. Transmit a standard LoRa packet (SX1262 / any compliant stack) on the same params from a few metres away.
3. Poll `getIrqFlags()`: **bug =** `0x10` latches, never `0x08`; a co-located SX1262 decodes the same packet.

### Local impact
Blocks Phase-1 of this bridge (R2 = LR1121 can't receive). Workaround under evaluation = drive R2 with Semtech's `lr11xx_driver` (env `core1121_oem_rx`) instead of RadioLib, or pin RadioLib 7.4.0 if the bisect confirms a regression.

---

## Template for future entries

```
## <Upstream project> — <one-line summary>

**Status:** <date discovered>; <action status — workaround applied / reported / merged / etc>
**Affected:** <project>, version, platform
**Repo:** <github URL>

### Symptom
<what the user sees>

### Workaround
<steps if any>

### Suggested fix
<high-level pointer to relevant code area>

### Reproduction steps
<numbered list>

### Local impact
<how it affects this project>
```
