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

**Session: 2026-06-07. Status: OPEN — root cause not yet confirmed; instrumentation added, two tests queued.**

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

**P1.2 — Measure the offset directly (instrumented build `c4445fa`/`53ec302`).** `pio run -t erase` then `-t upload`. Switch R2 to the **Meshtastic BW250** config (R2 completes packets there). Read the new **`freqErr=N Hz`** log line: R1 (SX1262) ≈ 0 vs R2 (LR1121) offset. Gives the magnitude **and** tells us whether BW250 carries the same offset (chip-wide mistune) or only BW62.5 fails (BW-specific).

**Then the FIX (P2):** the offset is >+30 kHz (≈33–50 ppm) **low** — far beyond TCXO tolerance ⇒ a calibration/synthesis issue, likely tied to the recurring **`HF_XOSC_START_ERR` (0x0020)**. In `Core1121::begin()`, one variable at a time (re-measure `freqErr` after each): (1) review TCXO/XOSC config so the XOSC locks clean (clears `0x0020`); (2) force an explicit image/frequency recalibration for the operating band.

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

## Quick recovery for a fresh session

1. Read this file, then `CORE1121.md`.
2. `git log --oneline -15` to confirm HEAD.
3. For the Wio-LR1121 history/baseline, switch to `lr1121-phase1` (or browse it on GitHub).
4. **Confirm the live bench state with the owner before assuming the firmware matches the docs.**
