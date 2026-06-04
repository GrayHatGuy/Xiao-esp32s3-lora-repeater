# QUAD-SPEC — Phase 2: 4-Up Triband Omnidirectional Repeater

Status: **implemented (stages A–E) on branch `T_LORA_QUAD`; both firmwares build clean; on-air
bring-up pending hardware.** Companion to `V8-SPEC.md` (portal config) and `LR1121-SPEC.md`
(Phase-1 Wio cross-band); shipped detail in [`CHANGELOG.md`](CHANGELOG.md) v10.0, contest
write-up in [`CONTEST-PHASE2.md`](CONTEST-PHASE2.md).

## Goal

Grow the bridge from **2 radios → 4 radios** by adding the LilyGO **T-Lora-Dual** (ESP32
PICO-D4 + two **LR1121** radios) as **R3/R4**, connected to the XIAO ESP32-S3 over a **UART**
link. Result: a single node that simultaneously covers **sub-GHz + 2.4 GHz (+ S-band stub)**
and cross-bridges Meshtastic / MeshCore / Reticulum across all four radios under a
**user-configurable routing matrix**.

Contest: Seeed/Meshtastic Build-Off **Phase 2** (`Seeed-Projects/meshtastic-build-off-2026#2`).

## Motivation & pivot

Phase 1 (`lr1121-phase1`) added a Seeed **Wio-LR1121** as a 2.4 GHz radio but hit an
unresolved LR1121 **RX deficit** — it detects every preamble yet completes only a small,
unreliable fraction of packets. The evidence isolates this to an **RSSI/image-calibration or
modem-config class issue** (RadioLib `-20 WRONG_MODEM`, RSSI-cal flagged as top candidate,
image-cal-with-TCXO); the **SKY13373 RF switch was swept exhaustively and exonerated**, and a
clean module reproduced it. Rather than keep fighting one module, the project **pivots to the
T-Lora-Dual**, whose Factory firmware demonstrates **working RX+TX** on its LR1121s. The
Phase-1 firmware + diagnostics remain on `lr1121-phase1` as the record.

## Architecture & radio numbering

```
        ┌─────────────────────── XIAO ESP32-S3 (the "brain") ───────────────────────┐
        │  BridgeConfig · NodeDB · CaptivePortal · bridge core (routing matrix)      │
        │                                                                            │
        │   R1 = SX1262 (B2B)  ─┐                         ┌─ UART link ─┐            │
        │   R2 = SX1262 (edge) ─┴─ shared SPI + mutex     │  (framed)   │            │
        └──────────────────────────────────────────────── │ ──────────── │ ─────────┘
                                                           │             │
        ┌────────────────── T-Lora-Dual (ESP32 PICO-D4 co-processor) ────┴──────────┐
        │   UART slave · RadioLib LR1121 driver · Factory RF-switch table           │
        │   R3 = LR1121 (radio_1)  ─┐ shared SPI                                     │
        │   R4 = LR1121 (radio_2)  ─┘  (sub-GHz / 2.4 GHz / S-band-stub)            │
        └───────────────────────────────────────────────────────────────────────────┘
```

| Radio | Chip | Board | Link to brain | `BridgeConfig` idx | Bands |
|------|------|-------|---------------|--------------------|-------|
| R1 | SX1262 | XIAO (B2B) | direct SPI | 0 | sub-GHz |
| R2 | SX1262 | XIAO (edge) | direct SPI | 1 | sub-GHz |
| R3 | LR1121 | T-Lora-Dual `radio_1` | **UART** (co-proc local 0) | 2 | sub-GHz / 2.4 GHz / S-band* |
| R4 | LR1121 | T-Lora-Dual `radio_2` | **UART** (co-proc local 1) | 3 | sub-GHz / 2.4 GHz / S-band* |

\* S-band is accepted in config but **disabled** — the co-processor logs and refuses to key up
(stub for future work).

## Pin maps

**XIAO ESP32-S3 (host)** — unchanged for R1/R2; the link uses the only clean free pads.

