# lw-frame-gen — LoRaWAN (sync 0x34) test-frame generator

Standalone firmware that transmits the canonical hand-built LoRaWAN frames from
[`../../BENCH-v8.3.md`](../../BENCH-v8.3.md), so you can bench the v8.3 LoRaWAN
**capture / summary / relay / flood** features **without a real LoRaWAN gateway
or end-device**. The bridge only reads the cleartext MAC header, so an
unencrypted hand-built frame is all it needs.

## Hardware
A Xiao ESP32-S3 + Wio-SX1262 — i.e. **one of your bridges, temporarily reflashed**
(e.g. the COM6 unit). It transmits on **Radio 1** (the B2B-header radio), using
the same pins / SPI / TCXO (1.8 V) / DIO2 RF-switch as the bridge. Reflash the
bridge firmware (`pio run -e xiao_esp32s3 -t upload` from the repo root) when done.

## Flash + run
From **this** folder:

    pio run -t upload
    pio device monitor          # 115200 baud

## RF profile — match the DUT
The generator's RF **must equal the DUT's LoRaWAN radio exactly** (freq / BW / SF
/ CR / sync 0x34) or the DUT will not demodulate — the #1 cause of a silent
`LW-RX`. Defaults are bench profile **P-LW+MT, Radio 1**: `904.6 MHz / BW125 /
SF7 / CR5 / 20 dBm`. Change them in [`platformio.ini`](platformio.ini) (`GEN_*`)
and re-upload. For the **relay/flood** pass, keep the generator on **freqA**
(DUT-A R1); the DUT relays out on freqB to DUT-B.

## Use
In the generator's serial monitor:

| Key | Frame | DUT should log |
|---|---|---|
| `1` | FRAME-U  (UnconfDataUp) | `proto=LW mtype=UnconfDataUp devaddr=0x26011f8a fcnt=1 fport=2` |
| `2` | FRAME-U2 (FCnt 2) | distinct frame — passes dedup |
| `3` | FRAME-D  (ConfDataUp, no FPort) | `fport=-1` |
| `4` | FRAME-J  (JoinRequest) | `mtype=JoinRequest`; summary `DevEUI 0004a30b001a2b3c` |
| `5` | FRAME-C  (11 B) | `parse=fail` |
| `n` | next in cycle | (the **BOOT button** also sends next) |

- **LW-LOOP / dedup:** press `1` twice within 60 s → the DUT logs a capture then
  `drop=lw-dup`.
- **LW-RELAY / LW-FLOOD:** point the generator at DUT-A's R1 freq and watch DUT-A
  relay (`mode=raw`) to DUT-B.
- **Hands-off:** set `GEN_AUTO_MS` > 0 to auto-cycle through all five frames.

The RF profile, the frame name/length, the TX return code, and the expected DUT
decode are printed at boot and on every send, so you can cross-check the DUT log.

Frame bytes are decode-verified against `extractLoRaWANMeta()` in
`src/MeshDecoderDebug.h`.
