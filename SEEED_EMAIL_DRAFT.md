# Seeed Engineering Email — LOCKED-IN DRAFT


**To:** sensecap@seeed.cc
**CC:** iot@seeed.cc, techsupport@seeed.cc
**Subject:** Wio-LR1121 (SKU 113991415) RX path not working — full DOE test results from Seeed/Meshtastic Build-Off 2026 entry

---

Hello Seeed Studio team,

This email reports a bug in the Seeed Wio-LR1121 module (SKU 113991415, IPEX antenna variant). We found it during the Seeed/Meshtastic Build-Off 2026 contest. We have prepared a complete test data package (see links below) so engineering review can be fast.

Reference documents used in this investigation (direct PDF + stable fallback link for each):

- **Semtech LR1121 User Manual v2.2** (rev 2.2, Apr 2026 — referred to as "UM" below).
  - Direct PDF: https://semtech.my.salesforce.com/sfc/p/#E0000000JelG/a/RQ00000DClgP/D.pNG5l4FviPI634eCx8GFURZEwDO2ZBA33MpriB_FU
  - Stable product page (use if direct link expires): https://www.semtech.com/products/wireless-rf/lora-connect/lr1121

- **Semtech LR1121 Datasheet** (rev 2.1, Dec 2023 — referred to as "LR1121 Datasheet" below).
  - Direct PDF: https://semtech.my.salesforce.com/sfc/p/#E0000000JelG/a/RQ0000093ZiP/RV4Ba6LROsFrFjnAAVK2av5W11RGmCms_3Q2cyKHdDA
  - Stable product page (use if direct link expires): https://www.semtech.com/products/wireless-rf/lora-connect/lr1121

- **Seeed Wio-LR1121 Module Datasheet v1.0 (2025-07-01)** (referred to as "Module Datasheet" below).
  - Direct PDF: https://files.seeedstudio.com/wiki/Wio-LR1121/Wio-LR1121_Module_Datasheet_v1.0.pdf
  - Stable wiki page (use if direct link expires): https://wiki.seeedstudio.com/wio_lr1121_module/

## 1. Project context

