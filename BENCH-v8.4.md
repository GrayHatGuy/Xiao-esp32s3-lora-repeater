# BENCH-v8.4 — LoRaWAN ABP uplink encoder (P1–P4) bench plan

**Status: code-complete + build-green on `dev-ABP-lorawan`; NOT yet run on hardware.**
All of P1–P4 is implemented; on-air verification is the remaining gate before v8.4
ships. A 30-agent adversarial pre-bench review (2026-06-15) verified the crypto
**sound** (RFC4493 CMAC / B0 / A_i / FRMPayload / 1.0.x MIC byte-correct, confirmed
by an independent recompute) and produced six fixes (#1–#6) now folded in — see
**[Pre-bench review fixes](#pre-bench-review-fixes-folded-in)** below. The crypto
result means a single-device **LW-P1-ACCEPT** is expected to pass as soon as the
LNS is provisioned; the fixes harden the multi-device / redeploy / tagged paths.

## Prerequisites
- A **ChirpStack v4** instance + a **LoRaWAN gateway** in RF range of the bridge.
- ChirpStack provisioning per device: device-profile (your region · **MAC 1.0.x**
  · **ABP** · **Class A** · **ADR off**) → device with **DevAddr** + **NwkSKey** +
  **AppSKey** + the **FPort**; tick **"Disable frame-counter validation"** *or* rely
  on the bridge's persisted FCnt. Paste `tools/chirpstack-codec.js` into the
  device-profile codec.
  - ⚠️ **DevAddr NwkID must match the LNS NetID.** A type-0 DevAddr's top 7 bits are
    the NwkID; ChirpStack only ingests uplinks whose NwkID matches a NetID it is
    configured for, else it **silently drops** them (no device match, no error). The
    bench creds use **`01000001`** (NwkID 0), which matches ChirpStack's default
    private **NetID `0x000000`**. If your ChirpStack uses a different NetID, pick a
    DevAddr in its range. This is the **#1 suspect after a wrong MIC** when an
    uplink never appears.
- Bridge rig: one Xiao dual-SX1262 board; a LoRaWAN radio set to a channel your
  gateway listens on (the bench envs use US915 903.9 / BW125 / SF7).

## Builds
| Env | Purpose |
|---|---|
| `bench_lw_enc` | encoder + boot self-test + **build-flag** ABP creds (DevAddr `01000001`, **REGION=US**), R2=LoRaWAN 903.9/BW125/SF7. Autosaves (no portal). For LW-ENC-SELFTEST + LW-P1-ACCEPT. |
| `bench_lw_enc_dwell` | as `bench_lw_enc` but **SF12** (ToA ~1.5 s > 400 ms) — for **LW-P4-DWELL**. |
| `xiao_esp32s3_lwabp` | encoder on, **portal**-configured (no autosave, no build-flag keys). For P2/P3 (portal devices, weather station). |
| `xiao_esp32s3` | stock (encoder compiled out) — the do-no-harm control. |

`pio run -e <env> -t upload --upload-port COMx` then `pio device monitor --port COMx --baud 115200`.

---

## Tests

### LW-ENC-SELFTEST — crypto known-answer self-test  *(gate; no RF/LNS needed)*
Flash `bench_lw_enc`. **PASS:** boot prints `[lw-selftest] RFC4493 CMAC … PASS` ×4,
`FRMPayload A_1 keystream : PASS`, `frame … : PASS`, `[lw-selftest] overall : PASS`.
**This now gates the encoder (#2):** on a FAIL the bridge keeps running but refuses
to emit any ABP uplink (`evt=DROP … drop=lw-selftest-fail`) instead of putting a
wrong-MIC frame on air — so a broken-crypto build fails loudly here, not silently
at the LNS.

### LW-P1-ACCEPT — build-flag ABP uplink → ChirpStack  *(P1 acceptance, ⭐ gate)*
Provision ChirpStack with DevAddr `01000001` + the `bench_lw_enc` keys (NwkSKey
`2B7E…4F3C`, AppSKey `D414…5C90`), FPort 13. Flash `bench_lw_enc`. Send an MT text
to a radio bridged to the LoRaWAN radio.
**PASS:** serial `evt=QUEUE … dstproto=LW … fcnt=… fport=13 cred=flag`; ChirpStack
shows the decoded uplink for device `01000001`. **FAIL:** no LNS event (wrong MIC ⇒
silent drop — recheck keys / MAC-version / **DevAddr NwkID vs NetID**).

### LW-P2-PORTAL — portal ABP device config persists
Flash `xiao_esp32s3_lwabp` → boots into the captive portal. In **"LoRaWAN ABP
devices"** set Device 0: Enabled, *Any source*, DevAddr + NwkSKey + AppSKey +
FPort. Set a radio to LoRaWAN. **Save & reboot.**
**PASS:** boot prints `[LoRaWANConfig] loaded … anyConfigured=1` and a `dev0 …
devaddr=…` line; re-entering the portal shows the saved values. Route a mesh text
to the LoRaWAN radio → `evt=QUEUE … cred=nvs`.

### LW-P2-FCNT — FCnt is monotonic across reboot  *(anti-replay)*
With LW-P2-PORTAL configured, send several uplinks (note the `fcnt=` values),
**power-cycle**, send more.
**PASS:** post-reboot `fcnt=` **resumes ahead of** the last pre-reboot value (block
reservation: jumps forward up to `FCNT_RESERVE`=32, never repeats/decreases).
ChirpStack accepts each (increasing FCnt). **FAIL:** fcnt restarts at 0 ⇒ replay/drop.
Counters are now persisted **keyed by DevAddr** (`fc_<addr>`), and a reservation is
written + verified **before** its first value is issued (#1/#3). If a flash write
fails the uplink is dropped (`evt=DROP … drop=lw-fcnt-fail` + a `FCNT_PERSIST_FAIL`
line) rather than emitting a non-durable counter.

### LW-P2-FCNT-MOVE — moving a DevAddr between rows keeps its counter  *(#1, anti-replay)*
Configure Device 0 = DevAddr **X**, send N uplinks (note `fcnt=`). Clear Device 0,
add the **same DevAddr X** in Device **1**. Send again.
**PASS:** the first post-move `fcnt=` is **≥** the last value the LNS saw for X (the
counter follows the identity, not the slot). **FAIL (pre-#1 behavior):** it resumes
at slot 1's stale/zero counter and ChirpStack silently drops the replays.

### LW-P2-RESOLVE — per-source device selection
Configure Device 0 = *Meshtastic node id* = your sender's id, Device 1 = *Any
source* (different DevAddr). Send from that MT node, then from a different source.
**PASS:** the matching node's uplinks carry Device 0's DevAddr; others carry
Device 1's. (Verifies the sync→proto fix: MT-node/protocol selectors actually fire.)

### LW-P3-WEATHER — Custom raw-LoRa → ABP  *(weather-station scenario, ⭐ gate)*
Set one radio to **Custom** with the station's exact RF (freq/BW/SF/CR/sync). Add an
ABP device (Any source or *Source protocol = 4 Custom*). Transmit a raw frame from
the station (or a 2nd board on the same Custom RF). **Edit `decodeStation()` in the
codec to your real station byte layout first** (the shipped one is an example).
**PASS:** `evt=QUEUE … dstproto=LW … cred=nvs … msg="custom-raw"`; ChirpStack shows
the uplink; the codec's `decodeStation()` yields the readings. **FAIL:**
`drop=no-lw-abp-dest` (no LoRaWAN dest), `drop=loop-dup`, or — for an oversize raw
frame (>242 B, or >237 B with the source tag) — `drop=lw-payload-overflow` (#6, the
frame is dropped, **not** silently truncated).

### LW-P3-TAG — source tag round-trips through the codec  *(#5)*
Enable **"Prepend source tag"** on the device; set `HAS_SOURCE_TAG=true` in the
ChirpStack codec.
**PASS:** serial `… tag=1`; ChirpStack decodes `source:{proto,srcId}` + the payload,
and **`proto` is the correct name** ("meshtastic"/"meshcore"/"reticulum"/"custom"/
"lorawan"). The tag byte now carries the protocol enum (1/2/3/4/5), not the raw LoRa
sync word — pre-#5 every tagged MT/MC frame mis-decoded as "custom".

### LW-P4-DWELL — US915 per-TX dwell cap
Flash **`bench_lw_enc_dwell`** (US, SF12) and route a packet to the LoRaWAN radio.
**PASS:** `evt=DROP radio=… drop=dwell toa=<~1480> limit=400`; no out-of-spec TX. On
`bench_lw_enc` (SF7) the same packet sends normally. EU region: dwell never fires
(duty cycle via `BRIDGE_TX_DUTY_PERCENT`). Note the dwell gate also applies to v8.3
keyless LW↔LW relay when REGION=US (an over-400 ms captured frame logs `drop=dwell`
instead of relaying verbatim — intended, FCC-correct).

### R-DO-NO-HARM — stock build unaffected
Flash `xiao_esp32s3`. **PASS:** normal MT↔MC/RNS/keyless-LW behavior; **no** ABP
QUEUE lines, no `[LoRaWANConfig]`/`[lw-selftest]` output, `MT/MC→LW` still
`drop=no-lw-encoder`. (All ABP/encoder code compiles out when `BRIDGE_LW_ENCODE` is
unset; the stock binary differs only by ~112 B of never-called code.)

---

## Pre-bench review fixes (folded in)
30-agent adversarial review (crypto verified sound; 18 confirmed findings). Fixes on
`dev-ABP-lorawan` (`d98da4b` storage + `1496717` seam):

| # | Fix | Where |
|---|---|---|
| 1 | FCnt persisted by **DevAddr** (`fc_<addr>`), not slot index → survives portal row moves | `LoRaWANConfig.cpp` |
| 2 | Self-test **gates** the encoder (refuse-emit on KAT FAIL) | `main.cpp` |
| 3 | **Fail-closed** FCnt: persist+verify before issuing; drop on write failure; saturate near 2³² | `LoRaWANConfig.cpp`, `main.cpp` |
| 4 | Build-flag fallback FCnt now **reboot-safe** (was in-RAM, reset to 0/boot) + collision warn | `LoRaWANConfig.cpp`, `main.cpp` |
| 5 | Source tag stamps the **protocol enum**, not the raw sync word; codec updated | `main.cpp`, `chirpstack-codec.js` |
| 6 | Over-cap payload **dropped**, not silently truncated | `main.cpp` |

## New / changed serial drop reasons
`drop=lw-selftest-fail` (#2) · `drop=lw-fcnt-fail` + `FCNT_PERSIST_FAIL` /
`FCNT_EXHAUSTED` (#3) · `drop=lw-payload-overflow` (#6) · `[lw-warn] build-flag
DevAddr … collision` (#4).

## Notes
- **Migration:** FCnt moved to `fc_<addr>` NVS keys. Any board with old `fc<N>`
  slot keys from earlier P2 testing has them orphaned — a device resumes at **FCnt 0**
  on first boot after this change. Harmless with "Disable frame-counter validation"
  (above); otherwise let the LNS re-sync or reset the device's counter.
- A wrong MIC is **silently dropped** by ChirpStack — if an uplink never appears,
  suspect keys / MAC version / **DevAddr NwkID vs NetID** before the bridge.
- The encoder is **uplink-only** (Class A, ADR off, no RX windows) by design.
- **Not fixed (non-gating, deferred):** per-DR payload cap (only the 242 B absolute
  cap + the US dwell gate apply); `FPort==0`/`srcSel` portal range guards; a mutex on
  `nextFcnt()` (only needed if NR grows, e.g. the T_LORA_QUAD 4-radio line); a portal
  FCnt display. None affect the 2-radio bench.
- Deferred (not in v8.4): ABP/OTAA **decode**, dual-LNS crosslink, MT/MC→RNS.
