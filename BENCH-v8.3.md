# v8.3 Bench Test Protocol — XIAO ESP32-S3 Dual-SX1262 Bridge

**Target:** branch `v8.3-dev` (HEAD `ed81307`) at `C:\Users\6r4yh\workspace\Platformio\Projects\Xiao-esp32s3-lora-repeater - main v8.3`. RadioLib pinned **7.7.0**. Monitor: owner runs `pio device monitor --port COM13 --baud 115200` (CDC USB, 8N1) in their own terminal and reads the single-line `ts= evt= radio= key=val` records (V8.2-SPEC §13).

> **Log-string fidelity notes (verified in source, not guessed):**
> - Every `evt=` line is one `blogf()` record: `ts=<ms> evt=<TAG> radio=<R1|R2> key=val ...`. `<ms>` is free-running `millis()` — **match field names/values, not the number**. Floats (`rssi`/`snr`) use `%.1f`; hex `devaddr`/`hout` use `%08lx` → **lowercase**; `fport=%d` prints **-1** when absent.
> - The structured `radio=` tag is `R1`/`R2` (`kTag`, main.cpp:139). The **boot "ready" line** uses the radio object `_name` → `[Radio1-B2B]` / `[Radio2-Edge]` (WioSX1262.cpp:70). The **legacy decoder dump** uses `[R1 decoded]`/`[R2 decoded]` and is `[%8lu ms]` right-justified (MeshDecoderDebug.h:1018-1037).
> - The boot banner is literally `(v8.2)` even on v8.3-dev (main.cpp:1150) — expected, not a flash error.
> - The legacy decoder dump is **not under the log mutex** (V8.2-SPEC §15.2) — it can interleave across cores. **Assert only on `evt=` lines**; treat decoder dumps as informational.
> - The firmware **drains and ignores all serial input** during operation (main.cpp:1200) — there is no serial frame-injection hook. **All stimulus frames must go over the air.**

---

## A. Equipment & stimulus generators

| Item | Purpose |
|---|---|
| **DUT** — XIAO ESP32-S3 + dual Wio-SX1262, flashed v8.3-dev (`pio run -e xiao_esp32s3 -t upload`), on COM13 | Device under test |
| **Host PC** running `pio device monitor --port COM13 --baud 115200` in owner's terminal | Read the serial log |
| **WiFi laptop/phone** | Reach the DUT captive portal at `http://192.168.4.1` to set per-radio protocols |
| **STIMULUS S1** — a 2nd SX1262 board (spare Xiao+Wio / Heltec / T-Watch S3 / LilyGO) running a minimal RadioLib 7.7.0 transmit sketch | Generates hand-built 0x34 LoRaWAN frames AND 0x42 RNS frames. Must match the DUT radio's exact **freq/BW/SF/CR** and call `setSyncWord(0x34)` (or `0x42`), CRC on, preamble 8. Generator call: `radio.begin(freq, bw, sf, cr, sync, txdbm, 8); radio.transmit(frame, len);` |
| **Real Meshtastic node** (T-Watch/Heltec/T3S3) on the bridge MT channel/freq | MT↔MC + position-clock regression stimulus |
| **Real MeshCore node** on the bridge MC channel/freq | MC↔MT regression stimulus |
| *(optional)* 2nd MT node, or a 3rd RX radio | Same-channel raw-repeat (R5) / verify byte-identical relay output |
| *(optional, strengthens — see §E)* a **real LoRaWAN end-device** (RAK/Heltec/RFM95-LMIC ABP) on the DUT's 0x34 channel | Proves real-frame compatibility beyond hand-built frames |
| *(optional, strengthens LW-FLOOD)* a **2nd v8.3 bridge** on the same channels | True multi-bridge flood demo |

**Antennas on every radio; keep stimulus ~1–3 m from the DUT** to avoid front-end overload.

### Canonical hand-built LoRaWAN frames (decode-verified against `extractLoRaWANMeta`, MeshDecoderDebug.h:978-1012)

| Name | Bytes (hex) | len | Decodes to |
|---|---|---|---|
| **FRAME-U** (UnconfDataUp) | `40 8A 1F 01 26 00 01 00 02 AA BB 11 22 33 44` | 15 | `mtype=UnconfDataUp devaddr=0x26011f8a fcnt=1 fport=2` |
| **FRAME-U2** (FCnt bumped) | `40 8A 1F 01 26 00 02 00 02 AA BB 11 22 33 44` | 15 | `mtype=UnconfDataUp devaddr=0x26011f8a fcnt=2 fport=2` (distinct hash) |
| **FRAME-D** (ConfDataUp, no FPort) | `80 8A 1F 01 26 00 07 00 99 88 77 66` | 12 | `mtype=ConfDataUp devaddr=0x26011f8a fcnt=7 fport=-1` |
| **FRAME-J** (JoinRequest) | `00 01 00 00 00 00 00 00 00 3C 2B 1A 00 0B A3 04 00 55 66 DE AD BE EF` | 23 | `mtype=JoinRequest`; DevEUI `0004a30b001a2b3c`, JoinEUI `0000000000000001` |
| **FRAME-C** (too short → parse=fail) | `40 8A 1F 01 26 00 01 00 02 99 88` | 11 | `parse=fail` (data MType, <12 B) |

DevAddr/FCnt/EUIs are **little-endian on the wire**; send the bytes verbatim. To re-trigger a frame inside the 60 s dedup TTL, bump one byte (e.g. FCnt) to change the whole-frame hash.

---

## B. Captive-portal radio config profiles

Enter the portal: on first flash it auto-opens; otherwise within 5 s of boot press BOOT **or send any serial char** (main.cpp:1185-1203), then browse `http://192.168.4.1`. To set a radio LoRaWAN: pick **"LoRaWAN"** in its Protocol dropdown (CaptivePortal.cpp:147) and fill BW/SF/CR + freq; the portal **forces sync=0x34 and clears the channel key** (CaptivePortal.cpp:519-523). Channel name is a cosmetic label. Save → `ESP.restart()`.

| Profile | Radio 1 | Radio 2 | Used by |
|---|---|---|---|
| **P-DEFAULT** (factory) | MT, 906.875 / BW250 / SF11 / CR5, sync 0x2B | MC, 910.525 / BW62.5 / SF7 / CR5, sync 0x12 | R0–R9 regression, C1 |
| **P-RNS2** | RNS, 914.875 / BW250 / SF11 / CR5, sync 0x42 | RNS, 906.875 / BW250 / SF11 / CR5, sync 0x42 | C2 (RNS↔RNS) |
| **P-LW+MT** | LoRaWAN, 904.6 / BW125 / SF7 / CR5, sync 0x34 | MT, 906.875 / BW250 / SF11 / CR5, sync 0x2B | LW-RX, LW-DATA, LW-JOIN, LW-FAIL, LW-SUMMARY, LW-CAP0, LW-SUM0, LW-MT→LW |
| **P-LW2** | LoRaWAN, 903.9 / BW125 / SF7 / CR5, sync 0x34 | LoRaWAN, 904.3 / BW125 / SF7 / CR5, sync 0x34 | LW-RELAY, LW-LOOP, LW-FLOOD, LW-RELAY0 |

> Two LoRaWAN radios (P-LW2) — even same-freq — **pass the portal self-bridge guard**, which fires only for MT/MC (CaptivePortal.cpp:611-625). Use different freqs anyway so stimulus and relay output don't collide.
> Stimulus S1 must always be programmed to the **exact** freq/BW/SF/CR of the DUT radio it targets, plus the matching sync (0x34 or 0x42).

---

## C. Ordered test matrix

