# References

Authoritative reference material for this project. Local copies of datasheets and user manuals are checked into `docs/datasheets/` so the project remains self-contained when vendor pages change or expire.

---

## Semtech LR1121 chip

### LR1121 Datasheet v2.1 (December 2023)

The chip-level RF/electrical specification. Primary reference for sensitivity numbers, PA configuration, RFSWx pin mapping (§4.5.1, Table 4-1), and TX/RX timing.

- Local: [`datasheets/LR1121_V2_1_data_sheet.pdf`](datasheets/LR1121_V2_1_data_sheet.pdf)
- Vendor page: https://www.semtech.com/products/wireless-rf/lora-connect/lr1121
- Vendor direct PDF (may expire): https://semtech.my.salesforce.com/sfc/p/#E0000000JelG/a/RQ0000093ZiP/RV4Ba6LROsFrFjnAAVK2av5W11RGmCms_3Q2cyKHdDA

### LR1121 User Manual v2.2 (April 2026, 140 pages)

Chip-level command spec. Primary reference for `SetDioAsRfSwitch` (§4.2.1), `SetRssiCalibration` (§7.2.15, Tables 7-19 and 7-21), `CalibImage` (§2.1.3), power amplifier configuration, IRQ handling, and the chip-internal state machine. Cited extensively in `LR1121-RX-INIT-AUDIT.md` and the original Seeed inquiry.

- Local: [`datasheets/LR1121_UM_V2.2.pdf`](datasheets/LR1121_UM_V2.2.pdf)
- Vendor page: https://www.semtech.com/products/wireless-rf/lora-connect/lr1121
- Vendor direct PDF (may expire): https://semtech.my.salesforce.com/sfc/p/#E0000000JelG/a/RQ00000DClgP/D.pNG5l4FviPI634eCx8GFURZEwDO2ZBA33MpriB_FU

---

## Seeed Wio-LR1121 module (Phase 1 Radio 2 — superseded by the Core1121)

The Seeed Wio-LR1121 was the original Phase-1 Radio 2; it is **replaced by the WaveShare Core1121** on this branch (see below). The Wio module datasheet, the Skyworks **SKY13373-460LF** RF-switch datasheet, and the Seeed engineering correspondence that pinned down its `V1=DIO5 / V2=DIO6` switch wiring all live on the [`lr1121-phase1`](https://github.com/GrayHatGuy/Xiao-esp32s3-lora-repeater/tree/lr1121-phase1) branch (`docs/datasheets/` + `SEEED_*.md`).

---

## WaveShare Core1121-XF module (Phase 1 Radio 2 — current board)

The WaveShare Core1121 (Semtech LR1121) replaces the Seeed Wio-LR1121 as Radio 2 on the `CORE1121` branch — same chip, different board. Unlike Seeed, WaveShare **publishes the full schematic**, so the board-specific RF-switch wiring is derived directly rather than via support correspondence.

### Core1121-XF schematic

Authoritative source for the board's RF-switch wiring (pSemi **PE4259** SPDT controlled by DIO5/DIO6), TCXO (32 MHz @ 3.0 V), the 32.768 kHz crystal on DIO10/DIO11, and the dual-antenna topology (ANT2 sub-GHz behind the switch, ANT1 2.4 GHz direct on `RFIO_HF`).

- Local: [`datasheets/waveshare/Core1121_XF_Sch.pdf`](datasheets/waveshare/Core1121_XF_Sch.pdf)
- Vendor wiki: https://www.waveshare.com/wiki/Core1121-XF
- Vendor demos (ESP32-S3 / Raspberry Pi / Pico / STM32) and resources: https://www.waveshare.com/wiki/Core1121-XF

### Core1121 RF-switch table (schematic-derived)

The project's record of the Core1121 RF-switch truth table and how it differs from the Seeed SKY13373: only `MODE_TX` differs ({1,1}→{0,1}), because the PE4259 is an SPDT vs the SKY13373 SP3T. Cross-checked against WaveShare's own demo firmware. Cited by `src/Core1121.cpp`.

- Local: [`datasheets/waveshare/CORE1121-RF-SWITCH.md`](datasheets/waveshare/CORE1121-RF-SWITCH.md)

### pSemi PE4259 RF switch

The SPDT antenna switch on the Core1121 (the counterpart to the Seeed module's SKY13373). No local PDF — see the vendor page. The board wires it in complementary-pin control mode (pin 6 ← DIO5, pin 4 ← DIO6).

- Vendor product page: https://www.psemi.com/products/rf-switches/pe4259

### Core1121 bring-up handoff

The task-#8 board-vs-chip control plan and success criteria for the Core1121.

- Local: [`WAVESHARE-CORE1121-HANDOFF.md`](WAVESHARE-CORE1121-HANDOFF.md)

---

## Seeed Wio-SX1262 module (Phase 0 / Phase 1 Radio 1)

### Wio-SX1262 with XIAO ESP32-S3 product page

The reference radio used as R1 throughout Phase 0 and Phase 1. SX1262 sub-GHz only (150–960 MHz). All bench A/B comparisons against R2 use this module as the known-good reference.

- Vendor product page: https://www.seeedstudio.com/Wio-SX1262-with-XIAO-ESP32S3-p-5982.html
- Underlying chip: Semtech SX1262 (datasheet at https://www.semtech.com/products/wireless-rf/lora-connect/sx1262)

---

## Seeed XIAO ESP32-S3 host MCU

### XIAO ESP32-S3 pin multiplexing reference

The host MCU that drives both radios over SPI. The pin-multiplexing wiki is the source of truth for which GPIOs map to which functions (SPI MOSI/MISO/SCK, NSS chip-select, RST, BUSY, DIO interrupt lines). Project pin map in `../src/main.cpp` and [`../CORE1121.md`](../CORE1121.md) is derived from this reference.

- Vendor wiki: https://wiki.seeedstudio.com/xiao_esp32s3_pin_multiplexing/
- Board variant: Seeed XIAO ESP32-S3 (also covered by Espressif ESP32-S3 official datasheet)

---

## How this file is maintained

- New authoritative references go under their own section above
- Local PDFs go in `docs/datasheets/` with the vendor's original filename preserved
- Both local path AND vendor URL are recorded so the project survives vendor page changes
- Direct-link PDF URLs from Semtech Salesforce may expire — always include the stable product/wiki page as a fallback
- When a project doc cites a datasheet section (e.g. "UM v2.2 §4.2.1"), the citation refers to the version checked into `docs/datasheets/` so the section number remains stable even if vendor publishes a new revision
