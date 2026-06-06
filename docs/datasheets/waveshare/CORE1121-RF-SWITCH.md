# Core1121-XF RF-switch wiring + truth table (schematic-derived)

**Board:** WaveShare **Core1121-XF** (Semtech LR1121 module).
**Authoritative source:** WaveShare-published schematic **`Core1121_XF_Sch.pdf`**
(owner copy: `~/Downloads/CORE1121 Design Docs/Core1121_XF_Sch.pdf`; wiki
`https://www.waveshare.com/wiki/Core1121-XF`).
**Derived for:** the CORE1121 branch firmware (`src/Core1121.cpp` RF-switch
table). Satisfies project rule "any RF-switch table change needs written,
datasheet/schematic-cited justification" and handoff success-criterion #1.

> This is the WaveShare counterpart to the Seeed Wio-LR1121's switch record.
> The Seeed module needed a support email (David Du, 2026-05-28) because its
> module datasheet omitted the RF-switch wiring. WaveShare publishes a full
> schematic, so the wiring below is read **directly off the sheet** — no guessing.

---

## 1. The switch part and its control wiring

- **U1 = pSemi/Peregrine `PE4259` — a reflective SPDT (SP2T) RF switch.**
  (The Seeed Wio used a Skyworks **SKY13373** SP3T — a *different part with a
  different truth table*. This is the one substantive firmware difference.)

Schematic net connections at U1. **The PE4259 is wired in _complementary-pin
control_ mode** (PE4259 datasheet Table 6): *both* logic pins are driven — pin 6
by DIO5 and pin 4 by DIO6 (each through a 100 Ω series resistor, R6 / R5) — so
pin 6 is **not** a static VDD supply rail here but the second control input.

| PE4259 pin | Pin name | Connected net | Role |
|---|---|---|---|
| 6 | VDD/ctrl | **RFSW0_V1** = LR1121 **DIO5** (via R6 100 Ω) | control A |
| 4 | CTRL     | **RFSW1_V2** = LR1121 **DIO6** (via R5 100 Ω) | control B |
| 5 | RFC      | **LORA_ANT** (→ ANT2)         | common / antenna |
| 1 | RF1      | RFO_HP_LF / RFO_LP_LF (PA)    | **transmit** path |
| 3 | RF2      | RFI_P_LF0 / RFI_N_LF0 (LNA)   | **receive** path |

- **RF1 (TX):** fed from the LR1121 PA outputs through `R3` and `R4`. On the
  schematic **`R3 = 0 Ω` (populated)** wires the **high-power** output
  `RFO_HP_LF` (LR1121 pin 32) to RF1; **`R4 = NC`** leaves `RFO_LP_LF` (pin 31)
  disconnected. ⇒ **only the HP PA path is physically connected.**
- **RF2 (RX):** the differential LNA inputs `RFI_P_LF0` / `RFI_N_LF0`
  (LR1121 pins 30/29) through the L7/L8 matching.
- **RFC → LORA_ANT (ANT2)** is the **sub-GHz** antenna port.

## 2. Truth table (printed on the schematic)

The sheet prints (with an obvious `RFSW0_V2`→`RFSW1_V2` label typo; the two
control nets are `RFSW0_V1`=DIO5 and `RFSW1_V2`=DIO6):

| DIO5 (V1) | DIO6 (V2) | PE4259 routing |
|:---:|:---:|---|
| 0 | 1 | **RFC → RF1**  (transmit) |
| 1 | 0 | **RFC → RF2**  (receive)  |
| 0 | 0 | isolated (standby)        |

## 3. Mapped to RadioLib LR11x0 RF-switch modes

Installed by `Core1121::begin()` via `setRfSwitchTable()`:

