# Xiao-esp32s3-lora-repeater

Xiao ESP32S3 with dual SX1262 radio SPI cross-band repeater.

<img width="4096" height="3265" alt="PXL_20260507_021829300~2" src="https://github.com/user-attachments/assets/b9e68624-3cb4-46a3-9c2f-4927e6a8fdf2" />

###### *touched by claude but not by epstein*

## Introduction / Background

A bidirectional cross-protocol LoRa mesh bridge running on a single Seeed Xiao ESP32S3 Sense with two Seeed Wio SX1262 shields stacked back-to-back — one mated to the Xiao's edge pins, the other to the 40-pin B2B header. The two radios share one SPI bus through a FreeRTOS mutex and each run in their own task pinned to a separate ESP32-S3 core, so they can transmit and receive in parallel on completely different RF profiles.

Each radio gets an independent LoRa configuration (frequency, bandwidth, spreading factor, coding rate, sync word) set via PlatformIO build flags. The bridge picks decoders and encoders by LoRa sync word at runtime, so reconfiguring a protocol means flipping flags in `platformio.ini` and rebuilding.

Supported today:

- **Meshtastic LongFast** (sync `0x2B`) — AES-128-CTR + a hand-written protobuf walker that lifts the `TEXT_MESSAGE_APP` payload out of the on-air `Data` submessage. Bridged bodies are tagged `[MT] …`. A periodic NodeInfo announce makes phones surface the bridge as a known sender (`!b16b00b5`, "LoRa Bridge").
- **MeshCore public channel** (sync `0x12`) — AES-128-ECB decrypt of `GRP_TXT`, with the 2-byte truncated HMAC-SHA256 verified against the public channel key. Bridged bodies are tagged `[MC] …`.
- **Reticulum / RNode** (sync `0x42`, **stub**) — incoming frames are base64-encoded and bridged into the other mesh as text packets of the form `[rns <seq> <x>/<y>] <base64>`. The bridge auto-fragments across multiple MT/MC packets when a single one wouldn't hold the encoded frame, using a CRC-16 low-byte sequence ID so concurrent fragmented frames don't get mixed up on the receiving side, and pacing between fragments (2000 ms for SF11/BW250 MT, 500 ms for SF7/BW62.5 MC; max 8 fragments per frame, tunable via `BRIDGE_RNS_*` build flags). A proper RNS packet encoder is still TODO; until then, "destination = RNS" is a log-and-drop path with a `No TX 2 RNS:` prefix on serial.

Source-protocol markers double as loop-prevention: when the bridge's own re-transmitted packet bounces back via a relay node, the marker is recognised and the packet is dropped before being bridged a second time.

All crypto runs on the ESP-IDF's built-in mbedTLS — no extra library dependencies beyond `jgromes/RadioLib`.

## Parts Required