| Signal | R1 (B2B) | R2 (edge) | SPI | UART link (proposed) |
|---|---|---|---|---|
| NSS/CS | 41 | 5 | — | — |
| DIO1/IRQ | 39 | 2 | — | — |
| RESET | 42 | 3 | — | — |
| BUSY | 40 | 4 | — | — |
| ANT_SW | 38 | 6 | — | — |
| SCK/MISO/MOSI | — | — | 7 / 8 / 9 | — |
| UART1 TX → coproc RX | — | — | — | **GPIO43 (D6)** |
| UART1 RX ← coproc TX | — | — | — | **GPIO44 (D7)** |

`Serial` is USB-CDC (`ARDUINO_USB_CDC_ON_BOOT=1`), so UART0's pads (GPIO43/44) are free for a
second hardware UART. **Verify these are physically reachable on your stack** — build-flag
overridable (`BRIDGE_LINK_*`).

**T-Lora-Dual (co-processor)** — from the Factory example (authoritative HAL).

| Signal | R3 (`radio_1`) | R4 (`radio_2`) | shared | UART link (proposed) |
|---|---|---|---|---|
| CS | 27 | 13 | — | — |
| DIO9 (IRQ) | 37 | 34 | — | — |
| RESET | 26 | 21 | — | — |
| BUSY | 36 | 39 | — | — |
| SCK/MISO/MOSI | — | — | 25 / 33 / 32 | — |
| RF switch | internal DIO5–8 (`rfswitch_table`) | same | — | — |
| UART2 TX → host RX | — | — | — | **GPIO22** |
| UART2 RX ← host TX | — | — | — | **GPIO23** |

Other PICO-D4 pins in use: NeoPixel 5, BOOT 0, USB-serial 1/3. GPIO22/23 are free and
non-strapping. **Verify against the T-Lora-Dual header breakout** — build-flag overridable.

## Inter-board UART link

Baud **460800** (configurable; LoRa airtime ≫ UART time, so even 115200 suffices — 460800 is
margin). Binary-safe framed protocol that resyncs on a bad frame.

**Frame**

| Field | Size | Notes |
|-------|------|-------|
| preamble | 2 | `0xAA 0x55` |
| type | 1 | message type |
| radio | 1 | co-proc-local index `0`=R3, `1`=R4, or `0xFF` n/a |
| len | 2 | LE payload length |
| payload | len | |
| crc16 | 2 | LE CRC-16/CCITT over `type…payload` — **reuse `MeshDecoderDebug::crc16_ccitt`** |

**Messages** (H = host→coproc, C = coproc→host)

| Dir | Type | Name | Payload |
|-----|------|------|---------|
| H→C | 0x01 | CFG_RADIO | enable u8, band u8, freq f32, bw f32, sf u8, cr u8, sync u8, txPwr i8, preamble u16 |
| H→C | 0x02 | TX | raw LoRa bytes |
| H→C | 0x03 | START_RX | — |
| H→C | 0x04 | PING | — |
| H→C | 0x05 | RESET | — |
| C→H | 0x81 | RX | rssi f32, snr f32, raw LoRa bytes |
| C→H | 0x82 | TX_DONE | status i16 |
| C→H | 0x83 | READY | fw u8, radioCount u8, per-radio HW/FW version + band caps |
| C→H | 0x84 | LOG | ASCII (surfaces co-proc logs on the host `Serial`) |
| C→H | 0x85 | PONG | uptime u32 |

A shared header **`LinkProtocol.h`** defines the frame, enums, and CRC helper, and is included
by **both** firmwares so they never drift.

## Host firmware changes (XIAO)

- **Radio abstraction (reuse phase1):** `src/LoraRadio.h` already defines the abstract
  interface + shared `LoraConfig`; `WioSX1262` already implements it. Add **`RemoteRadio`** (a
  `LoraRadio` backed by the UART link) and a **`UartLink`** owning the `HardwareSerial`. One
  FreeRTOS **link-service task** deframes RX → per-radio queues, routes `LOG`→`Serial`, and
  tracks `READY`/`PONG`; TX frames are written under a UART mutex. `RemoteRadio::read()` pops
  its queue; `begin()` sends `CFG_RADIO`; `transmit()` sends a `TX` frame.
