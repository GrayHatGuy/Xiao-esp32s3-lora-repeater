# Path B implementation plan — port R2 (LR1121) to Semtech `lr11xx_driver`

**Status: IN PROGRESS — Milestone 0 STATIC LINK PASS (2026-06-07).** `pio run -e
xiao_esp32s3_colink_test` builds + links RadioLib + the vendored Semtech driver into one binary, no
conflict (Flash 8.8%/292 KB). The co-link gating unknown is resolved ⇒ **Strategy 1 (RadioLib R1 +
Semtech R2) confirmed; the all-Semtech fallback is not needed.** Runtime half (concurrent SPI/mutex)
pending owner flash. Code in `src/colink_test/`.
Companion to `CLAUDE.md` §0.11 (root-cause) and `docs/LR1121-7.7.0-PROPAGATION.md` (cross-project rollout).
Derived from a 5-agent design pass (co-link feasibility · interface mapping · HAL/mutex/ISR · scope gaps).

## Goal & architecture
Replace RadioLib on the **R2 / LR1121 RX+TX path** with Semtech's bench-proven `lr11xx_driver`, **keep
RadioLib for R1 / SX1262** (it works). A new `SemtechLR1121` class implements the existing `LoraRadio`
interface (same surface as `Core1121.h` — `begin/available/read/transmit/startReceive/debugIrqStatus`),
so `main.cpp`'s bridge logic is unchanged. A thin `SemtechLR1121Hal` binds the vendored driver to R2's
pins + the **shared `SPIClass` + FreeRTOS `spiMutex`**. The vendored tree `lib/waveshare_lr1121/` stays
**pristine** (all glue lives in `src/`). Gated by build flag **`USE_SEMTECH_LR11XX_DRIVER` (default OFF
== today's RadioLib baseline)**; `makeRadio()` selects the driver.

## Co-link verdict: FEASIBLE, with two HIGH-risk items to fix
No symbol collisions — `lr11xx_*` (C) vs RadioLib C++ are cleanly separated; headers include together.
The two real hazards are runtime SPI sharing, not link-time:
1. **`SPI.begin()` double-init** — both RadioLib's `Module` *and* the vendored `lr11xx_hal.cpp` call
   `SPI.begin()` unconditionally. Second call re-inits the bus. → Guard with a one-shot `_spiInitialized`
   flag (skip if the bus is already up). *(High)*
2. **No mutex in the vendored HAL** — `lr11xx_hal_*` does raw SPI with no locking; the bridge runs two
   FreeRTOS tasks on one bus. → Wrap every `lr11xx_*` SPI call in the shared `spiMutex` at the
   `SemtechLR1121` method boundary (the pattern `WioSX1262`/`Core1121` already use). *(High)*
3. Minor: drop the vendored HAL's **10 s BUSY timeout → ~1 s** so a wedged LR1121 can't starve R1's SPI.

## Two strategies + the trigger (the all-Semtech fallback)
- **Strategy 1 — RadioLib R1 + Semtech R2 (PRIMARY).** Less work, surgical; co-link came back feasible.
- **Strategy 2 — all-Semtech (FALLBACK).** If Milestone 0 shows the RadioLib↔Semtech bus-sharing is
  intractable, port R1/SX1262 to Semtech's `sx126x_driver` too and drop RadioLib entirely (two Semtech
  C drivers share one HAL pattern and co-link trivially). **Trigger = Milestone 0 fails to reconcile the
  shared SPI/mutex.** The `SemtechLR1121Hal` is deliberately chip-agnostic so falling back costs only an
  `sx126x` R1 wrapper, **not** a redo. Strategy 2 is *not* the opening move — we don't rewrite a working
  SX1262 unless the evidence forces it.

## Milestones (each with a gate; nothing skips ahead)
| # | Title | Deliverable | Gate |
|---|---|---|---|
| **0** | **Co-link smoke test** *(the gate)* — ✅ **STATIC LINK PASS** | `[env:xiao_esp32s3_colink_test]` (`src/colink_test/`): R1 (RadioLib SX1262) + Semtech R2 facade (separate TU) on the shared mutex; reads R2 version; a task alternates R1/R2 SPI for 10 s | **Links clean ✅** (RadioLib + vendored Semtech in one elf, no conflict). **Runtime pending owner flash:** both chips answer + concurrent SPI shows no corruption/deadlock (`CO-LINK PASS`). |
| 1 | `SemtechLR1121` wrapper | the 4 new files + BridgeConfig offset field + `makeRadio()` select | builds with `-DUSE_SEMTECH_LR11XX_DRIVER=1`; boot shows "Radio 2 driver: Semtech"; both ready banners |
| 2 | Sub-GHz RX **+ TX** validation | fine-sweep (offset=0) finds the sweet spot; R2 completes; TX both directions | R2 decodes ≥5 consecutive pkts at the sweet spot; TX `state=0`; R1 unaffected; **flag OFF still = clean RadioLib baseline** |
| 3 | 2.4 GHz | extend `setFrequency()` for >1 GHz + HF RF-switch | **DEFERRED** post-MVP (sub-GHz first; needs ANT1/HF bench) |
| 4 | Propagate to siblings | apply wrapper to Seeed Wio + T_LORA_DUAL co-proc, **per-board offset measured** | each sibling passes §7 + §7a on its own hardware |

