# CLAUDE.md — Project context for Claude sessions

**This file is the single source of truth for project state. Read it first. Do not generate "handoff documents" — update this file instead.**

---

## What this is

Multi-protocol LoRa mesh bridge on Seeed Xiao ESP32-S3. Bridges Meshtastic, MeshCore, and (stub) Reticulum networks across two radios sharing one SPI bus.

- **Repo:** https://github.com/GrayHatGuy/Xiao-esp32s3-lora-repeater
- **Local path:** `C:\Users\6r4yh\workspace\Platformio\Projects\Xiao-esp32s3-lora-repeater - main dev-ABP-lorawan` (note: contains spaces)
- **Owner:** GrayHatGuy — `grayhatguyllc@protonmail.com`
- **Contest:** Seeed/Meshtastic Build-Off 2026, issue #2 at `Seeed-Projects/meshtastic-build-off-2026`

## Current state

| Item | Value |
|---|---|
| **Production line** | `main` @ **v8.4** (`99c544b`, tag `v8.4-ABP-lorawan`, **GitHub Latest**) — "LoRaWAN ABP uplink encoder (keyed)", shipped & contest-submitted 2026-06-17. Lineage: v8.2/v8.2.1 LBT/CAD routing → v8.3 keyless LoRaWAN → v8.3.1 R2 V1.0/V1.1 module-revision variants → v8.4 ABP encoder. v8.1 = prior dual-SX1262 baseline. |
| **Active cycle** | **v8.4.1 "UI_UM_config"** — captive-portal look-and-feel cleanup (Phase 1, owner-approval-gated) + a WiFi-config-portal user manual PDF (Phase 2). |
| **Investigation branch** | `lr1121-phase1` |
| **Branch tip** | Check with `git rev-parse lr1121-phase1` |
| **Snapshot tag (shared with Seeed)** | `lr1121-bringup-2026-05-26` — mutable, force-push acceptable; bump after material commits |
| **Default build flag** | `LR1121_RX_AUDIT_RUN=0` in `platformio.ini` (clean state) |

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
