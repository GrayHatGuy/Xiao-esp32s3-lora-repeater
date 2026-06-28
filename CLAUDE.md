# CLAUDE.md — Project context for Claude sessions

**This file is the single source of truth for project state. Read it first. Do not generate "handoff documents" — update this file instead.**

---

## What this is

Multi-protocol LoRa mesh bridge on Seeed Xiao ESP32-S3. Bridges Meshtastic, MeshCore, and (stub) Reticulum networks across two radios sharing one SPI bus.

- **Repo:** https://github.com/GrayHatGuy/Xiao-esp32s3-lora-repeater
- **Local path:** `C:\Users\6r4yh\workspace\Platformio\Projects\lora_cam_xiao` (renamed 2026-06-28 from `…repeater – dev_2xiao_4sx1262`; on branch `lora_cam_xiao`)
- **Owner:** GrayHatGuy — `grayhatguyllc@protonmail.com`
- **Contest:** Seeed/Meshtastic Build-Off 2026, issue #2 at `Seeed-Projects/meshtastic-build-off-2026`

## Current state

| Item | Value |
|---|---|
| **Production line** | `main` @ **v9.0** (`aa47c66`, tag `v9.0`, **GitHub Latest**, shipped 2026-06-20) — **"four-radio bridge (2xiao_4sx1262)"**: two Xiao over a UART crossover → 4 sub-GHz SX1262 radios (master R1/R2 + co-processor R3/R4) with a per-radio routing matrix; **fully backwards-compatible** (single Xiao = original 2-radio bridge; `BridgeConfig` schema **v4→v5** migrates 2-radio configs). Lineage: v8.2/v8.2.1 LBT/CAD → v8.3 keyless LoRaWAN → v8.3.1 R2 V1.0/V1.1 → v8.4 ABP encoder → v8.4.1 UI_UM_config → **v9.0 2xiao_4sx1262**. |
| **Active cycle** | **none — v9.0 SHIPPED 2026-06-20** (cycle was "v8.5 2xiao_4sx1262"; renamed v8.5→v9.0 at release since R2→R4 is a major change). `dev-2xiao-4sx1262` ff-merged → `main`@`aa47c66`; tag `v9.0` force-moved off the old LR1121 commit (16329b4→aa47c66) + pushed; GitHub release **Latest** w/ **8 bins** — 4 master (`xiao-dual-sx1262-v9.0-{v1.0,v1.1}-{app,vanilla-factory}`) + 4 co-proc (`xiao-coproc-sx1262-v9.0-{v1.0,v1.1}-{app,factory}`); release notes carry download + web-flasher + build-from-source for both boards. **Next: OTAA** (own release; A3/B3 bench + the deferred garble/auto-resend eyeball ride along there). |
| **Active branch (NOT main)** | **`lora_cam_xiao`** — LoRaCam (camera + LoRa-commanded edge radio); Phase-1 C2 core **proven end-to-end on silicon 2026-06-28**, pushed to `origin/lora_cam_xiao`, **NOT merged to main** (owner-gated). See the LoRaCam section below. |
| **Investigation branch** | `lr1121-phase1` |
| **Branch tip** | Check with `git rev-parse lr1121-phase1` |
| **Snapshot tag (shared with Seeed)** | `lr1121-bringup-2026-05-26` — mutable, force-push acceptable; bump after material commits |
| **Default build flag** | `LR1121_RX_AUDIT_RUN=0` in `platformio.ini` (clean state) |

## ⭐ LoRaCam — camera + LoRa-commanded edge radio (ACTIVE branch `lora_cam_xiao`, NOT merged)

A LoRa-commanded camera built ON the v9.0 bridge firmware: a XIAO ESP32-S3 **Sense** (OV2640 cam + mic +
microSD on the rear B2B 40-pin) + a **perimeter-pin Wio-SX1262** edge radio. Commanded over LoRa — encrypted,
sender-whitelisted, replay-protected — and (later) a SoftAP web portal with live video + config + messaging.
Branch `lora_cam_xiao` off `main`@`24528ad` (v9.0). Design of record: `LORACAM-SPEC.md`; bench guide
`BENCH-CAMC2.md`; HW handoff `C:\Users\6r4yh\XIAO-Sense-Wio-SX1262-Compatibility-Handoff.md`.