## Files
**New (all in `src/`, vendored tree untouched):**
- `SemtechLR1121Hal.{h,cpp}` — binds the driver to R2 pins + shared SPI + mutex; init sequence mirrors
  `src/oem_rx/lr1121_config.cpp` (reset→wakeup→standby XOSC→calibrate_image→reg_mode→rf_switch→
  tcxo_mode→lfclk→clear_errors→calibrate 0x3F); one-shot `SPI.begin()` guard; all SPI calls mutex-wrapped.
- `SemtechLR1121.{h,cpp}` — `LoraRadio` impl. `read()` = `get_and_clear_irq_status` → on `0x08`:
  `get_rx_buffer_status` → `regmem_read_buffer8` → `get_lora_pkt_status` (rssi/snr) → map status to
  RadioLib codes (`OK→ERR_NONE`, CRC→`ERR_CRC_MISMATCH`) → clear IRQ → standby(XOSC) → `set_rx(continuous)`.
  `transmit()` = `set_pa_cfg`/`set_tx_params`/`set_tx` then auto-return to RX. External DIO9 ISR sets a
  flag (dumb); IRQ is polled inside `read()`. Runtime `_frequencyOffsetHz` from BridgeConfig (**never a
  compile-time constant**).

**Modify:**
- `platformio.ini` — add `[env:xiao_esp32s3_colink_test]`; add `-DUSE_SEMTECH_LR11XX_DRIVER` (commented,
  OFF) + `lib_ldf_mode=deep+` note for the full-port env; mark the old `CORE1121_FREQ_OFFSET_HZ` block
  **DEPRECATED (Path A)**.
- `src/main.cpp` — `makeRadio()` selects `Core1121` (RadioLib) vs `SemtechLR1121` by the flag; resolve
  per-radio offset from BridgeConfig; log which driver is active; ensure `spiMutex` exists before ctors.
- `src/BridgeConfig.{h,cpp}` — add per-radio `frequencyOffsetHz` (NVS schema **v5→v6**, default 0,
  auto-migrate); getter/setter; optional portal field. **This is the offset-tolerant mechanism.**
- `src/LoraRadio.h` — doc only (clarify `read()` returns RadioLib-standard codes from either driver).
- `docs/LR1121-7.7.0-PROPAGATION.md` — add the CORE1121 Path B sub-recipe + advance §5/§8 as milestones land.

## Key design decisions (made by the pass)
- **Offset-tolerance = per-board RUNTIME value in BridgeConfig (NVS), measured per board** — *never* the
  compile-time `CORE1121_FREQ_OFFSET_HZ` (that's the Path-A mistake). Optional auto-sweep/AFC is a later task.
- **TX/PA: IN-SCOPE** for the first port (the bridge is bidirectional; ~30 lines via `set_pa_cfg`/`set_tx*`).
- **2.4 GHz: DEFERRED** to Milestone 3 (sub-GHz is what's proven + release-gating).
- **Vendored driver stays pristine** (diff-able against upstream); glue lives in `src/`.
- **RF-switch is per-driver** (each chip configures its own DIOs — R1 RadioLib, R2 `set_dio_as_rf_switch`); no conflict.
- **Keep the external DIO9 ISR** as a dumb flag-setter; poll the driver's IRQ status inside `read()`.

## Top risks → mitigations
- **SPI `begin()` double-init** → one-shot guard; caught in Milestone 0.
- **BUSY wait starving R1's SPI** → do the ~142 ms post-reset BUSY poll *before* taking the mutex; 1 s fail-fast.
- **Concurrent `read()` race on the bus** → hold the mutex across the whole IRQ-read+payload+re-arm sequence.
- **Boot-variable offset (FM4)** → per-board NVS cal (not a fixed trim); auto-lock if it proves to drift per boot.
- **Accidental hardcoded-offset regression** → Milestone 2 acceptance test runs with **zero compile-time offset**; if R2 needs one to complete, the offset-tolerant requirement is violated → not release-grade.
- **RadioLib protected-method idiom (`LR1121Access`) coexistence** → RadioLib-specific, no Semtech equivalent; Milestone 0 flushes any symbol clash.

## Open decisions for the owner
1. **Offset-tolerance mechanism:** (a) auto-sweep-lock every boot (~7 s/power-cycle, "just works"), (b)
   per-board NVS calibration measured once + portal-editable (clean, needs a cal step) — *pass recommends (b)*,
   or (c) full AFC (out of MVP scope). **Your call.**
2. **Strategy 1 vs the all-Semtech fallback:** proceed Strategy 1 (RadioLib R1) since co-link is feasible,
   with Strategy 2 held as the Milestone-0-triggered fallback? *(recommended)*
3. **Build-flag lifecycle:** keep `USE_SEMTECH_LR11XX_DRIVER` default-OFF until all three projects ship
   Path B, then flip default-ON and retire the RadioLib LR1121 path as dead code? *(recommended)*
4. **Sibling order:** Seeed Wio-LR1121 (single LR1121, simpler) **before** T_LORA_DUAL (dual-LR1121 co-proc)? *(recommended — de-risks the dual-instance case)*