| RadioLib mode | DIO5 | DIO6 | Rationale |
|---|:---:|:---:|---|
| `MODE_STBY`  | 0 | 0 | switch isolated |
| `MODE_RX`    | 1 | 0 | RFC→RF2 (LNA). Used for **both** bands — harmless for 2.4 GHz (see §4) |
| `MODE_TX`    | 0 | 1 | RFC→RF1 (PA) |
| `MODE_TX_HP` | 0 | 1 | RFC→RF1 (PA). Operative path at our TX power — only the HP PA is wired (R3=0 Ω) |
| `MODE_TX_HF` | 0 | 0 | 2.4 GHz bypasses the PE4259 (see §4) |
| `MODE_GNSS`  | 0 | 0 | unused |
| `MODE_WIFI`  | 0 | 0 | unused |

**Switch pins:** only **DIO5 + DIO6**. `DIO7`/`DIO8` are broken out on the
module header (U3) but are not switch controls; **`DIO10`/`DIO11` carry the
32.768 kHz crystal `Y2`** on this board — so all of DIO7/8/10 stay
`RADIOLIB_NC` and the chip never drives them.

### Difference vs the Seeed Wio-LR1121 table

| Mode | Seeed Wio (SKY13373) | **Core1121 (PE4259)** |
|---|:---:|:---:|
| STBY | {0,0} | {0,0} |
| RX | {1,0} | {1,0} |
| **TX** | **{1,1}** | **{0,1}** |
| TX_HP | {0,1} | {0,1} |

Only **`MODE_TX`** differs (`{1,1}`→`{0,1}`). The Seeed SP3T had a distinct
low-power TX port at `{1,1}`; the Core1121 SPDT has a single TX port (RF1), so
`MODE_TX` and `MODE_TX_HP` are the same `{0,1}`.

## 4. Two antennas — the 2.4 GHz path bypasses the switch

The Core1121-XF is **dual-antenna**:

- **ANT2 / `LORA_ANT`** — sub-GHz, behind the PE4259 (RF1=TX / RF2=RX).
- **ANT1 / `2.4G_ANT`** — 2.4 GHz, fed from `RFIO_HF` (LR1121 pin 26) through
  the L9/L10/C24… matching, **with no external switch**.

So 2.4 GHz TX/RX share `RFIO_HF` on their own antenna and never touch the
PE4259. That is why `MODE_TX_HF = {0,0}` and why a single `MODE_RX = {1,0}`
serves both bands (the DIO5-high state is simply irrelevant to the HF port).

## 5. Cross-check against WaveShare's own firmware

WaveShare's ESP32-S3 demo (`Core1121_XF_Demo platformio.zip`) configures the
LR1121 RF switch identically (Semtech `smtc_shield` HAL,
`lr1121_common.c`):

```c
const lr11xx_system_rfswitch_cfg_t ..._rf_switch_cfg = {
    .enable  = LR11XX_SYSTEM_RFSW0_HIGH | LR11XX_SYSTEM_RFSW1_HIGH, // DIO5+DIO6
    .standby = 0,                       // {0,0}
    .rx      = LR11XX_SYSTEM_RFSW0_HIGH, // {1,0}
    .tx      = LR11XX_SYSTEM_RFSW1_HIGH, // {0,1}
    .tx_hp   = LR11XX_SYSTEM_RFSW1_HIGH, // {0,1}
    .tx_hf   = 0,                        // {0,0}
};
```

Schematic and vendor firmware **agree** — high confidence in the table above.

## 6. Other board facts (from the same schematic / demo)

- **TCXO:** 32 MHz (Y1), reference **3.0 V** — WaveShare demo sets
  `LR11XX_SYSTEM_TCXO_CTRL_3_0V`. Firmware: `LR1121_TCXO_VOLTAGE = 3.0f`
  (unchanged from the Seeed value).
- **LF clock:** 32.768 kHz crystal `Y2` on DIO10/DIO11; demo uses
  `LR11XX_SYSTEM_LFCLK_XTAL`. Not required for the continuous-RX repeater path
  (RadioLib's default LF handling is fine); noted for completeness.
- **Regulator:** DC-DC inductor `L6` is fitted (DCC_SW), so DC-DC mode is
  available; demo uses `REG_MODE_DCDC`. RadioLib's default LDO is safe and is
  what we use — flagged here as an option, not a change.
