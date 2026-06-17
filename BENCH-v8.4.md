# BENCH-v8.4 — LoRaWAN ABP uplink encoder (P1–P4) bench plan

**Status (2026-06-16):** code-complete + build-green on `dev-ABP-lorawan`. **Tier A/B core PASSED on
hardware** — A1, A2, A5, B1, B2, B5. Results + methods (auditable) in **`BENCH-RESULTS.md`**. Open:
A4/B3/B4/B6/B7 (owner, WiFi config page), A3 (needs a >242 B raw transmitter), C1–C4 (colleague's
ChirpStack). Each test below is self-contained — this is the full procedure.

| Tier | Needs | Who runs it |
|---|---|---|
| **A** — DUT board only | one bridge + USB serial | you |
| **B** — Synthetic LNS | DUT bridge + a 2nd "sniffer" bridge, **no ChirpStack** | you |
| **C** — ChirpStack LNS | DUT bridge + ChirpStack + gateway | colleague |

---

## Shared setup (read once)
- **Boards / ports:** DUT-A = **COM13** · SNIFFER = **COM6** · DUT-B/spare = **COM14**.
- **Run from the project root** (PowerShell):
  `cd "C:\Users\6r4yh\workspace\Platformio\Projects\Xiao-esp32s3-lora-repeater - main dev-ABP-lorawan"`
- **Every board = 3 commands:** `erase` → `upload` → `monitor` (spelled out in each test).
- **⚠️ Erase before every env/config change.** Autosave + the portal only act on an
  *unconfigured* board, so re-flashing without erase keeps the OLD config. *Exception:* the
  FCnt tests (B2/B3) erase **once**, then reboot with the **RST button** (re-erasing wipes
  the counter under test).
- **Bench creds** (baked into `bench_lw_enc`/`_dwell`): DevAddr `01000001` ·
  NwkSKey `2B7E151628AED2A6ABF7158809CF4F3C` · AppSKey `D41420B7F5A3C96E1D8204F7B3A65C90` · FPort 13.
- **Sync / proto legend:** `0x2B`=Meshtastic(1) · `0x12`=MeshCore(2) · `0x42`=Reticulum(3)
  · `0x34`=LoRaWAN(5) · else Custom(4). DevAddr `01000001` = NwkID 0 (matches ChirpStack
  default NetID `0x000000`; a mismatched NwkID is silently dropped).
- **Config check:** open the monitor, press **RST**, and confirm the `Confirm at boot:`
  line each test names. A wrong value almost always means you skipped `-t erase`.

---

## Tier A — DUT board only

### A1 · LW-ENC-SELFTEST — crypto self-test *(gate)*
**Board:** DUT-A COM13 · **env:** `bench_lw_enc`
```
pio run -e bench_lw_enc -t erase  --upload-port COM13
pio run -e bench_lw_enc -t upload --upload-port COM13
pio device monitor --port COM13 --baud 115200
```
**Confirm at boot:** `[lw-selftest] overall : PASS` · `[lw-enc] … ready=1 DevAddr=0x01000001 FPort=13` · `radio2 RF … sync=0x34 903.900 … SF7` · `region=1`.
**Do:** just watch the boot output.
**PASS:** 4× `RFC4493 CMAC … PASS`, `FRMPayload A_1 … PASS`, `frame … PASS`, `overall : PASS`.
**FAIL:** any `FAIL` — the encoder then refuses to emit (`drop=lw-selftest-fail`).

### A2 · LW-DWELL — US915 per-TX dwell cap (P4)
**Board:** DUT-A COM13 · **env:** `bench_lw_enc_dwell`
```
pio run -e bench_lw_enc_dwell -t erase  --upload-port COM13
pio run -e bench_lw_enc_dwell -t upload --upload-port COM13
pio device monitor --port COM13 --baud 115200
```
**Confirm at boot:** `radio2 RF … sync=0x34 903.900 BW125.0 SF12` · `region=1`.
**Do:** send any MT text to R1 (or let a routed frame reach R2).
**PASS:** `evt=DROP radio=… drop=dwell toa=1811 limit=400` — frame NOT transmitted (exact `toa` scales with payload; any value > `limit=400` passes).
**FAIL:** an `evt=TX_START` / on-air frame at SF12 (dwell not enforced).

### A3 · LW-OVERFLOW — over-cap payload dropped, not truncated (#6)
**Board:** DUT-A COM13 · **env:** `xiao_esp32s3_lwabp` (portal)
```
pio run -e xiao_esp32s3_lwabp -t erase  --upload-port COM13
pio run -e xiao_esp32s3_lwabp -t upload --upload-port COM13
pio device monitor --port COM13 --baud 115200
```
**Portal config:** set a radio to **Custom** (your raw-LoRa freq/BW/SF/sync); add an ABP device (Any source). Save & reboot.
**Confirm at boot:** `[LoRaWANConfig] … anyConfigured=1` + a `dev0 …` line · the Custom `radioN RF` line.
**Do:** transmit a raw frame **> 242 B** (or > 237 B if the source tag is on).
**PASS:** `evt=DROP … drop=lw-payload-overflow len=… cap=…`; no QUEUE for it.
**FAIL:** a truncated frame is queued/sent.

