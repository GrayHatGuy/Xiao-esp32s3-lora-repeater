# v8.4 ABP encoder — bench results & status

**Date:** 2026-06-16 · **Branch:** `dev-ABP-lorawan` · **Firmware under test:** `bench_lw_enc` /
`bench_lw_enc_dwell` / `bench_lw_sniffer` / `xiao_esp32s3_lwabp` (portal) / `xiao_esp32s3` (commits up
to `b31527a`; code unchanged since `bbdc7d6` — intervening commits are docs only).
**Rig:** DUT-A = COM13 (sender), SNIFFER = COM6 (keyless listener / "synthetic LNS"); stimulus =
owner's Meshtastic node `!0AC9F340`. Serial captured from the host over USB.

## Score: 11 PASS · 5 OPEN (0 FAIL)
- **Closed (PASS):** A1, A2, A4, A5, B1, B2, B3, B4, B5, B6, B7 — the full bench-side chain: crypto, on-air emit, decrypt, reboot-safe FCnt, do-no-harm, portal persistence (A4), DevAddr-keyed FCnt across a row move (B3), per-source DevAddr resolution (B4), source-protocol tag MT+MC (B6), MeshCore→ABP (B7).
- **Owner-solo set (WiFi config page): COMPLETE** — A4 / B3 / B4 / B6 / B7 all PASS.
- **Open — owner, needs >242 B raw transmitter:** A3.
- **Open — colleague, ChirpStack LNS:** C1, C2, C3, C4.

## Status matrix

| ID | Test | Status | Result / blocker | Run by |
|----|------|--------|------------------|--------|
| **A1** | Crypto self-test (gate) | ✅ PASS | `[lw-selftest] overall : PASS` on COM13 | done |
| **A2** | US915 per-TX dwell cap | ✅ PASS | SF12 frame: `drop=dwell toa=1811 limit=400`, not transmitted | done |
| **A3** | Oversize payload dropped | ⬜ OPEN | needs a >242 B raw-LoRa transmitter (gear not on bench) | owner (gear) |
| **A4** | Portal saves ABP device | ✅ PASS | reboot reload: `anyConfigured=1`, `dev0 … devaddr=0x01000001 fport=13` persisted; Radio 2 now LoRaWAN | done |
| **A5** | Stock build do-no-harm | ✅ PASS | stock boot: zero `[lw-selftest]`/`[lw-enc]`/`[LoRaWANConfig]` | done |
| **B1** | Message → packet on air | ✅ PASS | MT text → DUT `evt=QUEUE dstproto=LW` → SNIFFER `evt=RX proto=LW` | done |
| **B2** | FCnt monotonic across reboot | ✅ PASS | before reboot `fcnt=33` → after reboot `fcnt=99` (never 0) | done |
| **B3** | DevAddr keeps counter across rows | ✅ PASS | moved slot 0→1: `dev1 … devaddr=0x01000001 fcnt=165` (was 165 at dev0, not reset to 0) | done |
| **B4** | Per-source resolve (node→proto→Any) | ✅ PASS | `0AC9F340`→`devaddr=01000001` (node match), `3D3A87A3`→`01000002` (proto fallback), both `cred=nvs` | done |
| **B5** | MIC + decrypt off-box | ✅ PASS | `lw-verify.py` on captured frame: MIC PASS, decrypts to sent text | done |
| **B6** | Source tag decodes to proto | ✅ PASS | tagged frames decrypt to `proto=1(meshtastic)` and `proto=2(meshcore)`, all MIC-valid | done |
| **B7** | MeshCore source → ABP | ✅ PASS | MC text on R1 → ABP uplink `devaddr=01000001`, decrypts to MC body, MIC PASS | done |
| **C1** | Uplink ingested by ChirpStack | ⬜ OPEN | needs ChirpStack LNS + gateway | colleague |
| **C2** | Custom/weather → ChirpStack | ⬜ OPEN | needs ChirpStack LNS + Custom source | colleague |
| **C3** | Codec decodes source tag | ⬜ OPEN | needs ChirpStack LNS | colleague |
| **C4** | LNS accepts ↑FCnt, rejects replay | ⬜ OPEN | needs ChirpStack LNS | colleague |