The contest entry (issue #2 in `Seeed-Projects/meshtastic-build-off-2026`) - https://github.com/Seeed-Projects/meshtastic-build-off-2026/issues/2 is a multi-protocol LoRa mesh bridge. The host MCU is Seeed Xiao ESP32-S3.

- **Phase 0** — Two Wio-SX1262 modules. Production-stable. Released as v8.1 - https://github.com/GrayHatGuy/Xiao-esp32s3-lora-repeater/releases/tag/v8.1 This is the contest deliverable.
- **Phase 1** — One Wio-SX1262 + one Wio-LR1121, for sub-GHz ↔ 2.4 GHz cross-band bridging. **This is where the bug is.**

## 2. Bug summary

- **TX works.** The Wio-LR1121 transmits correctly. External Meshtastic and MeshCore devices receive its packets at expected RSSI.
- **RX does not work.** The Wio-LR1121 produces zero `RX_DONE` events for over-the-air traffic. We tested with a Meshtastic phone antenna touching the LR1121 antenna port directly. Signal level at the port is about –20 dBm. This is about 120 dB above the LR1121 sensitivity floor specified in the LR1121 Datasheet (linked above). The chip still does not report a valid RX.
- **The radio is not fully deaf.** In our DOE Run 5, we captured one `RADIOLIB_ERR_CRC_MISMATCH` event (RadioLib error code –7). This means the LR1121 detected a preamble and header strong enough to try CRC. So the RX chain is partially working. Our estimate: the RX sensitivity is degraded by **40 to 50 dB** compared to LR1121 Datasheet specification.
- **Two units tested.** Two Wio-LR1121 modules from two different orders. Both modules show identical behavior.
- **Chip firmware version:** `Base FW version: 1.3` (reported by `GET_VERSION` command, defined in UM §3.2).

## 3. Investigation summary — what we have tested

We ran two complete DOE phases. Every RadioLib API call returned success (state = 0) on every run.

### 3.1 Phase A — RF switch DIO sweep (12 iterations)

Per UM §4.2.1, the only DIOs on the LR1121 that can drive the RF switch are DIO5, DIO6, DIO7, DIO8, and DIO10.

- DIO9 is interrupt-only.
- DIO11 is not connected on the Wio-LR1121 module (confirmed from Seeed's published KiCad library).

We tested all combinations of these five DIOs that the UM allows. **Result:** self-echo RSSI changed by less than 4 dB across all 12 iterations. Zero OTA `RX_DONE` events in any iteration.

**Additional finding from Seeed KiCad library:** the chip pins DIO5/DIO6/DIO7 are routed to bottom-side test pads labeled `MCU_DIO5`, `MCU_DIO6`, `MCU_DIO7`. The `MCU_` prefix suggests these pins are exposed for host-MCU use, not used as RF switch outputs on the Wio-LR1121 module. **This means we do not know what mechanism actually controls the integrated RF switch on this module.**

### 3.2 Phase B — Chip initialization DOE (4 effective runs)

Following UM v2.2 recommendations, we ran a structured DOE controlled by a single build flag (`LR1121_RX_AUDIT_RUN`):

| Run | Treatment applied | UM section | Result |
|---|---|---|---|
| 0 | Baseline. Read `GetErrors()` register. | §7.2.10 | **`Errors = 0x0020` = `HF_XOSC_START_ERR`**, persistent at every POR. Zero OTA RX. |
| 2 | `SetRssiCalibration` using Table 7-21 "600 MHz – 2 GHz" reference values. | §7.2.15 | Command accepted. Self-echo RSSI shifted 4 dB. Zero OTA RX. |
| 3 | `CalibImage(902, 928)` called after `SetTcxoMode`. | §2.1.3 | Command accepted. Zero OTA RX. |
| 5 | Combined: `Standby(STBY_RC)` before switch table install + RSSI calibration + image calibration + `SetRxBoostedGainMode(true)`. | §§4.2.1, 7.2.12, 7.2.15 | All commands accepted (state = 0). Zero OTA `RX_DONE`. **One `ERR_CRC_MISMATCH` event observed.** |

**Important observation about `HF_XOSC_START_ERR` (Run 0):** The Wio-LR1121 has an integrated TCXO. UM §2.1.3 documents that automatic POR image calibration fails on chips with TCXO. We see a related HF crystal start error (bit 5, mask `0x0020`) in the chip's error register at every boot of every unit. We tried explicit `CalibImage` after `SetTcxoMode` (Run 3) and the combined remedies (Run 5). Neither resolved the RX problem.

## 4. Hypotheses we have already refuted

| # | Hypothesis | How we refuted it |
|---|---|---|
| 1 | Defective single unit | Two units from different orders, identical behavior |
| 2 | RF switch table wrong | 12-iteration sweep + Run 5 |
| 3 | IRQ flag stuck at boot | `getIrqFlags()` returns `0x00000000` after `begin()` |
| 4 | `startReceive()` rejected by chip | Returns 0 on every call |
| 5 | Sensitivity floor at lab level | Zero RX even with antenna touch (–20 dBm at port) |
| 6 | Default RSSI calibration wrong (EVK band) | DOE Run 2 |
| 7 | POR image calibration failure (TCXO) | DOE Run 3 |
| 8 | Wrong RX gain mode | DOE Run 5 (RxBoosted enabled) |
| 9 | Switch table installed outside `STBY_RC` mode | DOE Run 5 (pre-standby applied) |
| 10 | All UM-recommended firmware remedies combined | DOE Run 5 |

**Provisional conclusion:** The antenna → RF switch → LNA → demodulator path is electrically connected. TX radiates correctly. Near-field signals from the on-PCB SX1262 demodulate at –42 to –46 dBm. But sensitivity for normal over-the-air signals is reduced by 40 to 50 dB. **All firmware remedies described in UM v2.2 have been tested. None of them fixes the RX problem.**

Remaining possible causes:

- Hardware design issue (RF matching network, switch insertion loss, LNA isolation)
- LR1121 chip firmware v1.3 errata

## 5. Questions for Seeed engineering

1. **Integrated RF switch — control mechanism.** Our KiCad-library observation shows DIO5/DIO6/DIO7 are exposed as `MCU_DIO5/6/7` test pads, not connected as RF switch outputs. **What controls the integrated RF switch on the Wio-LR1121?** Is host configuration required? The Module Datasheet (linked above) confirms the integrated RF switch exists but does not show its truth table or its host-control interface.

2. **`SetRssiCalibration` tune values for the Wio-LR1121 PCB.** UM §7.2.15 states: *"The RSSI must be calibrated for each hardware type."* The Table 7-21 reference tunes did not work in our Run 2. **Does Seeed have the Wio-LR1121-specific tune values from production calibration?**

3. **`HF_XOSC_START_ERR` (bit 5, mask `0x0020`) set at every POR.** Is this expected on Wio-LR1121 (with integrated TCXO)? Is it benign? If not, what is the correct initialization order to avoid it?

4. **Replication test.** Can Seeed engineering reproduce this issue on a fresh production unit, using the stock RadioLib example `LR11x0_Receive_Interrupt`? If RX works on your bench, we can compare initialization sequences directly.

5. **Production lot / errata.** Are the two units we have likely from the same production lot? Is there a known LR1121 chip firmware errata for v1.3? Is there a Wio-LR1121 hardware revision history?

## 6. Test data package

All documents are in a dedicated branch of the project repository. The "snapshot tag" link pins the exact state cited in this email.

- 🔗 **Bug report (start here)** — full hardware/software setup, what works, what does not, complete DOE table, serial log excerpts:
https://github.com/GrayHatGuy/Xiao-esp32s3-lora-repeater/blob/lr1121-phase1/SEEED_SUPPORT_INQUIRY.md

- 🔗 **DOE bench plan and results** — Phase B chip-init audit, run-by-run results with raw serial logs:
https://github.com/GrayHatGuy/Xiao-esp32s3-lora-repeater/blob/lr1121-phase1/LR1121-RX-INIT-AUDIT.md

- 🔗 **Recommendations to Seeed (Tier 1–4)** — feedback on documentation gaps (Tier 1: publish RF switch truth table and per-PCB RSSI tunes), engineering verification (Tier 2), hardware-design improvements (Tier 3), and process improvements (Tier 4):
https://github.com/GrayHatGuy/Xiao-esp32s3-lora-repeater/blob/lr1121-phase1/SEEED_RECOMMENDATIONS.md

- 🔗 **Full Phase 1 design and investigation specification** — bench-test history, software architecture, firmware status:
https://github.com/GrayHatGuy/Xiao-esp32s3-lora-repeater/blob/lr1121-phase1/LR1121-SPEC.md

## 7. Project links

- Project repository: https://github.com/GrayHatGuy/Xiao-esp32s3-lora-repeater
- Investigation branch: https://github.com/GrayHatGuy/Xiao-esp32s3-lora-repeater/tree/lr1121-phase1
- Snapshot tag (exact state shared in this email): https://github.com/GrayHatGuy/Xiao-esp32s3-lora-repeater/tree/lr1121-bringup-2026-05-26
- Production release (Phase 0, contest deliverable): https://github.com/GrayHatGuy/Xiao-esp32s3-lora-repeater/releases/tag/v8.1
- Contest submission: https://github.com/Seeed-Projects/meshtastic-build-off-2026/issues/2
- Build-Off contest page: https://www.seeedstudio.com/meshtastic-build-off

## 8. What this email is not

- **This is not an RMA request.** The modules are electrically functional. The chip responds to SPI commands. TX works. BUSY behaves correctly. Near-field signals demodulate.
- **This is not a complaint.** The Wio-LR1121 is a good module. We want to use it in production.

What we need:

- Missing configuration information (RF switch control mechanism, and/or per-PCB RSSI calibration values), **or**
- Confirmation of a chip firmware errata, **or**
- Confirmation of a hardware issue.

Any one of these will let us finish the Phase 1 cross-band bridging milestone for the contest.

We can share more serial logs, oscilloscope captures, register dumps, or run additional bench tests on request. **We have the modules, the bench setup, and the host firmware ready.** If your engineers want a direct technical conversation (email, WeChat, Teams, etc.), please tell us.

Thank you for your time and for hosting the Build-Off.

Best regards,
GrayHatGuy
grayhatguyllc@protonmail.com
https://github.com/GrayHatGuy/Xiao-esp32s3-lora-repeater