- **`BridgeConfig` schema v5 → v6:**
  - Grow `RadioRf radio[2]` → `radio[4]`; `clampRadio()` → `0..3`; add `NUM_RADIOS = 4`.
  - Add per-radio **`band`** (`BAND_SUBGHZ`/`BAND_2G4`/`BAND_SBAND`) and **`routeMask`**
    (uint8_t, bits 0..3 = bridge-RX-to destinations). `chip` already exists (v5).
  - Per-radio **channel name/key for 4 slots** — promote the `r1/r2ChannelName/Key` fields to
    an array `radioChannel{Name,Key}(idx)` (recommended) or add `r3/r4` fields (less churn).
  - **Migration v5→v6** (array grows, so the v4→v5 pad-byte trick does **not** apply — a real
    migration): copy `radio[0..1]` + r1/r2 channel → idx 0/1; default `radio[2..3]` =
    `PROTO_NONE`, `chip=CHIP_LR1121`, `band` from default freq, `routeMask=0`. Keep the v2→v5
    chain so older blobs still upgrade.
- **Bridge core (`main.cpp`):** generalize 2→4. `g_chan[4]`, `g_radioEnabled[4]`,
  `LoraRadio* radios[4]` (R1/R2 = `WioSX1262`, R3/R4 = `RemoteRadio`). On RX from radio *i*,
  fan out to every *j* where `routeMask[i]` bit *j* is set, *j*≠*i*, and radio *j* is enabled —
  via the **existing generic `bridgePacket(srcChan, dstChan, dstRadio, …)`**. The existing
  `[MT]/[MC]/[rns]` loop markers prevent re-bridging. Rebalance work across the 2 cores + the
  link-service task.
- **`NodeDB`:** add a mutex — with 4 radios + matrix routing, more than one task may
  `upsert/lookup` (today only `radio1Task` does). No schema change.
- **`CaptivePortal`:** render radios **1..4** (`appendRadio(n)` is already parameterized);
  extend the inline JS to `upd(1..4)` and the token/`PC[]` tables. **Reuse the phase1 2.4 GHz
  JS** (`is24`/`PRE24`/`BAND24`/`slot(...,wide)`) for R3/R4, plus a per-radio **band selector**
  (sub-GHz / 2.4 GHz / S-band[disabled]). Add the **routing-matrix UI** per radio ("Bridge RX
  to: ☐R1 ☐R2 ☐R3 ☐R4", self excluded). Extend the same-protocol/same-channel guard to a
  pairwise check among radios that route to each other.
- **`platformio.ini` / `LoraConfigCheck.h`:** add `LORA_RADIO3_*`/`LORA_RADIO4_*` first-boot
  defaults (R3/R4 default `PROTO_NONE`, `CHIP_LR1121`, sub-GHz, S-band stub off) and
  `BRIDGE_LINK_*` (UART num/pins/baud) flags; extend the static_asserts to R3/R4 (**reuse the
  phase1 wideLora BW set + 2.4 GHz range guards**).

## Co-processor firmware (T-Lora-Dual) — new in-repo PIO project

Self-contained PlatformIO project in the repo (e.g. `coproc-tlora-dual/`, `board = pico32`,
RadioLib **7.7.0** to match the host). **Factory takes precedence for the HAL and `begin()`**:

- Exact Factory pin map + the Factory **`rfswitch_table`** (DIO5–8; `MODE_TX_HP` sub-GHz /
  `MODE_TX_HF` 2.4 GHz — known-working on this board) + the Factory **`begin()`/`radio_config()`**
  sequence (its calibration/modem setup is exactly the part the Wio path never got right).
- Phase1's `WioLR1121.cpp` is a **reference only** for RadioLib-7.7.0 call shapes (the LR11x0
  `begin()` `high`-band selection, TCXO 3.0 V); its SKY13373 switch table and RX-bug-hunt
  diagnostics are **not** ported.
- Replace the Factory demo loop with the **UART slave**: emit `READY`; serve
  `CFG_RADIO`/`TX`/`START_RX`/`PING`/`RESET`; IRQ-driven RX (`setPacketReceivedAction`) →
  `RX` frame with RSSI/SNR; band-aware power (22 dBm sub-GHz / 13 dBm 2.4 GHz).
- **S-band:** `BAND_SBAND` is accepted on the wire but the co-proc logs a `LOG` line and
  refuses to configure/transmit (stub for future development).

