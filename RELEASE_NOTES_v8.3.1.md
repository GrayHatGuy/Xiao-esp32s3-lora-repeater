# v8.3.1 — Radio 2 module-revision (V1.0 / V1.1) variants + bring-up hardening

A reliability / compatibility patch on [v8.3](https://github.com/GrayHatGuy/Xiao-esp32s3-lora-repeater/releases/tag/v8.3). No protocol or routing changes — V1.0 boards behave exactly as on v8.3.

## What this fixes
Seeed ships the **"Wio-SX1262 for XIAO" edge module (Radio 2) in two pinouts**: the chip-select (NSS) is on **D4 / GPIO5** on **V1.0** but **D3 / GPIO4** on **V1.1**. Every v8.x build assumed the V1.0 map — so on a **V1.1** module, Radio 2's real chip-select is left floating during Radio 1's `begin()`, drives the shared SPI MISO bus, and Radio 1 fails to detect (`[Radio1-B2B] begin() failed: -2`, `CHIP_NOT_FOUND`) → the bridge **FATAL-halts**. It reproduced on one board but not another with otherwise identical hardware. (V1.1 map bench-confirmed on a physical V1.1 module.)

## Pick the variant that matches your Radio 2 module
Read the silkscreen on the **Radio 2** edge module and flash the matching build:

| Radio 2 silkscreen | NSS lands on | Build env · release bin |
|---|---|---|
| **`V1.0`** | D4 / GPIO5 | `xiao_esp32s3` · `…-v1.0-vanilla-factory.bin` |
| **`V1.1`** | D3 / GPIO4 | `xiao_esp32s3_v1_1` · `…-v1.1-vanilla-factory.bin` |

The boot log echoes the active map so you can confirm: `[diag] R2 edge module = V1.0  (NSS=5 DIO1=2 RST=3 BUSY=4 RF_SW=6)`. See [README "Wiring"](README.md#radio-2-module-revision-v10-vs-v11) for photos of each silkscreen.

## Bring-up hardening (both variants)
Radio 2 is now held in hardware reset (RST is GPIO3 on **both** revisions) through Radio 1's bring-up, then released afterward. **A wrong-variant flash — or any Radio-2 fault — can no longer halt the bridge:** Radio 1 always comes up, Radio 2 is disabled gracefully, and the log names the env to flash. No more silent FATAL on a mismatched edge module.

## Compatibility / do-no-harm
- **V1.0 (default) behavior is unchanged** from v8.3 — the only serial-log difference is that the Radio-2 reset diagnostic now prints (`R2 release  BUSY after reset …`) **after** `Radio1-B2B ready`.
- `BridgeConfig` schema **v4 unchanged** — no NVS migration; v8.3 configs load as-is.
- No protocol / routing / LoRaWAN changes.

## Downloads (pick the one matching your Radio 2 silkscreen)
- **`xiao-dual-sx1262-v8.3.1-v1.0-vanilla-factory.bin`** — V1.0 module · full image, flash `@ 0x0` (fresh/erased → captive portal).
- **`xiao-dual-sx1262-v8.3.1-v1.0-app.bin`** — V1.0 module · app image, flash `@ 0x10000`.
- **`xiao-dual-sx1262-v8.3.1-v1.1-vanilla-factory.bin`** — V1.1 module · full image `@ 0x0`.
- **`xiao-dual-sx1262-v8.3.1-v1.1-app.bin`** — V1.1 module · app image `@ 0x10000`.

Full notes: [CHANGELOG.md](CHANGELOG.md) · Change / porting record: [V8.3.1-R2-VARIANTS.md](V8.3.1-R2-VARIANTS.md). Flashing instructions: see the [v8.2 release](https://github.com/GrayHatGuy/Xiao-esp32s3-lora-repeater/releases/tag/v8.2).
