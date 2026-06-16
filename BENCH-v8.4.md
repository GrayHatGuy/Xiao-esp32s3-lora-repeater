# BENCH-v8.4 — LoRaWAN ABP uplink encoder (P1–P4) bench plan

**Status: code-complete + build-green on `dev-ABP-lorawan`; NOT yet run on hardware.**
This file accumulates the tests to run on the bench. All of P1–P4 is implemented;
on-air verification is the remaining gate before v8.4 ships.

## Prerequisites
- A **ChirpStack v4** instance + a **LoRaWAN gateway** in RF range of the bridge.
- ChirpStack provisioning per device: device-profile (your region · **MAC 1.0.x**
  · **ABP** · **Class A** · **ADR off**) → device with **DevAddr** (private prefix,
  NetID `0x000000`/`0x000001`) + **NwkSKey** + **AppSKey** + the **FPort**; tick
  **"Disable frame-counter validation"** *or* rely on the bridge's persisted FCnt.
  Paste `tools/chirpstack-codec.js` into the device-profile codec.
- Bridge rig: one Xiao dual-SX1262 board; a LoRaWAN radio set to a channel your
  gateway listens on (e.g. US915 903.9 / BW125 / SF7).

## Builds
| Env | Purpose |
|---|---|
| `bench_lw_enc` | encoder + boot self-test + **build-flag** ABP creds (DevAddr `26011B22`), R2=LoRaWAN 903.9. Autosaves (no portal). For P1 + crypto self-test. |
| `xiao_esp32s3_lwabp` | encoder on, **portal**-configured (no autosave, no build-flag keys). For P2/P3 (portal devices, weather station). |
| `xiao_esp32s3` | stock (encoder compiled out) — the do-no-harm control. |

`pio run -e <env> -t upload --upload-port COMx` then `pio device monitor --port COMx --baud 115200`.

---

## Tests

### LW-ENC-SELFTEST — crypto known-answer self-test  *(gate; no RF/LNS needed)*
Flash `bench_lw_enc`. **PASS:** boot prints `[lw-selftest] RFC4493 CMAC … PASS` ×4,
`FRMPayload A_1 keystream : PASS`, `frame … : PASS`, `[lw-selftest] overall : PASS`.

### LW-P1-ACCEPT — build-flag ABP uplink → ChirpStack  *(P1 acceptance)*
Provision ChirpStack with DevAddr `26011B22` + the `bench_lw_enc` keys (NwkSKey
`2B7E…4F3C`, AppSKey `D414…5C90`), FPort 13. Flash `bench_lw_enc`. Send an MT text
to a radio bridged to the LoRaWAN radio.
**PASS:** serial `evt=QUEUE … dstproto=LW … fcnt=… fport=13 cred=flag`; ChirpStack
shows the decoded uplink for device `26011B22`. **FAIL:** no LNS event (wrong MIC ⇒
silent drop — recheck keys / MAC-version / DevAddr prefix).

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
reservation: jumps forward up to 32, never repeats/decreases). ChirpStack accepts
each (increasing FCnt). **FAIL:** fcnt restarts at 0 ⇒ replay/drop at the LNS.

### LW-P2-RESOLVE — per-source device selection
Configure Device 0 = *Meshtastic node id* = your sender's id, Device 1 = *Any
source* (different DevAddr). Send from that MT node, then from a different source.
**PASS:** the matching node's uplinks carry Device 0's DevAddr; others carry
Device 1's. (Verifies the sync→proto fix: MT-node/protocol selectors actually fire.)

### LW-P3-WEATHER — Custom raw-LoRa → ABP  *(weather-station scenario)*
Set one radio to **Custom** with the station's exact RF (freq/BW/SF/CR/sync). Add an
ABP device (Any source or *Source protocol = 4 Custom*). Transmit a raw frame from
the station (or a 2nd board on the same Custom RF).
**PASS:** `evt=QUEUE … dstproto=LW … cred=nvs … msg="custom-raw"`; ChirpStack shows
the uplink; the codec's `decodeStation()` (edit it to your real layout) yields the
readings. **FAIL:** `drop=no-lw-abp-dest` (no LoRaWAN dest) or `drop=loop-dup`.

### LW-P3-TAG — source tag round-trips through the codec
Enable **"Prepend source tag"** on the device; set `HAS_SOURCE_TAG=true` in the
ChirpStack codec.
**PASS:** serial `… tag=1`; ChirpStack decodes `source:{proto,srcId}` + the payload.

### LW-P4-DWELL — US915 per-TX dwell cap
Region = **US**, LoRaWAN radio at a **high SF** (e.g. SF12/BW125) so a routed packet's
ToA > 400 ms. Route a packet to it.
**PASS:** `evt=DROP radio=… drop=dwell toa=<>400> limit=400`; no out-of-spec TX. At
SF7 the same packet sends normally (`evt=TX_START`). EU region: dwell never fires
(duty cycle via `BRIDGE_TX_DUTY_PERCENT`).

### R-DO-NO-HARM — stock build unaffected
Flash `xiao_esp32s3`. **PASS:** normal MT↔MC/RNS/keyless-LW behavior; **no** ABP
QUEUE lines, no `[LoRaWANConfig]`/`[lw-selftest]` output, `MT/MC→LW` still
`drop=no-lw-encoder`. (ABP code compiles out when `BRIDGE_LW_ENCODE` is unset.)

---

## Notes
- A wrong MIC is **silently dropped** by ChirpStack — if an uplink never appears,
  suspect keys / MAC version / DevAddr prefix before the bridge.
- The encoder is **uplink-only** (Class A, ADR off, no RX windows) by design.
- Deferred (not in v8.4): ABP/OTAA **decode**, dual-LNS crosslink, MT/MC→RNS.
