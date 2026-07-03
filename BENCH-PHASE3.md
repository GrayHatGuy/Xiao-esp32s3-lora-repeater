# BENCH-PHASE3.md — LoRaCam Phase 3 (always-on web portal) bench verification

## ⭐ RESULTS 2026-07-03 — ALL GATING TESTS PASS on silicon (cam COM14 + quad-rig-host commander COM19 + Pixel)

**Gating set: 100% PASS.** Boot/AP/camera (P3-10/11/12) · WPA2-not-open + captive auto-pop (P3-20/21) ·
login + wrong-user/pass rejected + route gating + logout (P3-22/23/29) · live MJPEG + :81 sid-gate 401
(P3-24/25) · snap `result=0` + real file + status fields via fetch, video uninterrupted (P3-26/27) · snap
during live stream (P3-28) · **R1 sabotage neutralized** — saved R1=Meshtastic, blob persisted `proto=0`,
boot `Radio1 = None` (P3-30) · config edit persisted (`…-p3`, P3-40) · login change + old-pass reject
(P3-41 login half) · **do-no-harm with portal live**: `g`+`s` round-trips (rssi −66) incl. after the
config-save reboot (P3-50/51/55) · **5/5 C2 commands decoded while the phone streamed video** (P3-52) ·
**messaging both directions** — portal→master `evt=C2TX type=msg` + CAD + TX_DONE (27 B frame), and
commander `m` → cam `evt=C2MSG` → the message auto-appeared on the phone's Messages list (P3-33/P3-60) ·
**pairing lifecycle functional**: unpair → `drop reason=not-whitelisted` (RX'd at −66 but refused, no ACK)
→ re-pair via the portal → `res=0` again (P3-54). Config/pairing pages rendered repeatedly, no stack reset
(P3-35). **Two fixes came OUT of the bench** (both flashed + re-verified): ① the :81 stream ended on one
null frame → frozen `<img>` until refresh — now retries ~3 s before giving up; ② Android kills the MJPEG
connection on screen sleep with no error event — the page now reloads the stream on `visibilitychange`.
**Skipped (owner call):** AP-passphrase change (shares the proven `camportal` save path with the login
change) · soaks/multi-viewer/specials (Wave 5). **Open follow-ups:** first-flash product provisioning =
OPEN AP (gap #2 below) · Wave-5 specials. Pairing UX note: re-creating a deleted slot requires re-typing
the 32-hex PSK ("A new slot needs a 32-hex PSK" — correct fail-closed behavior, briefly confused the
operator). Final sizes: stock **865781 B byte-identical**, cam 978929 B, commander 872193 B.

---

Ordered by **impact toward completion**: each *wave* is batched by setup cost, so one flash / one WiFi
connect / one commander power-on unlocks the most tests before the next, more expensive, setup. Run the waves
top-to-bottom. **Gating** tests (marked ⛔) must pass before Phase 3 is shippable; the rest are recommended.

## Rig

- **CAM** — XIAO ESP32-S3 Sense (OV2640 socketed, no microSD) + hand-wired Wio-SX1262 V1.0 edge radio (R2),
  on **COM14**. Env `bench_camc2`. Flash: `…\platformio.exe run -e bench_camc2 -t upload --upload-port COM14`.
  Serial capture: `python C:\Users\6r4yh\cap.py COM14 <secs> --reset`.
- **COMMANDER** (waves 4–5 only) — a dual-SX1262 bridge on **~COM19** (drifts). Env `bench_camc2_cmdr`. Serial
  keys: `s`=snap `r`=record `x`=stop `g`=get-status `m`=send a T_MSG message to the cam.
- **Client** — a phone + a laptop on the cam's WiFi. Default AP `LoRaCam-XX` / passphrase `loracam-portal`;
  login `admin` / `loracam-admin`.

> **Pre-bench gap status:**
> 1. ✅ **R1 re-activation on save — FIXED 2026-07-03.** `BridgeConfig` now re-applies `LORA_RADIO{1,2}_DISABLE`
>    after every NVS blob load/migration AND before every save (`applyBuildDisables()`), so no stored config can
>    ever boot R1 onto the camera's B2B pins. Stock build verified still byte-identical (865781 B). **P3-30 is
>    now expected to PASS** — bench it to confirm.
> 2. ⚠️ **First-flash provisioning is an OPEN AP — still open (accepted for bench).** A fresh *product* build
>    (`xiao_loracam`, no autosave) hits the old blocking `CaptivePortal::begin()` = `WiFi.softAP(ssid)` with no
>    passphrase and an unauthenticated `/save`, BEFORE the always-on WPA2 portal exists. `bench_camc2` autosave
>    hides this. Affects **P3-43**; fix rides a follow-up.
> 3. ✅ **Commander `T_MSG` hook — ADDED 2026-07-03.** The bench commander now has an `m` serial key (sends a
>    signed "bench msg from commander" T_MSG to the cam) and prints any received T_MSG as
>    `evt=C2MSG … text="…"`. **P3-60 (messaging RX, ring tests) is now benchable.**

---

## WAVE 0 — zero hardware (build + code-review)

- ⛔ **P3-01 stock byte-identical** — `pio run -e xiao_esp32s3` → Flash **865781 B**. *(Already passing.)*
- **P3-02 captive-not-exposed** — code-review: on the cam, only the login-gated `CamPortal` serves :80 once
  configured; the unauthenticated `CaptivePortal::begin()` loop is NOT entered. *(See gap #2 for first-flash.)*
- **P3-03 R1-reactivation review** — confirm gap #1 above. PASS = a saved active R1 cannot reach the radio init
  on a camera build (after the fix).

## WAVE 1 — one fresh/erased cam flash, boot serial only

Erase + upload `bench_camc2` to COM14, then `cap.py COM14 20 --reset` ONCE. From that one capture:

- ⛔ **P3-10 boot lines** — see `[CamPortal] SoftAP "LoRaCam-XX" WPA2 up @ 192.168.4.1` **and**
  `[CamStream] MJPEG server up on :81 (/stream, /jpg)`.
- ⛔ **P3-11 camera still inits** — `[CameraNode] OV2640 ready (sensor PID=0x26) default SVGA` (Phase-3 didn't
  break camera-init ordering).
- **P3-12 default-creds warning** — `[CamPortal] WARNING: portal/WiFi using DEFAULT credentials (user=admin)`.

## WAVE 2 — one WiFi connect + one login (the big unlock; no reboot)

Scan → connect → log in ONCE, then sweep everything below on that session. Do the non-rebooting tests here.

- ⛔ **P3-20 SSID + WPA2** — `LoRaCam-XX` advertised (XX = node-id low byte); joins ONLY with `loracam-portal`
  (lock icon), never open.
- **P3-21 captive redirect** — a bogus host 302s to the portal / login.
- ⛔ **P3-22 login** — right creds → `sid` cookie + Home; **wrong password** and **wrong username** both
  rejected, no cookie.
- ⛔ **P3-23 route gating** — logged out, each of `/ /config /save /cmd /msg /peers /sec` 302s to `/login`.
- ⛔ **P3-24 live video** — Home shows live MJPEG; `/jpg?sid=…` returns one frame.
- ⛔ **P3-25 stream sid-gate** — `:81/stream` and `/jpg` with **missing** or **bad** sid → **401**.
- ⛔ **P3-26 snap + status** — Snap → `result=0` + a real `file=snap_800x600_…`; Status → parsed
  battery/uptime/lastCmd; uptime tracks `millis`.
- ⛔ **P3-27 fetch, no reload** — camera buttons update the result line via fetch without tearing down the live
  `<img>`.
- ⛔ **P3-28 mutex (cam-only)** — a portal Snap during a live stream: both succeed, no "camera busy" storm, no
  freeze.
- **P3-29 logout** — `/logout` invalidates the session; the old sid then 401s on the stream.
- **P3-31 config render + gate** — `/config` renders the full 4-radio captive form with live values; R1 shows
  **None**; `/config`+`/save` are login-gated; no-reboot validations fire (all-radios-None guard, bad freq).
- **P3-32 pairing UI** — `/peers` login-gated; **add** a new peer (id+PSK) → saved; field rejections (bad
  8-hex id, blank key on a new slot, wrong-length PSK); toggle **Primary**; **delete** a slot.
- **P3-33 messaging UI (send half)** — empty whitelist hides the send form; after a peer is added, sending via
  `/msg` logs `evt=C2TX … type=msg`; sending to a non-whitelisted id → `evt=C2DROP reason=no-such-peer`.
- **P3-34 XSS-inert** — a message body `<script>alert(1)</script>` renders as literal escaped text (no alert).
- **P3-35 big-page stack** — loading `/config` and `/peers` repeatedly does not trip a CamPortal-task stack
  overflow / reset.
- **P3-36 two-session cap** — a 3rd login (spaced >1 s) evicts one slot; the evicted client is cleanly 302'd
  to `/login` on its next request; the two live ones keep working.

## WAVE 3 — reboot-cost cam-only (batch the reboots)

- ⛔ **P3-30 R1 stays disabled after save** — benign field edit → Save → reboot → R1 still `PROTO_NONE`
  (never fights the camera). Fixed 2026-07-03 (`applyBuildDisables()`) — bench to confirm. For the adversarial
  arm, set R1 to a protocol in the form, Save, and verify the boot log still shows `Radio1 protocol = None`.
- **P3-40 config persists** — the edited field survives the reboot (BridgeConfig v5 round-trip).
- **P3-41 security chain** — change login (no reboot) → re-login with the new pass (old fails); change AP
  passphrase → reboot → reconnect on the NEW pass (old fails); the default-creds banner clears and stays clear.
  Reject a <8-char AP passphrase (no reboot, AP stays up).
- **P3-42 pairing persists** — a saved peer survives a power-cycle (`debugDump` shows the id).

## WAVE 4 — power on the commander once (~COM19), all two-board tests

- ⛔ **P3-50 do-no-harm round-trip** — with WiFi + both servers live, `g` from the commander still →
  `evt=C2CMD … res=0` (authenticated + executed) + ACK.
- ⛔ **P3-51 capture with portal up** — `s` still captures a real JPEG with the portal running.
- ⛔ **P3-52 RX not corrupted by WiFi** — fire `g` **5×** while a live MJPEG stream runs → **5/5** decode,
  rssi in the ~-58…-61 baseline.
- **P3-53 C2-snap vs stream (starvation)** — with 1–2 stream viewers up, fire `s` 10× → ≥9/10 give `res=0`
  (an occasional "capture skipped" is tolerable; repeated `res=4` = stream starves the snap mutex).
- ⛔ **P3-54 pairing functional** — delete the commander via `/peers` → its `g` is dropped
  `reason=not-whitelisted`; re-pair the id+key via `/peers` → `g` commands again (`res=0`); a wrong PSK →
  `reason=bad-mic`; a blank-key edit of the same id keeps the stored PSK (still commands).
- **P3-55 edge radio survives save** — after a config save+reboot, `g` still round-trips (persistence didn't
  break the control channel).

## WAVE 5 — special builds / extra clients / soaks (last)

- **P3-43 first-flash AP** ⚠️ — on a clean-NVS `xiao_loracam` (NOT bench), is provisioning open + unauthenticated?
  (Gap #2 — fix or consciously accept.)
- **P3-60 messaging RX (master→cam)** — commander `m` key → cam `evt=C2MSG` → the message appears in the portal
  Messages list (newest first; 13 sends wrap the 12-slot ring). **UNBLOCKED 2026-07-03** (commander hook added);
  runnable in Wave 4 while the commander is up.
- **P3-61 forged sender dropped** — a wrong-key `T_MSG` is dropped before `storeMessage` (needs a hostile TX).
- **P3-62 stream soak** — 15–30 min continuous stream + periodic snaps + `g`: no reset / watchdog / AP-drop /
  RX decay.
- **P3-63 session cross-task race** — hammer `/login`+`/logout` on :80 while a client loops `:81/jpg`: no
  crash, no frame served to a dead sid. (Code-review: `s_sess` has no lock across the two tasks.)
- **P3-64 NVS edge cases** (code-review / probe builds) — short-AP-pass forces-open at `begin()`; a
  `camportal` persist failure must not silently revert to default creds; per-frame `rx_<id>` NVS write
  amplification under chatty valid traffic.

---

## Gating set (must pass to ship Phase 3)

P3-01 · P3-10/11 · P3-20 · P3-22/23 · P3-24/25 · P3-26/27/28 · **P3-30 (R1 safety)** · P3-50/51/52 · P3-54.

## Definition of done

1. **Do-no-harm holds** — stock 865781 B; Phase-1 C2 round-trip + Phase-2a capture still work with WiFi + both
   servers up; 5/5 commands decode under a live stream; a save can't reactivate R1.
2. **The portal works** (the headline) — WPA2 AP + captive DNS; login + route-gating; live video + /jpg +
   sid-gate; snap + status via fetch without tearing the stream; the camera mutex serializes snap-vs-stream.
3. **Features pass** — config render/edit/persist/validate + gate; pairing add/delete/primary/persist +
   field-validation + the functional pair/unpair proof; security login + AP-pass change persist; messaging
   send (TX) + forged-drop.
4. **Stability holds** — a 15–30 min stream + snaps + commands with no reset / AP-drop / RX decay; the two
   biggest pages render without a stack overflow.

## Open decisions for the owner

- ~~Fix R1-reactivation before benching?~~ **DONE 2026-07-03** (`applyBuildDisables()` in BridgeConfig).
- ~~Add the commander `T_MSG` hook?~~ **DONE 2026-07-03** (`m` key + inbound T_MSG print).
- **Secure first-flash provisioning (gap #2)?** A fresh product cam is provisioned over an open AP today.
  Rides a follow-up cycle unless the owner wants it before the Phase-3 commit.