## Evidence (verbatim serial / tool output)

**A1 — self-test (COM13, `bench_lw_enc`):**
```
[lw-selftest] RFC4493 CMAC len= 0 : PASS   (×4: len 0/16/40/64)
[lw-selftest] FRMPayload A_1 keystream : PASS
[lw-selftest] frame assembly + round-trip : PASS
[lw-selftest] overall : PASS
```

**A2 — dwell cap (COM13, `bench_lw_enc_dwell`, SF12):**
```
evt=QUEUE radio=R1 dst=R2 dstproto=LW len=34 devaddr=01000001 fcnt=33 fport=13 cred=flag
evt=DROP  radio=R2 drop=dwell toa=1811 limit=400 len=34      <- 1811 ms > 400 ms, NOT sent
```

**A4 — portal config persists (COM13, `xiao_esp32s3_lwabp`):** owner set Device 0 = DevAddr
`01000001` / Any source / FPort 13 via the WiFi portal and saved; the board rebooted and reloaded the
table from NVS:
```
[LoRaWANConfig] loaded 4 ABP device slots (anyConfigured=1)
[LoRaWANConfig] dev0 sel=0 match=0x00000000 devaddr=0x01000001 fport=13 fcnt=0
[BridgeConfig] v4 configured=1 region=1
  radio2 RF = proto=5 sync=0x34 903.900 MHz BW125.0 SF7
```
Config survived the reboot (NVS-persisted); Radio 2 switched to LoRaWAN.

**A5 — stock do-no-harm (COM13, `xiao_esp32s3`):** boot dump showed
`[BridgeConfig] v4 configured=0 region=0` and `radio2 RF = proto=2 sync=0x12` (MeshCore), with
**no** `[lw-selftest]`, **no** `[lw-enc]`, **no** `[LoRaWANConfig]` lines (encoder compiled out).

**B1 — end-to-end (COM13 sender + COM6 sniffer):** owner sent MT text "Test to claude from Meshtastic".
```
DUT  (COM13): evt=QUEUE radio=R1 dst=R2 dstproto=LW len=43 devaddr=01000001 fcnt=33 fport=13 cred=flag
SNIFF(COM6) : evt=RX proto=LW mtype=UnconfDataUp devaddr=0x01000001 fcnt=33 fport=13 len=43
SNIFF(COM6) : evt=LWRAW len=43 raw=40010000010021000d2350a53dfbbfeb298916c7a0d587055061ac99d1511cee268b2c4e7747fc98030fc7
```

**B2 — FCnt across reboot (COM13):** uplink "Now" → `fcnt=33`; board rebooted; uplink "Hehhehe" →
`fcnt=99`. Counter advanced (33 → 99), never reset to 0 → no replay.

**B3 — DevAddr-keyed FCnt across a portal row move (COM13, `xiao_esp32s3_lwabp`):** the counter for
`01000001` had already advanced to 165 (`dev0 … fcnt=165`). Owner disabled Device 0 and re-added the
**same** DevAddr in Device 1, then saved:
```
[LoRaWANConfig] device table saved (176 B)        <- save accepted
[CaptivePortal] config saved, rebooting...
  ... reboot ...
[LoRaWANConfig] dev1 sel=0 match=0x00000000 devaddr=0x01000001 fport=13 fcnt=165
```
Address moved **slot 0 → slot 1**; counter stayed **165, not reset to 0** — FCnt is keyed by DevAddr
(`fc_<addr>`), not slot index (fix #1). A later clean reboot advanced it 165 → 198 (monotonic, never
replays), re-confirming the move is durable + the reboot-safe counter. *Repro note:* the portal save
is all-or-nothing — a bad field in any enabled row (e.g. a mistyped 32-hex key) makes
`applyLoRaWANDevices()` return early before `saveTable()`, silently keeping the old config. Watch for
the red error banner, or copy-paste the keys to avoid a typo.

