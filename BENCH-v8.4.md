# BENCH-v8.4 — LoRaWAN ABP uplink encoder (P1–P4) bench plan

**Status: code-complete + build-green on `dev-ABP-lorawan`; NOT yet run on hardware.**
On-air verification is the remaining gate before v8.4 ships. A 30-agent adversarial
pre-bench review (2026-06-15) verified the crypto **sound** (RFC4493 CMAC / B0 / A_i /
FRMPayload / 1.0.x MIC byte-correct, confirmed by an independent recompute) and
produced six fixes (#1–#6), folded in — see
[Pre-bench review fixes](#pre-bench-review-fixes-folded-in).

## How to read this plan — three tiers by hardware need
The encoder mints a LoRaWAN uplink and re-emits it on RF. Almost everything about it
can be checked **without a ChirpStack LNS**, because (a) the bridge logs its own
encode decisions on serial, and (b) a **second bridge in keyless LoRaWAN capture mode
("synthetic LNS" sniffer)** receives the emitted frame and prints its *cleartext*
header (DevAddr / FCnt / FPort) plus the full PHYPayload hex. Only **MIC-acceptance
and FRMPayload-decrypt** truly need either a real ChirpStack **or** the off-box
`tools/lw-verify.py` (which validates the MIC + decrypts with the keys).

| Tier | Needs | Who runs it |
|---|---|---|
| **A — DUT board only** | one bridge + USB serial | either rig |
| **B — Synthetic LNS** | DUT bridge + a 2nd "sniffer" bridge (+ a stimulus source) — **no ChirpStack** | **your 3-bridge rig** |
| **C — ChirpStack LNS** | DUT bridge + ChirpStack + a LoRaWAN gateway in RF range | **colleague's rig** |

> **Split for your two rigs:** run **all of Tier A + Tier B** on your 3 bridges (no
> ChirpStack needed). Hand **Tier C** (true LNS ingestion + codec decode) to the
> colleague who has ChirpStack. Tier B's optional `lw-verify.py` step proves the MIC
> is valid and the payload decrypts, so a Tier-B PASS strongly predicts a Tier-C PASS.
>
> **Provisioning split:** because Tier B proves the per-source **resolve matrix** (B4)
> and the **crypto** (B5) off-box, the colleague does **not** need to provision the full
> 4-device matrix in ChirpStack — one representative ABP device (C1) + the weather device
> (C2) is enough. Provision ChirpStack only for the DevAddrs you actually ingest
> end-to-end; the bridge↔LNS keys/DevAddr must match for each.

---

## Hardware & roles

**Your rig (3× Xiao dual-SX1262 bridges, no ChirpStack):**
| Role | Board | Build | Purpose |
|---|---|---|---|
| **DUT-A** | bridge on COM13 | `bench_lw_enc` (or `bench_lw_enc_dwell` for P4) | R1=Meshtastic in, R2=LoRaWAN ABP encoder out (903.9/BW125/SF7) |
| **SNIFFER** ("synthetic LNS") | bridge on COM6 | `bench_lw_sniffer` | R2=keyless LoRaWAN capture on 903.9; prints `evt=RX proto=LW …` + `evt=LWRAW raw=…` |
| **STIMULUS / DUT-B** | bridge on COM14 / MT / MC node / Custom board | (see each test) | originates the mesh text or raw-LoRa frame the DUT transcodes; or a 2nd DUT for a parallel MC pass (B4/B7) |

Stimulus source options (the DUT needs *inbound* traffic to transcode — it does not
self-generate): a **Meshtastic node/app** on the DUT's R1 channel (default LongFast)
for MT tests; a **MeshCore node** for MC; a board transmitting **raw LoRa** on the
DUT's Custom radio for the weather path. `tools/lw-frame-gen/` can stand in for a
raw-LoRa source.

> **Why multiple MT + MC stimulus devices matter:** the DUT bridges *one* mesh source
> radio (R1) ↔ one LoRaWAN radio (R2), so it ingests **one mesh protocol per pass** (set
> R1 to MT or MC via the portal). With **several MT nodes** you can drive the whole
> per-source **resolve ladder** (node-specific → protocol → ANY) in a single MT pass
> (test B4); a second pass with R1=MeshCore adds the **MC→ABP** source path (B7) and the
> MeshCore tag (B6). All of this is verifiable on the sniffer (DevAddr per source) + the
> offline verifier — **no ChirpStack required**, so the resolve matrix is entirely yours
> to validate.

**Colleague's rig (1× bridge + ChirpStack):** the DUT bridge (`bench_lw_enc` or the
portal RC `xiao_esp32s3_lwabp`), a LoRaWAN gateway forwarding to **ChirpStack v4**,
and a Meshtastic/MeshCore node as the stimulus.

---

## Builds
| Env | Tier(s) | Purpose |
|---|---|---|
| `bench_lw_enc` | A, B, C | encoder + boot self-test + **build-flag** ABP creds (DevAddr `01000001`, **REGION=US**), R2=LoRaWAN 903.9/BW125/SF7. Autosaves (no portal). |
| `bench_lw_enc_dwell` | A | as `bench_lw_enc` but **SF12** (ToA ~1.5 s > 400 ms) — for the dwell-cap test. |
| `bench_lw_sniffer` | B | the **"synthetic LNS"**: keyless LoRaWAN capture on 903.9 + full-frame hex (`evt=LWRAW`). |
| `xiao_esp32s3_lwabp` | A, B, C | encoder on, **portal**-configured (no autosave, no build-flag keys). For portal/per-source/weather tests. |
| `xiao_esp32s3` | A | stock (encoder compiled out) — the do-no-harm control. |

**Commands & ports.** Run from the project root in PowerShell (the path has spaces):
```
cd "C:\Users\6r4yh\workspace\Platformio\Projects\Xiao-esp32s3-lora-repeater - main dev-ABP-lorawan"
```
Per board, three steps — substitute the test's **env** and the board's **COM port**:
```
pio run -e <env> -t erase  --upload-port COMx     # 1. wipe flash+NVS (clean config + FCnt)
pio run -e <env> -t upload --upload-port COMx     # 2. flash firmware
pio device monitor --port COMx --baud 115200      # 3. watch serial (Ctrl-] to exit)
```
Example port map (yours): **DUT-A=COM13 · SNIFFER=COM6 · DUT-B/spare=COM14** (COM6 and
COM14 are interchangeable for the sniffer-vs-spare/2nd-DUT roles). Always pass
`--upload-port`/`--port` (several boards are attached); one monitor terminal per board.

**⚠️ Erase is REQUIRED when you change env/config.** The autosave and the captive portal
only act on an *unconfigured* board (`if (!isConfigured())`), so re-flashing over existing
NVS **keeps the old config** — `-t erase` first guarantees the new env's settings (and a
clean ABP table + FCnt) actually take effect. *Exception:* for the FCnt-persistence tests
(B2/B3) erase **once** at the start, then reboot via **power-cycle / RST button** — do not
erase or re-flash mid-test or you wipe the counter under test. (Forgot to erase? Within
~5 s of boot, press BOOT or send any serial byte to force the portal.)

