# Solo bench tests — the ones you run by hand

These need the board's WiFi config page (or special gear), so you drive them. The rest
(A1, A2, A5, B1, B2, B5) I run from here. Each test below is self-contained.

## Stuff you repeat for every test (do once per flash)

**1. Flash the config-page build to the sender (COM13):**
```
pio run -e xiao_esp32s3_lwabp -t erase  --upload-port COM13
pio run -e xiao_esp32s3_lwabp -t upload --upload-port COM13
```

**2. Get on the config page:**
- After flashing, the board makes an **open WiFi network named `LoRa-Bridge-XX`** (the exact
  name prints on the serial monitor at boot). Connect your phone/laptop to it.
- A settings page should open by itself. If not, open a browser to **http://192.168.4.1**.

**3. Set Radio 2 to LoRaWAN** (so the packet goes where the listener can hear it):
- Radio 2 → **Protocol: LoRaWAN**, **Frequency: 903.9**, **Bandwidth: 125**, **Spreading factor: 7**.

**4. The keys** (paste where a test asks for them):
- NwkSKey: `2B7E151628AED2A6ABF7158809CF4F3C`
- AppSKey: `D41420B7F5A3C96E1D8204F7B3A65C90`

**5. Watch the result:** keep the listener's monitor open while you send —
`pio device monitor --port COM6 --baud 115200`. (A4 and A3 watch the sender COM13 instead.)

The device table is the section titled **"LoRaWAN ABP devices (v8.4 encoder)"**. Each device row
has: **Enabled** checkbox · **Applies to source** (Any source / Meshtastic node id / Source
protocol) · **Match value** · **DevAddr (8 hex)** · **NwkSKey** · **AppSKey** · **FPort** ·
**Prepend source tag** checkbox. After filling, scroll down and click **Save** — the board reboots.

---

## A4 — does the config page save?
1. Flash `xiao_esp32s3_lwabp`, get on the page, set Radio 2 = LoRaWAN.
2. Device 0: **Enabled** ✓ · Applies to source = **Any source** · DevAddr = `01000001` ·
   NwkSKey/AppSKey = the keys · FPort = `13`. **Save.**
3. Watch the sender: `pio device monitor --port COM13 --baud 115200`, press the board's **RST**.
4. **PASS:** you see `[LoRaWANConfig] loaded … anyConfigured=1` and
   `dev0 … devaddr=0x01000001 fport=13`.

## B3 — moving an address keeps its counter
1. Do A4 (Device 0 = `01000001`, Any). Send **3 texts** from your Meshtastic device; on the
   **listener (COM6)** note the `fcnt=` numbers.
2. Back on the page: **uncheck** Device 0's Enabled. In **Device 1**, enter the **same**
   address `01000001` + the same keys + FPort 13 + Any source. **Save.**
3. Send one text. On COM6 note the new `fcnt=`.
4. **PASS:** the new `fcnt=` is **higher** than the last one from step 1 (it did not reset).

## B4 — the right address is picked per sender
1. Flash `xiao_esp32s3_lwabp`, page, Radio 2 = LoRaWAN.
2. Two devices, same keys, different addresses:
   - **Device 0:** Applies to source = **Meshtastic node id** · Match value = **your node's
     8-hex id** (the one shown in the Meshtastic app, e.g. `0AC9F340`) · DevAddr `01000001` · FPort 13.
   - **Device 1:** Applies to source = **Source protocol** · Match value = **1** · DevAddr
     `01000002` · FPort 13.
   **Save.**
3. Send a text from the node whose id you put in Device 0. Then send from a **different**
   Meshtastic node.
4. **PASS (on COM6):** the first node's packet shows `devaddr=0x01000001` (its specific match
   won); the other node shows `devaddr=0x01000002` (matched by protocol).

## B6 — source tag decodes to the right protocol
1. Flash `xiao_esp32s3_lwabp`, page, Radio 2 = LoRaWAN.
2. Device 0: Enabled · Any source · DevAddr `01000001` · keys · FPort 13 · **check "Prepend
   source tag"**. **Save.**
3. Send a text from your Meshtastic device. On COM6, copy the `raw=` hex from the `evt=LWRAW` line.
4. Run:
   `python tools/lw-verify.py <paste-hex> 2B7E151628AED2A6ABF7158809CF4F3C D41420B7F5A3C96E1D8204F7B3A65C90 --tagged`
5. **PASS:** output shows `src tag : proto=1(meshtastic) …`. (Repeat from a MeshCore device → `proto=2(meshcore)`.)

## B7 — a MeshCore message also works
1. Flash `xiao_esp32s3_lwabp`, page. Set **Radio 1 → Protocol: MeshCore**, and Radio 2 → LoRaWAN
   903.9 / 125 / SF7.
2. Device 0: Enabled · Any source · DevAddr `01000001` · keys · FPort 13. **Save.**
3. Send a text from a **MeshCore** device.
4. **PASS (on COM6):** an `evt=RX proto=LW devaddr=0x01000001` line; run the captured `raw=` hex
   through `lw-verify.py` (no `--tagged`) → it decrypts to your MeshCore text.

## A3 — oversize packet is dropped (needs gear I have to prep)
This needs a transmitter that sends a **raw LoRa packet over 242 bytes** on a Custom channel —
nothing on your bench does that yet. Say the word and I'll flash a spare board (COM14) to be that
transmitter. Then:
1. Flash `xiao_esp32s3_lwabp`, page, set **Radio 1 → Protocol: Custom** (matching the
   transmitter's freq/BW/SF/sync), Radio 2 → LoRaWAN.
2. Device 0: Enabled · Any source · DevAddr `01000001` · keys · FPort 13. **Save.**
3. Trigger the transmitter.
4. **PASS (on COM13):** `evt=DROP … drop=lw-payload-overflow`.

---
When you finish one, paste me the serial line and I'll confirm — or just read the PASS line yourself.