| # | ID | Name | Priority | Area | Profile |
|---|---|---|---|---|---|
| 1 | **SETUP-01** | Boot banner + portal + LoRaWAN dropdown | must | bring-up | — |
| 2 | **SETUP-02** | Boot diagnostics: SPI/BUSY/raw probe | must | bring-up | P-DEFAULT |
| 3 | **SETUP-03** | Radio init + ready line (RF + sync) | must | bring-up | P-DEFAULT |
| 4 | **R0** | Dual-radio init + RouteQueue alloc | must | regression | P-DEFAULT |
| 5 | **LW-RX** | **GATING:** 0x34 radio RXes a hand-built frame | must | LoRaWAN | P-LW+MT |
| 6 | **C1** | POSITION clock-learn (cold-boot `evt=CLOCK`) | must | carryover | P-DEFAULT |
| 7 | **C2** | RNS↔RNS raw repeat + `rns-dup` + toggle | must | carryover | P-RNS2 |
| 8 | **LW-DATA** | Capture tap: UnconfDataUp / ConfDataUp / fport=-1 | must | LoRaWAN | P-LW+MT |
| 9 | **LW-JOIN** | Capture tap: JoinRequest (DevEUI) | must | LoRaWAN | P-LW+MT |
| 10 | **LW-FAIL** | Capture tap: `parse=fail` (short frame) | must | LoRaWAN | P-LW+MT |
| 11 | **LW-SUMMARY** | Summary-to-mesh onto MT dest (`virtualid=!b16b00b5`) | must | LoRaWAN | P-LW+MT |
| 12 | **LW-LOOP** | Dedup: identical frame → `drop=lw-dup`; changed passes | must | LoRaWAN | P-LW2 |
| 13 | **LW-RELAY** | Transparent LW→LW raw relay (`mode=raw`) | should | LoRaWAN | P-LW2 |
| 14 | **LW-FLOOD** | Dedup-bounded flood: distinct relay / re-inject dedup / TTL expiry | should | LoRaWAN | P-LW2 |
| 15 | **LW-MT→LW** | MT/MC → LoRaWAN refused (`drop=no-lw-encoder`) | must | LoRaWAN | P-LW+MT |
| 16 | **LW-CAP0** | `BRIDGE_LW_CAPTURE=0` — capture line gone, summary stays | should | LoRaWAN (rebuild) | P-LW+MT |
| 17 | **LW-SUM0** | `BRIDGE_LW_SUMMARY_TO_MESH=0` — capture stays, no summary | should | LoRaWAN (rebuild) | P-LW+MT |
| 18 | **LW-RELAY0** | `BRIDGE_LW_RELAY=0` — capture stays, no raw repeat | optional | LoRaWAN (rebuild) | P-LW2 |
| 19 | **R1** | MT→MC text + `@MT` name prefix | must | regression | P-DEFAULT |
| 20 | **R2** | MC→MT virtual node + synthetic NodeInfo | must | regression | P-DEFAULT |
| 21 | **R3** | Loop/echo suppression both directions (`loop-dup`) | must | regression | P-DEFAULT |
| 22 | **R4** | Position/telemetry text + NodeInfo ingest + self-echo | must | regression | P-DEFAULT |
| 23 | **R5** | Same-channel raw repeat + `hop0` | should | regression | 2× MT |
| 24 | **R6** | CAD listen-before-talk backoff | should | regression | P-DEFAULT |
| 25 | **R7** | Airtime throttle paces a burst | should | regression | P-DEFAULT |
| 26 | **R8** | RX-priority: long TX, burst RX on co-radio | should | regression | P-DEFAULT |
| 27 | **R9** | Do-no-harm: MT/MC never enters the LW branch | must | regression | P-DEFAULT |

**Gating order:** SETUP-01→03 → R0 establish the board boots and radios init. **LW-RX gates all LoRaWAN tests** (if a 0x34 radio cannot RX, skip LW-*). If any `evt=RX` never fires on known-good MT/MC traffic, **stop** and fix RX before the regression block. The rebuild tests (LW-CAP0/SUM0/RELAY0) are batchable into one extra-build pass or deferred if doing a single-build bench.

---

## D. Per-test detail

### SETUP-01 — Boot banner + portal + LoRaWAN dropdown · must
**Objective:** v8.3-dev runs, reaches the portal, offers LoRaWAN.
**Steps:** Reset DUT; if configured, send any char within ~5 s to force the portal. Open `http://192.168.4.1`; check the Protocol dropdown.
**Expected log:**
```
=== XIAO ESP32S3 Dual SX1262 Cross-Protocol Bridge (v8.2) ===
[setup] press BOOT — or send any character over serial — within 5 s to enter the config portal...
```
**Pass:** Banner verbatim (`(v8.2)` expected on v8.3-dev); the Protocol dropdown lists **LoRaWAN** (CaptivePortal.cpp:147).
**Fail implicates:** No banner → wrong baud/port. No LoRaWAN option → wrong/stale flash; re-upload v8.3-dev.

