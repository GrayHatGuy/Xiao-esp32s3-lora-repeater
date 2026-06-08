# COPROC-LR1121-DRIVER-PORT.md — plan: co-proc R3/R4 on Semtech `lr11xx_driver`

**Status:** PLAN ONLY (no code) — **live CONTINGENCY; the decision gate (§1) is NOT resolved.**
**Author:** GrayHatGuy · **Branch:** `T_LORA_QUAD_ROUTE` · 2026-06-07
**Gated on a control test — do not implement until it points here (see §1).**

> **Update 2026-06-07 (bench, PARTIAL — gate NOT resolved):** on-air bring-up showed
> the LR1121s receiving/transmitting over the air **only at close range** (R3 RX
> once @ −11 dBm, R4 RX @ −22…−49 dBm, R4 TX confirmed; **R3 TX not isolated**). That
> does **not** answer the §9 question — **weak/distant LR1121 RX was never tested** —
> so the RadioLib-7.7.0 LR11x0 RX-sensitivity deficit is still **OPEN, not disproven.**
> This port therefore remains a **live contingency** until the weak-signal test
> (and/or the 7.4.0 OEM control in §1) is actually run. (An earlier draft of this note
> claimed the LR1121s "complete RX and TX" and the port was "not required" — that
> over-claimed from close-range data and is corrected here.) See ROUTING-REDESIGN §9.

## 0. Why

The co-processor's R3/R4 are **LR1121s on RadioLib 7.7.0**. A Core1121 bench
session (`CORE1121` branch, CLAUDE.md §0.11, 2026-06-07) showed **Semtech's
official `lr11xx_driver` completes LoRa RX on this silicon while RadioLib never
does**, with every RadioLib-side knob ruled out — so RadioLib's LR11x0 RX path is
the leading root cause of the LR1121 RX deficit. This plan is the fix the
evidence points to **if** the deficit proves RadioLib-LR11x0-wide: swap *only the
co-proc's LR1121 radio HAL* from RadioLib to the Semtech driver, leaving the host
(SX1262 + the whole RX-priority pipeline) untouched. See ROUTING-REDESIGN §9.

## 1. Decision gate (run BEFORE any code)

