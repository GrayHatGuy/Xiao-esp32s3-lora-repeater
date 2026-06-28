# LoRaCam — Spec (DRAFT, for agreement)

**Status:** DRAFT — core decisions **Q1–Q4 LOCKED 2026-06-28** (see §9). Nothing built. No code on shared paths yet.
**Branch:** `lora_cam_xiao` (off `main` @ `24528ad` = v9.0). Remote unchanged (`GrayHatGuy/Xiao-esp32s3-lora-repeater`).
**Date:** 2026-06-28.
**Foundation:** the v9.0 four-radio repeater firmware in this clone. HW feasibility:
`C:\Users\6r4yh\XIAO-Sense-Wio-SX1262-Compatibility-Handoff.md`. Camera reference:
`…\Projects\CameraWebServer_for_esp-arduino_3.0.x-seeed_xiao_esp32s3` (read-only).

> This doc is the working agreement. Sections marked **[DECISION]** need owner sign-off; **[PROPOSED]**
> are my recommendations to confirm or veto. Grounded in a 6-agent read of the repeater + camera codebases.

---

## 1. Goal

A XIAO ESP32-S3 **Sense** (OV2640 camera + mic + microSD on the rear B2B 40-pin) + a **perimeter-pin
Wio-SX1262** edge radio, making a **LoRa-commanded camera**:

- **Downlink C2** over LoRa, **encrypted + sender-whitelisted + replay-protected**: `start-capture`,
  `record N-seconds`, `stop`, `set-quality`, `set-framerate`, `get-status`.
- **Uplink** over LoRa: ACK with the media **filename** / command result, asynchronous **events**, and a
  periodic **beacon/heartbeat** (battery %, SD free, uptime, last-cmd).
- **WiFi web portal** combining **live MJPEG video + config + a LoRa-messaging utility** (the messaging
  utility can inject Custom / Meshtastic / MeshCore / Reticulum frames out the radio).
- **Backwards-compatible** with the shipped repeater: the stock dual-radio and quad host+co-proc builds stay
  **byte-identical**.
- A dormant **plug for future BLE**.

**Scope guard (correction to the brief):** media files travel over **WiFi only**, never LoRa. LoRa carries
C2 + filename/ACK/telemetry only — full-frame imagery over LoRa is infeasible (airtime/duty/dwell). The
"reply with media filename" model already implies this; the portal serves the actual media.

---

## 1A. Roles & topology — master ↔ edge, with standalone

LoRaCam is an **edge/perimeter node of the repeater mesh**, not an island. Three roles, one codebase:

- **Edge (the cam):** speaks the Custom `0x33` C2 channel. Because a repeater **raw-repeats** Custom frames
  (`rawRepeatForDest`), a commander → repeater(s) → cam path works out of the box — the cam is reachable
  *through* the existing bridge mesh, extending C2 range with **no new code**.
- **Master (a repeater):** gains a **C2 commander** capability — mint signed commands, receive the cam's
  ACK/telemetry, surfaced in the repeater's web portal as a "LoRaCam control / messaging" panel. This is the
  "repeaters have CnC access for the cam" requirement. Intended to ship **dormant in the standard repeater
  build** (like the ABP encoder — proven in a flag/variant during dev, folded in at release), activated by
  adding a cam + its key in the portal. *(see Decision A)*
- **Standalone (the cam, always):** the cam always has its own SoftAP portal (config + live video + local
  messaging) and runs autonomously (beacon, local capture) **with or without a master present**. The master
  is additive, never required — "slave of the master, but standalone-capable."

**Master ↔ slave trust:** the cam's `c2auth` whitelist holds its commanders; a master repeater is one entry
(a shared per-sender PSK, provisioned out-of-band on both ends). The cam obeys *any* whitelisted commander,
ACKs **unicast to whoever sent the command**, and **broadcasts beacons** that any in-range whitelisted master
hears (so a mesh of repeaters all see it). Optionally one entry is flagged "primary master" for unsolicited
telemetry. *(see Decision B)*