### SETUP-02 — Boot diagnostics: SPI/BUSY/raw probe · must · P-DEFAULT
**Objective:** Both SX1262 present, SPI healthy before RX.
**Steps:** Reset to Bridge active (don't enter portal). Read `[diag]` lines.
**Expected log (values vary):**
```
[diag] R1  BUSY after reset = 0  (<N> ms)  OK
[diag] R1 raw SPI GetStatus = 0x<NN>  (0x00/0xFF = MISO open; 0x20-0x2E = chip alive)
[diag] R1 reg 0x0320 raw: <NN NN NN NN NN NN>  = 'SX1262'
```
**Pass:** busyWait `OK` (main.cpp:1284), GetStatus in 0x20–0x2E (1302), version reads `SX1262` (1316).
**Fail implicates:** `STUCK-HIGH` → wiring/power. `0x00`/`0xFF` → MISO float / CS contention. (R2 has no raw probe — only the busyWait line.)

### SETUP-03 — Radio init + ready line · must · P-DEFAULT
**Objective:** `begin()` succeeds; resolved RF + sync printed per radio.
**Expected log (RF echoes config):**
```
Found SX126x: RADIOLIB_SX126X_REG_VERSION_STRING:
[Radio1-B2B] ready — 906.875 MHz  BW 250.0 kHz  SF11  CR4/5  20 dBm  sync 0x2B
[Radio2-Edge] ready — 910.525 MHz  BW 62.5 kHz  SF7  CR4/5  20 dBm  sync 0x12
Bridge active.
```
**Pass:** Each enabled radio prints one `ready` line (WioSX1262.cpp:70) with the configured sync. Note the **em-dash** `ready — ` and the prefixes `[Radio1-B2B]`/`[Radio2-Edge]`.
**Fail implicates:** `begin() failed: -2` / `SX126x not found!` → SPI/chip. Wrong sync → portal saved wrong protocol.

### R0 — Dual-radio init + RouteQueue alloc · must · P-DEFAULT
**Objective:** Routing machinery allocates as in v8.2.
**Expected log:**
```
[RouteQueue:R1] depth=64 (<N> B in PSRAM) maxAge=30000 ms
[RouteQueue:R2] depth=64 (<N> B in PSRAM) maxAge=30000 ms
Bridge active.
```
**Pass:** Both alloc lines `depth=64 / maxAge=30000` (PSRAM **or** `internal RAM` fallback OK, RouteQueue.cpp:28); `Bridge active.` reached; no `FATAL: Radio1 init failed`, no `[WARN] Radio2 init failed`.
**Fail implicates:** FATAL R1 → wiring (cross-check SETUP-02). `alloc … FAILED` → out of heap.

---

### LW-RX — GATING: LoRaWAN-configured 0x34 radio RXes a hand-built frame · must · P-LW+MT
**Objective:** Prove a `protocol=LoRaWAN` radio programs the public sync (0x34→reg 0x3444) and demodulates a 0x34 frame. **Gates the entire feature.**
**Why it must work (verified in source):** `WioSX1262::begin` (WioSX1262.cpp:53-62) passes `syncWord=0x34` into `SX1262::begin` → `SX126x::begin` calls `setSyncWord(0x34)` with default `controlBits=0x44` (SX126x.cpp:47, SX126x.h:415) → `setSyncWord` writes `data[0]=0x34, data[1]=0x44` (SX126x_config.cpp:146) = register **0x3444 = LoRaWAN public sync**. LW-RX confirms this on air.
**Prereqs:** Profile P-LW+MT. Confirm R1 `ready` shows `sync 0x34`. S1 at the identical RF, sync 0x34.
**Steps:** Reset DUT; confirm boot `[BridgeConfig] … radio1 RF = proto=5 sync=0x34 …` and the R1 `ready … sync 0x34` line. S1 transmits **FRAME-U** once.
**Expected log (rssi/snr are real floats):**
```
[Radio1-B2B] ready — 904.600 MHz  BW 125.0 kHz  SF7  CR4/5  20 dBm  sync 0x34
ts=<ms> evt=RX radio=R1 proto=LW len=15 rssi=<-NN.N> snr=<N.N>
ts=<ms> evt=RX radio=R1 proto=LW mtype=UnconfDataUp devaddr=0x26011f8a fcnt=1 fport=2 len=15
```
**Pass:** A generic `evt=RX … proto=LW len=15` line (main.cpp:1041) AND the structured capture line (main.cpp:783-788) appear per TX, with plausible rssi (≈ -30…-120).
**Fail implicates:** **No `evt=RX` at all** → rule out RF-plan mismatch first (freq/BW/SF/CR must match S1 *exactly* — LoRa won't decode across a mismatch), then stimulus antenna/power, then that S1 truly sends sync 0x34. *Isolation:* temporarily set R1=MeshCore (0x12) and send a 0x12 frame from S1 — if that RXes but 0x34 doesn't, the sync mapping is at fault (recheck WioSX1262.cpp:53-62 forwarded the byte and RadioLib is 7.7.0). Per code the mapping is correct, so an air-only failure points to RF/stimulus. `proto=LW parse=fail len=15` → frame received but byte layout wrong (resend FRAME-U verbatim, LE intact).

---

### C1 — POSITION clock-learn · must · P-DEFAULT
**Objective:** A cold-booted bridge (`g_clockUnix==0`) hearing an MT `POSITION_APP` with a real Unix time emits exactly **one** `evt=CLOCK src=MT`, and the next MT→MC QUEUE shows a non-zero `mcts=`. Verify the coupling to `positionEnabled()`.
**Prereqs:** P-DEFAULT; `positionEnabled = 1` (default; confirm at boot). **The bridge must NOT have heard any timestamped MeshCore or prior MT POSITION since boot** (else `g_clockUnix` is already set, no `evt=CLOCK` fires) — keep the MC side silent. Stimulus = a **real Meshtastic node** broadcasting POSITION carrying BOTH a coordinate AND a time field (extractMeshtasticPosition returns on lat||lon||alt and requires `buf[13]==channelHash`; `learnClockFromMt` only runs when `pos.hasTime`, main.cpp:873-879). *A real GPS node is strongly preferred — hand-building an AES-CTR POSITION protobuf is error-prone.*
**Steps:** Power-cycle (guarantees `g_clockUnix==0`). Confirm `[BridgeConfig] … positionEnabled  = 1`, `radio1 RF = proto=1 sync=0x2B …`, `radio2 RF = proto=2 sync=0x12 …`, `Bridge active.`. Trigger the position broadcast. Then send an MT TEXT so the bridge re-encodes MT→MC; read the QUEUE `mcts=`. Send a 2nd POSITION → confirm NO new `evt=CLOCK`. *(Coupling sub-test:* portal-disable position, cold-boot, repeat → no `evt=CLOCK src=MT`, no `pos` body; restore after.)
**Expected log:**
```
[BridgeConfig] ... positionEnabled  = 1
ts=<ms> evt=RX radio=R1 proto=MT len=<N> rssi=<f> snr=<f>
ts=<ms> evt=CLOCK src=MT unix=<U> (calibrated — MC TX now timestamped)
ts=<ms> evt=DEDUP_PASS radio=R1 proto=MT nodeid=!<srcid> hin=0x<8hex> msg="pos <lat>,<lon> ..."
ts=<ms> evt=QUEUE radio=R1 dst=R2 dstproto=MC len=<N> virtualid=- hout=0x<8hex> qdepth=<n> qdropped=<n> mcts=<NONZERO> msg="<who>@MT: ..."
```
**Pass:** (a) exactly one `evt=CLOCK src=MT unix=<U>`, U > 1500000000 (main.cpp:283-289); (b) next MT→MC `dstproto=MC` line shows `mcts` ≈ U + elapsed, **not 0**; (c) a 2nd POSITION yields no further `evt=CLOCK`; (d) with position disabled, no `evt=CLOCK src=MT` and no `pos` body.
**Fail implicates:** No `evt=CLOCK` → `g_clockUnix` already set (re-cold-boot, MC silent), POSITION had no f4/f7 time, or position disabled. `mcts=0` despite a CLOCK line → `bridgeNowUnix()` returned 0 (escalate). POSITION RX but no `pos` DEDUP_PASS → `buf[13] != channelHash` (channel/PSK/region mismatch).

### C2 — RNS↔RNS raw repeat + `rns-dup` + toggle · must · P-RNS2
**Objective:** With both radios RNS (sync 0x42), a frame on R1 is raw-repeated byte-for-byte on R2 (`mode=raw`), a repeat is dropped `rns-dup`, and `BRIDGE_RNS_INPROTO_REPEAT=0` disables the repeat. RNS frames are **not decrypted**, so any ≥1-byte 0x42 payload from S1 works — **no real RNS device required**.
**Prereqs:** P-RNS2 (portal forces RNS to BW250/SF11/CR5; put R1/R2 on different freqs). S1 on R1's freq, sync 0x42, BW250/SF11/CR5, arbitrary payload.
**Steps:** Confirm boot `radio1 RF = proto=3 sync=0x42 … 914.875 MHz BW250.0 SF11 CR5` and `radio2 RF = proto=3 sync=0x42 … 906.875 MHz …`. Send one unique RNS frame into R1; watch RX + raw-repeat QUEUE + R2 TX. Re-send the identical frame within 60 s → `rns-dup`. *(Toggle:* rebuild with `-DBRIDGE_RNS_INPROTO_REPEAT=0`, repeat → RX line but no `mode=raw` QUEUE.)*
**Expected log:**
```
ts=<ms> evt=RX radio=R1 proto=RNS len=<N> rssi=<f> snr=<f>
ts=<ms> evt=QUEUE radio=R1 dst=R2 mode=raw len=<N> virtualid=- qdepth=<n> qdropped=<n>
ts=<ms> evt=TX_START radio=R2 cad=clear len=<N> rc=0
ts=<ms> evt=TX_DONE radio=R2 result=done
ts=<ms> evt=DROP radio=R1 proto=RNS drop=rns-dup
```
**Pass:** (a) `evt=QUEUE … dst=R2 mode=raw len=<N> virtualid=-` (srcId=0 → `-`, main.cpp:720-724) with len = received len, then R2 `TX_START`/`TX_DONE`; (b) identical re-send → exactly one `drop=rns-dup` (main.cpp:744) and no new `mode=raw` QUEUE; (c) with the toggle off, RX but no `mode=raw`, no R2 TX.
**Fail implicates:** No RX → sync/BW/SF/CR/freq mismatch. RX but no `mode=raw` → `BRIDGE_RNS_INPROTO_REPEAT=0` (stale build) or R2 not 0x42. A `dstproto=` (non-raw) QUEUE → the RNS→RNS branch was bypassed (check `g_chan[j].protocol==0x42` at dest, main.cpp:758-759). Note debugDump shows **`proto=3`** for Reticulum (enum `PROTO_RNS=3`), not 4.

---

### LW-DATA — Capture tap header decode · must · P-LW+MT
**Objective:** The capture tap (BRIDGE_LW_CAPTURE=1) emits exact `mtype/devaddr/fcnt/fport/len` for data frames, including the `fport=-1` (no-FPort) path.
**Prereqs:** LW-RX passed. R2=MT enabled (so a non-LW dest exists for later tests; here only the capture line is asserted).
**Steps:** Transmit **FRAME-U** once; read the burst. Then transmit **FRAME-D** once (ConfDataUp, no FPort).
**Expected log (FRAME-U):**
```
ts=<ms> evt=RX radio=R1 proto=LW len=15 rssi=<f> snr=<f>
[   <ms> ms][R1 decoded] LoRaWAN UnconfDataUp DevAddr=0x26011f8a FCtrl=0x00 FCnt=1 FPort=2 FRMlen=2 len=15   (informational; may interleave)
ts=<ms> evt=RX radio=R1 proto=LW mtype=UnconfDataUp devaddr=0x26011f8a fcnt=1 fport=2 len=15
```
**Expected log (FRAME-D):**
```
ts=<ms> evt=RX radio=R1 proto=LW mtype=ConfDataUp devaddr=0x26011f8a fcnt=7 fport=-1 len=12
```
**Pass:** Each structured capture line matches token-for-token (main.cpp:783-788): lowercase hex devaddr, decimal fcnt, fport (`-1` when absent), len. FRAME-D shows `fport=-1` (afterFhdr<1, MeshDecoderDebug.h:996).
**Fail implicates:** Byte-swapped devaddr/fcnt → stimulus packed big-endian (FHDR is LE). `parse=fail` on a 12-B frame → it was actually <12 B. Structured line absent but generic RX present → `BRIDGE_LW_CAPTURE` compiled 0 (see LW-CAP0). `fport=0` on FRAME-D → an extra byte slipped in; trim to exactly 12.

### LW-JOIN — JoinRequest decode · must · P-LW+MT
**Objective:** A 23-B MType-0 frame parses as JoinRequest; the capture line shows the data-field defaults for a join.
**Steps:** Transmit **FRAME-J** once.
**Expected log:**
```
[   <ms> ms][R1 decoded] LoRaWAN JoinRequest JoinEUI=0000000000000001 DevEUI=0004a30b001a2b3c len=23   (informational)
ts=<ms> evt=RX radio=R1 proto=LW mtype=JoinRequest devaddr=0x00000000 fcnt=0 fport=-1 len=23
```
**Pass:** Capture line exactly `mtype=JoinRequest devaddr=0x00000000 fcnt=0 fport=-1 len=23` (joins leave data fields at struct defaults, main.cpp:783-788). DevEUI is surfaced in the summary (LW-SUMMARY), not the capture line.
**Fail implicates:** `mtype` wrong → MHDR top 3 bits not 0 (must be `0x00`). `parse=fail len=23` → join branch needs len≥23 AND MType==0 (MeshDecoderDebug.h:1005). DevEUI byte-reversed in the dump → stimulus packed it big-endian (DevEUI is LE).

### LW-FAIL — parse=fail path · must · P-LW+MT
**Objective:** A data-MType frame shorter than the 12-B minimum logs `parse=fail` and produces no summary.
**Steps:** Transmit **FRAME-C** (11 B) once.
**Expected log:**
```
ts=<ms> evt=RX radio=R1 proto=LW len=11 rssi=<f> snr=<f>
[   <ms> ms][R1 decoded] LoRaWAN (sync 0x34) unparseable len=11   (informational)
ts=<ms> evt=RX radio=R1 proto=LW parse=fail len=11
```
**Pass:** Exactly one `evt=RX … proto=LW parse=fail len=11` (main.cpp:790-791), no `mtype/devaddr` fields, and **no** follow-on `evt=QUEUE … msg="LoRaWAN …"` (summary is gated on `parsed==true`, main.cpp:794).
**Fail implicates:** A normal capture line instead → frame was ≥12 B (trim to 11). A summary QUEUE after `parse=fail` → the parsed-gate regressed.

### LW-SUMMARY — Summary-to-mesh onto MT dest · must · P-LW+MT
**Objective:** BRIDGE_LW_SUMMARY_TO_MESH=1 builds the exact summary string and enqueues it via `enqueueTextForDest` (srcId=0) onto the MT dest, stamped with the bridge identity. Because the source protocol is LoRaWAN (neither MT nor MC), the identity-rewrite branches are skipped; the MT encode defaults `dstSrcId = mtNodeId() = 0xB16B00B5` → `virtualid=!b16b00b5`.
**Prereqs:** LW-RX passed; R2=MT enabled. *(A real MT client on R2's channel confirms reception but the QUEUE line alone proves encode+enqueue.)*
**Steps:** Transmit **FRAME-U** (data) → read capture then the MT QUEUE. Transmit **FRAME-J** (join) → read the join summary (bump a byte if within 60 s of a prior identical send).
**Expected log (FRAME-U):**
```
ts=<ms> evt=RX radio=R1 proto=LW mtype=UnconfDataUp devaddr=0x26011f8a fcnt=1 fport=2 len=15
ts=<ms> evt=QUEUE radio=R1 dst=R2 dstproto=MT len=<n> virtualid=!b16b00b5 hout=0x<8hex> qdepth=<n> qdropped=<n> mcts=0 msg="LoRaWAN UnconfDataUp DevAddr 0x26011f8a FCnt 1 FPort 2 len 15"
```
**Expected log (FRAME-J):**
```
ts=<ms> evt=QUEUE radio=R1 dst=R2 dstproto=MT len=<n> virtualid=!b16b00b5 hout=0x<8hex> qdepth=<n> qdropped=<n> mcts=0 msg="LoRaWAN JoinReq DevEUI 0004a30b001a2b3c"
```
**Pass:** Per frame, a `dstproto=MT virtualid=!b16b00b5` QUEUE whose `msg=` matches the snprintf format exactly (main.cpp:797-808): data → `LoRaWAN <MType> DevAddr 0x<8lowerhex> FCnt <u> FPort <d> len <u>`; join → `LoRaWAN JoinReq DevEUI <16lowerhex>` (uses `lw.devEui`). `mcts=0` is normal on MT dest. *(For an MC dest instead: identical `msg=`, but `dstproto=MC virtualid=-` and `mcts=<unix-or-0>`.)*
**Fail implicates:** No QUEUE after a valid capture → `BRIDGE_LW_SUMMARY_TO_MESH=0` (see LW-SUM0) or no MT/MC dest enabled (fan-out targets MT/MC only, main.cpp:813-814). Wrong `msg` text → note the summary uses the **spaced** form (`DevAddr 0x… FCnt … FPort … len …`), not the capture line's `devaddr=/fcnt=` tokens. `virtualid != !b16b00b5` → the bridge MT node id was changed in the portal.

### LW-LOOP — Dedup loop-safety · must · P-LW2
**Objective:** The raw-frame hash dedup (main.cpp:774) drops a byte-identical frame within the 60 s TTL; a changed byte passes.
**Steps:** Send **FRAME-U** → full capture (+relay, since P-LW2 — see LW-RELAY). Within ~5 s send the **identical** FRAME-U → expect `drop=lw-dup`, no capture, no QUEUE. Send **FRAME-U2** → fresh capture (`fcnt=2`).
**Expected log (duplicate):**
```
ts=<ms> evt=RX radio=R1 proto=LW len=15 rssi=<f> snr=<f>
ts=<ms> evt=DROP radio=R1 proto=LW drop=lw-dup
```
**Pass:** The identical re-send yields `evt=DROP radio=R1 proto=LW drop=lw-dup` (main.cpp:775) with no capture/QUEUE; FRAME-U2 yields a normal capture (`fcnt=2`).
**Fail implicates:** Duplicate not dropped → sends >60 s apart (TTL expired) or bytes actually differed. Even FRAME-U2 dropped → hash collision / TTL mis-sized. Dedup key is `hash(buf,len,0)` over the whole raw frame, so any byte change defeats it.

### LW-RELAY — Transparent LW→LW raw relay · should · P-LW2
**Objective:** A 0x34 frame on R1 is re-transmitted byte-for-byte on R2 (different freq) via `rawRepeatForDest` (`mode=raw`). With two LW radios there is **no MT/MC dest**, so **no summary QUEUE** appears.
**Prereqs:** LW-RX passed. P-LW2; S1 on R1's freq. *(To verify byte-identical output, capture R2's emission on a 3rd RX radio; at minimum verify the QUEUE+TX chain.)*
**Steps:** Transmit a fresh **FRAME-U** on R1's freq. Watch R1 capture, the raw-repeat QUEUE to R2, then R2 TX.
**Expected log:**
```
ts=<ms> evt=RX radio=R1 proto=LW mtype=UnconfDataUp devaddr=0x26011f8a fcnt=1 fport=2 len=15
ts=<ms> evt=QUEUE radio=R1 dst=R2 mode=raw len=15 virtualid=- qdepth=<n> qdropped=<n>
ts=<ms> evt=TX_START radio=R2 cad=clear len=15 rc=0
ts=<ms> evt=TX_DONE radio=R2 result=done
```
**Pass:** `evt=QUEUE radio=R1 dst=R2 mode=raw len=15 virtualid=-` (srcId=0, main.cpp:719-724), then R2 `TX_START len=15` + `TX_DONE result=done`. If captured on a 3rd radio, the 15 bytes equal FRAME-U verbatim (LW dest is not Meshtastic → no byte mutation, main.cpp:704-716). **No** summary QUEUE, **no** `no-lw-encoder` drop.
**Fail implicates:** No `mode=raw` QUEUE → R2 not 0x34 (main.cpp:823) or `BRIDGE_LW_RELAY=0` (see LW-RELAY0). No `TX_START` → CAD perpetually busy (jammed band) or queue stuck. `drop=tx-startfail rc=<n>` → R2 `startTransmit` failed (RF/SPI). TX len ≠ 15 → raw-repeat regression.

### LW-FLOOD — Dedup-bounded flood · should · P-LW2
**Objective:** Each unique 0x34 frame relays exactly once; a distinct frame relays independently; a re-inject within TTL is deduped; the same frame passes after TTL expiry.
**Steps:** Send **FRAME-U** → one relay. Immediately send **FRAME-U2** → a separate relay. Within 60 s re-send FRAME-U → `drop=lw-dup`. Wait >60 s with NO FRAME-U traffic, re-send FRAME-U → relays again.
**Expected log (sequence):**
```
FRAME-U : evt=RX … mtype=UnconfDataUp … fcnt=1 …  +  evt=QUEUE radio=R1 dst=R2 mode=raw len=15 …  +  R2 TX_START/TX_DONE
FRAME-U2: evt=RX … fcnt=2 …  +  a SEPARATE evt=QUEUE … mode=raw …  +  R2 TX_START/TX_DONE
FRAME-U re-inject <60 s: evt=DROP radio=R1 proto=LW drop=lw-dup   (only)
FRAME-U after >60 s idle: fresh evt=QUEUE … mode=raw …  +  R2 TX
```
**Pass:** Two distinct frames → two relays; one frame twice within 60 s → one relay + one `lw-dup`; same frame after >60 s → relays again.
**Fail implicates:** FRAME-U2 deduped vs FRAME-U → frames not actually distinct. Re-inject not deduped within 60 s → TTL expired or frames differ. Post-60 s still deduped → the entry was **refreshed** by intervening identical traffic (`insert()` refreshes ts on every hit, DedupCache.cpp:63-65) — ensure a full 60 s clear of that frame. *Multi-bridge:* with a 2nd v8.3 bridge on the same channels, bridge1 relays once, bridge2 hears + relays once, each drops the other's echo via its own DedupCache — flood terminates after one hop per bridge (no in-band TTL).

### LW-MT→LW — MT/MC → LoRaWAN refused · must · P-LW+MT
**Objective:** An MT/MC text destined to the LoRaWAN radio is log-and-dropped `no-lw-encoder`; nothing is transmitted on 0x34. This is the only `no-lw-encoder` emission and synthesizes no LoRaWAN frame.
**Prereqs:** R1=LoRaWAN, R2=MT (P-LW+MT). Send a text from a real MT node (or a 2nd SX1262 sending a valid 0x2B text) on R2's channel.
**Steps:** Send a short text (e.g. `dlw1`) the bridge receives on R2; watch R2 ingest then the drop toward R1.
**Expected log:**
```
ts=<ms> evt=RX radio=R2 proto=MT len=<n> rssi=<f> snr=<f>
ts=<ms> evt=DEDUP_PASS radio=R2 proto=MT nodeid=!<8hex> hin=0x<8hex> msg="dlw1"
ts=<ms> evt=DROP radio=R2 dst=R1 drop=no-lw-encoder msg="dlw1"
```
**Pass:** Exactly one `evt=DROP radio=R2 dst=R1 drop=no-lw-encoder msg="<text>"` (main.cpp:565-567); zero `QUEUE`/`TX_START`/`CAD`/`THROTTLE` for R1 attributable to it. *(Optional: a 0x34 listener on R1's freq hears nothing.)*
**Fail implicates:** A QUEUE/TX toward R1 → the LoRaWAN-dest guard regressed. `drop=bad-proto` instead → R1 not resolved as 0x34 (check portal saved LoRaWAN; `proto=5 sync=0x34`). Nothing on R2 → MT stimulus didn't decode (channel/key/RF mismatch — separate from this test).

### LW-CAP0 — `BRIDGE_LW_CAPTURE=0` · should · P-LW+MT (rebuild)
**Objective:** CAPTURE gates **only** the `evt=RX … proto=LW mtype=…` line. The summary still fires because `extractLoRaWANMeta` runs unconditionally (main.cpp:779-780, outside the `#if`).
**Prereqs:** Add `-DBRIDGE_LW_CAPTURE=0` to platformio.ini build_flags (the `#ifndef` lets `-D` win); `pio run -t clean` then upload. P-LW+MT.
**Steps:** Transmit a fresh **FRAME-U** (or FRAME-U2 to dodge dedup).
**Expected log:**
```
ts=<ms> evt=RX radio=R1 proto=LW len=15 rssi=<f> snr=<f>
ts=<ms> evt=QUEUE radio=R1 dst=R2 dstproto=MT len=<n> virtualid=!b16b00b5 hout=0x<8hex> qdepth=<n> qdropped=<n> mcts=0 msg="LoRaWAN UnconfDataUp DevAddr 0x26011f8a FCnt 2 FPort 2 len 15"
```
**Pass:** The `mtype/devaddr/fcnt/fport` capture line is **absent**; the generic `evt=RX … len= rssi= snr=` line (radioTask, not gated) is **present**; the summary QUEUE is **present**. Exactly one fewer line per LW RX vs LW-DATA.
**Fail implicates:** Capture line still present → flag didn't take (stale build; clean rebuild). Summary also gone → SUMMARY_TO_MESH also zeroed, or dedup blocked the frame (use a new FCnt). The surviving RX line has `len/rssi/snr`; the suppressed one has `mtype/devaddr/fcnt/fport`.

### LW-SUM0 — `BRIDGE_LW_SUMMARY_TO_MESH=0` · should · P-LW+MT (rebuild)
**Objective:** SUMMARY gates the metadata-summary enqueue. Capture still logs; no summary onto the mesh.
**Prereqs:** Rebuild with `-DBRIDGE_LW_SUMMARY_TO_MESH=0` (CAPTURE/RELAY default 1). P-LW+MT.
**Steps:** Transmit a fresh **FRAME-U**.
**Expected log:**
```
ts=<ms> evt=RX radio=R1 proto=LW mtype=UnconfDataUp devaddr=0x26011f8a fcnt=1 fport=2 len=15
```
**Pass:** The capture line is **present**; there is **no** `evt=QUEUE … msg="LoRaWAN …"` and no resulting R1 TX for it. (Periodic MT NodeInfo `op=mint` lines on R2 every ~5 min are unrelated.)
**Fail implicates:** A `msg="LoRaWAN …"` QUEUE still appears → flag didn't take (clean rebuild). Capture also gone → CAPTURE inadvertently disabled, or dedup blocked the frame.

### LW-RELAY0 — `BRIDGE_LW_RELAY=0` · optional · P-LW2 (rebuild)
**Objective:** RELAY gates only the raw repeat. Capture still logs; no `mode=raw` to the other 0x34 radio.
**Prereqs:** Rebuild with `-DBRIDGE_LW_RELAY=0`. P-LW2.
**Steps:** Transmit a fresh **FRAME-U** on R1's freq.
**Expected log:**
```
ts=<ms> evt=RX radio=R1 proto=LW mtype=UnconfDataUp devaddr=0x26011f8a fcnt=1 fport=2 len=15
```
**Pass:** Capture present; **zero** `mode=raw` QUEUE; **zero** R2 TX for the LW frame (the `#if BRIDGE_LW_RELAY` block, main.cpp:819-826, is compiled out). Re-flashing default (=1) restores LW-RELAY.
**Fail implicates:** Relay still occurs → build didn't pick up the flag (confirm `-DBRIDGE_LW_RELAY=0` in compile output / clean rebuild). Capture also stops → wrong flag toggled (CAPTURE vs RELAY). Note dedup still records the raw frame at ingest even with relay off — a re-inject still `drop=lw-dup`.

---

### R1 — MT→MC text + `@MT` prefix · must · P-DEFAULT
**Objective:** MT text crosses to MC re-encoded with the sender-name prefix.
**Steps:** From the MT node send `hello mesh`.
**Expected log:**
```
ts=<ms> evt=RX radio=R1 proto=MT len=<n> rssi=<f> snr=<f>
ts=<ms> evt=DEDUP_PASS radio=R1 proto=MT nodeid=!<srcid> hin=0x<8hex> msg="hello mesh"
ts=<ms> evt=QUEUE radio=R1 dst=R2 dstproto=MC len=<n> virtualid=- hout=0x<8hex> qdepth=<n> qdropped=0 mcts=<unix-or-0> msg="<who>@MT: hello mesh"
ts=<ms> evt=TX_START radio=R2 cad=clear len=<n> rc=0
ts=<ms> evt=THROTTLE radio=R2 air=<a> gap=<g> nexttx=<x>
ts=<ms> evt=TX_DONE radio=R2 result=done
```
**Pass:** QUEUE `dstproto=MC`, `msg="<who>@MT: hello mesh"` (`<who>` = NodeDB short_name if a NodeInfo was heard, else `!hexid`), `virtualid=-` (main.cpp:614-666). MC node displays the prefix.
**Fail implicates:** No RX → MT path dead (run LW-RX-style RX sanity / channel match). `loop-dup` instead of DEDUP_PASS → same body recently seen (wait >60 s or change text). Missing `@MT` → `BRIDGE_TAG_ORIGIN_PROTO=0`. `drop=encode-fail` → encoder regression.

### R2 — MC→MT virtual node + synthetic NodeInfo · must · P-DEFAULT
**Objective:** A MeshCore `Name: ` group text mints a deterministic virtual MT node, advertises its NodeInfo (`<Name> @MC`), stamps it as the MT src, and strips the name.
**Steps:** From the MC node send `Alice: hi there`.
**Expected log:**
```
ts=<ms> evt=RX radio=R2 proto=MC len=<n> rssi=<f> snr=<f>
ts=<ms> evt=DEDUP_PASS radio=R2 proto=MC nodeid=- hin=0x<8hex> msg="Alice: hi there"
ts=<ms> evt=NODEINFO radio=R1 op=virtual selfid=!<vhex> long="Alice @MC"
ts=<ms> evt=QUEUE radio=R2 dst=R1 dstproto=MT len=<n> virtualid=!<vhex> hout=0x<8hex> qdepth=<n> qdropped=0 mcts=0 msg="hi there"
ts=<ms> evt=TX_START radio=R1 cad=clear len=<n> rc=0
ts=<ms> evt=TX_DONE radio=R1 result=done
```
**Pass:** `NODEINFO op=virtual long="Alice @MC"` (main.cpp:530); the QUEUE uses the **same** `virtualid=!<vhex>`, `dstproto=MT`, clean `msg="hi there"` (main.cpp:591-666). MT app shows node `Alice @MC`. *(NodeInfo enqueues only when `nodeInfoDue` — a repeat within the re-advertise period may show QUEUE without a fresh NODEINFO; still a pass for the text path.)*
**Fail implicates:** No `op=virtual` and `Alice: ` still in body → `parseMcSenderName` failed (prefix not `Name: `, >32 chars, or non-printable). `drop=encode-fail what=virt-nodeinfo` → NodeInfo encoder regression. `mcts=0` is expected on MC source / fresh boot.

### R3 — Loop/echo suppression · must · P-DEFAULT
**Objective:** The content-hash DedupCache drops an echo of an already-bridged body (no storm).
**Steps:** Bridge one MT text (as R1). Watch for the echo/flood copy back, or resend the identical body from the same source within 60 s.
**Expected log:**
```
ts=<ms> evt=DROP radio=<r> proto=<MT|MC> drop=loop-dup nodeid=<..> hin=0x<8hex> msg="<body>"
```
**Pass:** The echo yields exactly one `drop=loop-dup` (main.cpp:941) with the **same `hin`** as the original DEDUP_PASS; no further QUEUE/TX for that body; no runaway QUEUE→TX→RX cycle.
**Fail implicates:** Repeated QUEUE/TX for one body → dedup not catching the echo (a body mutated in transit changes the hash). Never seeing `loop-dup` → bridge isn't hearing its own emission (raise power / move nodes) — a stimulus limitation, not a dedup failure. A storm **is** a regression.

### R4 — Position/telemetry text + NodeInfo ingest + self-echo · must · P-DEFAULT
**Objective:** MT POSITION/TELEMETRY decode to compact text and bridge; MT NodeInfo feeds NodeDB without bridging; the bridge's own NodeInfo echo is dropped.
**Expected log:**
```
ts=<ms> evt=NODEDB radio=R1 op=upsert ni_id=!<id> ni_short="<sn>" ni_long="<ln>"
ts=<ms> evt=DEDUP_PASS radio=R1 proto=MT nodeid=!<id> hin=0x<8hex> msg="pos <lat>,<lon> alt <a>m"
ts=<ms> evt=DEDUP_PASS radio=R1 proto=MT nodeid=!<id> hin=0x<8hex> msg="bat <p>% <v>V"
ts=<ms> evt=NODEINFO radio=R1 op=mint selfid=!b16b00b5 len=<n>
ts=<ms> evt=DROP radio=R1 proto=MT drop=self-echo ni_id=!b16b00b5
```
**Pass:** Peer NodeInfo → `op=upsert` and **no** QUEUE (main.cpp:848-852). Position → DEDUP_PASS+QUEUE, `msg` starts `pos`; telemetry `msg` starts `bat`/`env` (main.cpp:880-918). Periodic self NodeInfo → `op=mint selfid=!b16b00b5` (~10 s after boot, then every 5 min). Own NodeInfo echo → `drop=self-echo ni_id=!b16b00b5` (main.cpp:843-845).
**Fail implicates:** Position/telemetry not bridged → extractor regressed or position/telemetry disabled. NodeInfo getting QUEUEd → consume-branch broke. self-echo not dropped → `niNodeId==mtNodeId` guard regressed.

### R5 — Same-channel raw repeat + `hop0` · should · 2× MT (same channel, two freqs)
**Objective:** When both radios carry the SAME channel (protocol+hash+key, different freq), an MT packet is raw-repeated (sender preserved, hop_limit−1, relay_node=us); a hop_limit=0 packet is dropped.
**Prereqs:** Portal: R1=MT LongFast freqA, R2=MT LongFast freqB, identical name+PSK. An off-bridge MT sender.
**Expected log:**
```
ts=<ms> evt=RX radio=R1 proto=MT len=<n> rssi=<f> snr=<f>
ts=<ms> evt=DEDUP_PASS radio=R1 proto=MT nodeid=!<srcid> hin=0x<8hex> msg="<body>"
ts=<ms> evt=QUEUE radio=R1 dst=R2 mode=raw len=<n> virtualid=!<srcid> qdepth=<n> qdropped=0
ts=<ms> evt=DROP radio=R1 dst=R2 drop=hop0
```
**Pass:** QUEUE shows `mode=raw` (not `dstproto=/hout=/msg=`) with the **original** `virtualid=!<srcid>` (main.cpp:960-961, 719-724). A hop_limit=0 packet → `drop=hop0` (main.cpp:708-710), no QUEUE. *(Sniffer: original src unchanged, hop_limit−1, relay_node=0xB5.)*
**Fail implicates:** `dstproto=MT` (re-encode) instead of `mode=raw` → `sameChannel()` false (channelHash/key differ — recheck identical name+PSK). No `hop0` on a hop0 packet → decrement guard regressed. Repeating our own packets → the `srcId==mtNodeId` self-guard (main.cpp:706) broke.

### R6 — CAD listen-before-talk · should · P-DEFAULT
**Objective:** When the dest channel is busy at TX time, the bridge backs off (random 20–120 ms) and stays in RX.
**Steps:** Queue a bridge emission while occupying the dest channel.
**Expected log:**
```
ts=<ms> evt=CAD radio=<dst> cad=busy backoff=<20..120>
ts=<ms> evt=TX_START radio=<dst> cad=clear len=<n> rc=0
```
**Pass:** ≥1 `evt=CAD … cad=busy backoff=N` (N∈20..120, main.cpp:1098-1103) while the channel is keyed; `TX_START` always carries `cad=clear`. Packet delivered after the channel clears (or `drop=latch-stale` if jammed >30 s).
**Fail implicates:** No CAD line + immediate TX over a busy channel → CAD regressed. backoff outside 20..120 → backoff bounds overridden.

### R7 — Airtime throttle · should · P-DEFAULT
**Objective:** After each TX a radio is held off-air `gap ≈ air × 100/DUTY` (DUTY=50 → gap ≈ 2×air).
**Steps:** Inject 3–5 messages back-to-back to the same dest.
**Expected log:**
```
ts=<t1> evt=TX_START radio=<dst> cad=clear len=<n> rc=0
ts=<t1> evt=THROTTLE radio=<dst> air=<a> gap=<g> nexttx=<t1+g>
ts=<t1+a> evt=TX_DONE radio=<dst> result=done
ts=<t2≈t1+g> evt=TX_START radio=<dst> cad=clear len=<n> rc=0
```
**Pass:** Each THROTTLE shows `gap ≈ air×2` and `nexttx = TX_START + gap` (main.cpp:1121-1129); the next TX_START for that radio is ≥ the prior `nexttx`.
**Fail implicates:** `gap == air` → DUTY=100 / throttle disabled. Back-to-back TX_START with no spacing → throttle window not honored. Implausible `air` → `estimateAirtimeMs` fed wrong SF/BW.

### R8 — RX-priority headline · should · P-DEFAULT
**Objective:** While one radio is mid-TX (deaf), the other still RXes every packet — zero RX loss.
**Steps:** Arrange a long TX on radio X (slow SF); during X's on-air window, burst N packets into radio Y; count Y's RX lines.
**Expected log:**
```
ts=<t1> evt=TX_START radio=<X> cad=clear len=<n> rc=0
ts=<t2> evt=RX radio=<Y> proto=<..> len=<..> rssi=<..> snr=<..>   (one per injected packet, t1<t2<TX_DONE)
ts=<t3> evt=TX_DONE radio=<X> result=done
```
**Pass:** Every packet injected into Y during X's TX window produces `evt=RX radio=Y` (count = N). No RX gap correlated with X's TX_START..TX_DONE.
**Fail implicates:** Missing Y RX lines exactly during X's TX → TX is blocking RX (the regression this guards). A `result=timeout-recovered` indicates the in-flight wait misbehaved. Distinguish collision loss by injecting Y packets that don't overlap X's TX and confirming all arrive.

### R9 — Do-no-harm: MT/MC never enters the LW branch · must · P-DEFAULT
**Objective:** With both radios MT/MC, the v8.3 LoRaWAN source branch and the 0x34 dest drop are never reached. `ingestAndFanout` branches on `srcChan.protocol`, so an MT/MC/RNS radio never enters the LW source branch (main.cpp:773), and `no-lw-encoder` fires only when the **dest** protocol is 0x34 (main.cpp:565).
**Steps:** Run a representative mix of MT and MC traffic (text/position/telemetry/NodeInfo) for ≥1 min; scan the whole log.
**Expected (assert by ABSENCE):**
```
(none)  proto=LW
(none)  drop=no-lw-encoder
(none)  drop=lw-dup
(present, normal)  ts=<ms> evt=RX radio=<r> proto=<MT|MC> ...  + normal QUEUE/TX per R1–R4
```
**Pass:** Zero occurrences of `proto=LW`, `drop=no-lw-encoder`, or `drop=lw-dup` while both radios are MT/MC; normal routing proceeds as R1–R4.
**Fail implicates:** Any `proto=LW` with an MT/MC radio → the LW source branch is mis-triggering (sync/protocol-resolution bug) — capture the offending RX line's radio + configured protocol. Any `drop=no-lw-encoder` → `enqueueTextForDest` saw a 0x34 dest although neither radio is LoRaWAN (config/resolve bug).

---

## E. Completeness check

**Coverage vs spec:** All three LoRaWAN code paths are exercised — capture tap (LW-RX/DATA/JOIN/FAIL), summary-to-mesh (LW-SUMMARY), LW↔LW raw relay + dedup-bounded flood (LW-RELAY/LOOP/FLOOD) — plus the MT/MC→LW refusal (LW-MT→LW), all three feature toggles (LW-CAP0/SUM0/RELAY0), the two v8.3 carry-overs (C1 POSITION clock-learn, C2 RNS↔RNS repeat), and the full v8.2 regression set (R0–R9). The gating sync-0x34 RX bring-up runs first.

**De-duplication done:** The four area drafts each restated the same gating RX test (SETUP-04 / LW-T1 / LW-00×2) → collapsed to one **LW-RX**. The capture-tap data/no-fport/JoinRequest/parse-fail variants (LW-T2/T3/T4/T5, LW-01/02, LW-06) → folded into **LW-DATA / LW-JOIN / LW-FAIL**. The duplicate "no-lw-encoder" tests (SETUP-06, LW-T8, LW-01-drop) → one **LW-MT→LW**. Three relay/flood/loop drafts → **LW-RELAY / LW-LOOP / LW-FLOOD**. One canonical frame set (FRAME-U etc.) replaces the per-draft frames; expected strings recomputed against this set.

**Corrections applied from source (drafts had these wrong):**
- Boot "ready" line is **`[Radio1-B2B] ready — …`** (em-dash, object `_name`), **not** `[R1] ready - …` (hyphen). Structured lines and the decoder dump use `R1`/`R2`.
- RNS debugDump shows **`proto=3 sync=0x42`** (enum `PROTO_RNS=3`), **not** `proto=4` (the carryovers draft was wrong; `PROTO_CUSTOM=4`).
- debugDump RF line spacing is exactly `  radio1 RF     = proto=N sync=0xNN F.FFF MHz BWNN.N SFNN CRN TXNNdBm`.
- The generic `evt=RX … rssi= snr=` line is emitted in `radioTask` (main.cpp:1041) **before** the decoder dump (1044) **before** ingest (1045) — it is not gated by `BRIDGE_LW_CAPTURE`; only the structured `mtype=…` capture line is.

**Must-have vs optional:** Must = SETUP-01..03, R0, LW-RX, C1, C2, LW-DATA, LW-JOIN, LW-FAIL, LW-SUMMARY, LW-MT→LW, R1–R4, R9. Should = LW-LOOP, LW-RELAY, LW-FLOOD, LW-CAP0, LW-SUM0, R5–R8. Optional = LW-RELAY0.

**Real LoRaWAN device vs hand-built frame:**
- **Hand-built frame fully sufficient** (the firmware reads only the cleartext MAC header, never decrypts): LW-RX, LW-DATA, LW-JOIN, LW-FAIL, LW-SUMMARY, LW-LOOP, LW-RELAY, LW-FLOOD, LW-CAP0, LW-SUM0, LW-RELAY0. C2 likewise needs only a hand-built 0x42 payload (RNS is not decrypted).
- **A real LoRaWAN end-device only *strengthens*** LW-RX/DATA/JOIN/SUMMARY (realistic DevAddr/DevEUI, monotonic FCnt progression, real FPort) — **not required**.
- **A real device is genuinely preferred** only for **C1** (a GPS Meshtastic node with a time field; hand-building an AES-CTR POSITION protobuf is error-prone). **R1–R8** need real MT and MC nodes (standard mesh stimulus, no LoRaWAN gateway needed). A 2nd v8.3 bridge strengthens **LW-FLOOD** (true multi-bridge) but a single-bridge re-inject covers the dedup logic.

**Known gaps / caveats:**
- **Byte-identical relay output** (LW-RELAY, C2) is only fully proven with a 3rd RX radio capturing the dest emission; the QUEUE `mode=raw len=` + `TX_START len=` chain proves correct length and the no-mutation code path but not the bytes on air.
- **Rebuild tests** (LW-CAP0/SUM0/RELAY0, C2 toggle, R10's RNS variants) each need a separate firmware build; batch them or defer if doing a single-build pass. Do a `pio run -t clean` before each — the `#ifndef` defaults mean a stale object could retain `=1`.
- **R5/R10** require reconfiguring both radios (same-MT-channel / RNS); not part of the default profile.
- **CLAUDE.md lists COM6** as the standard monitor port; this protocol uses **COM13** per the task spec. Adjust the `--port` if the DUT enumerates elsewhere.
- **MeshCore has no on-air per-message id**, so identical MC text within the TTL is dedup-collapsed (accepted v8.2 limitation) — vary the body in R3/R4 if testing distinct MC sends.

---

## F. Appendix — this bench: 3 bridges (COM6/13/14) + 2 MT + 2 MC, RNS deferred

A bridge **cannot originate** a `0x34` frame (`MT/MC→LW` is dropped; the relay
only re-transmits a *received* frame), so a dedicated **LoRaWAN frame generator**
is required — flash [`tools/lw-frame-gen/`](tools/lw-frame-gen/) onto one bridge
(reflash bridge firmware afterward). RNS (`C2`) is deferred (no Reticulum gear);
when ready, the same generator on **sync `0x42`** covers it — RNS is not
decrypted, so any ≥1-byte `0x42` payload works, no Reticulum stack needed.

**Roles**

| Device | Role | Config |
|---|---|---|
| **COM6** | LoRaWAN frame generator | flash `tools/lw-frame-gen/` (1=U 2=U2 3=D 4=J 5=C) |
| **COM13** | DUT-A (primary, `!1de9dc80`) | every test |
| **COM14** | DUT-B (secondary) | relay/flood only (Pass C) |
| 2× MT, 2× MC | mesh stimulus | regression (R1–R9), C1, LW-MT→LW |

**Frequency plan** (US915; clear of MT 906.875 / MC 910.525). All LoRaWAN radios
BW125 / SF7 / CR5 / sync `0x34` (auto) / no key:
- **freqA = 904.6 MHz** — generator → DUT-A R1
- **freqB = 903.9 MHz** — DUT-A R2 relay-out → DUT-B R1

**Passes** (portal reconfig between them; only Pass D reflashes the DUT):

| Pass | DUT config | Tests | Stimulus |
|---|---|---|---|
| **A** Regression + clock | DUT-A `P-DEFAULT` (R1=MT, R2=MC→match your MC devices) | R0–R9, C1 | 2 MT + 2 MC. R5 uses both MT on one MT channel/2 freqs; C1 = cold-boot DUT-A, MC silent, one MT broadcasts POSITION w/ time |
| **B** Capture/summary/drop | DUT-A R1=LoRaWAN@904.6, R2=MT | LW-RX (gate), LW-DATA/JOIN/FAIL/SUMMARY, LW-MT→LW, LW-LOOP | generator @904.6; one MT device for LW-MT→LW |
| **C** Relay + flood | DUT-A R1=LoRaWAN@904.6 + R2=LoRaWAN@903.9; DUT-B R1=LoRaWAN@903.9 | LW-RELAY (byte-identical on DUT-B), LW-FLOOD | generator @904.6 |
| **D** Flag toggles | rebuild DUT-A with each `-DBRIDGE_LW_*=0` | LW-CAP0/SUM0/RELAY0 | generator @904.6 |

The generator's `GEN_*` build flags (`tools/lw-frame-gen/platformio.ini`) **must
match the DUT's LoRaWAN radio exactly**; defaults are freqA / BW125 / SF7 / CR5 /
20 dBm. Do a quick `SETUP-01..03` boot check on all three bridges before
converting COM6 into the generator.

**Configuring a DUT for LoRaWAN — two ways:**
- **WiFi captive portal** (no reflash): reset, BOOT within 5 s, join the AP,
  `http://192.168.4.1`, Radio 1 → LoRaWAN + freq/BW/SF/CR, **Save**. Verify the
  `ready … sync 0x34` line; if it comes back `0x2B`, the save was rejected — watch
  for a red error banner and confirm you saved on the board you're monitoring
  (the `op=mint selfid=!…` line is that board's identity).
- **Deterministic bench envs** (no WiFi; recommended): flash a helper env from
  `platformio.ini` with NVS erased so its first-boot defaults take effect —
  `pio run -e bench_lw_dutA -t erase --upload-port COM13` then `… -t upload …`.
  `bench_lw_dutA` = R1 LoRaWAN 904.6, `bench_lw_dutB` = R1 LoRaWAN 903.9 (→COM14),
  `bench_lw_relayA` = R1 904.6 + R2 903.9 (→COM13 for Pass C). Erasing NVS resets
  identity to MAC-derived (COM13 stays `!1de9dc80`) and clears the NodeDB. Revert
  a board to a normal bridge with `pio run -e xiao_esp32s3 -t erase --upload-port
  COMx` + upload.

---

## G. Bench results — 2026-06-14 (real hardware: 3 bridges COM6/13/14 + 2 MT + 2 MC)

**Verdict: ALL `must` tests PASS → v8.3 is release-ready.** Branch `v8.3-dev` @ `fa3e8bf`.

| Test | Pri | Result | Evidence (verbatim `evt=` lines) |
|---|---|---|---|
| SETUP-01..03, R0 | must | ✅ | boot banner `(v8.2)`, `[Radio1-B2B]/[Radio2-Edge] ready —`, `[RouteQueue:*] depth=64`, `Bridge active.` |
| LW-RX (gate) | must | ✅ | `evt=RX radio=R1 proto=LW len=15` + structured capture |
| LW-DATA/JOIN/FAIL/SUMMARY | must | ✅ | `mtype=UnconfDataUp/ConfDataUp(fport=-1)/JoinRequest/parse=fail`; summary `dstproto=MT virtualid=!b16b00b5` |
| LW-MT→LW | must | ✅ | `drop=no-lw-encoder` |
| LW-LOOP / LW-RELAY (+multibridge) | should | ✅ | `drop=lw-dup`; `mode=raw` → COM14 byte-identical |
| C1 MT-POSITION clock-learn | must | ✅ | `evt=CLOCK src=MT unix=1781467783` |
| **C2 / D1 — RNS↔RNS raw repeat** | must | ✅ | `evt=QUEUE radio=R1 dst=R2 mode=raw len=15 virtualid=-` → R2 `TX_DONE` → `drop=rns-dup`; **byte-identical**: COM14 3rd-RX `[R2 decoded] Reticulum/RNode raw 15 B (b64=20): QIofASYAAQACqrsRIjNE` == source |
| **D2 — RNS→MC tunnel** | should | ✅ | `evt=QUEUE radio=R1 dst=R2 dstproto=MC frag=1/1 seq=18 len=53 msg="[rns 18 1/1] QIofASYAAQACqrsRIjNE"` → R2 `TX_DONE`; repeated to MeshCore |
| **D3 — MC→RNS drop** | should | ✅ | `evt=RX radio=R2 proto=MC` → `evt=DROP radio=R2 dst=R1 drop=no-rns-encoder msg="9506C57C: MC sent"`; no R1 TX (+ bonus `evt=CLOCK src=MC`) |
| R1/R2/R3/R4 + R6/R7 | must/should | ✅ | MT→MC `@MT` prefix; MC→MT `op=virtual long="… @MC"`; `drop=loop-dup`; `bat 5.00V` + `drop=self-echo`; `CAD cad=busy backoff∈20..120`; `THROTTLE gap=2×air` |
| **R9 — do-no-harm** | must | ✅ | sustained mixed MT/MC load → **zero** `proto=LW` / `no-lw-encoder` / `lw-dup` |

**OPEN — `should`/optional, NONE gating the tag:**
- **R5** same-channel MT raw-repeat — real MT node available; needs both DUT radios on the IDENTICAL MT
  channel/PSK (captive portal, or a new `bench_mt_samechan` env). Self-guard is satisfied by a real node's
  own src id (≠ bridge `0xB16B00B5`).
- **R8** RX-priority headline (long TX on one radio, burst RX on the other).
- **Pass D** flag toggles LW-CAP0/SUM0/RELAY0 — all five `bench_lw_*`/`bench_rns*` envs compile green.
- **LW-FLOOD** bidirectional multi-bridge — one-way relay PASS; the symmetric echo-drop needs COM14 a
  `bench_lw_relayA`-mirror (R1=903.9 / R2=904.6), since `bench_lw_dutB` has only one LoRaWAN radio.

> **Correction to §C/§D notes:** the RNS raw-repeat QUEUE renders `virtualid=-` (srcId 0 → `-`, `fmtNodeId`
> main.cpp:262-263). The RNS→MC tunnel QUEUE carries `dstproto=MC frag=%u/%u seq=%02x … msg="[rns %02X …]"`
> (main.cpp:464-468). Byte-identity of an RNS repeat is provable from a 3rd RX bridge's `[Rn decoded]
> Reticulum/RNode … (b64=…)` base64 line (MeshDecoderDebug.h:885-890) — the documented "needs a 3rd RX
> radio" gap is closed by COM14.