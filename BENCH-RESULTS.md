# v8.4 ABP encoder — bench results & status

**Date:** 2026-06-16 · **Branch:** `dev-ABP-lorawan` · **Firmware under test:** `bench_lw_enc` /
`bench_lw_enc_dwell` / `bench_lw_sniffer` / `xiao_esp32s3` (commits up to `bbdc7d6`).
**Rig:** DUT-A = COM13 (sender), SNIFFER = COM6 (keyless listener / "synthetic LNS"); stimulus =
owner's Meshtastic node `!0AC9F340`. Serial captured from the host over USB.

## Score: 6 PASS · 10 OPEN (0 FAIL)
- **Closed (PASS):** A1, A2, A5, B1, B2, B5 — the core (crypto, on-air emit, decrypt, reboot-safe FCnt, do-no-harm).
- **Open — owner, WiFi config page:** A4, B3, B4, B6, B7.
- **Open — owner, needs >242 B raw transmitter:** A3.
- **Open — colleague, ChirpStack LNS:** C1, C2, C3, C4.

## Status matrix

| ID | Test | Status | Result / blocker | Run by |
|----|------|--------|------------------|--------|
| **A1** | Crypto self-test (gate) | ✅ PASS | `[lw-selftest] overall : PASS` on COM13 | done |
| **A2** | US915 per-TX dwell cap | ✅ PASS | SF12 frame: `drop=dwell toa=1811 limit=400`, not transmitted | done |
| **A3** | Oversize payload dropped | ⬜ OPEN | needs a >242 B raw-LoRa transmitter (gear not on bench) | owner (gear) |
| **A4** | Portal saves ABP device | ⬜ OPEN | needs WiFi config page | owner (portal) |
| **A5** | Stock build do-no-harm | ✅ PASS | stock boot: zero `[lw-selftest]`/`[lw-enc]`/`[LoRaWANConfig]` | done |
| **B1** | Message → packet on air | ✅ PASS | MT text → DUT `evt=QUEUE dstproto=LW` → SNIFFER `evt=RX proto=LW` | done |
| **B2** | FCnt monotonic across reboot | ✅ PASS | before reboot `fcnt=33` → after reboot `fcnt=99` (never 0) | done |
| **B3** | DevAddr keeps counter across rows | ⬜ OPEN | needs WiFi config page | owner (portal) |
| **B4** | Per-source resolve (node→proto→Any) | ⬜ OPEN | needs WiFi config page + 2 MT nodes | owner (portal) |
| **B5** | MIC + decrypt off-box | ✅ PASS | `lw-verify.py` on captured frame: MIC PASS, decrypts to sent text | done |
| **B6** | Source tag decodes to proto | ⬜ OPEN | needs WiFi config page | owner (portal) |
| **B7** | MeshCore source → ABP | ⬜ OPEN | needs WiFi config page + MeshCore node | owner (portal) |
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

**B5 — offline MIC + decrypt (host, on the B1 frame):**
```
python tools/lw-verify.py 40010000010021000d2350a53dfbbfeb298916c7a0d587055061ac99d1511cee268b2c4e7747fc98030fc7 <NwkSKey> <AppSKey>
MIC: got 98030fc7  expect 98030fc7  -> PASS
Decrypted as ASCII: 'Test to claude from Meshtastic'
VERDICT: MIC valid — ChirpStack would ACCEPT this frame
```

## What "closed" means here
The 6 passes cover the whole signal chain end-to-end **without a LNS**: a real mesh message is
encoded into a valid LoRaWAN 1.0.x uplink, transmitted on air, captured, and decrypted back to the
original text with a valid MIC — plus the dwell cap, reboot-safe counter, and stock do-no-harm. The
10 open items are not failures; they are blocked on the WiFi config page (owner), a >242 B raw
transmitter (owner), or a ChirpStack LNS (colleague). Step-by-step for the owner-solo items is in
`BENCH-SOLO.md`.