**Shared codec:** `CamC2.{h,cpp}` implements **both halves** — the *responder* (cam: RX → auth → execute →
ACK + beacon) and the *commander* (repeater: TX signed command → RX ACK/telemetry → display). One module, two
roles, flag-gated. So Phase 1 now delivers both sides; the bench "commander" is simply a repeater build, not a
throwaway tool.

**Two deployment modes, one firmware (chosen at provisioning — runtime, NOT a build fork):**
- **Standalone / self-mastered:** the cam's own SoftAP portal is the master — full local CnC (snap, record,
  stop, quality, framerate, live view) + config, **no repeater required**. Works out of the box; the `c2auth`
  whitelist may stay empty.
- **Paired with a repeater (the primary intent):** "pair" the cam = provision the repeater as a whitelisted
  LoRa commander on the cam (and add the cam on the repeater) with a shared key. The repeater becomes the LoRa
  master; the cam's local portal stays available.

Both surfaces **coexist**: the cam ALWAYS has local portal CnC; pairing only ADDS repeater control over LoRa.
A fresh cam boots standalone-capable with a prominent "Pair with a repeater" action. **Implementation
invariant:** camera actuation is a shared `executeCommand()` — the local portal calls it **directly** (local
trust), the LoRa C2 handler calls it **only after** auth → whitelist → replay. Both command surfaces converge
on one actuation path.

---

## 2. Why this fits the repeater cleanly (≈70% reuse)

The repeater's architecture happens to provide almost every seam this needs:

| Need | Existing mechanism (file:line) |
|---|---|
| Receive + decode a command frame | `ingestAndFanout()` single RX chokepoint; **`PROTO_CUSTOM` branch** `main.cpp:1133` |
| Send a reply / ACK / beacon | build bytes → `g_routeQ[R2].push()` (model: `enqueueAbpUplink` `main.cpp:809`) |
| Encrypt + authenticate | `LoRaWANCrypto.h` — AES-128 ECB/CTR + KAT-verified RFC-4493 **AES-CMAC** (`aesEcb` L43, `aesCmac` L52) |
| Per-sender key table + reboot-safe counter | clone `LoRaWANConfig` (`Device` table + `resolve()` + block-reserve FCnt) into a new NVS namespace |
| Loop/echo guard | `DedupCache` (FNV-1a, 60 s TTL) |
| Radio + SD on one SPI bus | `spiMutex` already brackets every radio SPI op (`WioSX1262.cpp`); SD just joins the same mutex |
| Do-no-harm feature gating | proven 3× (`BRIDGE_LW_ENCODE`, R3/R4 `PROTO_NONE`, ABP) — a build flag + `#if` |

What does **not** exist yet and is genuinely new: (a) an **RX-side authenticate→whitelist→replay-check seam**
(the bridge today only decode-to-log/re-encode — it never gates on *who* sent a packet), and (b) an
**always-on, non-blocking web portal** (today's portal is blocking, AP-only, boot-window-only, and
`ESP.restart()`s on Save).

---

## 3. Hardware (from the feasibility handoff, confirmed against the firmware)