**Config application:**
- **Autosave envs** (`bench_lw_enc`, `bench_lw_enc_dwell`, `bench_lw_sniffer`): after
  erase+upload the build-flag config self-saves on first boot (`[setup] BRIDGE_BENCH_AUTOSAVE:
  persisted build-flag config`) — **no portal, no manual setup**.
- **Portal env** (`xiao_esp32s3_lwabp`): after erase+upload it enters the captive portal
  (`[setup] … entering captive portal` + a Wi-Fi AP). Connect, apply the radio + ABP-device
  settings the test names, **Save** (the bridge reboots).

**Applied radio / region / cred config per env:**
| Env | R1 (mesh source) | R2 (LoRaWAN out) | Region | ABP creds |
|---|---|---|---|---|
| `bench_lw_enc` | Meshtastic 906.875 / BW250 / SF11 (0x2B) | LoRaWAN 903.9 / BW125 / SF7 (0x34) | US | DevAddr `01000001` + bench keys, FPort 13 (autosave) |
| `bench_lw_enc_dwell` | Meshtastic 906.875 / BW250 / SF11 | LoRaWAN 903.9 / BW125 / **SF12** | US | same (autosave) |
| `bench_lw_sniffer` | Meshtastic 906.875 / BW250 / SF11 (unused) | **LoRaWAN capture** 903.9 / BW125 / SF7 (0x34) | — | none (keyless) |
| `xiao_esp32s3_lwabp` | portal-set | portal-set (set a radio to LoRaWAN) | portal-set | portal-set ABP device(s) |
| `xiao_esp32s3` | Meshtastic 906.875 / BW250 / SF11 (0x2B) | MeshCore 910.525 / BW62.5 / SF7 (0x12) | unset | none |

