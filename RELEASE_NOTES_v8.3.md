# v8.3 — LoRaWAN keyless bridge/relay/mesh + carry-overs

> ⚠️ **DRAFT — do not publish until v8.3 is tagged.** Before release:
> 1. Complete or knowingly accept the open bench items (Reticulum D1–D3; optional Pass D) — see `BENCH-v8.3.md` and the session handoff.
> 2. ff-merge `v8.3-dev` → `main`, create the annotated tag `v8.3`, push `main`+tag.
> 3. Build the merged `.bin`s (esptool merge_bin, qio/80m/8MB) from the bench-verified firmware.
> 4. Replace the `…/tree/v8.3-dev` / placeholder links below with the `v8.3` tag.

Builds on [v8.2.1 (MeshCore timestamp fix)](https://github.com/GrayHatGuy/Xiao-esp32s3-lora-repeater/releases/tag/v8.2.1). Adds a keyless **LoRaWAN (LoRa sync `0x34`)** protocol plus two carry-over fixes. Bench-validated on hardware 2026-06-14 (LoRaWAN feature + both clock-learn paths; Reticulum RNS↔RNS code-verified, on-air bench in the 8.3.x patch).

## New — LoRaWAN (sync `0x34`), keyless
Select **LoRaWAN** for either radio in the captive portal. The bridge operates only on the **cleartext PHY frame** — no keys, no payload decryption:
- **Metadata capture tap** — decodes the cleartext MAC header (MType, DevAddr, FCtrl, FCnt, FPort, FRMPayload length; JoinEUI/DevEUI on join-requests) to the serial log (`evt=RX proto=LW …`).
- **Metadata summary to mesh** — emits a one-line summary onto your Meshtastic/MeshCore mesh (`BRIDGE_LW_SUMMARY_TO_MESH`, default on).
- **Transparent LW↔LW relay / dedup-bounded flood** — re-transmits raw `0x34` frames between radios and bridges (`BRIDGE_LW_RELAY`); a localized transparent bridge modeled after the [Tasmota LoRa & LoRaWAN Bridge](https://tasmota.github.io/docs/LoRa-and-LoRaWan-Bridge/).
- `MT/MC → LoRaWAN` is a deliberate log-and-drop (`no-lw-encoder`): keyless firmware cannot inject content into LoRaWAN.

## Also new
- **POSITION clock-learn** — the bridge now also learns wall-clock from a Meshtastic `POSITION_APP` time field, closing the cold-boot 1969-timestamp window left by v8.2.1 (which learned only from MeshCore).
- **Transparent in-protocol Reticulum repeat (RNS↔RNS)** — with both radios on Reticulum, an inbound RNS frame is re-transmitted byte-for-byte (range-extension repeater) instead of being dropped (`BRIDGE_RNS_INPROTO_REPEAT`, default on).

## Known limitations
- LoRaWAN is **single-channel per radio** (no LoRaWAN channel hopping) and does not honour Class-A RX windows — a localized capture/relay bridge, not a gateway.
- The flood "mesh" is **dedup/TTL-bounded** (each bridge repeats a unique frame once); there is no in-band hop counter (mutating the frame would break its MIC).
- LoRaWAN **content** decode/encode needs per-device session keys — see Deferred.

## Deferred → v8.3+ (8.3.1 patch)
- ABP/OTAA key-based **decode** of the operator's own devices → MT/MC/custom, and **encode** (MT/MC → LoRaWAN).
- Reticulum **MT/MC → RNS** encoder + fragment reassembly (today MT/MC→RNS is a clean log-and-drop).

## Build flags (all default `1`; override in `platformio.ini`)
`BRIDGE_LW_CAPTURE` · `BRIDGE_LW_SUMMARY_TO_MESH` · `BRIDGE_LW_RELAY` · `BRIDGE_RNS_INPROTO_REPEAT`
(Bench-only: `BRIDGE_BENCH_AUTOSAVE` on the `bench_*` envs skips the captive portal — never in a release build.)

## Compatibility
`BridgeConfig` schema **v4 unchanged** — no NVS migration; v8.2.x configs load as-is. LR1121 / co-processor code remains out of scope.

## Downloads (build at tag time)
- **`xiao-dual-sx1262-v8.3-vanilla-factory.bin`** — full image, flash `@ 0x0` (fresh/erased device → captive portal).
- **`xiao-dual-sx1262-v8.3-app.bin`** — app image, flash `@ 0x10000`.

Full notes: [CHANGELOG.md](CHANGELOG.md) · Design: [V8.3-SPEC.md](V8.3-SPEC.md) · Bench protocol: [BENCH-v8.3.md](BENCH-v8.3.md). Flashing instructions: see the [v8.2 release](https://github.com/GrayHatGuy/Xiao-esp32s3-lora-repeater/releases/tag/v8.2).
