# Firmware code changes — sessions 3–4 (function-level)

Discrete, code-level detail of every firmware commit on `lr1121-phase1` this run.
File-level summary is in `CLAUDE.md` §0.11; this is the function-by-function record.
All diffs verified from `git show`, not memory.

---

## `efbf88c` — MODE_STBY `{1,0}` → `{0,0}` revert (owner-committed; edit by Claude)
**File:** `src/WioLR1121.cpp`, `begin()`, the `rfswitch_table[]`.

- **Code:** the `MODE_STBY` entry changed `{ 1, 0, 0, 0, 0 }` → `{ 0, 0, 0, 0, 0 }` (DIO5 1→0).
- **Comment block above the table rewritten** to explain: `{0,0}` is SKY13373 *shutdown* (antenna
  isolated from the LNA input during TX→STBY), restoring the protection that commit `949176a`'s
  `{1,0}` (RX-latched) removed. The 20 µs shutdown→active re-arm penalty is accepted (negligible for
  a continuous-listen repeater; it's absorbed by the chip's mode-transition times — UM §2.4).
- **Behaviour:** during standby the RF switch now opens (V1=V2=0) instead of latching the antenna onto
  the RX/LNA path. **Net effect on the bug: none** — the `-20` later reproduced *with* `{0,0}`, so the
  revert is correct-and-kept but was **not** the cause.

## `8f29bed` — LR1121 chip-EUI boot logging
**Files:** `src/WioLR1121.cpp`; new `docs/testbed/MODULE-REGISTRY.md`.

1. **`LR1121Access` struct** — added `using LR11x0::getChipEui;` (the struct now publicly re-exposes
   the RadioLib-protected `getErrors`, `setRssiCalibration`, **`getChipEui`**).
2. **`begin()`** — after the `"[name] ready — …"` line, added a block that reads the unique 64-bit chip
   EUI and prints it:
   ```cpp
   uint8_t chipEui[8] = {0};
   int16_t euiState = static_cast<LR1121Access*>(_radio)->getChipEui(chipEui);
   if (euiState == RADIOLIB_ERR_NONE)
       Serial.printf("[%s] chip EUI = %02X:%02X:…:%02X (correlate w/ MODULE-REGISTRY.md)\n", …);
   else
       Serial.printf("[%s] chip EUI read FAILED: %d (module unidentified)\n", …);
   ```
3. **New doc** `MODULE-REGISTRY.md` — maps EUI → physical module (suspect-GOOD = `00:16:C0:01:F0:9B:37:D5`,
   no sharpie dot; suspect-BAD = red dot, EUI TBD).
- **Behaviour:** every boot prints `[Radio2-Edge] chip EUI = …`, so logs self-identify which of the two
  identical Wio-LR1121 modules is mounted (fixes the single-sample-ambiguity that contaminated earlier runs).

## `66dac8b` — IRQ-status diagnostic + `R2_RX_ONLY_TEST` mode
**Files:** `src/WioLR1121.h`, `src/WioLR1121.cpp`, `src/main.cpp`, `platformio.ini`.

1. **`WioLR1121.h`** — new public method declaration `uint32_t debugIrqStatus();` (with a comment that it
   reads the raw LR11x0 IRQ register so the heartbeat can see RX_DONE/header/preamble state even when the
   DIO9 edge ISR never fired).
2. **`WioLR1121.cpp` — new `debugIrqStatus()`** (appended after `startReceive()`):
   ```cpp
   uint32_t WioLR1121::debugIrqStatus() {
       xSemaphoreTake(_mutex, portMAX_DELAY);
       uint32_t irq = _radio->getIrqFlags();   // PUBLIC RadioLib method — raw LR11x0 IRQ register
       xSemaphoreGive(_mutex);
       return irq;
   }
   ```
   **NOTE (corrects an earlier handoff slip):** this uses the **public** `getIrqFlags()` directly — it is
   **not** routed through `LR1121Access`, and `getIrqStatus` was **not** added to that struct.
3. **`WioLR1121.cpp` — `transmit()`** — under `LR1121_DEBUG`, added a post-TX IRQ read and appended
   `irq=0x%08lX` to the existing `transmit(… ) tx=… post-rx=…` log line (`irqAfterTx = _radio->getIrqFlags()`).
4. **`main.cpp` — `radio1Task()`** — wrapped the R1→R2 forward in a compile gate:
   ```cpp
   #ifndef R2_RX_ONLY_TEST
       if (g_radioEnabled[1]) bridgePacket(g_chan[0], g_chan[1], radio2, "R1", buf, len);
   #endif
   ```
5. **`main.cpp` — `radio2Task()` heartbeat** — added `uint32_t irqRg = radio2_lr_diag->debugIrqStatus();`
   and extended the HB printf to `… irq=0x%08lX%s` with a `"  <-- RX_DONE latched!"` flag when `irqRg & 0x08`.
6. **`main.cpp` — `setup()`** — added, under `#ifdef R2_RX_ONLY_TEST`, a banner line printed before
   `"Bridge active."`: `*** R2_RX_ONLY_TEST: R1->R2 forward DISABLED — R2 is RX-only (diagnostic) ***`.
7. **`platformio.ini`** — added the build flag `-DR2_RX_ONLY_TEST` (with a comment that it's a temporary
   diagnostic and that deleting the `-D` restores normal bridging).
- **Behaviour:** R2 heartbeat now reads `[R2 HB] isr=N (+d/5s) rxFlag=x irq=0x________`. With the flag set,
  R2 only listens (no R1→R2 forwarding) — the clean config for an RX measurement. **This flag is CURRENTLY
  ACTIVE**; the bridge is half-live until the `-D` is removed and the firmware reflashed.

---

### IRQ-register bit reference (the `irq=` field)
`0x08` RX_DONE · `0x10` PREAMBLE_DETECTED · `0x20` SYNC_WORD/HEADER_VALID · `0x40` HEADER_ERR · `0x80` CRC_ERR.
(Seen in practice: `0x10` = preamble only; `0x50` = preamble + HEADER_ERR; `0x08` = full decode.)

### Errors seen in logs
`-1` UNKNOWN · `-5` TX_TIMEOUT · `-7` CRC_MISMATCH · `-20` WRONG_MODEM (RadioLib 7.7.0 `TypeDef.h`).