**Channel match (protocol conformance):** an **MT stimulus node** must be on the DUT's R1
— **Meshtastic US LongFast 906.875 / BW250 / SF11**; an **MC node** on the DUT's MeshCore
channel. The **SNIFFER** capture radio must match the DUT's R2 emission **903.9 / BW125 /
SF7** (the `bench_lw_sniffer` env already does; for the SF12 dwell test the sniffer won't
hear it — that test is DUT-serial-only).

**Bench creds** (throwaway, baked into `bench_lw_enc`/`_dwell`):
`DevAddr 01000001` · `NwkSKey 2B7E151628AED2A6ABF7158809CF4F3C` ·
`AppSKey D41420B7F5A3C96E1D8204F7B3A65C90` · `FPort 13`.
⚠️ **DevAddr NwkID must match the LNS NetID.** `01000001` is NwkID 0 → matches
ChirpStack's default private NetID `0x000000`. A DevAddr whose NwkID is outside the
LNS's NetID is **silently dropped** — the #1 suspect after a wrong MIC.

---

## Tier A — DUT board only (no second board, no LNS)

**Run (DUT-A only, COM13) — erase + upload + monitor, swapping the env per test:**
```
pio run -e bench_lw_enc -t erase  --upload-port COM13
pio run -e bench_lw_enc -t upload --upload-port COM13
pio device monitor --port COM13 --baud 115200
```
Per-test env: **A1** `bench_lw_enc` · **A2** `bench_lw_enc_dwell` · **A3/A4**
`xiao_esp32s3_lwabp` (apply the portal settings the test names) · **A5** `xiao_esp32s3`.
**Erase before each env switch.**

### A1 · LW-ENC-SELFTEST — crypto known-answer self-test  *(gate)*
- **HW:** DUT only. **Flash:** `bench_lw_enc`. Watch boot serial.
- **PASS:** `[lw-selftest] RFC4493 CMAC … PASS` ×4, `FRMPayload A_1 keystream : PASS`,
  `frame … : PASS`, `[lw-selftest] overall : PASS`.