Run the LR1121 OEM example on the **T-Lora-Dual** (it pins **RadioLib 7.4.0**):
- **7.4.0 RX works** → it's a **7.4.0→7.7.0 regression**. Cheaper fix first:
  pin the co-proc to 7.4.0 and reconcile the unified-IRQ API differences
  (`getIrqFlags`/`scanChannel`/`finishTransmit`); file the upstream PR
  (`CORE1121` task #5). **Do NOT do this port.**
- **7.4.0 RX also fails** → RadioLib-LR11x0-wide → **do this port.**

One variable per experiment: change the driver, nothing else, and re-run the
existing bench checklist.

## 2. Invariant: what does NOT change

The port is **co-proc-only**. Untouched:
- Host firmware (`src/`), the captive portal, `BridgeConfig`, the SX1262 driver.
- The RX-priority pipeline, dedup, route queues, airtime throttle — all above the
  `LoraRadio` interface and ignorant of the co-proc's internals.
- `LinkProtocol.h` (the UART wire format) and therefore `UartLink`/`RemoteRadio`.
- The co-proc's **architecture**: UART slave + per-radio TX queue + non-blocking
  `startTransmit`/TX-done-IRQ + CAD-gated dispatch + IRQ-driven RX. Only the calls
  *inside* those steps move from `RadioLib LR1121::…` to `lr11xx_…`.

Net: the host can't tell which driver the co-proc uses. The blast radius is
`coproc-tlora-dual/` only.

## 3. Reference to reuse

The Core1121 repo already vendored the driver and a working HAL shim:
`lib/waveshare_lr1121/` (Semtech `lr11xx_driver` + SPI@1 MHz + post-reset BUSY
wait) and `src/oem_rx/` + `[env:core1121_oem_rx]`. **That env bench-completed RX**,
so it is the authoritative init/RX sequence to port from. Lift its HAL shim and
init order; adapt pins/SPI to the T-Lora-Dual (two radios, not one).

## 4. Work items (co-proc)

1. **Vendor the driver** into `coproc-tlora-dual/lib/lr11xx/` (mirror the
   Core1121 vendoring) and drop the `jgromes/RadioLib` dep from the co-proc env
   (host keeps RadioLib). Build it as a PlatformIO `lib/`.
2. **HAL shim** `lr11xx_hal_*` (SPI read/write/read-write, reset, wakeup, BUSY
   wait) over Arduino `SPIClass`. The driver threads a `void* context` to every
   call — pass a per-radio struct {SPI, nss, busy, reset, dio9} so **both** R3 and
   R4 share one shim with distinct contexts. (Core1121 was single-radio; the only
   real new work vs. its shim is making it context-parameterised for two radios.)
3. **Replace the radio HAL calls** in `coproc-tlora-dual/src/main.cpp`. Indicative
   mapping (confirm exact names/params against the vendored headers + OEM example;
   the LR1121 user manual is the RF-claims source):
   - `radio.begin()` → reset + `lr11xx_system_set_reg_mode` +
     `…_set_tcxo_mode` + `…_system_calibrate` + `lr11xx_radio_set_pkt_type(LORA)`.
   - `setFrequency` → `lr11xx_radio_set_rf_freq`.
   - `setBandwidth/SF/CR` (+ LDRO) → `lr11xx_radio_set_lora_mod_params`.
   - `setSyncWord` → `lr11xx_radio_set_lora_sync_word`.
   - preamble/header/CRC/IQ → `lr11xx_radio_set_lora_pkt_params`.
   - power/PA → `lr11xx_radio_set_pa_cfg` + `lr11xx_radio_set_tx_params`
     (**sub-GHz HP PA vs 2.4 GHz HF PA differ — this is the band-aware bit; cite
     the UM**).
   - RF switch (DIO5–8, Factory table) → `lr11xx_system_set_dio_as_rf_switch`.
   - IRQ routing → `lr11xx_system_set_dio_irq_params` (RX_DONE/TX_DONE/CAD on DIO9).
   - `startReceive` → `lr11xx_radio_set_rx`.
   - **RX read** (on DIO9): `lr11xx_system_get_irq_status` → if RX_DONE →
     `lr11xx_radio_get_rx_buffer_status` + `lr11xx_regmem_read_buffer8` +
     `lr11xx_radio_get_lora_pkt_status` (RSSI/SNR) → `…_clear_irq_status`.
   - `startTransmit` → `lr11xx_radio_set_buffer_base_address` +
     `…_write_buffer8` + `lr11xx_radio_set_tx`.
   - `finishTransmit` → clear IRQ + back to RX.
   - `scanChannel` (CAD) → `lr11xx_radio_set_cad_params` + `lr11xx_radio_set_cad`;
     result via the CAD_DONE/CAD_DETECTED IRQ bits.
4. **Free win:** with `lr11xx_system_get_irq_status` read explicitly, the loop
   distinguishes TX_DONE vs RX_DONE vs CAD_DONE **directly** — this *retires* the
   `g_txInFlight`-based DIO9 disambiguation and resolves the review's coproc-fsm
   "TxDone vs RxDone ambiguity" finding by construction. The UART-side contract
   (every `MSG_TX` answered by exactly one `MSG_TX_DONE`, CAD-then-non-blocking-TX)
   is unchanged.

## 5. Risks / unknowns to confirm at implementation

- **Dual-instance state:** the C driver is context-based; verify nothing is
  static/global so two LR1121s coexist (the Core1121 shim was single-radio).
- **2.4 GHz HF PA + wideLora BW:** the band-aware PA config and the 406.25/812.5/
  1625 kHz BW values must come from the LR1121 UM / OEM example, not guessed — this
  is the same 2.4 GHz wideLora unknown that already gates the project.
- **Image/RSSI calibration:** port the OEM env's calibration order verbatim (it's
  what made RX complete); do not re-derive.
- **No `RemoteRadio` change** is needed, but re-run the full bench checklist —
  this swaps the one component the whole Phase-2 pivot assumed was sound.

## 6. Effort / shape

Contained: 1 vendored lib + 1 HAL shim (≈the Core1121 one, parameterised) + a
rewrite of the radio calls in one file (`coproc-tlora-dual/src/main.cpp`). Host
and protocol untouched. Build-verifiable on its own (`pio run -d
coproc-tlora-dual`); on-air still owner-validated.