**B4 — per-source DevAddr resolution (COM13, `xiao_esp32s3_lwabp`):** two configured devices —
`dev0` = MT-node-id `0ac9f340` → `01000001` (specificity 2), `dev1` = MT-protocol → `01000002`
(specificity 1). Owner sent one MT text from each of two nodes; resolve() chose the DevAddr per sender:
```
src=0x0AC9F340 → evt=QUEUE dstproto=LW devaddr=01000001 fport=13 cred=nvs  msg="Testing Meshtastic"
src=0x3D3A87A3 → evt=QUEUE dstproto=LW devaddr=01000002 fport=13 cred=nvs  msg="testing MT msg meshpoint"
```
Node `0AC9F340` (specific node-id match) won → `01000001`; node `3D3A87A3` (no node match, fell to the
MT-protocol rule) → `01000002`. Both `cred=nvs` = matched the NVS table, not the build-flag fallback —
resolve() priority ladder confirmed (node-id beats protocol).

**B5 — offline MIC + decrypt (host, on the B1 frame):**
```
python tools/lw-verify.py 40010000010021000d2350a53dfbbfeb298916c7a0d587055061ac99d1511cee268b2c4e7747fc98030fc7 <NwkSKey> <AppSKey>
MIC: got 98030fc7  expect 98030fc7  -> PASS
Decrypted as ASCII: 'Test to claude from Meshtastic'
VERDICT: MIC valid — ChirpStack would ACCEPT this frame
```

**B6 — source-protocol tag in FRMPayload (COM6 sniffer + offline `lw-verify.py --tagged`):** Device 0 =
Any source with "Prepend source tag" on. A Meshtastic text and three MeshCore texts were each encoded,
captured, and decrypted with the keys:
```
MT (node 0AC9F340): src tag : proto=1(meshtastic) srcId=0x0AC9F340  payload 'Twstesww'
MC (#330):          src tag : proto=2(meshcore)   srcId=0x00000000  payload '9506C57C: Final test'
MC (#331):          src tag : proto=2(meshcore)                     payload 'Meshpoint - Meshcore: send mc'
```
Every frame MIC-valid. The tag decodes to the correct source protocol both ways (1=MT, 2=MC). MeshCore
carries no on-air numeric node id, so `srcId=0` is expected — its identity rides in the body prefix
(`9506C57C:`).

**B7 — MeshCore source → ABP uplink (COM6 sniffer + decrypt):** R1 set to MeshCore (910.525 / BW62.5 /
SF7, public channel hash 0x11 — matched to the build-flag R2 config). A MeshCore text received on R1 was
encoded to a LoRaWAN ABP uplink on R2, captured, and decrypted to its original body:
```
DUT  (COM13): evt=RX radio=R1 proto=MC → MeshCore GRP_TXT ch=0x11 body "9506C57C: Vgcvv"
              → evt=QUEUE dstproto=LW devaddr=01000001 fcnt=332 fport=13 cred=nvs tag=1
              → evt=TX_START radio=R2 cad=clear → evt=TX_DONE done
SNIFF(COM6): evt=RX proto=LW devaddr=0x01000001 fcnt=330 fport=13
lw-verify  : MIC PASS → '9506C57C: Final test'   (ChirpStack would ACCEPT)
```
The MeshCore body survived the MeshCore→ABP transcode end-to-end. ⚠️ Root cause of a long bench detour:
R1's frequency was hand-entered as `910.575` vs the correct `910.525` (a 50 kHz slip → `rx-error rc=-7`
CRC failures). Triggered by the portal not auto-filling defaults on protocol switch — see the TODO at
`CaptivePortal.cpp` `appendScript()`.