| Part | Notes |
|------|-------|
| [Wio SX1262 with Xiao ESP32S3 (B2B 40-pin)](https://www.seeedstudio.com/Wio-SX1262-with-XIAO-ESP32S3-p-5982.html) | Radio 1 (B2B). Kit ships with the Xiao ESP32S3 Sense MCU |
| [Wio SX1262 for Xiao (edge-pin)](https://www.seeedstudio.com/Wio-SX1262-for-XIAO-p-6379.html) | Radio 2, sits on the Xiao's edge-pin header |
| 2 × LoRa antennas tuned for your ISM band | **Don't skip this.** Running an SX1262 at +20 dBm into a missing antenna kills your TX range and risks the PA |
| USB-C cable | Power, programming, serial monitor |

*Some assembly required.*

## Instructions

1. **Stack the hardware.** Mate the B2B shield (radio 1) underneath the Xiao, the edge-pin shield (radio 2) on top. Connect antennas to **both** radios before powering on.
2. **Install [PlatformIO](https://platformio.org/install)** — the VS Code extension is the easiest path.
3. **Configure the radios** in [`platformio.ini`](platformio.ini). Defaults are Meshtastic LongFast on radio 1, MeshCore public channel on radio 2. Each radio takes an independent set of LoRa parameters; the sync word is what the bridge dispatcher uses to pick a protocol, so swapping protocols is just one byte plus the matching RF preset:
   ```ini
   -DLORA_RADIO1_FREQUENCY=906.875f
   -DLORA_RADIO1_BANDWIDTH=250.0f
   -DLORA_RADIO1_SPREAD_FACTOR=11
   -DLORA_RADIO1_CODING_RATE=5
   -DLORA_RADIO1_TX_POWER=20
   -DLORA_RADIO1_SYNC_WORD=0x2B   ; 0x12 MeshCore, 0x2B Meshtastic, 0x42 Reticulum
   ```
4. **Configure the bridge's Meshtastic identity** (also in `platformio.ini`). The numeric ID and `!`-prefixed string must encode the same value, and string macros must be single-quoted so values with spaces survive shell tokenization:
   ```ini
   -DBRIDGE_MT_NODE_ID=0xB16B00B5u
   '-DBRIDGE_MT_NODE_ID_STR="!b16b00b5"'
   '-DBRIDGE_MT_LONG_NAME="LoRa Bridge"'
   '-DBRIDGE_MT_SHORT_NAME="BR"'
   ```
5. **Clean + build.** `pio run -t clean && pio run` — the clean is important whenever a header changes.
6. **Upload.** `pio run -t upload` or use the PlatformIO toolbar.
7. **Monitor.** `pio device monitor` at 115200 baud. Expect RX hex dumps, protocol-decoded summaries, bridge re-encode lines, NodeInfo broadcasts, and `loop-drop` messages when relay echoes come back to the bridge.

## Roadmap

### Adding Reticulum / RNode

The `0x42` Reticulum sync word is already wired into the bridge dispatcher as a third protocol, but only the receive half is implemented today:

| Direction | Status |
|-----------|--------|
| `RX:RNS → TX:MT or MC` | ✅ stub. Raw RNS bytes are base64-encoded and re-transmitted as one or more `[rns <seq> <x>/<y>] <base64>` text packets on the destination radio. CRC-16 low-byte sequence ID, per-protocol fragment pacing, 8-fragment cap. |
| `RX:MT or MC → TX:RNS` | ❌ log-only. The decoded body is printed with a `No TX 2 RNS: [MT/MC] …` prefix; nothing is transmitted on the RNS radio. |
| `RX:RNS → human-readable decode` | ❌ base64 dump only — RNS packet framing isn't parsed yet. |
| `MT/MC fragment reassembly → RNS TX` | ❌ stub function present (`reassembleReticulumFragment()` in `MeshDecoderDebug.h`), no logic yet — lands with the RNS encoder. |

Outstanding work to lift the stub:

- **RNS packet decoder.** Parse the RNS LoRa frame: header byte (IFAC flag, hops, header type, propagation/context bits), destination/transport address hashes, context byte, ciphertext. Produce a structured decode line analogous to the Meshtastic/MeshCore ones in `MeshDecoderDebug.h`.
- **RNS packet encoder.** Build a valid outgoing RNS frame in `MeshEncoderDebug.h`: write the header byte, attach the right destination hash, set the context byte for the payload type, and slot the body bytes into the ciphertext region. Wiring it into `bridgePacket()` is then a one-line dispatcher change.
- **Fragment reassembly.** Fill in `reassembleReticulumFragment()` in `MeshDecoderDebug.h`: parse `[rns <seq> <x>/<y>] <base64>` out of the incoming MT/MC body, accumulate slots keyed on `<seq>`, base64-decode each chunk, time out stale entries after ~30 s, and emit the reassembled raw RNS frame once `count == total`. Needed before `MT/MC → RNS` can actually transmit.
- **Optional IFAC support.** If the encoder ever needs to talk on a network with Identify-Fail Authentication enabled, the IFAC HMAC-SHA256 trailer and salt handling come along with it.

### Other future work

- [ ] More Meshtastic portnums (`POSITION_APP`, `TELEMETRY_APP`, `NODEINFO_APP`) bridged where they map cleanly to MeshCore advert/group-data equivalents.
- [ ] MeshCore private-channel support (channel/key table instead of the hard-coded public channel `0x11`).
- [ ] Persistent NodeDB so `[MT] …` prefixes can be replaced with the actual sender's short name learned from upstream NodeInfo packets, giving the destination mesh a real attribution instead of an anonymous bridge tag.