**Locked design (LORACAM-SPEC §9):** SoftAP portal · per-sender PSK + allowlist auth · microSD on the shared
SPI bus (spiMutex) · binary C2 on a `PROTO_CUSTOM` radio (sync **0x33**) · encrypt-then-MAC reusing
`LoRaWANCrypto` (AES-CTR + 8-byte CMAC, keys derived from the PSK) · the cam is an **edge node of the repeater
mesh** (a repeater raw-repeats Custom frames → commander→repeater→cam works for free); three roles = edge cam
(responder) / repeater = commander (ships dormant in the standard build, ABP-encoder pattern) / standalone
self-mastered (cam's own portal). Two deployment modes (standalone vs paired) chosen at provisioning.

**HW (handoff, confirmed in-firmware):** the camera occupies the B2B pins R1 uses (GPIO38-42) → a camera build
MUST disable R1 and use only the EDGE radio R2 (V1.0 map NSS=GPIO5/DIO1=GPIO2/RST=GPIO3/BUSY=GPIO4 + SPI
D8/D9/D10). microSD CS=GPIO21 shares the SPI bus. TX capped **+20 dBm**. Mechanical: the Wio baseboard and the
Sense daughterboard both want the XIAO underside → **flying-leads prototype**. **Wiring CONFIRMED on silicon**
(R2 reads its chip ID, `[Radio2-Edge] ready sync 0x33`).

**✅ Phase 1 DONE + PROVEN ON SILICON (2026-06-28) — the C2 security core.**
- New: `src/CamC2.{h,cpp}` (frame `[ver0xC2][type][senderId:4][recipientId:4][seq:4][ct][cmac:8]`,
  encrypt-then-MAC, constant-time tag compare, fail-closed boot self-test `ready()`, responder + commander +
  beacon), `src/CamC2Config.{h,cpp}` (`c2auth` NVS whitelist + persist-on-accept fail-closed anti-replay +
  reboot-safe block-reserved tx seq), `tools/cam-c2.py` (offline gen/verify/selftest, reuses lw-verify crypto).
  Shared-code touch = ONE guarded hook in `ingestAndFanout()` (PROTO_CUSTOM branch, before the LW-ENCODE block)
  + `CamC2::begin(camC2Emit)` in setup + the `camC2Emit` seam (dedup-record + `g_routeQ` push). Everything
  `#if defined(BRIDGE_ROLE_CAMERA)||defined(BRIDGE_CAM_COMMANDER)` → **stock builds byte-identical (do-no-harm
  proven: `xiao_esp32s3` = 865781 B unchanged)**. `BridgeConfig.cpp` gained a do-no-harm `LORA_RADIO{1,2}_DISABLE`
  seam (forces a slot PROTO_NONE).
- **Adversarially verified SOUND** (6-lens crypto review): MAC coverage / encrypt-then-MAC / key derivation /
  no nonce reuse / replay+whitelist ordering / memory safety. Pre-flight fixes: 1-based-seq first-frame bug,
  `ackCap<1` guard, log-noise demoted off the `evt=` stream; `R_OK` enum → `RES_*` (POSIX `<unistd.h>` collision);
  the seed sat after `begin()`'s no-NVS early-return (skipped on erased boards) → restructured to guard reads.
- **Build envs:** `xiao_loracam` (product: R1 off, R2 Custom 0x33, MAC-derived id, portal-provisioned) ·
  `bench_camc2` (cam, autosave, fixed id **0xCA00** + seeded peer 0xC0DE) · `bench_camc2_cmdr` (commander,
  `-DBRIDGE_CAM_COMMANDER`, id **0xC0DE** + peer 0xCA00, serial trigger `s/r/x/g`). Bench provisioning flags:
  `BRIDGE_C2_MY_ID` (fixed C2 id, since MAC ids aren't known at build time), `BRIDGE_C2_PEER_ID/_KEY/_PRIMARY`.
- **END-TO-END ROUND-TRIP on real LoRa:** cam COM14 (`bench_camc2`) + commander = a dual-SX1262 bridge COM19
  (`bench_camc2_cmdr`). `g` → CMDR `evt=C2TX to=0x0000ca00 cmd=6 seq=1` → CAM `evt=RX rssi=-61 snr=11` +
  `evt=C2CMD from=0x0000c0de cmd=6 seq=1 res=0` (authenticated+executed) → signed ACK → CMDR `evt=C2RX type=2`.
  Frame sizes match spec (cmd 23B, ACK 31B). Driver: `C:\Users\6r4yh\c2cmd_test.py <cam> <cmdr> <key>`.
  ⚠️ **Native-USB flashing gotchas (carry forward):** a board whose running firmware wedges esptool
  (`PermissionError 31`) needs **manual download mode** (hold BOOT, tap RESET, release) — the port
  re-enumerates (COM18→COM19) — and after flashing, a clean **power-cycle WITHOUT BOOT** to RUN the app (else
  it sits in `boot:0x22 DOWNLOAD`). `cap.py --reset` works on a healthy board; the wedged one needed the manual steps.

**✅ Phase 2a DONE + PROVEN ON SILICON (2026-06-28) — camera capture on a LoRa command.** New
`src/CameraNode.{h,cpp}` = OV2640 init for the XIAO S3 Sense via `esp_camera` (links from the **bundled core, no
lib_dep**; the example's LED-flash pin **GPIO21 is NOT driven — it's the microSD CS**; degrades to
`ready()==false` with no daughterboard; SVGA default), wired into `executeCommand` (CMD_START_CAPTURE) +
`CameraNode::begin()` in setup BEFORE the radio tasks (camera-init inrush ≠ TX, brown-out). **Proven:** `s` from
the commander → cam `[CamC2] CAPTURE 800x600 14315B -> snap_…jpg` + `evt=C2CMD cmd=1 res=0` → ACK. **Camera EMI
does NOT block radio RX** (`g` works with the camera running, rssi −58). Cam env 931741 B (27.9%); stock still
865781 B (do-no-harm). Bench note: a freshly-flashed cam can miss the FIRST commands until it settles — let it
fully boot.

**🔄 Phase 2b (NEXT) — microSD persistence + record:** `SD.begin(21, <radio HSPI SPIClass>)` + wrap every SD op
in the existing `spiMutex` (the radio releases the bus during on-air TX so SD writes slot into the gaps); keep
J3 intact; reconcile SD-lib vs radio SCK/MOSI; save the JPEG and return the real filename (replacing the PSRAM
descriptor); then video record (`r`, still a stub). **Needs a microSD card inserted (owner has none in yet).**
**Phase 3 (after):** always-on SoftAP portal = MJPEG
video + config + LoRa-messaging + pairing UI; **decide then** whether the portal's direct `executeCommand` honors
the `s_ok` fail-closed latch (review MED item). Deferred: NVS-DoS telemetry surface. **NOT merged to main — owner-gated.**

## ⭐ v8.5 → SHIPPED as v9.0 "2xiao_4sx1262" (2026-06-20)

> **Shipped as v9.0** (`aa47c66`, tag `v9.0`, GitHub Latest, 4 master bins). The cycle below
> was developed under the working name "v8.5"; at release it was renamed **v9.0** (the R2→R4 jump
> is a major change). Final release work on top of the staged cycle: removed the standalone README
> "Four radios" section and **wove** the 4-radio content into the existing README sections per owner
> guidance (tagline, intro, routing-matrix bullet, what's-new→v9.0, parts, UART-crossover wiring
> table, key points, co-proc CLI, master/co-proc instructions) + the 4-radio photo
> (`images/4-radio-dual-xiao.jpg`); CONFIG-USER-MANUAL §1+§4 four-radio notes; boot banner →
> "XIAO ESP32S3 SX1262 Cross-Protocol Bridge (v9.0)" (dropped "Dual"); CHANGELOG v9.0; BENCH-v8.5.md
> → BENCH-v9.0.md. Two backwards-compat code fixes shipped: RemoteRadio fails fast when the co-proc
> was never seen (no 10 s stall on a 2-radio build with R3/R4 mistakenly enabled), and the portal
> hides routing-matrix checkboxes for None radios. ⚠️ **DO NOT generate a 4-radio README section as
> its own top-level block** — owner reaction "train wreck"; weave new content into the existing
> structure (see [[feedback-grayhatguy-repo-conventions]]).

Combine **two Xiao dual-SX1262 bridges** into **one 4-radio bridge**, the two Xiaos joined by a **UART
crossover**. Reuses the UART crossover from the abandoned `T_LORA_QUAD_ROUTE` branch (which paired a Xiao
with a T-Lora-Dual/LR1121 — abandoned on the LR1121 RX deficit; the UART half is reused, the LR1121 half
dropped). Because all 4 radios are now **SX1262 sub-GHz**, the exact risk that killed the reference is gone.
Branch `dev-2xiao-4sx1262` off `main`@`0a036d0`. Target tag **`v8.5 "2xiao_4sx1262"`** (owner-gated).

**Architecture (owner-decided 2026-06-18):**
- **Host + co-processor** (NOT symmetric peers). Board A = full bridge brain (routing, dedup, captive
  portal, config for all 4 radios); R1/R2 = its local SX1262. Board B = a dumb SX1262 radio head running a
  small co-processor firmware (reuses `WioSX1262` + a `LinkProtocol` slave loop), driving its two SX1262 as
  R3/R4, configured/driven by the host over UART. Board B has no portal.
- **Routing = per-radio routing matrix** (`routeMask`, a portal "bridge to" grid) — not all-to-all.
- **UART crossover:** both boards' UART1 D6 (GPIO43, TX) / D7 (GPIO44, RX) @ 460800, wired CROSSED
  (A.D6→B.D7, A.D7→B.D6, GND↔GND). Same firmware pin defaults both ends; the cable does the crossover.

**Stages (mirrors the reference A–F; A–E done & build-green across all host + co-proc envs):**
- ✅ **A — schema (`edddada`, Flash 25.1%):** `BridgeConfig` v4→**v5**. `NUM_RADIOS=4`; new `PersistedV5`
  with `RadioSlot radio[4]` (channel name/key moved INTO the slot); per-radio `routeMask` reuses a v4 pad
  byte; v8.4.1's `lwRegion` (CO-9) + `PROTO_LORAWAN` kept; the reference's `chip`/`band` enums dropped.
  Real v4→v5 migration preserves R1/R2 RF+channels+lwRegion + the R1↔R2 crossover; R3/R4 default
  `PROTO_NONE` (single-board build = byte-identical behaviour). Generic `radioChannelName/Key(idx)` +
  `radioRouteMask(idx)` + setters; `radio1/2*` kept as wrappers. `platformio.ini`: cloned R1/R2 flags →
  `LORA_RADIO3/4_*` + documented `BRIDGE_LINK_TX/RX_PIN/BAUD`.
- ✅ **B — UART transport (`abc9a62`, Flash 25.4%):** ported `LinkProtocol.h` / `UartLink.{h,cpp}` /
  `RemoteRadio.{h,cpp}` from the reference verbatim (chip-agnostic; `RemoteRadio` matches our `LoraRadio`).
  Standalone — not yet referenced by `main.cpp`, so do-no-harm.
- ✅ **C — bridge core (`9556477`, Flash 25.8%):** `main.cpp` `NR=2`→4 + all `[NR]` arrays widened
  (kTag gained "R3"/"R4"); R3/R4 = `RemoteRadio` over a shared `UartLink g_link(Serial1)`, opened only if
  R3/R4 enabled (single-board never touches Serial1); every `ingestAndFanout()` fan-out loop now gates on
  `g_routeMask[srcIdx] & (1<<j)` (loaded from `radioRouteMask()`); `LinkProtocol::BAND_SUBGHZ`;
  `BRIDGE_LINK_TX/RX_PIN/BAUD` (43/44/460800). `WioSX1262` ISR `_inst[2]` left as-is (only R1/R2 are
  local). Do-no-harm: R3/R4 default PROTO_NONE + default routeMask R1↔R2.
- ✅ **D — portal (`3d39209`, Flash 25.9%):** `renderForm` loops `appendRadio` over NUM_RADIOS; R3/R4 carry
  a "second XIAO" hint; **routing-matrix UI** ("Bridge received traffic to" `rN routeTo j` checkboxes) +
  parse → `setRadioRouteMask`; fixed the R3/R4 channel accessor bug (was `(n==1)?r1:r2`); generic
  `setRadioChannelName(idx)`; `handleSave` loops applyRadio + the self-bridge guard is now routeMask-aware
  over all pairs; JS arrays/updAll widened to 4. (lwabp + v1_1 envs also green.)
- ✅ **E — co-processor firmware (`0795538`, Flash 9.6%):** self-contained subproject
  `coproc-xiao-sx1262/` (envs `xiao_coproc_sx1262` + `_v1_1`). Reuses `LinkProtocol.h`/`LoraRadio.h`/
  `WioSX1262.h` via `-I ../src` (LinkProtocol stays single-source) + compiles `../src/WioSX1262.cpp` via
  `build_src_filter` (no duplication). `coproc_main.cpp` = UART slave (framing mirror + CFG_RADIO/TX/
  START_RX/PING/RESET ↔ RX/TX_DONE/READY/LOG/PONG) + non-blocking CAD-gated TX; link on Serial1 D6/D7.
  Added `WioSX1262::setConfig()` for in-place re-tune on CFG_RADIO (no reconstruction).
- 🔄 **F — IN PROGRESS.** ✅ **Scrub DONE (`6cce3da`):** removed ALL 2.4 GHz / LR1121 / T-Lora-Dual /
  S-band refs — incl. the `LinkProtocol::Band` enum AND the `band` wire field of CfgRadio (RemoteRadio
  band param/member gone too). v8.5 is **sub-GHz only**; band was always SUBGHZ so no functional change;
  host+coproc rebuilt consistently. ✅ **`BENCH-v9.0.md` written (`fbd64e3`):** bench plan — Group 0
  (link/config/do-no-harm), A (full-mesh A1 / restricted-matrix A2 + cross-board + dedup), B (uplink
  encode+sniffer-decode B1 / 2 devices + **B3 RX2 923.3/SF12 downlink-listener** + B4 mesh-board +
  LoRaWAN-board split), C (link resilience / load); gating = 0.1-0.3+A1+A2+B1; OTAA-downlink rationale.
  **Remaining:** owner finalizes the bench list + runs it on the 2-board rig → then the rest of the docs
  (README dual-Xiao wiring, `CONFIG-USER-MANUAL.md` R3/R4 + routing matrix, CHANGELOG) + tag **v8.5**
  (owner-gated; NEVER force-push main).

**▶⭐ HARDWARE BENCH 2026-06-19 — TEST 0.1 (LINK UP) PASSED on the real 2-board rig** (host COM13 +
co-proc COM6, UART crossover). After a long wiring debug, the host↔co-proc link is **fully working**:
COM13 shows `[UartLink] co-processor READY` (timing-dependent) + `[coproc] R3 cfg ok: 905.000 MHz…` +
`[coproc] R4 cfg ok: 909.000 MHz…` and `evt=TX_DONE radio=R3 result=done` — i.e. a packet went host →
UART link → co-proc → R3 transmit → confirmed (no more `timeout-recovered`). The 4-radio bridge is alive
on silicon. **Root cause of the link failure = a wiring crossover error** (owner originally had no
crossover on the co-proc→host return line; host→co-proc worked the whole time). DIAGNOSIS METHOD (Claude
drove flashing + serial capture on the owner's machine via `C:\Users\6r4yh\cap.py` = pyserial RTS-reset +
read): proved host→co-proc good via a co-proc debug build (`-DCOPROC_PLAINTEXT_DEBUG`) that hex-dumped the
link RX — saw a clean `AA 55 02 00 3B …` MSG_TX frame arriving — then proved co-proc→host dead (host saw
nothing on a co-proc reset), pinpointing the return wire. ⚠️ **Known bring-up friction (carry to bench):**
the host sends R3/R4 CFG **once at boot**, so after any co-proc reset you must reboot the host (or it stays
"idle until CFG"); the **`re-send CFG on co-proc READY`** robustness tweak (offered, not yet built) would
self-heal this — RECOMMENDED before heavier bench. Co-proc flashed back to the CLEAN release build (no
debug). **⭐ TEST A (4-RADIO MESH BRIDGE) ALSO PASSED 2026-06-19** — owner sent an MT text "Whoooooo" on R1;
COM13: `QUEUE radio=R1 dst=R2 (MT→MC) / dst=R3 (raw MT) / dst=R4 (MT→MC)` then **`TX_DONE radio=R2 result=done`
+ `TX_DONE radio=R4 result=done` + `TX_DONE radio=R3 result=done`** — a real mesh message bridged host →
UART → co-proc → transmitted on BOTH R3 and R4 (full mesh, cross-protocol translate + raw repeat), all
`result=done`. Loop-dup guard also confirmed (the node's own re-broadcasts dropped `drop=loop-dup`). So
Group 0 (0.1 link-up) + the core of Group A are PROVEN on silicon.

**▶⭐ A2 (routeMask isolation) PASSED 2026-06-19.** Owner set the restricted matrix via the portal —
R1 route=0x2 (→R2 only), R2 route=0x1 (→R1), R3 route=0x8 (→R4), R4 route=0x4 (→R3) — and sourced
traffic. An MT "Hello" on R1 queued **`dst=R2` only** (vs Test A's full-mesh fan-out to R2+R3+R4);
an MC "Yo" on R2 queued **`dst=R1` only**. Same radios, same traffic, different mask → different
fan-out = isolation proven (the generic `g_routeMask[src] & (1<<j)` gate). Gap: no node sits on R3/R4's
freqs (905.0/909.0), so the R3→R4-only half is proven by the shared code path, not on-air (optional).

**▶⭐ C1 (link resilience) PASSED 2026-06-19 + auto-resend tweak shipped (`4025d60`).** Added "re-send
R3/R4 config when the host sees a new co-proc READY": `UartLink` counts `MSG_READY` edges (`readyGen()`);
host `loop()` re-pushes CFG_RADIO+START_RX to the enabled remote radios when that count moves. The first
READY after boot also re-pushes (closes the setup()-sent-CFG-before-coproc-listening race); single-board
builds never open the link so `readyGen` stays 0 → no-op (do-no-harm). Builds green all 4 envs;
adversarial review clean. **Bench: 5× co-proc reset → host stayed up 5/5, R3 re-applied 5/5, R4
re-applied 4/5; host NEVER rebooted.** Co-proc *debug build* (COM6 hex) confirmed both R3+R4 CFG frames
arrive and BOTH apply. The intermittent missing console line is purely a **USB-CDC console-output drop
under the reboot burst** (proof: `READY` line printed only 1/5 yet the re-push — gated on that very
READY — fired 5/5), NOT a link or recovery failure → confirm C1 recovery by the `cfg ok` lines that DO
arrive / by traffic, not by every status line. An experimental 3 s settle delay was tried and reverted
(unnecessary — the host→co-proc config direction is reliable without it).

**▶ Console anti-garble logging fix (`41f7323`, 2026-06-19).** Owner reported the same mid-line
interleaving in the normal serial monitor (multiple FreeRTOS tasks doing raw `Serial.printf` at once —
only the `evt=` lines were mutexed; the per-packet `MeshDecoderDebug::print` dump + `[coproc]`/`[UartLink]`/
`[R*]`/`[link]` status lines were not). New `src/SerialLog.{h,cpp}` = one shared RECURSIVE console mutex
(`logf()` atomic line + `lock()/unlock()` to bracket a multi-line block). `blogf()` re-pointed onto it
(old `logMutex` dropped); the decoder dump bracketed; all task-context status prints + the rare NodeDB-evict
/ LoRaWAN-FCnt-error lines routed through it; boot/setup prints left raw (single-threaded). Added
`Serial.setTxBufferSize(4096)` before `Serial.begin()` to absorb bursts (HWCDC no-host path stays
non-blocking → no stall risk). Builds green all 4 envs; 1 adversarial-review agent = no deadlock / no
lock-ordering inversion / co-proc unaffected / `setTxBufferSize`-before-`begin` correct. **NOTE:** the
de-garble can't be fault-injected on the bench (needs concurrent dual-radio MT+MC traffic) → **owner to
eyeball `pio device monitor` during traffic** to confirm lines stop interleaving. (My pyserial multi-reset
tally is NOT a valid garble/drop instrument — its `READY 1/5` etc. are DTR/open-close capture artifacts,
unchanged by the buffer; the prints provably execute. Use a real monitor.)

**▶⭐⭐ B1 (LoRaWAN uplink encode+decode) PASSED 2026-06-19 — v8.5 GATING SET COMPLETE** (`e52bc41`).
Proved the real v8.5 path: an MT text is encoded to a keyed LoRaWAN ABP uplink and TRANSMITTED ON THE
REMOTE radio R3 (co-proc) over the UART link. Rig: host COM13 (`bench_lw_enc_r3`) + co-proc COM6
(`xiao_coproc_sx1262`) + sniffer COM14 (`bench_lw_sniffer`). 7 MT texts on LongFast → host
`QUEUE radio=R1 dst=R3 dstproto=LW devaddr=01000001 fcnt=0..6 fport=13 cred=flag` → `TX_DONE radio=R3` →
sniffer `evt=RX proto=LW mtype=UnconfDataUp devaddr=0x01000001 fcnt=N` + `evt=LWRAW raw=…`.
`tools/lw-verify.py` on 3 frames: **MIC PASS + FRMPayload decrypts to the exact text** ("Hello"/"B1
test"/"B1 test decrypt peers"), FCnt monotonic. New bench infra (committed): `[env:bench_lw_enc_r3]` +
two do-no-harm BridgeConfig build-flag hooks (`LORA_RADIO{3,4}_ENABLE`, `LORA_RADIO{1..4}_ROUTE_MASK`)
so a bench env can stand up a 4-radio config + routing without the portal (stock build unchanged).
**Gotcha learned:** an autosave/erased build leaves the MT channel key EMPTY (`BRIDGE_MT_PSK_B64`
default ""), so R1 can't decrypt LongFast → set `LORA_RADIO1_CHANNEL_KEY="AQ=="` (done in the env).
**GATING: 0.1 ✓ · 0.2 ✓ · 0.3 (do-no-harm, code-verified) · A1 ✓ · A2 ✓ · B1 ✓ — ALL MET.**
Remaining = recommended-not-gating (A3 cross-board, B3 RX2 923.3/SF12 downlink-listener) + remaining docs
(README dual-Xiao wiring, CONFIG-USER-MANUAL R3/R4 + routing matrix, CHANGELOG) → then tag **v8.5**
(owner-gated; never force-push main).
v8.5 is **sub-GHz only** (4× SX1262); all 2.4 GHz/LR1121/T-Lora-Dual
refs scrubbed (`6cce3da`, incl. the wire `band` field). **`BENCH-v9.0.md`** = the bench plan (gating =
0.1-0.3+A1+A2+B1). **Release set = 4 envs** (host V1.0/V1.1 + coproc V1.0/V1.1); the `_lwabp` + `bench_*`
envs are dev/bench, kept intentionally (owner Option A) so anyone can recreate the bench. After the gating
bench passes → remaining docs
(README dual-Xiao wiring, `CONFIG-USER-MANUAL.md`, CHANGELOG) + tag v8.5. **NOT pushed/tagged.**

## ⭐ v8.2 / v8.2.1 — "LBT/CAD routing" (SHIPPED 2026-06-13)

RX-priority routing (Listen-Before-Talk / CAD) + source-identity preservation, backported from
`T_LORA_QUAD_ROUTE` onto the 2-radio dual-SX1262 `main`. **Bench-verified on hardware and published.**

**v8.2.1 patch (tag `v8.2.1`, `1ce870d`, GitHub Latest):** MeshCore timestamp fix. `MT→MC`/`RNS→MC`
stamped the MC `GRP_TXT` Unix ts as 0 → MC clients showed 1969. The bridge (no RTC/NTP) now LEARNS
wall-clock from inbound MeshCore packets' ts (`learnClockFromMc`/`bridgeNowUnix` in `main.cpp`;
`extractMeshCoreBody` gained `tsOut`) and stamps it on MC encodes (surfaced as `mcts=` on QUEUE
lines + a one-time `evt=CLOCK`). Bench-verified (rx_ts 1781399207 → mcts 1781399220). Known: a
fresh boot stamps 0 until the first timestamped MC packet calibrates it (inherent, no RTC); future
option = learn from Meshtastic `POSITION_APP` time field.

- **Release:** "v8.2 - LBT/CAD/hash dedup routing" — GitHub **Latest**, tag `v8.2` → `cecf9b7`.
  Assets: `xiao-dual-sx1262-v8.2-vanilla-factory.bin` (full image @ `0x0`; fresh/erased device →
  captive portal pre-filled with **R1=Meshtastic / R2=MeshCore**) + `xiao-dual-sx1262-v8.2-app.bin`
  (app @ `0x10000`).
- **What it does:** non-blocking CAD-gated TX + per-destination PSRAM `RouteQueue` + airtime throttle
  (a TX never blocks the other radio's RX); content-hash dedup keyed on **body + src + MT packet_id**
  (replaces the `[MT]/[MC]/[rns]` text markers → clean far-side bodies); **source-identity**: MC→MT
  virtual MT nodes (`FNV-1a("MC|name")`) + synthetic NodeInfo, MT→MC `Name@MT:` body prefix,
  same-channel transparent raw repeat; structured `evt=` serial logging; RadioLib pinned **7.7.0**.
  `BridgeConfig` schema **v4 unchanged** (no NVS migration). **LR1121 / co-processor code OUT of scope.**
- **Bench (2026-06-13):** §9 Phase A tests **1–9 + §15.1 all PASS on air**. Test 10 (queue max-age
  expiry under sustained jam) accepted on code-review (needs a continuous-carrier jam to exercise).
- **Docs:** `V8.2-SPEC.md` (design, decisions §10, serial-log schema §13, LoRaWAN analysis §14, §15.1
  fix), `CHANGELOG.md` v8.2, `README.md` "Routing & protocol support". Cross-session memory:
  `project-xiao-v82-router-backport`.
- **⚠️ History was REWRITTEN (owner-authorized one-time force-push):** v8.2 commit messages contained
  bare `@MT`/`@MC` examples that GitHub auto-linked to unrelated users (github.com/mc, /mt). Scrubbed
  via `git filter-branch` (commit msgs) + code-spans (rendered docs) + `gh release edit` (release body).
  **Old commit hashes are DEAD; `origin/main` @ `cecf9b7`, tag `v8.2` → `cecf9b7`.** Any *other* clone
  needs `git fetch && git reset --hard origin/main`. **Lesson: never put a bare `@`-word in
  GitHub-rendered text** (release notes, README/CHANGELOG/spec, or commit messages) — wrap protocol
  tags in `code spans` and avoid `@`+letters in commit messages.
- **Deferred / next (priority order; see README roadmap):**
  1. **Reticulum bidirectional routing** — today RNS→MT/MC is a base64 text tunnel only; needs an RNS
     packet encoder + fragment reassembly for `MT/MC → RNS`.
  2. **LoRaWAN capture/metadata tap** (sync `0x34`) — content bridging is architecturally precluded
     (per-device keys, no group cleartext; V8.2-SPEC §14); realistic scope = one-way RX metadata.
  3. **Sub-GHz ↔ 2.4 GHz cross-band** = the Phase 1 work below (still Seeed-blocked).
  - MeshCore identical-text dedup (no on-air per-message id) — accepted limitation.
  - §15.2 cosmetic: the legacy `MeshDecoderDebug::print` hex-dump isn't under the log mutex, so it can
    interleave across cores; the structured `evt=` lines are unaffected.

## ✅ v8.3 — LoRaWAN keyless bridge/relay/mesh + carry-overs (SHIPPED 2026-06-14)

**SHIPPED 2026-06-14.** ff-merged `v8.3-dev` → `main` @ `6c92cf4`; annotated tag **`v8.3`** pushed to origin
(clean fast-forward, no force-push). GitHub release **PUBLISHED as Latest** 2026-06-14 (notes + `vanilla-factory`
+ `app` bins): https://github.com/GrayHatGuy/Xiao-esp32s3-lora-repeater/releases/tag/v8.3 . All `must` bench tests passed on hardware
(LoRaWAN, both clock-learn paths, full Reticulum block, v8.2 routing regression incl. R9 do-no-harm).

**Post-ship docs (2026-06-14/15): `main`/`origin/main` now @ `6e11a5a`.** README restructured for v8.3
(de-versioned intro → "Routing & behavior" + v8.3 release-notes link, LoRaWAN protocol bullet, version-grouped
"Build flags & compile-time configuration" reference; stale v8.1 prod refs fixed). `BENCH-v8.3.md` gained
§G results + the byte-identity-via-base64 method and the `fmtNodeId(0)=-` / RNS→MC tunnel-QUEUE string
corrections. **Contest submission (Seeed `meshtastic-build-off-2026` issue #2) updated to v8.3 + posted:**
https://github.com/Seeed-Projects/meshtastic-build-off-2026/issues/2 — the exact posted body is kept locally as
`CONTEST_SUBMISSION_v8.3.md` (gitignored via `CONTEST_SUBMISSION_*.md`). **Optional bench NOT run** (all
`should`/optional, non-gating): LW-FLOOD (single + bidirectional multi-bridge), `bench_lw_nosum`/`norelay`
(Pass-D remaining two), R8 RX-priority — **R5 + LW-CAP0 PASSED**. Bench-env addition this cycle:
`bench_mt_samechan` (R5; both radios MT LongFast @906.875/905.0). CLI gotcha for next session: the generator's
`rns`/`xiao_esp32s3` envs live in `tools/lw-frame-gen` — run with `pio run -d "<repo>\tools\lw-frame-gen" -e <env> …`;
the root project has the `bench_*` envs (no `-d`). Design of record: `V8.3-SPEC.md` (APPROVED + IMPLEMENTED, LW-Q1..Q5 resolved). Bench protocol:
`BENCH-v8.3.md`. All commits build green (`pio run -e xiao_esp32s3`, Flash ~24.6%). Bench rig: 3× Xiao
bridges on **COM6/COM13/COM14** + 2 MT + 2 MC; LoRaWAN/RNS stimulus = `tools/lw-frame-gen/` flashed on a
spare bridge (COM6). **The v8.3 folder may be checked out on `main` or a `bench_*` env —
`git checkout v8.3-dev` to see the source.**

### Implemented (all build-green)
- `9499804` **POSITION clock-learn** (v8.2.1 follow-up): MT `POSITION_APP` `Position.time` (f4)/`.timestamp`
  (f7) calibrates the wall-clock. `g_mcClock*`→`g_clock*`, `learnClock(ts,src)`.
- `e337afd` **RNS→RNS transparent repeat** (audit fix): in-protocol RNS was silently dropped; now uses
  `rawRepeatForDest`, gated `BRIDGE_RNS_INPROTO_REPEAT=1`.
- **LoRaWAN (sync `0x34`) keyless** (`39f432e`→`63c6cac`→`e2fb434`→`dfe7da4`): new `LoRaWAN` per-radio
  protocol — capture tap (`evt=RX proto=LW`), summary→MT/MC (`BRIDGE_LW_SUMMARY_TO_MESH=1`), transparent
  LW↔LW raw relay / dedup-bounded flood (`BRIDGE_LW_RELAY`), `MT/MC→LW` = `no-lw-encoder` drop. No keys, no
  `FRMPayload` decrypt, no `FCnt`/`MIC` synthesis. Single-channel per radio (Tasmota-style).
- `5f03e55` review nits fixed (26-agent adversarial review: 21 raised → 2 cosmetic, 19 refuted; no functional bugs).
- **Bench tooling:** `tools/lw-frame-gen/` generator (LoRaWAN `0x34` default + `[env:rns]` for `0x42`);
  bench envs `bench_lw_dutA/dutB/relayA`, `bench_rns`, `bench_rns_relay`, `bench_lw_nocap/nosum/norelay` —
  all carry `BRIDGE_BENCH_AUTOSAVE` (`41b4dc6`) so an erased bench board boots configured, **no captive portal**.

### Bench results — real hardware, 2026-06-14 (detail in BENCH-v8.3.md §G)
- **ALL `must` tests PASS → release-ready.** LW-RX (gate; SX126x `0x34`→`0x3444` proven), LW-DATA
  (Unconf/Conf/FPort±), LW-JOIN, LW-FAIL, LW-SUMMARY (seen on MeshCore app), LW-LOOP+TTL, LW-MT→LW drop,
  **LW-RELAY + multi-bridge** (COM13→COM14, byte-identical), MC clock-learn, **C1 MT-POSITION clock-learn**
  (`evt=CLOCK src=MT unix=1781467783`), regression **R1/R2/R3/R4 + R6/R7**.
- **Reticulum block CLOSED on air 2026-06-14** (gear-free generator + real MC nodes): **C2/D1** RNS↔RNS raw
  repeat — `QUEUE … mode=raw virtualid=-` + R2 TX + `drop=rns-dup`; byte-identical confirmed via COM14 3rd-RX
  base64 `QIofASYAAQACqrsRIjNE`. **D2** RNS→MC tunnel — `QUEUE … dstproto=MC frag=1/1 msg="[rns 18 1/1] …"`,
  repeated to MeshCore. **D3** MC→RNS — `evt=DROP radio=R2 dst=R1 drop=no-rns-encoder`, no R1 TX.
- **R9 do-no-harm PASS** (must): under sustained mixed MT/MC load, **zero** `proto=LW` / `no-lw-encoder` /
  `lw-dup`; normal MT↔MC routing, virtual nodes, loop-dup, self-echo, CAD, throttle all live.
- **OPEN — `should`/optional, NONE gating the tag:** **R5** same-channel raw-repeat (real MT node on hand;
  needs both radios on the IDENTICAL MT channel — captive portal or a `bench_mt_samechan` env); **R8**
  RX-priority headline; **Pass D** flag toggles (LW-CAP0/SUM0/RELAY0 — all five bench envs build-green);
  **LW-FLOOD** bidirectional multi-bridge (one-way relay already PASS; bidirectional needs COM14 a `relayA`-mirror env).

### Deferred → v8.3+ (follow-up work; V8.3-SPEC §10)
ABP/OTAA key-based **decode** (own fleet → MT/MC/custom) and **encode** (MT/MC → LoRaWAN); Reticulum
`MT/MC → RNS` encoder + reassembly (today MT/MC→RNS is a clean log-and-drop).

### To tag v8.3 (owner-gated outbound)
Finish/skip the OPEN bench items, then: ff-merge `v8.3-dev`→`main`, annotated tag `v8.3`, push `main`+tag,
draft GitHub release notes + bins for owner review. **NEVER force-push `main`.** RNS↔RNS + the deferred
items can ship code-verified with full on-air bench in the 8.3.x patch, per owner's framing.

## ✅ v8.4 — ABP LoRaWAN uplink encoder + ChirpStack ingestion (SHIPPED 2026-06-17)

**SHIPPED & SUBMITTED 2026-06-17 (verified on disk + GitHub).** `dev-ABP-lorawan` ff-merged → `main` with
v8.3.1 (R2 V1.0/V1.1 module-revision variants) already integrated; final commit **`99c544b`** "feat(build):
ship the ABP encoder in the standard V1.0 + V1.1 builds". **`origin/main` = `main` = `dev-ABP-lorawan` =
`99c544b`**, annotated tag **`v8.4-ABP-lorawan`**. GitHub release **"v8.4 — LoRaWAN ABP uplink encoder
(keyed)" = Latest** (4 bins: v1.0/v1.1 × `app` + `vanilla-factory`):
https://github.com/GrayHatGuy/Xiao-esp32s3-lora-repeater/releases/tag/v8.4-ABP-lorawan . Contest issue #2
updated to v8.4. **Bench 12/16 PASS** — Tier A+B core proven on silicon 2026-06-16 (A1 self-test / A2 dwell /
A5 stock do-no-harm / B1 MT→ABP emit / B2 FCnt reboot-safe / B5 offline `lw-verify.py` MIC+decrypt); **only
Tier C (colleague's ChirpStack) remains** = LW-P1-ACCEPT + LW-P3-WEATHER + codec/tag (give the colleague
DevAddr `0x01000001` + bench keys, edit `decodeStation()` to real station bytes for C2). Solo portal/WiFi
bench items are written up in `BENCH-SOLO.md`.

Scope: mint valid LoRaWAN **ABP** uplinks so a raw-LoRa source (canonical: a weather station) is ingested by
a ChirpStack LNS. **OTAA dropped; MT/MC→RNS coding deferred** (owner). Design of record: `ABP-LORAWAN-SPEC.md`;
bench protocol `BENCH-v8.4.md`. The implementation history below is retained for reference.

**Owner decisions (locked 2026-06-15, SPEC §0.0):** raw-LoRa **Fork B** · delivery **B1 (RF re-emit)**,
no WiFi · device model **M1 (per-source)**.

**P1 — IMPLEMENTED, build-green, crypto-verified (2026-06-15); on-air bench OPEN.**
- New `src/LoRaWANCrypto.h`: self-contained **RFC 4493 AES-CMAC** over `mbedtls_aes_*` (CMAC is
  compiled OUT of the prebuilt esp32s3 lib — `mbedtls_cipher_cmac` won't link; SPEC §12/A4) +
  `encodeUplink()` (AES-CTR FRMPayload + CMAC MIC, ADR=0, Unconfirmed) + `selfTest()` KATs.
- `src/main.cpp`: the `no-lw-encoder` drop in `enqueueTextForDest()` is now a keyed transcode →
  `g_routeQ` RF re-emit (B1), **gated by `BRIDGE_LW_ENCODE` + parsed creds** so a stock build keeps
  v8.3's do-no-harm drop. `BRIDGE_LW_ENC_*` build flags; build-flag credential resolver (P1 stand-in;
  P2 swaps in the schema-v5 per-source store + NVS-persisted FCnt); boot self-test under
  `BRIDGE_LW_ENC_SELFTEST`.
- `platformio.ini`: `[env:bench_lw_enc]` — R2=LoRaWAN 903.9/BW125/SF7, encoder+self-test on,
  throwaway ABP creds. DevAddr is now `0x01000001` (**NwkID 0** → matches ChirpStack's default
  private **NetID `0x000000`**; the old `0x26011B22` was NwkID 19 and would be silently dropped by a
  default LNS). `selfTest()` keeps `0x26011B22` as an internal crypto KAT only (never on air).
  Bench env also sets `REGION=US`; new `[env:bench_lw_enc_dwell]` (SF12) exercises the P4 dwell cap.
- **Verified:** stock build green @ 24.6% (unchanged → do-no-harm); `bench_lw_enc` green @ 24.7% and
  *links* (proves the hand-rolled CMAC avoids the absent primitive). Crypto cross-checked against an
  independent `cryptography`-lib CMAC: RFC4493 vectors match + a minted frame is MIC-valid and
  round-trips (`40221b0126 00 0100 0d f62f3b0a401acae9 53c1715d`).
- **P1 acceptance OPEN (owner bench):** flash `bench_lw_enc`, provision the ABP device in ChirpStack
  (DevAddr `0x01000001` + bench keys, MAC 1.0.x, ABP, Class A, ADR off, disable-FCnt-validation or
  persist), send an MT text → confirm ChirpStack shows the decoded uplink. Expect
  `[lw-selftest] overall : PASS` at boot first.

**P2–P4 IMPLEMENTED 2026-06-15 (build-green; on-air bench pending — `BENCH-v8.4.md`).**
- **P2:** new `src/LoRaWANConfig.{h,cpp}` — a 4-slot per-source ABP identity table in its OWN NVS
  namespace (`lwabp`, NOT a BridgeConfig schema bump) + a captive-portal "LoRaWAN ABP devices"
  section + reboot-safe FCnt (block reservation). Seam resolves per-source device → else build-flag.
  New env `xiao_esp32s3_lwabp` (encoder on, portal-configurable). Fixed a latent sync→proto bug in
  `resolve()`.
- **P3:** shared `enqueueAbpUplink()`; **Custom raw-LoRa → ABP** weather-station path (raw RX bytes
  as FRMPayload); optional per-device source tag (`[proto][srcId]`); `tools/chirpstack-codec.js`.
- **P4:** `regionDwellMs()` US915 400 ms per-TX dwell cap in the TX scheduler (EU868 via
  `BRIDGE_TX_DUTY_PERCENT`).
- **Deferred → later:** ABP/OTAA decode, dual-LNS crosslink, MT/MC→RNS encoder.

**Pre-bench adversarial review + fixes #1–#6 (2026-06-15; `dev-ABP-lorawan` `d98da4b`+`1496717`, LOCAL/not pushed).**
30-agent workflow (7 dims → per-finding skeptic → completeness critic; 22 findings, 18 confirmed). **Crypto
VERIFIED SOUND** — an independent Python recompute raised 0 findings; RFC4493 CMAC/B0/A_i/FRMPayload/MIC are
1.0.x byte-correct. Verdict: a single-device **LW-P1-ACCEPT should pass as-was**; the traps were in the
multi-device / redeploy / tagged paths. Fixes folded in (build-green `xiao_esp32s3` / `bench_lw_enc` /
`xiao_esp32s3_lwabp`; stock binary +112 B of never-called code only → do-no-harm intact):
- **#1** FCnt persisted by **DevAddr** (`fc_<addr>`) not slot index — survives portal row moves; `setDevice()`
  re-seeds on DevAddr change. **#3** **fail-closed** reserve-before-issue (`advanceFcnt()` persists+verifies
  the NVS write before issuing; `FCNT_INVALID`→`drop=lw-fcnt-fail`; saturates near 2³²). **#4** build-flag
  fallback FCnt now reboot-safe via `nextFcntForDevAddr()` (was an in-RAM `++` that reset to 0 each boot) +
  DevAddr-collision boot warn. **#2** `selfTest()` result latched → encoder refuses to emit on a KAT FAIL
  (`drop=lw-selftest-fail`). **#5** source tag stamps `protoOf(sync)` enum (1/2/3/4/5), not the raw sync word
  (codec was decoding MT/MC as "custom"); `chirpstack-codec.js` updated. **#6** over-cap payload dropped
  (`drop=lw-payload-overflow`), not silently truncated.
- **Bench-prep (this pass):** bench DevAddr → `0x01000001` (NwkID-0, LNS-default-friendly); `bench_lw_enc`
  set `REGION=US`; new `bench_lw_enc_dwell` (SF12) for LW-P4-DWELL; `BENCH-v8.4.md` updated (new tests
  LW-P2-FCNT-MOVE / overflow, DevAddr-NetID note, drop-reason glossary, migration note).
- ⚠️ **Migration:** counters moved to `fc_<addr>` keys → old `fc<N>` slot keys orphaned, a device resumes at
  FCnt 0 on first boot after this (fine with disable-FCnt-validation).
- **NOT fixed (non-gating, deferred):** per-DR payload cap (242 B absolute + dwell only); `FPort==0`/`srcSel`
  portal guards; `nextFcnt()` mutex (only if NR grows — T_LORA_QUAD); portal FCnt display.

## ✅ v8.4.1 — captive-portal UI cleanup + user manual + ChirpStack tooling (SHIPPED 2026-06-18)

**SHIPPED 2026-06-18.** ff-merged `dev-v8.4.1-UI_UM_config` → `main` (clean ff, no force); annotated tag
**`v8.4.1-UI_UM_config`** → `d098420` (`origin/main` now `b85fa1a` incl. this doc); **GitHub release Latest**
(4 bins v1.0/v1.1 × app + vanilla-factory):
https://github.com/GrayHatGuy/Xiao-esp32s3-lora-repeater/releases/tag/v8.4.1-UI_UM_config . **UI / docs /
tooling only — no protocol/routing/on-air change; `BridgeConfig` schema v4 unchanged** (the new per-radio
LoRaWAN region reuses a spare `RadioRf` byte). Builds green `xiao_esp32s3` / `xiao_esp32s3_v1_1` /
`xiao_esp32s3_lwabp`.

**What shipped:**
- **Captive-portal cleanup** (`src/CaptivePortal.cpp`): bridge-behaviour toggles moved into the top identity
  frame; Meshtastic `AQ==` default key; MT BW/SF/CR shown read-only from the modem preset; MeshCore "public
  key `8b…`" hint; Channel name `N/A` for LoRaWAN/Custom; custom-PSK/channel-name warning.
- **LoRaWAN region/slot picker** — per-radio region (US915/AU915/AS923/EU868) + channel-slot dropdown
  auto-fills freq/SF/BW (CR fixed 4/5) from RP002-1.0.3; region **persisted** in `RadioRf.lwRegion` (renamed
  `_pad[0]`, no schema bump; new `BridgeConfig::LwRegion` enum + `radioLwRegion()`/`setRadioLwRegion()`).
- **MAC-derived identity** (`deriveMacIdentity()` in `main.cpp`): long name `<NodeID> LoRa Bridge`, short
  `BR<low-byte>`. The residual `BRIDGE_MT_NODE_ID/_STR/_LONG_NAME/_SHORT_NAME` flags were **removed** from
  `platformio.ini` (always overridden by the MAC derivation; `BridgeConfig.cpp` `#ifndef` fallbacks remain).
- **Protocol-switch autofill:** preset change sets the MT Channel name (until a custom key unlocks it; same
  on MeshCore); Reticulum auto-fills RNode defaults (914.875 / 125k / SF8 / CR5); RNS BW/SF/CR now editable.
- **User manual `CONFIG-USER-MANUAL.md`** (linked from README Instructions) — field-by-field for all 10
  portal screens (`images/manual/`), each field → its build flag, the ABP Applies-to-source / source-tag
  detail, 3 example setups, and **the full build-flag catalog. ⚠️ Build flags are documented HERE now, NOT
  in the README** (README's "Build flags" + "Captive-portal top-frame fields" sections were deleted).
- **ChirpStack tooling `tools/chirpstack/`** — importable device-profile templates (US915/AU915/AS923/EU868
  ABP profiles + vendor/device manifests + codec test vectors) + the hardened `tools/chirpstack-codec.js`
  decoder (UTF-8, `warnings`/`errors`, device-variable-driven source tag).
- **Compile-time preloading:** new per-radio channel flags `LORA_RADIO{1,2}_CHANNEL_NAME` / `_KEY` + three
  commented "scenario" blocks in `platformio.ini` (MT↔MC, MT→LoRaWAN, MT public→private). README trimmed
  8→5 install steps; per-protocol table merged into "Routing & behavior".

**Release-bin recipe (next release).** Build `xiao_esp32s3` + `xiao_esp32s3_v1_1`, then per env:
`<penv-py> <tool-esptoolpy>/esptool.py --chip esp32s3 merge_bin -o factory.bin --flash_mode keep
--flash_freq keep --flash_size keep 0x0 bootloader.bin 0x8000 partitions.bin 0xe000
<framework-arduinoespressif32>/tools/partitions/boot_app0.bin 0x10000 firmware.bin` (app bin =
`.pio/build/<env>/firmware.bin`). esptool isn't on PATH → use
`C:\Users\6r4yh\.platformio\penv\Scripts\python.exe`. Factory = the 0x10000 boot region + app, NOT
full-flash-padded (v8.4.1: app 839664 B / factory 905200 B). Bins + `RELEASE_NOTES_*.md` are gitignored;
publish via `gh release create … --latest <4 bins>`.

## Open work (priority order) — start here next session

1. **(MED) Close the v8.4 Tier-C ChirpStack gate** — the only open v8.4 bench item, no firmware needed.
   Give the colleague DevAddr `0x01000001` + the bench `NwkSKey`/`AppSKey` + `tools/chirpstack/` (templates +
   codec); they provision a device (MAC 1.0.x, ABP, Class A, ADR off, FCnt-validation off) and confirm
   LW-P1-ACCEPT (a minted MT/MC uplink ingests + the codec shows the text) + LW-P3-WEATHER.
2. **(HIGH — owner-gated, own release) v8.5 OTAA** — LoRaWAN join (DevEUI / JoinEUI / AppKey) alongside ABP.
   HIGH→VERY-HIGH: the crux is receiving the **JoinAccept downlink in a Class-A RX1/RX2 window** — a new
   downlink-RX subsystem (`LoraRadio` has **no runtime retune** today; RF is fixed at `begin()`) that's
   **un-benchable without a downlink-capable gateway**. ~8–12+ sessions; make the **build-vs-adopt** call
   (hand-roll on `WioSX1262` vs adopt RadioLib `LoRaWANNode` / LMIC) early — it swings the effort ~3×.
   Crypto + NVS substrate already exist (`LoRaWANCrypto.h` CMAC/AES-ECB; `LoRaWANConfig` FCnt pattern).
3. **(LOW — blocked on owner) Weather-station decoder** — `decodeStation()` in `tools/chirpstack-codec.js`
   is a placeholder; fill it once the station HW + byte layout are chosen.
4. **(LOW-MED) Reticulum `MT/MC → RNS` encoder** + fragment reassembly — `encodeReticulum()` is a stub
   returning false (`src/MeshEncoderDebug.h`); the only un-implemented bridge direction.
5. **(ON HOLD) LR1121 Phase 1 (sub-GHz ↔ 2.4 GHz)** — hardware-blocked on Seeed's reply (see Phase status +
   Decision tree below). No firmware action while waiting.

## Phase status

- **Phase 0 (v8.1)** — ✅ Shipped. Dual-SX1262 multi-protocol bridge. The contest deliverable.
- **Phase 1** — ⏸️ **HARDWARE-BLOCKED on Seeed Wio-LR1121 module (SKU 113991415).** All firmware-side investigation complete. RX path is non-functional despite TX working. Sensitivity degraded ~40–50 dB versus LR1121 datasheet. Seeed engineering inquiry sent 2026-05-27 with full DOE evidence; **awaiting reply**. Firmware infrastructure is complete on `lr1121-phase1` branch.
- **Phase 2** — 📋 Dual-LR1121. Compile-verified, awaits Phase 1 resolution.

## What was investigated (firmware-side, COMPLETE)

Two full DOE phases against Semtech LR1121 User Manual v2.2 + comparison to stock Meshtastic firmware:

- **Phase A — 12-iteration RFSWx switch-table sweep.** All chip-level RFSWx-capable DIOs (DIO5/6/7/8/10) exhausted. Self-echo RSSI invariant within ~7 dB across all states.
- **Phase B — 5 chip-init runs** (`LR1121_RX_AUDIT_RUN` build flag, values 0/2/3/5/6 bench-tested):
  - Run 0 baseline → `errors=0x0020 = HF_XOSC_START_ERR` persistent at every POR
  - Run 2 `SetRssiCalibration` (UM Table 7-21 600M-2G) → failed; +4 dB AGC shift
  - Run 3 `CalibImage(902, 928)` → failed
  - Run 5 kitchen-sink → failed; **one `RADIOLIB_ERR_CRC_MISMATCH` signal-of-life event**
  - Run 6 Meshtastic-style (DCDC + 2-DIO switch table + `setPreambleLength` before `startReceive`) → failed; **17 dB self-echo reduction** (D7/D8/D10 NC closed a parasitic path)

Runs 1 (pre-standby alone) and 4 (RxBoosted alone) were intentionally skipped — rationale invalidated by Run 0 + mathematically incapable of closing the gap.

**Comparative bench evidence (devastating for Seeed):** Five working LoRa RX paths exist on the same physical bench — 2× LilyGO T3S3 LR1121 (sub-GHz + 2.4 GHz), 1× T-Watch S3 (SX1262 + SX1280). Only the two Wio-LR1121 modules fail. Chip + firmware + RF environment all ruled out. The bug is Seeed-Wio-LR1121-carrier-specific.

## What's blocked on what

- **Phase 1 ship** is blocked on EITHER:
  - Seeed engineering reply with a working fix, OR
  - Hardware pivot to a known-good LR1121 carrier (bare module on Xiao or LilyGO T-Lora Dual on ESP32-C5)

- **No firmware action is required while waiting.** The branch is stable at `lr1121-phase1` tip. Default flag `LR1121_RX_AUDIT_RUN=0` restored.

- **Owner is intentionally NOT sending Seeed any further messages.** No follow-up emails. No partial updates. The 2026-05-27 inquiry stands as the single decisive evidence package.

## Decision tree — when Seeed replies

| Reply content | Action |
|---|---|
| RF switch control mechanism + truth table | Implement in `WioLR1121::begin()`, retest, release v9.1 |
| Per-PCB `SetRssiCalibration` tune values | Implement, retest, release v9.1 |
| Chip-firmware v1.3 errata confirmation | Apply prescribed workaround OR wait for firmware update |
| Hardware design defect confirmation | Pivot to bare LR1121 module on Xiao (preferred) or T-Lora Dual |
| **No reply in 10 business days** | Owner sends ONE polite ping in their own voice (not AI-drafted). After that: pivot to bare LR1121 module. |

## Operational rules (read carefully)

**Shell:**
- Owner runs PowerShell. Bash heredocs (`<<EOF`) do not work in their terminal.
- For multi-line content (commit messages, release notes): write to a file via `Write` tool, then `git commit -F <file>` or `gh release edit --notes-file <file>`.
- The internal Bash tool runs on a Git Bash shim. When using it, prefix with `cd "/c/Users/6r4yh/workspace/Platformio/Projects/Xiao-esp32s3-lora-repeater - main dev-ABP-lorawan" && ...` due to spaces in path.

**Tools and paths:**
- `pdftotext`: `C:/Program Files/Git/mingw64/bin/pdftotext.exe` — but verify PDF table extractions against a clean screenshot; multi-column cells get OCR-scrambled.
- `pio device monitor --port COM6` is the standard bench monitor command.

**Git:**
- **Never force-push `main`** unless explicitly resetting to a known tag.
- **Force-pushing `lr1121-bringup-2026-05-26` IS expected** — that's its purpose; bump after material commits so Seeed-correspondence links resolve to current state.
- Commit messages: HEREDOC via `git commit -m "$(cat <<'EOF' ... EOF)"`. Always include `Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>`.

**RadioLib quirks:**
- `LR11x0::getErrors()` and `LR11x0::setRssiCalibration()` are `protected` in RadioLib 7.7.0. Access via the `LR1121Access` struct in `WioLR1121.cpp` (using-declaration access-promoting subclass).
- `setRfSwitchTable()` takes `const uint32_t (&)[5]`, not a pointer — must pass the array by name.

## DO NOT (lessons earned)

- **Do not generate "handoff documents."** Update this file instead. One source of truth.
- **Do not draft follow-up emails to Seeed.** Owner has decided no further outreach until Seeed replies. Drafting one anyway makes them look like they're flailing.
- **Do not propose bench iterations "for rigor"** when the technical question is already answered. Runs 1 and 4 were the last casualty of this pattern.
- **Do not propose proactive public-surface updates, README polish, or release-body refreshes** unless owner specifically asks.
- **Do not propose hardware purchases unless asked** for purchase decisions.
- **Do not use TaskCreate/TaskUpdate** for this project — external state tracking via this file and the deep docs is sufficient.
- **Do not "check on the Seeed reply"** — owner is monitoring their own inbox.
- **Do not re-draft the SEEED_EMAIL_DRAFT.md.** Owner edits it directly on GitHub when needed.

## File pointers (read these for depth, do not duplicate them here)

| File | Purpose |
|---|---|
| `LR1121-SPEC.md` | Phase 1 design + complete investigation history |
| `LR1121-RX-INIT-AUDIT.md` | DOE bench plan + per-run results (rev 3 + Run 6 appendix) |
| `SEEED_SUPPORT_INQUIRY.md` | Full bug report sent to Seeed; the decisive evidence package |
| `SEEED_EMAIL_DRAFT.md` | Locked-in email body sent to Seeed engineering 2026-05-27 |
| `SEEED_RECOMMENDATIONS.md` | Tier 1–4 design feedback for Seeed |
| `src/WioLR1121.cpp` | LR1121 driver wrapper — where DOE code lives, gated by `LR1121_RX_AUDIT_RUN` |
| `src/WioSX1262.cpp` | SX1262 driver wrapper — Phase 0 production code |
| `src/LoraRadio.h` | Abstract base class — `WioSX1262` and `WioLR1121` both implement |
| `platformio.ini` | Build config — `RADIO_PROFILE` + `LR1121_RX_AUDIT_RUN` flags documented in comments |

## Reference documents (external)

- **Semtech LR1121 User Manual v2.2** — direct PDF: https://semtech.my.salesforce.com/sfc/p/#E0000000JelG/a/RQ00000DClgP/D.pNG5l4FviPI634eCx8GFURZEwDO2ZBA33MpriB_FU · stable page: https://www.semtech.com/products/wireless-rf/lora-connect/lr1121
- **Semtech LR1121 Datasheet v2.1** — direct PDF: https://semtech.my.salesforce.com/sfc/p/#E0000000JelG/a/RQ0000093ZiP/RV4Ba6LROsFrFjnAAVK2av5W11RGmCms_3Q2cyKHdDA · stable page: https://www.semtech.com/products/wireless-rf/lora-connect/lr1121
- **Seeed Wio-LR1121 Module Datasheet v1.0** — https://files.seeedstudio.com/wiki/Wio-LR1121/Wio-LR1121_Module_Datasheet_v1.0.pdf · wiki: https://wiki.seeedstudio.com/wio_lr1121_module/
- **Meshtastic firmware LR1121 init (known-good reference)** — `meshtastic/firmware` → `src/mesh/LR11x0Interface.cpp` + `variants/esp32s3/tlora_t3s3_v1/rfswitch.h`

## Update protocol for this file

- Update **only** when material state changes (Seeed replies, branch tip lands a significant commit, phase status changes, owner makes a strategic pivot).
- Do not snapshot — overwrite in place.
- Owner can edit directly; AI can edit when asked or when state genuinely changes.
- If you feel the urge to create a NEW doc summarizing state, edit this one instead.