### A4 · LW-PORTAL — portal ABP config persists (P2)
**Board:** DUT-A COM13 · **env:** `xiao_esp32s3_lwabp` (portal)
```
pio run -e xiao_esp32s3_lwabp -t erase  --upload-port COM13
pio run -e xiao_esp32s3_lwabp -t upload --upload-port COM13
pio device monitor --port COM13 --baud 115200
```
**Portal config:** "LoRaWAN ABP devices" → Device 0: Enabled, *Any source*, DevAddr + NwkSKey + AppSKey + FPort. Set a radio to LoRaWAN. **Save & reboot.**
**Confirm at boot:** `[LoRaWANConfig] loaded … anyConfigured=1` + `dev0 … devaddr=… fport=…` · `radioN RF … sync=0x34 …`.
**PASS:** the saved DevAddr/FPort appear in the boot dump AND re-opening the portal shows them.
**FAIL:** `anyConfigured=0` / values blank (didn't persist).

### A5 · R-DO-NO-HARM — stock build unaffected
**Board:** DUT-A COM13 · **env:** `xiao_esp32s3`
```
pio run -e xiao_esp32s3 -t erase  --upload-port COM13
pio run -e xiao_esp32s3 -t upload --upload-port COM13
pio device monitor --port COM13 --baud 115200
```
**Confirm at boot:** **NO** `[lw-selftest]` and **NO** `[lw-enc]` lines · `radio2 RF … sync=0x12 910.525 BW62.5 SF7` (MeshCore).
**Do:** run normal MT↔MC traffic.
**PASS:** normal routing; **no** ABP `evt=QUEUE`; `MT/MC→LW` still `drop=no-lw-encoder`.
**FAIL:** any ABP output (encoder leaked into a stock build).

---

## Tier B — Synthetic LNS (DUT + sniffer, no ChirpStack)

**Flash the SNIFFER once (COM6) — it stays put for all of Tier B:**
```
pio run -e bench_lw_sniffer -t erase  --upload-port COM6
pio run -e bench_lw_sniffer -t upload --upload-port COM6
pio device monitor --port COM6 --baud 115200
```
**Confirm at boot (sniffer):** `radio2 RF … sync=0x34 903.900 BW125.0 SF7` (no `[lw-enc]`/`[lw-selftest]` — encoder off). When the DUT emits, the sniffer prints `evt=RX proto=LW … devaddr=… fcnt=… fport=…` + `evt=LWRAW raw=<hex>`. Keep this terminal open for every B test.

### B1 · LW-AIR-EMIT — the encoder transmits a valid uplink
**Board:** DUT-A COM13 · **env:** `bench_lw_enc`
```
pio run -e bench_lw_enc -t erase  --upload-port COM13
pio run -e bench_lw_enc -t upload --upload-port COM13
pio device monitor --port COM13 --baud 115200
```
**Confirm at boot (DUT):** `[lw-selftest] overall : PASS` · `[lw-enc] … ready=1 DevAddr=0x01000001 FPort=13` · `radio2 RF … sync=0x34 903.900 … SF7`.
**Do:** send one MT text from a node on the DUT's R1 (Meshtastic US LongFast 906.875/BW250/SF11).
**PASS:** SNIFFER prints `evt=RX proto=LW mtype=Data(Up,Unconf) devaddr=0x01000001 fport=13 len=…`.
**FAIL:** nothing on the sniffer — check the DUT shows `evt=QUEUE … dstproto=LW` and both radios share 903.9/BW125/SF7.

### B2 · LW-FCNT-REBOOT — FCnt monotonic across reboot (#3/#4)
**Board:** DUT-A COM13 (`bench_lw_enc`, from B1). **Erase once, then power-cycle — do NOT re-flash.**
```
# (only if not already on bench_lw_enc:)
pio run -e bench_lw_enc -t erase  --upload-port COM13
pio run -e bench_lw_enc -t upload --upload-port COM13
pio device monitor --port COM13 --baud 115200
```
**Do:** send several texts (note the SNIFFER `fcnt=` values) → **press RST on the DUT** → send more.
**PASS:** post-reboot `fcnt=` **resumes ahead of** the last pre-reboot value (may jump up to 32; never repeats/decreases).
**FAIL:** `fcnt` restarts low (would replay at a real LNS).

### B3 · LW-FCNT-MOVE — a DevAddr keeps its counter across rows (#1)
**Board:** DUT-A COM13 · **env:** `xiao_esp32s3_lwabp` (portal)
```
pio run -e xiao_esp32s3_lwabp -t erase  --upload-port COM13
pio run -e xiao_esp32s3_lwabp -t upload --upload-port COM13
pio device monitor --port COM13 --baud 115200
```
**Portal config:** Device 0 = DevAddr **X** (+ keys/FPort), *Any source*; set a radio to LoRaWAN. Save.
**Do:** send N texts (note SNIFFER `fcnt=`) → portal: **clear Device 0, add the same DevAddr X in Device 1** → reboot → send again.
**PASS:** the first post-move `fcnt=` is **≥** the last value seen for X.
**FAIL:** it resumes at slot 1's stale/zero counter.

### B4 · LW-RESOLVE — per-source selection (MT-node → protocol → ANY)
**Board:** DUT-A COM13 · **env:** `xiao_esp32s3_lwabp`, source radio = Meshtastic
```
pio run -e xiao_esp32s3_lwabp -t erase  --upload-port COM13
pio run -e xiao_esp32s3_lwabp -t upload --upload-port COM13
pio device monitor --port COM13 --baud 115200
```
**Portal config (distinct DevAddrs):** Device 0 = *MT node id* = node A → **W** · Device 1 = *Source protocol = Meshtastic* → **X** · Device 3 = *Any source* → **Z**. Save.
**Confirm at boot:** three `dev0/1/3 …` lines with sel/devaddr as set.
**Do:** send from MT node A, then MT node B (different id), then any other source.
**PASS (SNIFFER):** A → `devaddr=…W` · B → `…X` · other → `…Z`.
**FAIL:** wrong DevAddr per source (resolve order broken).
*MC note:* MeshCore matches only *Source protocol*/*Any* (no MC-node selector). For an MC pass, set the source radio to MeshCore + add a *Source protocol = MeshCore* device.

### B5 · LW-MIC-OFFLINE — validate MIC + decrypt, no ChirpStack
**Board:** none (host PC). Uses a frame captured by the SNIFFER in any B test.
```
pip install cryptography
python tools/lw-verify.py <raw-hex-from-evt=LWRAW> 2B7E151628AED2A6ABF7158809CF4F3C D41420B7F5A3C96E1D8204F7B3A65C90
```
**PASS:** `MIC … -> PASS` and `Decrypted … as ASCII : '<your text>'` → `VERDICT: MIC valid — ChirpStack would ACCEPT`.
**FAIL:** `MIC … -> FAIL` (recheck keys / the copied hex). A PASS predicts a Tier-C PASS.

### B6 · LW-TAG — source tag decodes to the right protocol (#5)
**Board:** DUT-A COM13 · **env:** `xiao_esp32s3_lwabp`
```
pio run -e xiao_esp32s3_lwabp -t erase  --upload-port COM13
pio run -e xiao_esp32s3_lwabp -t upload --upload-port COM13
pio device monitor --port COM13 --baud 115200
```
**Portal config:** one ABP device with **"Prepend source tag" ON**; set the source radio to MT (then repeat with MC).
**Do:** send a text; copy the SNIFFER `evt=LWRAW raw=…`; run
`python tools/lw-verify.py <raw-hex> <NwkSKey> <AppSKey> --tagged`.
**PASS:** `src tag : proto=1(meshtastic)` (MT) and `proto=2(meshcore)` (MC) + the payload.
**FAIL:** `proto=43`/"?" (raw sync word — the pre-#5 bug).

### B7 · LW-MC-SOURCE — MeshCore → ABP
**Board:** DUT-A COM13 · **env:** `xiao_esp32s3_lwabp`, source radio = **MeshCore**
```
pio run -e xiao_esp32s3_lwabp -t erase  --upload-port COM13
pio run -e xiao_esp32s3_lwabp -t upload --upload-port COM13
pio device monitor --port COM13 --baud 115200
```
**Portal config:** source radio = MeshCore; one ABP device (*Any* or *Source protocol = MeshCore*). Save.
**Do:** send an MC text from an MC node.
**PASS:** DUT `evt=QUEUE … dstproto=LW … cred=nvs`; SNIFFER `evt=RX proto=LW devaddr=…`; `lw-verify.py` decrypts the MC body.
**FAIL:** `drop=no-lw-abp-dest` (no LoRaWAN dest radio) or no sniffer RX.

---

## Tier C — ChirpStack LNS (colleague's rig)

**Provisioning (every C test):** ChirpStack v4 + a gateway in range. Device-profile =
your region · **MAC 1.0.x · ABP · Class A · ADR off**; device = **DevAddr + NwkSKey +
AppSKey + FPort matching the DUT**; tick **"Disable frame-counter validation"** (or rely
on persisted FCnt); paste `tools/chirpstack-codec.js` into the codec. **DevAddr NwkID
must match the LNS NetID.** Stick to **C1 + C2** — Tier B already proved the resolve
matrix + crypto off-box.

### C1 · LW-P1-ACCEPT — uplink ingested by ChirpStack *(⭐ gate)*
**Board:** DUT COM13 · **env:** `bench_lw_enc`
```
pio run -e bench_lw_enc -t erase  --upload-port COM13
pio run -e bench_lw_enc -t upload --upload-port COM13
pio device monitor --port COM13 --baud 115200
```
**ChirpStack:** provision DevAddr `01000001` + the bench keys + FPort 13.
**Confirm at boot (DUT):** `[lw-selftest] overall : PASS` · `[lw-enc] … ready=1 DevAddr=0x01000001`.
**Do:** send an MT text to the DUT's R1.
**PASS:** DUT `evt=QUEUE … cred=flag`; **ChirpStack shows the decoded uplink** for `01000001` (FCnt increasing).
**FAIL:** no LNS event — wrong MIC / MAC version / DevAddr NwkID vs NetID (run B5 first to rule the crypto out).

### C2 · LW-P3-WEATHER — Custom raw-LoRa → ABP → ChirpStack *(⭐)*
**Board:** DUT COM13 · **env:** `xiao_esp32s3_lwabp`
```
pio run -e xiao_esp32s3_lwabp -t erase  --upload-port COM13
pio run -e xiao_esp32s3_lwabp -t upload --upload-port COM13
pio device monitor --port COM13 --baud 115200
```
**Portal config:** set a radio to **Custom** (station's exact freq/BW/SF/CR/sync); add an ABP device (Any or *Source protocol = Custom*). **Edit `decodeStation()` in the codec to your real layout first.**
**ChirpStack:** provision that device's DevAddr/keys/FPort.
**Do:** transmit a raw frame from the station (or a 2nd board on that RF).
**PASS:** DUT `evt=QUEUE … cred=nvs … msg="custom-raw"`; ChirpStack shows the uplink + codec readings.
**FAIL:** `drop=no-lw-abp-dest` / `drop=loop-dup` / `drop=lw-payload-overflow`.

### C3 · LW-P3-TAG — codec decodes the source tag (#5)
Same board/flash/portal as C2, but the device has **"Prepend source tag" ON** and the codec has `HAS_SOURCE_TAG=true`.
**PASS:** ChirpStack decodes `source:{proto,srcId}` with the correct proto name + payload.
**FAIL:** proto wrong / not split.

### C4 · LW-FCNT-LNS — LNS accepts increasing FCnt, rejects replays
Same board/flash as C1, but provision the device **with** frame-counter validation **enabled**.
**PASS:** consecutive uplinks accepted; after a DUT RST the LNS still accepts (resumed-ahead FCnt); a forced low/replayed FCnt is rejected.
**FAIL:** valid uplinks dropped, or a replay accepted.

---

## Reference

**Offline verifier** — `tools/lw-verify.py` (mirrors `src/LoRaWANCrypto.h`):
```
python tools/lw-verify.py <PHYPayload-hex> <NwkSKey-hex32> <AppSKey-hex32> [--fcnt-msb N] [--tagged]
```
Prints MHDR / DevAddr(+NwkID) / FCnt / FPort, **MIC PASS/FAIL**, decrypted FRMPayload.

**Serial events:** `evt=QUEUE … cred=flag|nvs tag=0|1` (queued) · `evt=RX proto=LW … devaddr=… fcnt=… fport=…` (sniffer) · `evt=LWRAW … raw=<hex>` (sniffer, full frame) · `drop=lw-selftest-fail` · `drop=lw-fcnt-fail` / `FCNT_PERSIST_FAIL` · `drop=lw-payload-overflow` · `drop=dwell` · `[lw-warn] … DevAddr … collision`.

**Pre-bench fixes (folded in):** #1 FCnt-by-DevAddr (B3) · #2 self-test gate (A1) · #3 fail-closed FCnt (B2) · #4 reboot-safe build-flag FCnt (B2) · #5 proto-enum tag (B6/C3) · #6 over-cap drop (A3). Commits `d98da4b`,`1496717`,`9aaf423` on `dev-ABP-lorawan`.

**Notes:** a wrong MIC is silently dropped by ChirpStack — suspect keys / MAC version / DevAddr NwkID before the bridge. Migration: FCnt moved to `fc_<addr>` NVS keys; old `fc<N>` keys orphaned → a device resumes at FCnt 0 on first boot (fine with "Disable frame-counter validation"). Encoder is uplink-only (Class A, ADR off). Deferred (not v8.4): ABP/OTAA decode, dual-LNS, MT/MC→RNS.
