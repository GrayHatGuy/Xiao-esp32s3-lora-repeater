# v8.4 bench guide (plain English)

The job: prove the bridge can take a normal mesh text message and send it out as a
proper LoRaWAN packet that a LoRaWAN network would accept.

## What you need

- **Board 1 — the sender.** A Xiao dual-radio bridge on USB. It listens for a mesh
  message and re-sends it as a LoRaWAN packet. (Yours is on **COM13**.)
- **Board 2 — the listener.** A second Xiao bridge on USB. It just catches the LoRaWAN
  packet out of the air so you can see it. (Plan calls this **COM6**.)
- **A Meshtastic device** (phone app or a node) to send the text that starts it off.
  For the MeshCore tests, a MeshCore device instead.
- **Optional, your colleague only:** a ChirpStack server + a LoRaWAN gateway. Not needed
  for any test below — those are for the final "a real network accepts it" check.

Jargon, once:
- **Captive portal** = a WiFi config page the board hosts. When you flash the portal
  build and the board is blank, it shows up as a WiFi hotspot; connect to it and a
  settings page opens.
- **ABP device** = NOT a physical thing. It's a set of LoRaWAN credentials (an address +
  two keys + a port number) you type into that config page, or that come baked into the
  `bench_lw_enc` build. They sign/encrypt the packet.

## Setup (do once)
Open PowerShell here:
```
cd "C:\Users\6r4yh\workspace\Platformio\Projects\Xiao-esp32s3-lora-repeater - main dev-ABP-lorawan"
```
Every board takes the same 3 commands — wipe, flash, watch:
```
pio run -e <build> -t erase  --upload-port <COMx>
pio run -e <build> -t upload --upload-port <COMx>
pio device monitor --port <COMx> --baud 115200
```
The bench credentials (already baked into `bench_lw_enc`): address `01000001`,
keys `2B7E151628AED2A6ABF7158809CF4F3C` and `D41420B7F5A3C96E1D8204F7B3A65C90`, port 13.

---

## The 3 tests that prove it works (no ChirpStack, no config page)

### 1. Self-test — the math is right  ✅ ALREADY PASSED on your COM13 board
Flash `bench_lw_enc` to the sender and watch it boot:
```
pio run -e bench_lw_enc -t erase  --upload-port COM13
pio run -e bench_lw_enc -t upload --upload-port COM13
pio device monitor --port COM13 --baud 115200
```
**Pass:** the boot text shows `[lw-selftest] overall : PASS`. (It did.)

### 2. See the packet go out
Plug in the second board as the listener and flash it:
```
pio run -e bench_lw_sniffer -t erase  --upload-port COM6
pio run -e bench_lw_sniffer -t upload --upload-port COM6
pio device monitor --port COM6 --baud 115200
```
Now, from your Meshtastic device, send any text on the default LongFast channel.
**Pass:** the listener prints a line like
`evt=RX proto=LW devaddr=0x01000001 fcnt=… fport=13` and a line `evt=LWRAW raw=<hex>`.
That `<hex>` is the actual LoRaWAN packet, on the air.

### 3. Prove a real network would accept it (off-line, on your PC)
Copy the `raw=<hex>` from step 2 and run:
```
pip install cryptography
python tools/lw-verify.py <paste-the-hex> 2B7E151628AED2A6ABF7158809CF4F3C D41420B7F5A3C96E1D8204F7B3A65C90
```
**Pass:** it prints `MIC … -> PASS` and shows your message text decrypted back out, ending
with `VERDICT: MIC valid — ChirpStack would ACCEPT this frame`.

If those three pass, the feature works. Everything below is optional.

---

## Optional extras

- **Reboot counter test:** in step 2, note the `fcnt=` numbers, press the sender's RST
  button, send more texts. The number must keep going **up**, never restart. (Proves it
  won't get rejected as a replay after a power cycle.)
- **Dwell-time limit:** flash the sender with `bench_lw_enc_dwell` instead (same commands,
  swap the build name). It uses a slow setting on purpose; the sender should print
  `drop=dwell` and **not** transmit — that's the legal air-time cap working.
- **MeshCore source:** same as step 2 but send from a MeshCore device. Needs the config
  page to set the sender's first radio to MeshCore (see "config page" below).
- **Config page (only if you want to set credentials without recompiling):** flash
  `xiao_esp32s3_lwabp`, connect to the board's WiFi hotspot, open the page, fill the
  "LoRaWAN ABP devices" row (address + the two keys + port), set one radio to LoRaWAN,
  Save. On reboot the serial should say `anyConfigured=1`. This is just a convenience —
  `bench_lw_enc` already has working credentials built in.

## For your colleague (has ChirpStack)
Flash `bench_lw_enc`, add a device in ChirpStack with the same address `01000001`,
the two keys, port 13 (LoRaWAN 1.0.x, ABP, Class A, turn off frame-counter checking).
Send a text from a Meshtastic device. Pass = the message shows up decoded in ChirpStack.

## If something fails
- Nothing on the listener in step 2: the two boards aren't on the same radio settings, or
  the sender didn't send — check the sender's serial for `evt=QUEUE … dstproto=LW`.
- `MIC -> FAIL` in step 3: the keys you typed don't match, or the hex got truncated.
- Wrong values in the boot text: you skipped the `erase` step, so the board kept old
  settings — erase, flash, recheck.
