# BENCH-CAMC2 — end-to-end LoRaCam C2 round-trip (two boards)

Goal: prove a real **encrypted, signed, whitelisted** command goes from a commander board
to the cam board over LoRa, the cam acts on it, and ACKs back — on your bench.

## What you need
- **Board A = the cam** — the XIAO ESP32-S3 Sense + the Wio you already wired. On **COM14**.
- **Board B = the commander** — a SECOND XIAO + a Wio, wired the SAME way (SPI on D8/D9/D10,
  NSS→D4 / BUSY→D3 / RST→D2 / DIO1→D1, 3V3, GND). **A repeater board works as-is** (its R2 is
  that exact radio). No camera needed on board B.

Both boards already share a throwaway bench key and fixed IDs (cam = `0xCA00`, commander = `0xC0DE`),
baked into the build flags — so they whitelist each other with no manual key entry.

## Steps (one at a time)

**1. Flash the cam** (board A, COM14) — already done for you:
```
pio run -e bench_camc2 -t erase  --upload-port COM14
pio run -e bench_camc2 -t upload --upload-port COM14
```
Boot log should show: `[CamC2Config] bench-seeded peer 0x0000c0de` and `[Radio2-Edge] ready ... sync 0x33`.

**2. Flash the commander** (board B). Find its port (e.g. COM13), then:
```
pio run -e bench_camc2_cmdr -t erase  --upload-port COM13
pio run -e bench_camc2_cmdr -t upload --upload-port COM13
```

**3. Open BOTH serial monitors** (two terminals):
```
pio device monitor --port COM14      ← the cam
pio device monitor --port COM13      ← the commander
```

**4. Wait ~6 seconds** after the commander boots (it has a 5 s "press BOOT for portal" window first).

**5. In the COMMANDER monitor, type one key** (just press the letter — no Enter needed):
- `g` = get status   ·   `s` = snap photo   ·   `r` = record 30 s   ·   `x` = stop

**6. What you should see — the round-trip:**

Commander:
```
evt=C2TX radio=1 to=0x0000ca00 cmd=6 seq=1
evt=C2RX radio=1 from=0x0000ca00 type=2 seq=1 len=9      ← the cam's signed ACK
```
Cam (COM14):
```
evt=C2CMD radio=1 from=0x0000c0de cmd=6 seq=1 res=0      ← authenticated + executed (res=0 = OK)
[CamC2] (stub) ...                                       ← Phase-1 stub; real camera lands in Phase 2
```

`res=0` = the command was accepted (valid signature, in-whitelist, fresh seq) and run. The camera
actions are stubs for now (`stub.jpg`) — Phase 2 wires the real OV2640/SD.

## Quick checks it's really secure (optional)
- **Wrong key rejected:** flash board B with a different `BRIDGE_C2_PEER_KEY` (edit the env) → the cam
  logs `[CamC2] drop ... reason=bad-mic` and does NOT execute.
- **Replay rejected:** every command uses a new `seq`, so re-sends are new. To force a replay, capture a
  frame and resend it (see the offline tool) → cam logs `evt=C2DROP ... reason=replay`.

## Offline frame tool (no hardware)
`tools/cam-c2.py` builds/verifies frames with the same crypto as the firmware:
```
python tools/cam-c2.py selftest                                  # crypto KAT
python tools/cam-c2.py gen 000102030405060708090a0b0c0d0e0f 1 0000C0DE 0000CA00 1 06   # a GET_STATUS cmd
python tools/cam-c2.py verify <frame-hex> 000102030405060708090a0b0c0d0e0f             # decode/verify
```
Use `verify` on a frame captured by a sniffer to confirm what's on air, byte-for-byte.

## Notes
- Both boards are on the same C2 channel: **Custom sync 0x33, 906.875 MHz, BW250, SF9** (build defaults).
- The bench key/IDs are throwaway — never a real deployment key.
- These are **bench** envs (`BRIDGE_BENCH_AUTOSAVE`, fixed IDs, build-flag key); the shipping `xiao_loracam`
  build uses MAC-derived IDs and portal/serial provisioning (Phase 3), and stays do-no-harm.