- **Radio = edge/perimeter Wio-SX1262 V1.0** on the R2 pin map: NSS=GPIO5(D4), DIO1=GPIO2(D1), RST=GPIO3(D2),
  BUSY=GPIO4(D3), RF_SW=GPIO6(D5); SPI SCK=GPIO7/D8, MISO=GPIO8/D9, MOSI=GPIO9/D10. TCXO 1.8 V (DIO3),
  DIO2-as-RF-switch (already firmware default). **TX capped at +20 dBm** (3V3 rail can't sustain +22).
- **Camera/mic/SD = Sense daughterboard on the B2B 40-pin** (internal GPIO10–48). **microSD CS = GPIO21**,
  bus shared with the radio (SCK/MISO/MOSI on D8/D9/D10).
- **⚠️ R1 (the firmware's B2B radio, GPIO38–42) collides head-on with the camera/mic.** A LoRaCam build
  **must run R1 = `PROTO_NONE`** and use only the edge R2 radio.
- **⚠️ Mechanical (the real blocker):** the Wio edge board and the Sense daughterboard both want the XIAO's
  underside. **Prototype with flying leads** (handoff §7) — "works on the bench" ≠ "assembles flush."
- **⚠️ Brown-out:** camera-init inrush + TX ramp + SD write on the 700 mA rail can reset the S3. Needs **bulk
  caps on 3V3** + software staggering. **Antenna (u.FL) must be fitted before first boot** — the beacon TXes
  autonomously.
- **⚠️ Camera LED-flash must be disabled** — the example mis-defines it on GPIO21/22, and **GPIO21 is the SD
  chip-select**; left on, LEDC fights the SD-CS.

---

## 4. Design decisions

### 4.1 Backwards-compat mechanism — **[PROPOSED: new build env + flag]**
New `[env:xiao_loracam]` extends `xiao_esp32s3` with `-DBRIDGE_ROLE_CAMERA=1`. All camera/C2/web code lives in
**new files** (`CamC2.{h,cpp}`, `CameraNode.{h,cpp}`, `CameraWeb.{h,cpp}`) compiled only under that flag;
heavier `lib_deps` (esp32-camera, web stack) and any custom partition table exist only in that env. The **only**
touch to shared code is one guarded hook — `#if BRIDGE_ROLE_CAMERA … consumeCameraCommand(...) … #endif` —
in the `PROTO_CUSTOM` branch of `ingestAndFanout()`. New config goes in **new NVS namespaces** (`c2auth` /
`loracam`), never a `BridgeConfig` v5→v6 bump. **Verification gate each commit:** `pio run -e xiao_esp32s3`
(+ `_v1_1` + both coproc envs) must build byte-identical. (Matches the repo's proven do-no-harm pattern.)

### 4.2 C2 transport — **[PROPOSED: binary on PROTO_CUSTOM]**
Commands ride a binary frame on a **`PROTO_CUSTOM` radio (sync word 0x33)** — already a first-class dispatcher
branch with dedup; portal/BridgeConfig already expose Custom + sync word + RF plan. A binary frame is the
natural carrier for a CMAC tag + counter, and an unmodified repeater can **raw-repeat** the frames to extend
C2 range. A Meshtastic/MeshCore *text* command path is rejected as the primary channel because those can only
ever be authenticated by the shared *group* channel key (no per-sender identity) — unacceptable for a physical
actuator. **[DECISION — see Q]** optionally add a text path **later, restricted to non-actuating status reads**.

### 4.3 Crypto / auth — **[PROPOSED: reuse LoRaWANCrypto, encrypt-then-MAC]**
AES-128-CTR **encrypt-then-CMAC** over `[ver‖senderId‖seq‖cmd‖ciphertext]`, all in-tree, zero new deps:
- **Confidentiality:** AES-CTR, nonce = `senderId‖seq`.
- **Auth + whitelist:** **8-byte truncated AES-CMAC** under the sender's key — a valid tag *is* the whitelist
  (only key-holders forge it). 8 bytes (not LoRaWAN's 4) for a physical actuator's offline-forgery margin.
- **Replay:** monotonic `seq` inside the MAC'd region; RX rejects `seq ≤ highest-seen` per sender, persisted
  with the `LoRaWANConfig` block-reserve pattern **inverted** (store highest-seen, reject ≤). `DedupCache` stays
  as a cheap secondary echo guard, *not* the anti-replay primitive.
- **Fail-closed:** whole handler gated by a `g_c2CryptoOk` boot self-test (model: `g_lwCryptoOk`). Constant-time
  tag compare (none exists today — add one). ACK/beacon signed the same way with the device's own TX counter.

**[LOCKED Q2]** key model = **per-sender PSK + allowlist**: each commander has its own 16-byte key in a
bounded table (clone of `LoRaWANConfig::Device` in the `c2auth` NVS namespace), with its own RX replay
high-water. A frame's `senderId` selects the key; unknown sender or bad MIC ⇒ silent drop. Revocable per
commander; the ACK is signed under that commander's key so only it can verify the reply.

### 4.4 Web stack — **[PROPOSED: two servers, reuse the config form]** · WiFi = **[LOCKED Q1: SoftAP]**
WiFi is **SoftAP-only** (field-portable, no infrastructure) — so no STA credential store / connect-retry logic
is needed; the portal is reached at the camera's own AP (reuse the repeater's `LoRa-Bridge-XX`-style SSID).
Keep the repeater's Arduino-`WebServer` config form (`renderForm`/`handleSave` — ~12 KB of proven HTML/JS +
every `BridgeConfig` setter) but **lift it out of the blocking boot-only `CaptivePortal::begin()` into a
persistent non-blocking task on :80**. Add **`esp_http_server`** (the camera example's stack, purpose-built for
MJPEG) for `/capture` + `/stream` on **:8080/:8081**. Two stacks coexist fine in RAM; the combined page links
across the two origins. Consolidating onto ESPAsyncWebServer is deferred (avoids a pinned-dep gamble) unless
two-stack RAM proves tight. The **non-blocking rewrite is unavoidable in every option** — it's the cost of an
always-on portal, not extra.

### 4.5 Concurrency — **[PROPOSED: keep v9.0 model + high-current interlock]**
Keep the v9.0 RX-priority core-pinned `radioTask` (RX is ISR-flagged and re-armed *before* decode → never
starved). Run camera + web tasks on the **other core**. **Modify the stock MJPEG `stream_handler`** (a tight
non-yielding `while(true)`) to `vTaskDelay`/throttle at **lower priority** than radio RX. Add a lightweight
**interlock**: never start a TX during `esp_camera_init`; defer SD writes out of the TX window; throttle the
stream during capture/record and TX. (Software staggering + bulk caps together cover the brown-out case.)

### 4.6 Storage / SD — **[PROPOSED: microSD on shared bus + mutex, MVP may run SD-less]**
Keep **J3 intact**; pass the existing `spiMutex` into the SD subsystem and wrap every `SD.*` call exactly as
`WioSX1262` does (it deliberately releases the bus during on-air time, so SD writes slot into the gaps).
Reconcile SD-lib vs radio SCK/MOSI to one consistent mapping. Disable the GPIO21/22 LED-flash. **[LOCKED Q3:
microSD]** — persistent media on the Sense microSD is the end state (filename/ACK/"SD full" semantics need it).
The Phase-1 C2 MVP runs SD-less (filename = stub) only because no camera is attached yet; SD lands in Phase 2.

### 4.7 BLE future plug — **[PROPOSED: no-op stub, dormant]**
A `-DLORACAM_BLE` flag + an empty `Bluetooth.{h,cpp}` no-op interface, dormant by default (mirrors
`BRIDGE_LW_ENCODE`). **Do not link NimBLE or build a GATT yet** — WiFi+BLE coexistence is the RAM-heavy case
most likely to blow the partition budget, for a non-requirement. Reserve the seam at zero cost.

---

## 5. C2 wire frame (proposed)

```
[ magic/ver : 1 ]      0x?? (version + a LoRaCam magic nibble)
[ type      : 1 ]      CMD | ACK | EVT | BEACON
[ senderId  : N ]      commander identity (selects the auth key + replay counter)
[ seq       : 2 ]      LE monotonic, inside the MAC'd region (anti-replay)
[ payload   : var]     AES-CTR ciphertext of the command/status fields
[ cmac tag  : 8 ]      truncated AES-CMAC over [ver‖type‖senderId‖seq‖ciphertext]
```
Whole frame fits in one small LoRa packet (< ~60 B), far under `LORA_MAX_PACKET=256`. Commands: `START_CAPTURE`,
`RECORD(dur)`, `STOP`, `SET_QUALITY(q)`, `SET_FRAMERATE/SIZE(x)`, `GET_STATUS`. ACK carries `{result, filename}`.
BEACON carries `{battery%, sd_free, uptime, last_cmd_result}`.

---

## 6. Phasing (proposed)

- **Phase 0 — HW proof + skeleton (gates all).** Flying-leads prototype; confirm Wio V1.0; fit antenna; bulk
  caps; stand up `[env:xiao_loracam]` (R1=NONE, R2=CUSTOM 0x33, TX=20). **Gate:** stock envs still byte-identical.
- **Phase 1 — C2 over LoRa MVP, no camera.** `CamC2.{h,cpp}`; the auth→whitelist→replay seam; ACK + beacon.
  Bench-test on the existing 3-board rig (a 2nd XIAO mints commands) **without a camera** — proves the security
  core in isolation.
- **Phase 2 — Camera + SD, command-driven (no web).** `esp_camera_init` (PSRAM, bounded fb); JPEG snap + N-sec
  clip → microSD (shared mutex); wire C2 commands to real capture; ACK carries the real filename; interlock +
  heartbeat with battery/SD-free.
- **Phase 3 — Always-on WiFi portal.** Lift the config form into a persistent task; `esp_http_server`
  /capture+/stream on 8080/8081; throttle/yield the stream; WiFi creds + camera defaults in `loracam` NVS;
  LoRa-messaging utility page (inject via `g_routeQ`).
- **Phase 4 — Hardening + optional.** Brown-out soak; custom partition table if needed (verify OTA); BLE no-op
  stub; optional read-only text status bridge; user-manual; update `CLAUDE.md`.

---

## 7. Top risks
1. **Mechanical stack-up** (Wio edge board vs Sense daughterboard) — prototype flying-leads first.
2. **Brown-out** under camera-init + TX + SD on 700 mA — caps + staggering.
3. **R1/B2B vs camera pin collision** — force R1=`PROTO_NONE`; guard the portal from re-enabling it.
4. **Portal is a rewrite, not a route** — blocking/boot-only → always-on non-blocking; stream loop must yield.
5. **RX auth seam is net-new** — get encrypt-then-MAC ordering right; invert the counter; constant-time compare; fail closed.
6. **Memory/flash/partition squeeze** — camera + WiFi + RadioLib + mbedtls; bound framesize/fb; no NimBLE now.
7. **Arduino-ESP32 core mismatch** — camera example assumes 3.0.x; repeater pinned 2.0.17; port camera code to the repeater's core.

---

## 8. Backwards-compat guarantee
With `BRIDGE_ROLE_CAMERA` undefined: new files/libs/partition don't exist in the image; the single shared-code
hook compiles out; new NVS namespaces are never touched; C2 rides a `PROTO_CUSTOM` slot leaving MT/MC/RNS/LoRaWAN
untouched; R1=NONE is a per-env default invisible to non-camera builds. ⇒ v9.0 master (V1.0/V1.1) + co-proc
(V1.0/V1.1) binaries are **byte-identical**. An unmodified repeater can even raw-repeat the camera's C2/telemetry.

---

## 9. Decisions — LOCKED 2026-06-28
- **Q1 WiFi mode → SoftAP** (field-portable; no STA credential store / connect logic needed).
- **Q2 C2 key model → per-sender PSK + allowlist** (true whitelist, revocable; `c2auth` NVS table).
- **Q3 Media storage → microSD** (shared SPI bus + `spiMutex`; J3 intact).
- **Q4 First milestone → C2-over-LoRa MVP, no camera** (prove the auth core on the bench first).

**[PROPOSED — accepted unless owner vetoes]** the §4 engineering choices: build-env/flag gating (§4.1),
binary C2 on PROTO_CUSTOM 0x33 (§4.2), reuse-LoRaWANCrypto encrypt-then-MAC (§4.3), two-server web with the
config form lifted non-blocking (§4.4), v9.0 + high-current-interlock concurrency (§4.5), BLE no-op stub (§4.7).
Optional **later**: a read-only Meshtastic/MeshCore text status path (non-actuating; channel-key auth only).

**Q5–Q6 LOCKED 2026-06-28 (master ↔ edge, see §1A):**
- **Q5 Commander deployment → dormant in the standard repeater build** (ABP-encoder precedent; configured via
  portal). Proven via a build flag/variant during dev, folded into the standard repeater at release.
- **Q6 Master/slave telemetry → broadcast beacons + unicast ACK to the commander + optional "primary master"
  for unsolicited telemetry** (supports a mesh of repeaters + full standalone).
```