- **FAIL:** any `… : FAIL`. The encoder now **gates on this (#2)** — on a FAIL the
  bridge refuses to emit (`drop=lw-selftest-fail`) instead of putting a bad frame on air.

### A2 · LW-DWELL — US915 per-TX dwell cap (P4)
- **HW:** DUT only. **Flash:** `bench_lw_enc_dwell` (REGION=US, SF12). Send/route a
  packet to the LoRaWAN radio (any MT stimulus, or just let a routed frame hit R2).
- **PASS:** `evt=DROP radio=… drop=dwell toa=<~1480> limit=400`; the frame is **not**
  transmitted. On `bench_lw_enc` (SF7) the same frame sends normally.
- **Note:** also applies to v8.3 keyless LW↔LW relay when REGION=US (FCC-correct).

### A3 · LW-OVERFLOW — over-cap payload is dropped, not truncated (#6)
- **HW:** DUT only (a Custom-raw source > 242 B, or inspect via review). **Flash:**
  `xiao_esp32s3_lwabp` + a Custom radio + an ABP device, feed a >242 B raw frame
  (>237 B with the source tag).
- **PASS:** `evt=DROP … drop=lw-payload-overflow len=… cap=…`; no QUEUE for that frame.
  **FAIL (pre-#6):** a silently truncated frame is queued/sent.

### A4 · LW-PORTAL — portal ABP device config persists (P2)
- **HW:** DUT only. **Flash:** `xiao_esp32s3_lwabp` → boots into the captive portal.
- **Setup:** in **"LoRaWAN ABP devices"** set Device 0: Enabled, *Any source*, DevAddr
  + NwkSKey + AppSKey + FPort. Set a radio to LoRaWAN. **Save & reboot.**
- **PASS:** boot prints `[LoRaWANConfig] loaded … anyConfigured=1` and a `dev0 …
  devaddr=…` line; re-opening the portal shows the saved values.

### A5 · R-DO-NO-HARM — stock build unaffected
- **HW:** DUT only. **Flash:** `xiao_esp32s3`.
- **PASS:** normal MT↔MC/RNS/keyless-LW behavior; **no** ABP QUEUE lines, no
  `[LoRaWANConfig]`/`[lw-selftest]` output; `MT/MC→LW` still `drop=no-lw-encoder`.
  (All ABP code compiles out when `BRIDGE_LW_ENCODE` is unset; the stock image differs
  from v8.3 only by ~112 B of never-called code.)

---

## Tier B — Synthetic LNS (2–3 bridges, NO ChirpStack)

**Common setup for Tier B:**
1. **DUT-A** = bridge on COM13, `bench_lw_enc`. (R1=MT LongFast, R2=LoRaWAN 903.9.)
2. **SNIFFER** = bridge on COM6, `bench_lw_sniffer`. (R2 captures 903.9/BW125/SF7.)
3. **STIMULUS** = a Meshtastic node/app on the DUT R1 LongFast channel.
4. Open a monitor on **both** COM13 (DUT-A) and COM6 (SNIFFER).
Each MT text you send appears on the DUT as `evt=QUEUE … dstproto=LW … fcnt=… cred=flag`
and on the SNIFFER as `evt=RX proto=LW … devaddr=0x01000001 fcnt=… fport=13` (+ `evt=LWRAW raw=…`).

**Flash both boards (erase → upload → monitor, one terminal each):**
```
# DUT-A — the encoder (COM13)
pio run -e bench_lw_enc -t erase  --upload-port COM13
pio run -e bench_lw_enc -t upload --upload-port COM13
pio device monitor --port COM13 --baud 115200         # terminal 1

# SNIFFER — the synthetic LNS (COM6)
pio run -e bench_lw_sniffer -t erase  --upload-port COM6
pio run -e bench_lw_sniffer -t upload --upload-port COM6
pio device monitor --port COM6 --baud 115200          # terminal 2
```
**B3/B4/B6/B7** swap the DUT to `xiao_esp32s3_lwabp` (erase first; apply the named portal
ABP device(s) + source radio, Save). **Stimulus:** set MT/MC nodes to the DUT's R1 channel
(Meshtastic US LongFast 906.875/BW250/SF11, or the MeshCore channel for B7). The SNIFFER
stays on `bench_lw_sniffer` (903.9/BW125/SF7) throughout.

### B1 · LW-AIR-EMIT — the encoder actually transmits a valid-shaped uplink
- **Steps:** send one MT text from the stimulus node.
- **PASS:** SNIFFER prints `evt=RX proto=LW mtype=Data(Up,Unconf) devaddr=0x01000001
  fport=13 len=…` — proving the frame is on air with the right MHDR/DevAddr/FPort.
  **FAIL:** nothing on the SNIFFER (DUT didn't emit, or wrong RF) — cross-check the DUT
  `evt=QUEUE` and that both radios share freq/BW/SF.

### B2 · LW-FCNT-REBOOT — FCnt monotonic across reboot  *(anti-replay, #3/#4)*
- **Steps:** send several texts (note SNIFFER `fcnt=` values), **power-cycle the DUT**,
  send more.
- **PASS:** post-reboot `fcnt=` **resumes ahead of** the last pre-reboot value (jumps
  forward up to `FCNT_RESERVE`=32, never repeats/decreases). **FAIL:** `fcnt` restarts
  low ⇒ would replay at a real LNS. (Build-flag path is now reboot-safe via the
  DevAddr-keyed NVS counter; pre-#4 it reset to 1 each boot.)

### B3 · LW-FCNT-MOVE — moving a DevAddr between portal rows keeps its counter (#1)
- **HW/Flash:** DUT = `xiao_esp32s3_lwabp` (portal). **Steps:** Device 0 = DevAddr **X**;
  send N texts (note `fcnt=`). Clear Device 0, add the **same DevAddr X** in Device **1**,
  reboot, send again.
- **PASS:** the first post-move `fcnt=` (on the SNIFFER) is **≥** the last value seen for
  X. **FAIL (pre-#1):** it resumes at slot 1's stale/zero counter.

### B4 · LW-RESOLVE — per-source device selection (the M1 resolve ladder)
Priority is **MT-node-specific → protocol → ANY**. Several MT nodes let you exercise the
whole ladder in one MT pass.
- **HW/Flash:** DUT = `xiao_esp32s3_lwabp`, source radio = Meshtastic. **Provision
  (portal), distinct DevAddrs:** Device 0 = *Meshtastic node id* = node A's id → **W**;
  Device 1 = *Source protocol = Meshtastic* → **X**; Device 3 = *Any source* → **Z**.
- **Steps:** send from MT node A, then MT node B (different id), then an RNS/Custom source
  if you have one.
- **PASS (on SNIFFER):** node A → `devaddr=…W` (node match wins), node B → `…X` (protocol
  match), other → `…Z` (ANY) — confirming the sync→proto fix and the priority order.
- **MC pass (optional):** source radio = MeshCore, add Device 2 = *Source protocol =
  MeshCore* → **Y**; an MC node's text → `…Y`. ⚠️ **No MC-node selector exists** — MC
  (and RNS/Custom) match only by *Source protocol* or *Any*, never per node id (only
  `SRC_MT_NODE` is node-specific). With 3 bridges you can run the MT-DUT and an MC-DUT in
  parallel into the one sniffer, or just do two sequential passes on one DUT.

### B5 · LW-MIC-OFFLINE — validate MIC + decrypt without ChirpStack  *(optional but powerful)*
- **Steps:** copy a full frame from the SNIFFER's `evt=LWRAW radio=… raw=<hex>` line and run:
  ```
  pip install cryptography      # once
  python tools/lw-verify.py <raw-hex> 2B7E151628AED2A6ABF7158809CF4F3C D41420B7F5A3C96E1D8204F7B3A65C90
  ```
- **PASS:** `MIC … -> PASS` and `Decrypted … as ASCII : '<your text>'` (for an MT text,
  the bridged body; for the weather path, the raw station bytes). The script prints
  `VERDICT: MIC valid — ChirpStack would ACCEPT this frame`. **FAIL:** `MIC … -> FAIL`
  ⇒ a real LNS would silently drop it (recheck keys / the captured hex).
- This is the off-box equivalent of Tier C's acceptance: a PASS means the crypto is
  correct end-to-end on air, so Tier C should succeed barring NetID/provisioning.

### B6 · LW-TAG-OFFLINE — source tag round-trips (#5)
- **HW/Flash:** DUT = `xiao_esp32s3_lwabp`, device with **"Prepend source tag"** on.
- **Steps:** send from a known MT node; capture `evt=LWRAW raw=…`; run
  `python tools/lw-verify.py <raw-hex> <NwkSKey> <AppSKey> --tagged`.
- **PASS:** `src tag : proto=1(meshtastic) srcId=0x…` followed by the payload — i.e. the
  tag byte is the **protocol enum**, decoded to the right name. **FAIL (pre-#5):** proto
  decodes as `43`/"?" (the raw sync word).
- **Run it for both an MT source (`proto=1 meshtastic`) and an MC source (`proto=2
  meshcore`)** — the #5 bug made *both* decode as "custom", so covering both protocols is
  the real regression check.

### B7 · LW-MC-SOURCE — MeshCore → ABP (second source protocol)
Exercises the MC body-extraction → encode seam, which is distinct from the MT path (same
encoder, different source decoder).
- **HW/Flash:** DUT = `xiao_esp32s3_lwabp`; source radio = **MeshCore**; one ABP device
  (*Any* or *Source protocol = MeshCore*). **Steps:** send an MC text from an MC node.
- **PASS:** DUT `evt=QUEUE … dstproto=LW … cred=nvs`; SNIFFER `evt=RX proto=LW devaddr=…`;
  `lw-verify.py` decrypts the MC body to your text. Confirms both source protocols mint a
  MIC-valid uplink.

---

## Tier C — ChirpStack LNS required (colleague's rig)

**Common setup:** ChirpStack v4 + a gateway in RF range. Per device: device-profile
(region · **MAC 1.0.x** · **ABP** · **Class A** · **ADR off**) → device with **DevAddr
+ NwkSKey + AppSKey + FPort**; tick **"Disable frame-counter validation"** *or* rely on
the bridge's persisted FCnt. Paste `tools/chirpstack-codec.js` into the device-profile
codec. **DevAddr NwkID must match a NetID ChirpStack is configured for** (bench creds
use `01000001` = NwkID 0 = default NetID `0x000000`). Each ABP device is **one
DevAddr** in ChirpStack with the matching keys; reproducing the full B4 matrix
end-to-end means one ChirpStack device per DevAddr — usually unnecessary given the
off-box Tier-B result (see the provisioning split above), so stick to C1 + C2.

**Flash the DUT (erase → upload → monitor):**
```
pio run -e bench_lw_enc -t erase  --upload-port COM13   # C1 (build-flag creds)
pio run -e bench_lw_enc -t upload --upload-port COM13
pio device monitor --port COM13 --baud 115200
```
The ChirpStack device must **match the DUT's creds**: **C1** = DevAddr `01000001` + the
bench keys + FPort 13; **C2/C3** = `xiao_esp32s3_lwabp` (erase first, then provision the
portal device's DevAddr/keys/FPort and paste the codec). Erase before switching env.

### C1 · LW-P1-ACCEPT — build-flag ABP uplink → ChirpStack  *(⭐ acceptance gate)*
- **Flash:** `bench_lw_enc`. Provision DevAddr `01000001` + the bench keys, FPort 13.
  Send an MT text to the DUT's MT radio.
- **PASS:** DUT serial `evt=QUEUE … dstproto=LW … fport=13 cred=flag`; **ChirpStack
  shows the decoded uplink** for device `01000001` (increasing FCnt). **FAIL:** no LNS
  event — wrong MIC, MAC-version, or **DevAddr NwkID vs NetID** (a wrong MIC is silent).

### C2 · LW-P3-WEATHER — Custom raw-LoRa → ABP → ChirpStack  *(⭐ weather-station)*
- **Flash:** `xiao_esp32s3_lwabp`. Set one radio to **Custom** (station's exact
  freq/BW/SF/CR/sync); add an ABP device (Any source or *Source protocol = 4 Custom*).
  **Edit `decodeStation()` in the codec to your real station layout first.** Transmit a
  raw frame from the station (or a 2nd board on that Custom RF).
- **PASS:** `evt=QUEUE … cred=nvs … msg="custom-raw"`; ChirpStack shows the uplink and
  the codec yields the readings. **FAIL:** `drop=no-lw-abp-dest`, `drop=loop-dup`, or
  `drop=lw-payload-overflow` (raw frame > cap).

### C3 · LW-P3-TAG — source tag decoded by the ChirpStack codec  *(#5)*
- **Setup:** device with **"Prepend source tag"** on; `HAS_SOURCE_TAG=true` in the codec.
- **PASS:** ChirpStack decodes `source:{proto,srcId}` with the **correct proto name**
  (meshtastic/meshcore/reticulum/custom/lorawan) + the payload. (Same property B6 proves
  offline.)

### C4 · LW-FCNT-LNS — LNS accepts increasing FCnt / rejects replays
- **Setup:** provision the device **with** frame-counter validation **enabled**.
- **PASS:** consecutive uplinks accepted (monotonic FCnt); after a DUT reboot the LNS
  still accepts (resumed-ahead FCnt). A deliberately replayed/low FCnt is rejected by
  ChirpStack (visible as a dropped/late-frame event). This is the true-LNS counterpart
  to B2/B5.

---

## Offline verifier — `tools/lw-verify.py`
Validates a captured uplink with the keys, off-box (no LNS). Mirrors
`src/LoRaWANCrypto.h` byte-for-byte. `pip install cryptography`, then:
```
python tools/lw-verify.py <PHYPayload-hex> <NwkSKey-hex32> <AppSKey-hex32> [--fcnt-msb N] [--tagged]
```
Prints MHDR/DevAddr(+NwkID)/FCnt/FPort, **MIC PASS/FAIL**, and the decrypted FRMPayload
(ASCII + hex; with `--tagged`, the `[proto][srcId]` split). Use `--fcnt-msb` once the
counter passes 65535 (the on-air FCnt is only the low 16 bits).

---

## Pre-bench review fixes (folded in)
30-agent adversarial review (crypto verified sound; 18 confirmed findings). Fixes on
`dev-ABP-lorawan` (`d98da4b` storage + `1496717` seam; bench-prep `9aaf423`):

| # | Fix | Where | Verified by |
|---|---|---|---|
| 1 | FCnt persisted by **DevAddr** (`fc_<addr>`), not slot index | `LoRaWANConfig.cpp` | B3 |
| 2 | Self-test **gates** the encoder (refuse-emit on KAT FAIL) | `main.cpp` | A1 |
| 3 | **Fail-closed** FCnt: persist+verify before issuing; drop on write fail; saturate near 2³² | `LoRaWANConfig.cpp`, `main.cpp` | B2 |
| 4 | Build-flag fallback FCnt now **reboot-safe** + collision warn | `LoRaWANConfig.cpp`, `main.cpp` | B2 |
| 5 | Source tag stamps the **protocol enum**, not the raw sync word; codec updated | `main.cpp`, `chirpstack-codec.js` | B6 / C3 |
| 6 | Over-cap payload **dropped**, not silently truncated | `main.cpp` | A3 |

## Serial drop / event reference
`evt=QUEUE … dstproto=LW … fcnt=… fport=… cred=flag|nvs tag=0|1` (uplink queued) ·
`evt=RX proto=LW … devaddr=… fcnt=… fport=… len=…` (sniffer capture) ·
`evt=LWRAW … raw=<hex>` (sniffer full frame, `bench_lw_sniffer` only) ·
`drop=lw-selftest-fail` (#2) · `drop=lw-fcnt-fail` + `FCNT_PERSIST_FAIL`/`FCNT_EXHAUSTED` (#3) ·
`drop=lw-payload-overflow` (#6) · `drop=dwell` (P4) · `[lw-warn] build-flag DevAddr … collision` (#4).

## Notes
- A wrong MIC is **silently dropped** by ChirpStack — if a Tier-C uplink never appears,
  suspect keys / MAC version / **DevAddr NwkID vs NetID** before the bridge. (Run B5 to
  rule the crypto in/out off-box first.)
- **Migration:** FCnt moved to `fc_<addr>` NVS keys. A board with old `fc<N>` keys from
  earlier P2 testing has them orphaned — a device resumes at **FCnt 0** on first boot
  after this change. Harmless with "Disable frame-counter validation".
- The encoder is **uplink-only** (Class A, ADR off, no RX windows) by design.
- **Not fixed (non-gating, deferred):** per-DR payload cap (242 B absolute + US dwell
  only); `FPort==0`/`srcSel` portal range guards; a `nextFcnt()` mutex (only if NR grows
  — the T_LORA_QUAD 4-radio line); a portal FCnt display. None affect this bench.
- Deferred (not in v8.4): ABP/OTAA **decode**, dual-LNS crosslink, MT/MC→RNS.