## Reuse provenance (from `lr1121-phase1`)

**✅ Keep (inherited):** `LoraRadio.h` interface; `WioSX1262` interface refactor; `BridgeConfig`
v5 + `Chip` enum + `radioChip()`; `RegionPreset.h` 2.4 GHz math (`modemPresetParams(…,wideLora)`,
`BAND_2G4_*`, `slotFrequency2G4()`, `bandCenter2G4()`); `LoraConfigCheck.h` wideLora BW + 2.4 GHz
range; `CaptivePortal.cpp` 2.4 GHz JS + the validation fixes (freq-field no-clobber; Custom =
no BW check; MeshCore BW required; SF/CR bounded; 13 dBm 2.4 GHz cap; wideLora BW on MT preset
save); RadioLib **7.7.0**; `RadioProfile.h`.

**⚠️ Reference, not host-linked:** `WioLR1121.cpp/.h` (mine its RadioLib call shape; the
LR1121s now live on the co-processor, reached over UART).

**🗑️ Strip in a later impl pass:** diagnostic flags `LR1121_DEBUG`,
`LR1121_BRUTEFORCE_RX_DIOMASK`, `LR1121_RX_AUDIT_RUN`, and the active **`R2_RX_ONLY_TEST`**
(leaves the bridge half-live); the SKY13373 switch table + RX-init DOE scaffolding; the
Seeed/HackRF bring-up docs (`SEEED_*`, `HACKRF-*`, `LR1121-RX-INIT-AUDIT.md`, `docs/testbed/`)
→ archive as Phase-1 history.

## Build staging

- **A** `BridgeConfig` schema **v6** (radio[4] + band + routeMask + per-radio channel) + v5→v6
  migration.
- **B** `RemoteRadio` + `UartLink` + shared `LinkProtocol.h` (host side); reuse `LoraRadio`.
- **C** bridge core 2→4 + configurable routing matrix; `NodeDB` mutex; task/core rebalance.
- **D** portal (radios 1..4 + band + routing matrix + validation) + `platformio.ini` flags +
  `LoraConfigCheck` extension.
- **E** T-Lora-Dual **co-processor firmware** (Factory HAL + `begin()`; UART slave; band-aware
  power; S-band stub).
- **F** docs (CHANGELOG, README) + contest Phase-2 writeup; build both firmwares (`pio run`)
  and on-air bridge test.

## Verification (per future pass)

- `pio run` on the host env and on the co-processor project.
- UART link bring-up: host sees co-proc `READY`; `PING`/`PONG` round-trips; `LOG` surfaces.
- On-air: traffic on any radio is repeated to the radios selected in its routing mask, with
  correct cross-protocol translation and no loops; 2.4 GHz LongFast decodes on R3/R4.

## Open items

- UART GPIOs vs each board's physical header (defaults build-flag overridable).
- RadioLib 7.7.0 LR11x0 `begin()` signature when porting the Factory sequence.
- v5→v6 migration correctness (array growth) + chip handling for the remote R3/R4 slots.
- Routing-matrix loop prevention in a 4-way mesh (same-proto/same-channel pairwise guard).
- Whether `band` is stored explicitly (needed for the S-band stub) or derived from frequency
  as phase1 does for the 2.4 GHz case.

## Out of scope (this phase)

- Working S-band TX/RX (stub only).
- Reviving the Wio-LR1121 RX path (Phase-1 line; superseded by the T-Lora-Dual).
- Pushing the branch / posting the contest issue update (owner-reviewed; drafts only).

## Contest Phase-2 writeup (draft for `meshtastic-build-off-2026#2`)

> **Phase 2 — 4-Up Triband Omnidirectional Repeater.** Extends the dual-SX1262 cross-protocol
> bridge with a LilyGO T-Lora-Dual (two LR1121 radios) over a UART link, adding 2.4 GHz (and an
> S-band stub) alongside sub-GHz for a four-radio, tri-band, any-to-any repeater with a
> portal-configurable routing matrix across Meshtastic / MeshCore / Reticulum. Builds on the
> Phase-1 LR1121 bring-up; pivoted from the Wio-LR1121 (unresolved calibration/modem RX deficit)
> to the T-Lora-Dual (working RX+TX).
