# HANDOFF — HONEST RESET (2026-06-08)

**READ THIS BEFORE `CLAUDE.md` §0.11.** The §0.11 "RadioLib 7.7.0 LR11x0 RX deficit" conclusion is
**UNVERIFIED and likely overstated.** This document corrects the record. The owner is (rightly)
out of patience with elaborate scaffolding built on an unproven premise — keep the next steps small
and skeptical.

## The core mistake
Every "RadioLib fails to receive" data point compared the proven vendor **Semtech `lr11xx_driver`**
(env `core1121_oem_rx`) against **Claude-written bridge code** (`src/Core1121.cpp` + the begin /
RF-switch / TCXO-calibration / IRQ-DIO / frequency-offset setup). It was **never** compared against a
clean, minimal, *correct* RadioLib LR1121 RX. So "RadioLib's RX path is deficient" may actually be
**"the bridge's RadioLib usage is buggy."** The one proven-good reference — the **LilyGO T-Lora-Dual
Factory code** — was available the whole time and was not used as the baseline.

That unverified conclusion was then written into `CLAUDE.md` §0.11, `docs/LR1121-7.7.0-PROPAGATION.md`,
and `docs/PATH-B-PLAN.md`, which the **Seeed Wio-LR1121** and **T_LORA_DUAL** projects were told to
follow. **Treat all of it as suspect until the test below is run.**

## What is actually PROVEN
- The Core1121's LR1121 is alive (Base FW 1.1) and **can receive ≥1 real packet** under Semtech's
  `lr11xx_driver` (RX_DONE / CRC-OK, 149 B @ 910.545 MHz, −74 dBm — `core1121_oem_rx`). So the
  hardware / antenna / wiring **can** receive. (Caveat: it was ~1 completion in 3.5 min, frequency
  confounded.)
- RadioLib (SX126x) + Semtech (lr11xx) **link cleanly in one binary** (Path-B M0 static co-link OK).
- **The bridge's `logf()` races across the two FreeRTOS cores** — serial output clips/interleaves
  (seen: `Mech=0x11` for `MeshCore GRP_TXT ch=0x11`, `o1-B2B]` for `[Radio1-B2B]`). So **every bench
  log read through it is partially unreliable**, including the ones §0.11 rests on.
- RadioLib **7.4.0**: the bridge compiles + links clean (the "7.7.0-only symbols" worry was wrong).
  One bench run: R2 reached only `0x10` (preamble) at commanded 910.545 — but the offset is
  boot-variable and the bench sources were +0.6 and +9.8 kHz off that run, so this is **not** clean.

## What is SUSPECT / UNVERIFIED
- **All of §0.11's "RadioLib RX deficit."** Never tested against clean RadioLib usage.
- The fine-sweep "reaches header-err `0x50`, never completes `0x08`" — measured through the buggy
  bridge + the racing logger.
- "Path A (trim) ruled out", "Path B (Semtech port) is the fix", the Path-C reasoning — **all** rest
  on the above.
- `docs/LR1121-7.7.0-PROPAGATION.md` and `docs/PATH-B-PLAN.md` — premature; built on the unverified
  premise. Do not propagate to the siblings yet.

## DO THIS FIRST — the one experiment that settles it
Before trusting ANY prior conclusion: build a **minimal, single-radio, correct RadioLib LR1121 RX**
for the Core1121 that mirrors the **Factory** bring-up exactly — **no bridge, no `Core1121.cpp`, no
smoke-test / port scaffolding.** Receive the bench MeshCore stream (910.525 / BW 62.5 / SF7 / sync
0x12) from a few metres, with the source actively transmitting.
- **Completes packets → RadioLib works; the bridge code is the bug.** The deficit, the Semtech port,
  and the propagation docs are all moot. Fix = rewrite the bridge's LR1121 path to match the
  Factory / the minimal sketch.
- **Doesn't complete → the deficit is real,** now established cleanly without the noise.

**Prerequisite (unresolved):** confirm what radio library the LilyGO Factory actually uses. Its API
(`radio.begin/setFrequency/setBandwidth/setTCXO/startReceive`) is RadioLib-style, but RadioLib is
**not** pinned in its `platformio.ini` (`lib_deps = ./lib`, only Adafruit_NeoPixel). Read
`…\Projects\T-Lora-Dual-master\T-Lora-Dual-master\T-Lora-Dual\examples\Factory\Factory.ino` and its
`lib/` to settle it. If it's RadioLib and the Factory RXes on hardware, that alone implicates the
bridge, not RadioLib.

## Dead ends — do NOT repeat
- `src/colink_test/` (Path-B M0 runtime smoke test): the vendored Semtech HAL reads return **garbled
  data** (consistent `hw=0x13 type=0x00 fw=0xC000`) when co-linked with RadioLib on the shared SPI,
  even on the boot read before R1 runs. ~7 bench iterations were burned on it (SPI `begin`/`ss`/CS
  theories — all wrong). The vendored HAL's persistent-transaction + raw-`SPI.transfer` design does
  not share an SPI bus. **Paused, unsolved.** Don't resume without rewriting the read framing
  (RadioLib-style: two transactions, bus released between phases).

## Also fix (independent, real)
- The **cross-core serial-logging race** (`src/SerialLog.h` / `logf`). It claims to serialise across
  cores but doesn't — it corrupts bench logs. Fix this before trusting ANY future bench data;
  otherwise every result is read through a garbled lens.

## Current repo state (uncommitted/test pins to be aware of)
- `platformio.ini` `[env:xiao_esp32s3]`: **RadioLib pinned `@ 7.4.0`** (Path-C test) and
  `CORE1121_FREQ_OFFSET_HZ=20000` (sweep test value). Baseline was `@ 7.7.0` / offset `0`. Revert if
  starting fresh.
- `src/colink_test/` + `[env:xiao_esp32s3_colink_test]`: the paused Path-B smoke test (excluded from
  the bridge build via `build_src_filter`).
- The `core1121_oem_rx` env (vendored Semtech driver) is intact and is the one thing that
  demonstrably received.