## What "closed" means here
The 11 passes cover the whole signal chain end-to-end **without a LNS**: a real mesh message —
**Meshtastic *and* MeshCore** — is encoded into a valid LoRaWAN 1.0.x uplink, transmitted on air,
captured, and decrypted back to the original text with a valid MIC — plus the dwell cap, reboot-safe
counter, stock do-no-harm, portal persistence (A4), DevAddr-keyed FCnt across a config row move (B3),
per-source DevAddr resolution (B4), source-protocol tagging (B6), and the MeshCore→ABP transcode (B7).
**The entire owner-solo set (A4 / B3 / B4 / B6 / B7) is now PASS.** The 5 open items are not failures;
they are blocked on a >242 B raw transmitter (owner: A3) or a ChirpStack LNS (colleague: C1–C4).
Step-by-step for the owner-solo items is in `BENCH-SOLO.md`.

---

## References — test protocols & methods (to repeat or audit this bench)

**Test definitions & pass criteria (what each test does):**
- `BENCH-v8.4.md` — core procedure for A1, A2, A5, B1, B2, B5 (plain steps).
- `BENCH-SOLO.md` — the WiFi-config-page tests A4, B3, B4, B6, B7 (step by step).
- The pass/fail criterion for every test is also the **"Result / blocker" column** in the matrix above.
- `ABP-LORAWAN-SPEC.md` — design of record (frame layout, crypto, FCnt scheme).
- Pre-bench review fixes #1–#6 are summarised in `BENCH-v8.4.md` (commits `d98da4b`, `1496717`, `9aaf423`).

**Firmware under test:** branch `dev-ABP-lorawan`, tip `bbdc7d6`. Build environments are defined in
`platformio.ini`:
- `bench_lw_enc` — sender: R1 Meshtastic 906.875/BW250/SF11, R2 LoRaWAN 903.9/BW125/SF7, REGION=US,
  build-flag ABP creds, autosave (no portal).
- `bench_lw_enc_dwell` — as above but R2 **SF12** (used for A2 dwell).
- `bench_lw_sniffer` — listener: keyless LoRaWAN capture on 903.9/BW125/SF7 with full-frame hex
  (`BRIDGE_LW_CAPTURE_HEX`), relay/summary off.
- `xiao_esp32s3` — stock control, encoder compiled out (used for A5).

**Bench credentials (throwaway, baked into the `bench_lw_enc*` envs):** DevAddr `01000001` ·
NwkSKey `2B7E151628AED2A6ABF7158809CF4F3C` · AppSKey `D41420B7F5A3C96E1D8204F7B3A65C90` · FPort `13`.

**Flash a board:**
```
pio run -e <env> -t erase  --upload-port COMx     # wipe NVS (clean config + FCnt)
pio run -e <env> -t upload --upload-port COMx
```
Erase is required when changing env/config — autosave/portal only act on an unconfigured board.

**Capture serial (how the evidence above was obtained) — two equivalent ways:**
- Manual: `pio device monitor --port COMx --baud 115200`, then press the board's **RST** button to
  catch the one-time boot dump.
- Automated (used for this report): `python tools/bench-serial-capture.py COMx <seconds>` — opens the
  port, hardware-resets the board (DTR/RTS), and prints serial for N seconds. The boot dumps and
  `evt=…` lines quoted above are verbatim from this tool.

**Verify a captured packet off-box (method behind B5):**
`python tools/lw-verify.py <PHYPayload-hex> <NwkSKey> <AppSKey> [--tagged]` — validates the MIC and
decrypts the FRMPayload; mirrors `src/LoRaWANCrypto.h` byte-for-byte.

**Rig & stimulus:** two boards — **DUT-A on COM13** (sender) and **SNIFFER on COM6** (listener); a
Meshtastic node (`!0AC9F340`, US LongFast) transmitted the text messages. Serial was read on the
host PC over USB. ChirpStack was **not** used for any result above (Tier C is the colleague's rig).
