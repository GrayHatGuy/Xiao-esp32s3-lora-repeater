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

## Seeed Wio-LR1121 module (Phase 1 Radio 2)

### Wio-LR1121 Module Datasheet

Module-level integration spec. Documents the antenna pads (`SUBG_RF` pad 23, `2.4G_RF` pad 2), TCXO integration, pinout, RF switch wiring (incomplete — see Seeed correspondence chain in `../SEEED_EMAIL_DRAFT.md` for the authoritative SKY13373-460LF truth table provided by David Du after the published datasheet did not cover the V1/V2 control wiring).

- Local: [`datasheets/Wio-LR1121_Module_Datasheet.pdf`](datasheets/Wio-LR1121_Module_Datasheet.pdf)
- Vendor wiki: https://wiki.seeedstudio.com/wio_lr1121_module/
- Vendor direct PDF (may expire): https://files.seeedstudio.com/wiki/Wio-LR1121/Wio-LR1121_Module_Datasheet_v1.0.pdf

### Skyworks SKY13373-460LF RF switch datasheet

The on-module SP3T antenna switch. Document number 310060742. Provided by David Du (Sensecap Support) with his 2026-05-28 reply to the original engineering inquiry. The truth table in this datasheet plus David's wiring confirmation (V1 ↔ DIO5, V2 ↔ DIO6) is what allowed Phase 1 to lock the RF switch table in commit `949176a`.

- Local: [`datasheets/310060742_SKYWORKS_SKY13373-460LF_Datasheet.pdf`](datasheets/310060742_SKYWORKS_SKY13373-460LF_Datasheet.pdf)
- Vendor product page: https://www.skyworksinc.com/Products/Switches/SKY13373-460LF

---

## Seeed Wio-SX1262 module (Phase 0 / Phase 1 Radio 1)

### Wio-SX1262 with XIAO ESP32-S3 product page

The reference radio used as R1 throughout Phase 0 and Phase 1. SX1262 sub-GHz only (150–960 MHz). All bench A/B comparisons against R2 use this module as the known-good reference.

- Vendor product page: https://www.seeedstudio.com/Wio-SX1262-with-XIAO-ESP32S3-p-5982.html
- Underlying chip: Semtech SX1262 (datasheet at https://www.semtech.com/products/wireless-rf/lora-connect/sx1262)

---

## Seeed XIAO ESP32-S3 host MCU

### XIAO ESP32-S3 pin multiplexing reference

The host MCU that drives both radios over SPI. The pin-multiplexing wiki is the source of truth for which GPIOs map to which functions (SPI MOSI/MISO/SCK, NSS chip-select, RST, BUSY, DIO interrupt lines). Project pin map in `../src/main.cpp` and `../LR1121-SPEC.md` is derived from this reference.

- Vendor wiki: https://wiki.seeedstudio.com/xiao_esp32s3_pin_multiplexing/
- Board variant: Seeed XIAO ESP32-S3 (also covered by Espressif ESP32-S3 official datasheet)

---

## How this file is maintained

- New authoritative references go under their own section above
- Local PDFs go in `docs/datasheets/` with the vendor's original filename preserved
- Both local path AND vendor URL are recorded so the project survives vendor page changes
- Direct-link PDF URLs from Semtech Salesforce may expire — always include the stable product/wiki page as a fallback
- When a project doc cites a datasheet section (e.g. "UM v2.2 §4.2.1"), the citation refers to the version checked into `docs/datasheets/` so the section number remains stable even if vendor publishes a new revision
