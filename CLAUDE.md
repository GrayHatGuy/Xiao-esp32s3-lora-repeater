# CLAUDE.md — project operating manual

**Project:** XIAO ESP32-S3 dual-radio LoRa mesh bridge (sub-GHz ↔ 2.4 GHz cross-band; Meshtastic ↔ MeshCore ↔ Reticulum-stub).
**Repo:** https://github.com/GrayHatGuy/Xiao-esp32s3-lora-repeater
**Owner:** `GrayHatGuy` (pseudonym), `grayhatguyllc@protonmail.com`. **Real name is ephemeral — never persist it to docs.**
**Shell:** PowerShell (Windows). HEREDOC commit messages via the Bash tool; build/flash via PlatformIO (`pio`).

> **This is the `CORE1121` branch.** Start with **[`CORE1121.md`](CORE1121.md)**.
> The full Wio-LR1121 investigation that this branch builds on is the *background* —
> it lives on the [`lr1121-phase1`](https://github.com/GrayHatGuy/Xiao-esp32s3-lora-repeater/tree/lr1121-phase1)
> branch and is intentionally **not duplicated here**.

---

## 0. BENCH INVESTIGATION — Core1121 RX deaf at narrow BW (READ FIRST)

**Status: OPEN. ⭐ JUMP TO [§0.11](#011--session-2026-06-07-evening--the-chip-receives-radiolib-is-the-deficient-path-read-first-next-session) — the latest + most important finding: Semtech's `lr11xx_driver` COMPLETES packets on this Core1121 where RadioLib never does ⇒ RadioLib's LR1121 RX path is the leading root cause. §0.1–§0.10 below are the earlier (frequency-offset / TCXO / RSSI-cal) investigation, all now superseded by §0.11.**

### 0.1 Setup
- **R1** = Wio-SX1262 (B2B header) — healthy reference. **R2** = WaveShare **Core1121** (LR1121), hand-wired to the XIAO edge pins (per [`CORE1121.md`](CORE1121.md) §5).
- **Core1121 identity:** chip EUI **`00:16:C0:01:F0:61:63:6F`**, base FW **1.1** (Seeed Wio modules were FW 1.3). `getErrors()=0x0020` (**HF_XOSC_START_ERR**) on *every* boot — assumed benign POR latch (as on Seeed), but see hypotheses.
- **External sources:** **Heltec V2** (SX1262, MeshCore, node `9506C57C`) and **T3S3** (LR1121, Meshtastic, node `…80`), plus ambient public-mesh traffic.

### 0.2 Wiring incidents (both resolved; chip NOT damaged)
1. **Missing 3V3** to the Core1121 → `R2 BUSY stuck HIGH → module absent/unpowered`, `begin()` aborts, all R2 ops `-705`. Fixed by wiring 3V3.
2. **Suspected GND/VCC swap (reverse polarity)** earlier. Chip **survived**: boots, reads EUI, **TX works (`tx=0`)**, detects preambles, decoded a −109 dBm packet (see 0.3). Missing-wire = unpowered (not damaged); brief current-limited reverse polarity off the XIAO LDO evidently did no harm.

### 0.3 Findings
- **R2 hardware is healthy.** Boots clean (BUSY releases ~142 ms), installs the PE4259 RF-switch table, reads EUI, **TX returns `tx=0`** (MeshCore→Meshtastic bridge direction works, `TX OK`).
- **R2 RX WORKS at the Meshtastic config** (906.875 / **BW250** / SF11 / sync **0x2B**): decoded a real POSITION packet at **−109 dBm / SNR −18.5 dB** (≈ SF11 demod floor) + one CRC-fail (`state=-7`). Low completion rate, but it *completes* even weak packets. → **NOT the Seeed deficit** (Seeed dropped a −42 dBm point-blank packet at SF11/BW250).
- **R2 is DEAF at the MeshCore config** (910.525 / **BW62.5** / SF7 / sync **0x12**): **zero completions** over hundreds of seconds, even for **21-byte packets at −56 dBm** (≈67 dB above the SF7/BW62.5 sensitivity floor). `irq` latches **0x10** (preamble) only — **never 0x50** (header-err) or **0x08** (RX-done); `isr` stays 0. R1 (SX1262) on the **identical** config decodes ~10 strong packets (−56…−83 dBm) fine.

### 0.4 Failure-mode characterization
- **Hard / deterministic**, not marginal — even the shortest + strongest packets fail. **Length-independent** (a "short telemetry/position packet survives" theory was tested and **refuted**: 21-byte packets fail too).
- **Strong-signal failure (−56 dBm) ⇒ NOT sensitivity / SNR.** Systematic.
- **Clean preamble-only (`0x10`), no header errors** ⇒ never gets past the sync/header stage.
- **Config-dependent:** fine at BW250, hard-fails at BW62.5.

### 0.5 Hypotheses (ranked)
1. **LEADING — LR1121 RX *frequency offset*, fatal at narrow BW.** A carrier offset of ~20–30 kHz sits *within* LoRa tolerance at BW250 (≈±37 kHz) but is ~48% over at BW62.5 (4× tighter) → hard, length-independent, strong-signal-immune fail; clean-preamble-only. Possibly tied to `HF_XOSC_START_ERR` / an image/frequency-calibration miss at 910.525. **Driver-fixable** if confirmed (explicit `calibrateImageRejection()` for the band, TCXO/XOSC config, frequency recal).
2. **BW mismatch (owner's hypothesis):** the BW *programmed* into the LR1121 ≠ the BW the chip *actually uses* (so R2's RX filter doesn't match the Heltec's BW62.5). Fails identically. **Must verify** the on-chip BW == programmed BW62.5 (read-back / spectral scan / freqErr-scaling cross-check). NOTE: the nudge test (0.6 #2) **discriminates** — shifting R2's frequency recovers RX ⇒ frequency offset; no recovery ⇒ suspect BW.
3. **Sync word 0x12 — DOWNWEIGHTED.** Source-checked: `LR11x0::setLoRaSyncWord(0x12)` sends the raw byte to the chip, which expands it SX126x-compatibly (`SX126x_config.cpp`: `0x12→0x1424`; prior session proved the LR1121 matches for `0x2B→0x24B4`). So `0x12` *should* match R1. The hard-fail pattern is consistent with sync too, but the source analysis makes it unlikely.
4. **REFUTED — sensitivity / marginal-demod / packet length.** Killed by the 21-byte −56 dBm hard fail.
5. **REFUTED — reverse-polarity / wiring damage.** R2 boots, TX, reads EUI, decoded −109 dBm — a damaged chip can't.

### 0.6 Instrumentation added (this session) + NEXT STEPS
- **ADDED (built, uncommitted):** `getFrequencyError()` logging in **both** `Core1121::read()` (under `LR1121_DEBUG`) and `WioSX1262::read()` — each read logs `freqErr=N Hz`. R1 is the ≈0 Hz reference; a large R2 offset on the same packets confirms LR1121 mistuning. Valid even on CRC-failed reads.
1. **Measure the offset (instrumented build):** flash, run the **Meshtastic** config (906.875/BW250/SF11, where R2 *completes* packets), compare R1 `freqErr` (≈0) vs R2 `freqErr`. Tens-of-kHz on R2 = confirmed.
2. **Nudge test (owner running now):** at the MeshCore BW62.5 config, change **only R2 RX frequency** (910.510 / 910.540 / 910.495 / 910.555). Decoding recovers ⇒ frequency offset (shift = the offset, and a workaround); no recovery ⇒ suspect BW mismatch (#2).
3. **Verify on-chip BW** (owner's hypothesis): confirm the LR1121's actual RX BW == programmed BW62.5.
4. **If frequency offset confirmed — fixes (one variable each):** explicit `calibrateImageRejection()` for the band; TCXO/XOSC config review (clear `HF_XOSC_START_ERR`); explicit frequency recal after XOSC stable.

### 0.7 Config / bench state at session end
- **`platformio.ini`:** R1 & R2 both MeshCore **910.525 / BW62.5 / SF7 / CR5 / sync 0x12** (A/B race), **`-DR2_RX_ONLY_TEST` ON** (R2 pure-listen), `-DRADIO2_CHIP=RADIO_CHIP_LR1121`, MC key `8b3387…` / `public`, MT `LongFast` / `AQ==`. **Build flags are first-boot defaults only** — erase NVS (`pio run -t erase`) to apply, else portal/NVS wins.
- **Serial logs interleave/garble** (two radio tasks → USB-CDC, byte-level). A `Serial` mutex would fix it (offered, not yet added) — a good first task next session for clean captures.
- **Bridge pipeline is functional** (MC→MT forwarded with `tx=0`); the only blocker is **R2 RX at narrow-BW MeshCore**.

### 0.8 Nudge-test RESULT (2026-06-07) — frequency offset CONFIRMED, R2 tuned LOW
Swept R2 RX frequency at the MeshCore BW62.5 config (R1 fixed 910.525, Heltec 910.525):

| R2 RX freq | offset | R2 `irq` | meaning |
|---|---|---|---|
| 910.495 | −30 kHz | `0x10` | preamble only |
| 910.510 | −15 kHz | `0x10` | preamble only |
| 910.525 |  0 | `0x10` | preamble only |
| 910.540 | +15 kHz | `0x50` | preamble + **HEADER_ERR** (reached header) |
| 910.555 | +30 kHz | `0x50` | preamble + HEADER_ERR |

**Tuning R2 UP advances `0x10`→`0x50`; DOWN stays `0x10`.** ⇒ R2's RX center is **LOW by >30 kHz**, correction is **UP**. Frequency IS a cause (nudging changes behavior) ⇒ **NOT a pure BW mismatch**. But **no offset completed** (never `0x08`/RX_DONE), so either the offset is **>+30 kHz** OR a **BW co-factor** also blocks completion. Hypothesis 0.5#1 (frequency offset) is now **largely confirmed**; 0.5#2 (BW) still possible *in addition*. **DISCRIMINATE:** (a) flash the instrumented build (`getFrequencyError`), run the **Meshtastic BW250** config (R2 completes there) → read R2 `freqErr` magnitude; (b) continue the nudge **UP** at BW62.5 (910.570 / 910.585 / 910.600) → if R2 hits `0x08`/`[R2 decoded]`, it's pure frequency; if it never completes, it's (also) BW. Then fix: TCXO/XOSC config + explicit image/frequency recal on the LR1121.

### 0.9 ⭐ PRIORITY 1 — START HERE NEXT SESSION
Frequency offset is **confirmed** (§0.8); the open question is **pure-frequency vs frequency+BW**. Do BOTH P1 tests.

**P1.1 — Continue the nudge UP (portal-only, no reflash).** MeshCore BW62.5 config; set ONLY R2 RX freq to **910.570**, then **910.575**, **910.585**. *Why these:* +30 kHz (910.555) reached HEADER_ERR (`0x50`) but didn't complete; BW62.5 needs residual offset < ~10–15 kHz to finish a packet, so the true offset is likely **~+45–50 kHz** → sweet spot ≈ 910.570–910.575.
  - R2 hits **`0x08` / `[R2 decoded]`** at one → **PURE FREQUENCY OFFSET**; `winning_freq − 910.525` = the exact offset. Diagnosis done.
  - Stays **`0x50`** however far up → a **BW co-factor** too → verify the on-chip BW actually == 62.5.

**P1.2 — ⚠️ SUPERSEDED by §0.10 — DO NOT RELY ON R2 `freqErr`.** RadioLib's `getFrequencyError()` is a stub returning 0 on the LR1121, so R2's `freqErr` is always 0 and conveys nothing (verified — see §0.10). The offset must come from the **nudge (P1.1)** + the new **post-`begin()` `getErrors()`** read, not freqErr. *(Original, now-invalid plan: flash, switch R2 to Meshtastic BW250, read `freqErr=N Hz` for R1 vs R2.)*

**Then the FIX (P2):** the offset is >+30 kHz (≈33–50 ppm) **low** — far beyond TCXO tolerance ⇒ a calibration/synthesis issue, likely tied to the recurring **`HF_XOSC_START_ERR` (0x0020)**. In `Core1121::begin()`, one variable at a time (re-measure `freqErr` after each): (1) review TCXO/XOSC config so the XOSC locks clean (clears `0x0020`); (2) force an explicit image/frequency recalibration for the operating band.

### 0.10 Grounding + fix (2026-06-07, session 2) — root cause grounded, **P1.2 CORRECTED**, recal experiment ready
A 4-strand research pass (RadioLib source · LR1121 DS/UM · Core1121 schematic · adversarial cross-check) reconciled the evidence. **Two findings change the plan.**

**⚠️ CORRECTION — P1.2 (read R2 `freqErr` at BW250) is INVALID.** RadioLib `LR11x0::getFrequencyError()` is a hardcoded **stub**: `// TODO implement this` → `return(0)` (`.pio/.../LR11x0/LR11x0.cpp:1203-1206`; declared `LR11x0.h:490`; **no** LR1120/LR1121 override; no freq-error register in the LR11x0 map — verified directly). So the R2 `freqErr=` line **always prints 0 Hz and means nothing**; only **R1**'s SX1262 `freqErr` is real. The misleading read-time R2 `freqErr` line is **removed** (replaced with a cited note in `Core1121::read()`). **Characterize the R2 offset with the BW62.5 RX-frequency nudge + the new post-`begin()` `getErrors()` read — never freqErr.**

**Root-cause ranking (cited):**

| # | Cause | Likelihood | Key evidence |
|---|---|---|---|
| 1 | PLL/freq calibration baked against an unstable/unconfirmed 32 MHz TCXO during `begin()` | **leading** | RadioLib `modSetup` runs `setTCXO(3.0 V, ~5 ms budget)` then `config()`→`calibrate(ALL)` with **no XOSC-stable check** (`LR11x0.cpp:1656,1712`). LR1121 **skips POR calibration when a TCXO is fitted**; host must "program the TCXO … and re-launch the calibrations before further usage" (**DS §1.2.4 p.8**); HF_XOSC_START_ERR remedy = "SetTcxoMode + redo calibrations" (**UM v2.2 §3.6.1 p.32**). RadioLib never recalibrates after settle. Carrier = PLL × 32 MHz ref (**UM §7.2.1 p.51**) ⇒ wrong-ref trim = N-ppm carrier error. Matches bench: >33 ppm LOW, correction UP. |
| 2 | **Fixed** HW reference / board-TCXO offset (no driver change fixes it) | plausible | 33 ppm static+monotonic fits a fixed error; >±10 ppm crystal budget (**DS §3.5 T3-12 p.22**). **This is the null hypothesis the experiment is built to refute.** |
| 3 | Sync word 0x12 | unlikely | nudge reached HEADER_ERR (demod past sync) — a directional *frequency* response, not sync |
| 4 | Stale band image cal | **refuted** | image cal tunes mixer image-rejection, **not** center (UM p.17); RadioLib already runs `calibrateImageRejection(906.5,914.5)` in begin() |
| 5 | BW mismatch (owner hyp. 0.5#2) | **refuted** | `setBandwidth(62.5,high=false)`→`LORA_BW_62_5` maps correctly; a wrong filter can't give a *directional* response to a pure carrier shift |

**Hardware confirmed (schematic):** Y1 is a real **active TCXO** (4-pin GND/GND/OUT/VCC, "32MHz", single-ended into **XTA**, XTB floating), VCC from the LR1121's internal **VTCXO** regulator via ferrite L1 — no external LDO. So 3.0 V is a firmware register choice (RegTcxoTune=0x06), the topology is correct, and WaveShare's own demo runs this board ⇒ favors a driver/config cause over a defective part. **POR latch:** today's `getErrors()=0x0020` is read **only pre-`begin()`** (`Core1121.cpp:181`) = the POR latch (POR cal fails when a TCXO is fitted, UM p.17). The post-`setTCXO`+`calibrate` XOSC state has **never** been observed — the fix adds that read.

**THE FIX — `CORE1121_TCXO_RECAL` (build flag, DEFAULT OFF).** In `Core1121::begin()`, **sub-GHz only**, after the begin() success guard and before RX is armed (inside the already-held mutex): re-issue `setTCXO(3.0 V, CORE1121_TCXO_DELAY_US)` with the TCXO now warm, clear HF_XOSC_START_ERR if set, `calibrate(CALIBRATE_ALL)`, BUSY-wait, `setFrequency()` (relocks the synth; no image re-trigger), then log `getErrors()` before/after as a `[recal] … errPre=0x…. errPost=0x….` line. Flag OFF ⇒ begin() is **byte-for-byte the baseline**. This is BOTH the candidate fix AND the discriminator. (`LR1121Access` was extended with `using LR11x0::calibrate/clearErrors`.)

**Owner-run A/B (next bench session):**
1. **Baseline (flag OFF):** `pio run -t erase -t upload`. MeshCore BW62.5 — confirm R2 still deaf; note pre-begin `getErrors`=0x0020.
2. **Nudge (flag OFF, portal-only):** sweep ONLY R2 RX up `910.555 → .570 → .575 → .585 → .600`. First freq hitting `0x08`/[R2 decoded] = the **offset** (`winning − 910.525`). Ground truth for the A/B.
3. **Fix ON:** rebuild with `-DCORE1121_TCXO_RECAL`, erase+upload. Read the `[recal]` line — **PASS-1 = `errPost=0x0000`** (XOSC error cleared, not re-triggered). Re-run the nudge — **PASS-2 = winning freq moves DOWN toward 910.525**, ideally R2 completes at nominal with no nudge.
4. **Discriminator:** after a clean warm recal (`errPost=0x0000`), if the winning nudge freq is **UNCHANGED** ⇒ **fixed HW reference error** (rank #2), stop chasing the driver. If it **moves** ⇒ cold-XOSC-calibration confirmed.
5. **Variable 2 (only if needed):** if `errPost` stays 0x0020 or the nudge doesn't move, sweep `-DCORE1121_TCXO_DELAY_US=100000` then `=1000000` (RadioLib's LR2021 successor defaults the TCXO timeout to **1 s**).

**Also added this session — Serial mutex** (`src/SerialLog.h`/`.cpp`: `logf()` + `SerialLogGuard`): one recursive mutex serialises all USB-CDC output across the two per-core radio tasks + drivers, so `[recal]`/`[Rn RX]` capture lines stop byte-interleaving. *Residual:* RadioLib's own `RADIOLIB_DEBUG_BASIC` spew bypasses the lock (serialised against itself by the SPI mutex); drop `-DRADIOLIB_DEBUG_BASIC=1` for fully clean captures.

**Open unknowns:** post-begin XOSC state until the bench runs PASS-1; true offset magnitude (no freqErr — nudge only); warm-vs-fixed not resolvable from source; the adequate TCXO startup delay is in no datasheet (swept empirically); BW co-factor not fully excluded until a nudge COMPLETES a packet (`0x08`).

> **Firmware status:** all of the above is **built (baseline + flag-on both compile) but UNCOMMITTED and UNVERIFIED on hardware** — the bench was offline this session. Confirm live state before assuming firmware matches docs.

### 0.11 ⭐⭐ SESSION 2026-06-07 (evening) — THE CHIP RECEIVES; RadioLib is the deficient path (READ FIRST NEXT SESSION)

**⭐ STRATEGIC STATUS (2026-06-07 eve, owner): CORE1121 is now the lead / pioneering platform and the RELEASE GATE for all three LR1121 projects.** Its mandate: prove a feasible LR1121 sub-GHz RX implementation that does **NOT** depend on RadioLib 7.7.0, then port that solution to the **Seeed Wio-LR1121** and **T_LORA_DUAL** bridges. **T_LORA_DUAL has now itself been upgraded to RadioLib 7.7.0**, so it is expected to fail identically — it is **no longer the clean 7.4.0 control** of experiment #1 below (it would need pinning back to 7.4.0 to serve as one). On 7.7.0 it instead confirms the failure follows RadioLib 7.7.0 across **self-contained hardware** (no hand-wiring), which kills hypothesis #4 (wiring/RF) and nails the cause to RadioLib 7.7.0 itself. Two candidate alternatives gate everything downstream: **(a)** port the LR1121 RX path to Semtech's `lr11xx_driver` (proven to complete here — the heavier but clearly-portable fix, #4); **(b)** if the **fine sweep** (the now-gating experiment — see the frequency-confound block below) salvages RadioLib 7.7.0 with a ~+20 kHz carrier trim, a cheap config fix portable to all three. **The fine sweep selects between (a) and (b); run it first.**

**Headline result:** Driving the SAME physically-wired Core1121 with **Semtech's official `lr11xx_driver`** (not RadioLib), the chip **COMPLETED a real packet**: `*** RX_DONE @ 910.545 MHz *** 149 B  RSSI -74 dBm  SNR -1 dB  CRC-OK`. Our RadioLib build **never completed a single packet** in any bench test. So the Core1121 hardware *can* receive, and **RadioLib's LR11x0 RX path is the leading suspect** — confirming the owner's hypothesis (both failing boards run Claude/RadioLib code; others run both LR1121 chips fine with OEM code).

**Every RadioLib-side config knob was matched to the OEM working code and bench-eliminated:**
| Knob | OEM (works) | Ours (RadioLib) | Bench result |
|---|---|---|---|
| LoRa sync word | `0x12` | `0x12` (raw byte, same) | identical — NOT it |
| RF-switch DIO map | RX=DIO5, TX=DIO6 | same | identical — NOT it |
| IQ / header / CRC / LDRO | std / explicit / on / auto | same | match (R2 completes at BW250 on these) |
| TCXO recal (`CORE1121_TCXO_RECAL`) | — | warm `setTCXO`+`calibrate(ALL)` | **cleared `HF_XOSC_START_ERR` (errPost=0x0000) but RX still dead** ⇒ cold-cal theory FALSIFIED |
| Freq offset (`CORE1121_FREQ_OFFSET_HZ=50000`) | — | command +50 kHz | preamble only, no completion |
| RSSI/AGC cal (`CORE1121_RSSI_CAL`) | OEM 600 M–2 G table | **was missing → now programmed** | **no change** — ruled out |

**Failure modes observed (cleanly enumerated):**
- **FM1 — RadioLib, both boards (Seeed Wio-LR1121 + WaveShare Core1121):** detects preamble (`irq 0x10`), **never** advances to sync/header/RX_DONE. `isr` ~0. Strong (−56 dBm) and weak alike.
- **FM2 — OEM Semtech driver, same Core1121:** reaches **sync/header-VALID** (`0x20`) and occasionally **RX_DONE** — further than RadioLib, but rare (1 completion in ~3.5 min).
- **FM3 — STRONG-signal anomaly (key):** point-blank sends (−38 dBm) only trip preamble; the one packet that COMPLETED was **weak (−74 dBm, SNR −1 dB)**. Strong-fails-weak-completes ⇒ smells like **front-end overload / near-field from transmitting point-blank**, or the strong source sitting off-frequency.
- **FM4 — boot-variable carrier offset:** RX center is LOW and the magnitude varies per boot (RadioLib nudge said ~+30–50 kHz; the OEM completion was at **+20 kHz / 910.545**; another OEM boot reached header at nominal 910.525). Tied to TCXO/calibration; present regardless of driver.
- **FM5 — SPI unreliable at 8 MHz** over the hand-soldered jumpers (the OEM driver's default). At 8 MHz the chip never answered → BUSY stuck → 10 s HAL timeouts. **Fixed by dropping to 1 MHz** (the bridge already uses 1 MHz).
- **FM6 — chip needs ~142 ms post-reset BUSY wait;** the OEM `lr11xx_hal_reset()` didn't wait. **Fixed** by adding the wait (mirrors `Core1121::begin()`).

**Root-cause hypotheses (ranked):**
1. **LEADING — RadioLib's LR11x0 RX path is deficient vs Semtech's `lr11xx_driver`.** OEM completes / reaches header; RadioLib never completes / only preamble, identical config. The *specific* RadioLib defect is not yet isolated. Remaining candidates: the **standby-RC-vs-XOSC + calibrate ordering**, an **RX-config gap**, or a **7.4.0→7.7.0 regression**. ~~AGC/RSSI-cal application~~ **— ELIMINATED 2026-06-07 (source diff, not bench):** RadioLib's `setRssiCalibration` *does* pack 18 nibbles vs Semtech's 17 (real bug — now a standalone entry in `docs/UPSTREAM-PR-CANDIDATES.md`), but it **cannot be this deficit**: RadioLib has *zero internal callers* of it (baseline RX fails with the AGC table never programmed), and our `CORE1121_RSSI_CAL` path passes an 18-element array with `tune[17]=0` → byte-identical to Semtech, which is why the bench saw "no change."
2. **PLAUSIBLE — real boot-variable carrier offset (~+20–50 kHz LOW)** from the Core1121 TCXO/PLL calibration (recurring `HF_XOSC_START_ERR=0x0020` at POR). Present under both drivers; the OEM tolerates it enough to complete occasionally. `LR11x0::getFrequencyError()` is a **RadioLib stub returning 0**, so freqErr can't quantify it — only the BW62.5 nudge/sweep can.
3. **PLAUSIBLE — front-end overload / near-field from point-blank transmitting** (FM3). Cheap to test (move the source a few metres).
4. **PLAUSIBLE — the hand-wired XIAO setup** (long jumpers, shared SPI, grounding) degrades RF/SPI; common to BOTH failing boards. The self-contained **T-Lora-Dual** (no hand-wiring) is the control that isolates this.
5. **DOWNWEIGHTED — chip/board hardware defect** on this specific Core1121 (a completion at all argues against a dead part).

**Prioritized open experiments / path forward (do in this order):**
1. **T-Lora-Dual control — ⚠️ STATUS CHANGED (now on RadioLib 7.7.0):** the owner's T_LORA_DUAL has been upgraded to **7.7.0**, so it is expected to fail like the others and is **no longer a 7.4.0 control**. As-is (7.7.0) it still confirms the failure follows RadioLib 7.7.0 across self-contained hardware ⇒ rules out wiring/RF (hypothesis #4). To recover its original discriminating power (7.4.0 works ⇒ 7.7.0 regression → bisect, pin 7.4.0, PR task #5), it must be **pinned back to RadioLib 7.4.0**. Given the strategic status above, the higher-value path is now the **fine sweep (salvage vs port decision)** + building the non-7.7.0 alternative, not chasing the 7.4.0 control.
2. **Distance test on `core1121_oem_rx`:** send MeshCore from a few metres (NOT point-blank) — does completion rate jump? Tests FM3/H3. Cheap, high-value (the bench may have been fighting near-field the whole time).
3. **`core1121_oem_rx` reliability at +20 kHz:** the env auto-sweeps 910.490–910.585 (7 s/step, locks on RX_DONE) — let it run with steady traffic at realistic distance; see if completions become reliable near 910.545.
4. **Decide the fix:** if the OEM/Semtech driver reliably completes → **port R2 (the LR1121) of the bridge to Semtech's `lr11xx_driver`** (keep RadioLib for R1/SX1262). That is the real fix the evidence points to. Otherwise pin RadioLib 7.4.0 if the bisect shows a regression.
5. **Isolate the RadioLib defect** (for the upstream PR): with the OEM proven good, diff the on-air/register behaviour RadioLib-vs-Semtech. ~~The `setRssiCalibration` 17-vs-18 nibble mismatch is the first thing to check.~~ **Done 2026-06-07 — checked & eliminated as root cause** (see hypothesis #1 + `UPSTREAM-PR-CANDIDATES.md`; it's a real but separate latent bug). **Next, still pure-code:** diff **calibrate/standby ordering** (RC-vs-XOSC at `begin()`) and the **`startReceive` RX-config** RadioLib-vs-Semtech, then sync-word expansion. The 7.4.0→7.7.0 source diff feeds experiment #1's bisect.
6. **Minor PR candidate:** implement `LR11x0::getFrequencyError()` (currently a stub returning 0).

**⭐ Frequency-controlled head-to-head — the comparison never actually run (analysis 2026-06-07, source/bench-record review, no new bench).** The RadioLib-vs-OEM verdict above is partly **confounded by carrier frequency**: the two drivers were never tested at the same Hz. Closing this is cheaper than the driver port (#4) and could re-weight #1 vs #2:
- The OEM **completed at +20 kHz (910.545)**. On the SAME board, RadioLib's §0.6 nudge reached **HEADER** (`0x50`) at **+15 kHz (910.540)** *and* **+30 kHz (910.555)** but **never completed**; the build-flag test jumped to **+50 kHz** (preamble only — overshoot). So **RadioLib has never been run at the OEM's exact winning frequency** (the nudge stepped 15→30 kHz, straight over +20).
- **The decisive cheap test:** run the bridge in a **fine sweep around 910.545** (`910.535/.540/.545/.550`, ≤5 kHz steps — portal-only on R2, or `-DCORE1121_FREQ_OFFSET_HZ≈20000`). Two clean, mutually-exclusive outcomes:
  - **RadioLib completes** at/near 910.545 ⇒ the deficit was **frequency + the 15-kHz nudge granularity skipping a narrow BW62.5 sweet spot**, NOT the RX path. Fix = a ~+20 kHz trim / AFC; the port is unnecessary. (Re-weights #2 over #1.)
  - **RadioLib still reaches header but never completes** at the OEM's exact 910.545 ⇒ **hypothesis #1 confirmed with frequency fully controlled** — RadioLib can't finish a packet the OEM finishes on the same chip at the same Hz. The confound-free confirmation §0.11 wanted; greenlights the port (#4) + PR (#5).
- Current lean is still #1 (RadioLib bracketed +20 kHz at both +15 **and** +30, reaching header, never completing where the OEM did) — but the 15-kHz step is too coarse to exclude a narrow completion window. **Run this fine sweep before the driver port.** Caveat FM4: the offset is boot-variable, so sweep per boot and note the variation.

**▶ FINE-SWEEP BENCH PROCEDURE (verified 2026-06-07 against source; the gating experiment):**
- **⚠️ The offset trap:** `-DCORE1121_FREQ_OFFSET_HZ=50000` (platformio.ini:68) is **compile-time** and **added to every sub-GHz freq** (Core1121.cpp:267-268) — NOT in NVS, so erase/portal does **not** remove it. The banner `[Radio2-Edge] ready — <commanded> MHz … (cfg 910.525 MHz, offset +N Hz)` shows the real commanded freq (the number before "MHz"). Handle it or the sweep is silently +50 kHz off.
- **TX source (external, mandatory):** the board can't self-generate MeshCore (R1 beacon is Meshtastic-gated; R1→R2 forward gated by `R2_RX_ONLY_TEST`). Use the Heltec V2 (SX1262) MeshCore node `9506C57C`, public (key `8b33…cd72`), **910.525/BW62.5/SF7/CR4-5/sync0x12/preamble8, FIXED** — only R2's RX center moves. Distance **2–5 m, NOT point-blank** (FM3; target R2 ≈ −70…−80 dBm). R2 sub-GHz antenna on **ANT2**. Abs-max RF in **+10 dBm** — never cable TX→RX. R1 stays as the positive control (`[R1 RX] N bytes`).
- **Method 1 — build-flag sweep (recommended, cleanest):** keep cfg 910.525; set platformio.ini:68 to `=10000`/`=15000`/**`=20000`**/`=25000` → effective **910.535/.540/.545/.550**. Per step: `pio run -e xiao_esp32s3 -t erase -t upload -t monitor` (`-t erase` mandatory — NVS else pins a stale freq). Press **RESET 2–3×** per freq (no reflash) to sample FM4 boot-variation. 910.545 (=20000) is the OEM's winner — watch hardest.
- **Method 2 — portal-live (faster per step; for fine sub-5 kHz localizing only):** set the offset flag `=0` once (rebuild+erase+upload), then sweep R2's "Frequency (MHz)" in the captive portal (no reflash, ~60–90 s/step). Keep the MeshCore name/key + Meshtastic identity fields intact or Save is rejected; re-enter via BOOT/serial in the ~5 s post-reset window.
- **Read the log:** completion (PASS) = `[R2 RX] N bytes RSSI… SNR…` (main.cpp:660) — that alone = RX_DONE; the follow-on `[R2 decoded] …` only prints for public text packets, so its absence ≠ failure. Heartbeat `[R2 HB] … irq=0x…` (5 s) auto-flags `0x08` (`<-- RX_DONE latched!`); read the rest by hand: `0x10`=preamble-only(fail), `0x40`/`0x50`=header reached(partial), `0x20`=sync/header-valid. `[Radio2-Edge] read: … state=-7` = CRC mismatch = demod ran **past the header** (near the sweet spot — better than `0x10`). Driver lines are `[Radio2-Edge]`, not `[R2]`. Optional clean capture: drop `-DRADIOLIB_DEBUG_BASIC=1` (platformio.ini:30) as a *separate* pass (its spew bypasses the serial mutex).
- **Outcome:** RX_DONE at/near effective 910.545 ⇒ frequency + nudge-granularity, fix = trim/AFC, no port (Path A). Stuck ≤`0x50`/only `state=-7` at the OEM's exact 910.545 across boots ⇒ RadioLib RX-path deficit confirmed with frequency controlled ⇒ greenlight Path B (`docs/LR1121-7.7.0-PROPAGATION.md`).

**The OEM control test is in the repo** (separate env, zero impact on the bridge):
- `lib/waveshare_lr1121/` — vendored Semtech `lr11xx_driver` (90 files). **Edited only for our board:** pins remapped to the R2 wiring (`wavesahre_lora_1121.h`), SPI 8→1 MHz, post-reset BUSY wait (`lr11xx_hal.cpp`). RX logic untouched.
- `src/oem_rx/` — `main.cpp` (auto-freq-sweep RX harness, per-event correlated logging: `rssi_inst`/`held ms`/`buf_len`/freq) + `lr1121_config.{h,cpp}` (OEM init, MeshCore-matched).
- `platformio.ini` → `[env:core1121_oem_rx]`. Flash: `pio run -e core1121_oem_rx -t erase -t upload -t monitor`. The bridge env (`xiao_esp32s3`) excludes `oem_rx/` via `build_src_filter`; both envs build clean.

**Debug build flags added to `Core1121.cpp`/`platformio.ini` (all default-OFF; flag-off == baseline):** `CORE1121_TCXO_RECAL` (+`CORE1121_TCXO_DELAY_US`), `CORE1121_FREQ_OFFSET_HZ`, `CORE1121_RSSI_CAL`, `CORE1121_RX_SYNC_OVERRIDE`. All bench-tested and ruled out (above); left in place as instruments.

**Upstream PR ledger:** `docs/UPSTREAM-PR-CANDIDATES.md` has the (conditional) RadioLib-LR1121 regression entry + the `getFrequencyError()` stub note. Session task #5 tracks "draft the PR if the regression is confirmed."

**Cross-project propagation:** `docs/LR1121-7.7.0-PROPAGATION.md` is the living guide for carrying the fix proven here to the two siblings (**Seeed Wio-LR1121** on `lr1121-phase1`; **T_LORA_DUAL** = the `coproc-tlora-dual` co-processor on branch `T_LORA_QUAD`). It has the affected-projects table, the three fix paths (A trim / B Semtech-driver port / C 7.4.0 regress) with per-project file-level recipes, a living findings ledger, and the shared acceptance test. Update it whenever a CORE1121 finding lands.

---

## Branches

| Branch | What it holds |
|---|---|
| **`main`** | Phase 0 — dual Wio-SX1262, sub-GHz only. Ships at v8.1. |
| **`lr1121-phase1`** | Phase 1 — Wio-LR1121 bring-up **and the full RX-deficit baseline** (Seeed correspondence, `LR1121-RX-INIT-AUDIT.md`, testbed, measurement method, detailed `CLAUDE.md`). The **baseline-of-record**; keep it intact until the RX-deficit question is resolved. |
| **`CORE1121`** *(this branch)* | Radio 2 swapped to the **WaveShare Core1121** (same LR1121 chip, published schematic) as a board-vs-chip control. See [`CORE1121.md`](CORE1121.md). |

**Plan:** eventually merge all branches to `main` once the Wio-LR1121 RX-deficit question is resolved.

## The goal

Phase 1: one XIAO ESP32-S3 hosting two LoRa radios bridging Meshtastic ↔ MeshCore across bands —
**R1 = Wio-SX1262** (sub-GHz, healthy reference radio), **R2 = LR1121** (dual-band). The open
issue is the LR1121's **marginal sub-GHz RX deficit** observed on the Seeed board. The Core1121
(this branch) is the board-vs-chip control that decides whether that deficit is a Seeed *board*
flaw or an LR1121 *chip* issue — full reasoning and status in [`CORE1121.md`](CORE1121.md).

## Rules of engagement (firm)

- **Don't guess** — cite the datasheet/schematic/code for electrical/timing/safety/RF-switch
  claims; say "I don't know" otherwise. Reverts and "no action needed" conclusions need the
  same citation.
- **One variable per experiment.** No mid-experiment edits that contaminate the record.
- **No RF-switch / PA / timing-sensitive changes without a written, datasheet/schematic-cited
  rationale.**
- **Never cable a high-power source into an RX front end** (LR1121 abs-max RF input = +10 dBm).
- **Owner reviews all outbound** (Seeed correspondence, PRs) — drafts only. **Accepted edits
  ship — don't re-ask.**
- **Pseudonym in docs; never the real name. Never force-push `main`.**

## Key docs (this branch)

1. **[`CORE1121.md`](CORE1121.md)** — the Core1121 summary, board facts, wiring, bring-up plan.
2. [`README.md`](README.md) — build / flash / wiring / captive-portal config.
3. [`docs/datasheets/waveshare/CORE1121-RF-SWITCH.md`](docs/datasheets/waveshare/CORE1121-RF-SWITCH.md) — RF-switch truth table, schematic-derived.
4. [`docs/REFERENCES.md`](docs/REFERENCES.md) — datasheet / reference index.
5. [`CHANGELOG.md`](CHANGELOG.md) — release history. [`V8-SPEC.md`](V8-SPEC.md) — Phase-0 portal-config spec.
6. **Background:** the `lr1121-phase1` branch (full Wio-LR1121 record).
7. **[`docs/LR1121-7.7.0-PROPAGATION.md`](docs/LR1121-7.7.0-PROPAGATION.md)** — ⭐ the cross-project conversion/regression guide: how the LR1121 RX fix proven here propagates to the Seeed Wio-LR1121 and T_LORA_DUAL siblings (living; per-project file-level recipes for fix paths A/B/C + a shared acceptance test).

## Quick recovery for a fresh session

1. Read this file, then `CORE1121.md`.
2. `git log --oneline -15` to confirm HEAD.
3. For the Wio-LR1121 history/baseline, switch to `lr1121-phase1` (or browse it on GitHub).
4. **Confirm the live bench state with the owner before assuming the firmware matches the docs.**
