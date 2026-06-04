# CLAUDE.md — Project Handoff

**Project:** Xiao ESP32-S3 dual-radio LoRa mesh bridge (sub-GHz ↔ 2.4 GHz cross-band)
**Repo:** https://github.com/GrayHatGuy/Xiao-esp32s3-lora-repeater
**Local path:** `C:\Users\6r4yh\workspace\Platformio\Projects\xiao esp32 wio sx1262 dual repeater`
**Active branch:** `lr1121-phase1` (HEAD: `e07e39f` as of this handoff — see §4 for last-touched functional code commit `12d685d`; later commits are doc-only)
**Owner identity:** `GrayHatGuy` (pseudonym), `grayhatguyllc@protonmail.com`. **Real name is ephemeral, do not persist or write to docs.**
**Shell:** PowerShell (Windows). Use HEREDOC via Bash tool for commit messages; no inline newlines in shell commands.

---

## 0. HANDOFF — session 4 (2026-06-04) — READ THIS FIRST

**Repo:** https://github.com/GrayHatGuy/Xiao-esp32s3-lora-repeater
**Local:** `C:\Users\6r4yh\workspace\Platformio\Projects\xiao esp32 wio sx1262 dual repeater`
**Branch/HEAD:** `lr1121-phase1` @ `4216f12` (pushed). Snapshot tag `lr1121-bringup-2026-05-26` = HEAD (force-pushable; bump after each branch commit).
**Owner:** `GrayHatGuy` (pseudonym), `grayhatguyllc@protonmail.com`. **Real name is ephemeral — never persist to docs.**
**Shell:** PowerShell (Windows). HEREDOC commit messages via the Bash tool; no inline newlines in shell commands. Build/flash via PlatformIO (`pio`).

### 0.1 Ultimate goal
**Phase 1:** one XIAO ESP32-S3 hosting **two** LoRa radios on a cross-protocol bridge (Meshtastic ↔ MeshCore):
- **R1 = Wio-SX1262** — sub-GHz only. **Healthy reference radio.** Receives flawlessly (−22 to −88 dBm seen).
- **R2 = Wio-LR1121** — dual-band (sub-GHz + 2.4 GHz). **The problem child.** "Interpretation B" is locked by the owner: **both bands must eventually work** on R2; a 2.4G-only partial does not ship. *Current debugging is sub-GHz* (906.875 MHz) because it gives a direct A/B against the SX1262.
- Phase 0 (dual SX1262, sub-GHz) already ships at **v8.1 on `main`**. All Phase-1 work is on `lr1121-phase1`.
- **Phase 1 ships when R2 receives well enough to bridge.** That is gated entirely on the one open bug below.

### 0.2 Hardware inventory
- **XIAO ESP32-S3 bridge** on **COM6**: carries R1 (Wio-SX1262) + R2 (Wio-LR1121). Bridge node id `0x75D7AC1C`, name "B16B00B5 LoRa Bridge".
- **R2 modules (two physical Wio-LR1121s, identical parts):** marked with sharpie — **red dot = "suspect-BAD"** (the one that showed `-20`), **no dot = "suspect-GOOD"**. **Suspect-GOOD is currently mounted**, chip-EUI **`00:16:C0:01:F0:9B:37:D5`**, LR1121 base FW **1.3**. (2 more pristine Wio-LR1121 on order.)
- **Heltec V4** on **COM11**: Meshtastic node `0x0AC9F340`, US LongFast. **Has a PA front-end → up to +30 dBm (1 W).** Real-LoRa source.
- **T3S3 LilyGO (LR1121)**: also a real-LoRa source/reference; interoperates on the mesh at sync 0x2B. Earlier swapped out as *receiver* over 2.4↔subG switch concerns — fine as a **sub-GHz transmitter**.
- **Test gear (mostly unused now):** HackRF One SDR + SDRAngel; **KT3-2N-90/1S** step attenuator (0–90 dB / 1 dB); a **5 dB SMA fixed pad**; SMA↔N adapters, jumpers, IPEX-SMA pigtails. **WaveShare Core1121** LR1121 board on order (~2–10 days) for #8.

### 0.3 How the bridge works (orientation for code edits)
- `src/main.cpp` runs two FreeRTOS tasks: **`radio1Task`** (R1 RX → forward to R2 TX) and **`radio2Task`** (R2 RX → forward to R1 TX). Each: poll `available()` → `read()` → log `[Rx RX]`/`[Rx decoded]` → `bridgePacket()` → re-`startReceive()`.
- `makeRadio()` (`main.cpp:676`) builds `WioSX1262` or `WioLR1121` from `(nss, irqDio, reset, busy)`; **R2 IRQ = DIO9**.
- `src/WioLR1121.cpp/.h` = the LR1121 driver (chip-generic despite the name). Uses the **`LR1121Access`** struct (`using`-promotes the RadioLib *protected* methods `getErrors`, `setRssiCalibration`, `getChipEui`). The IRQ diagnostic `debugIrqStatus()` instead calls the **public** `_radio->getIrqFlags()` directly — no shim. (Function-level changelog: `docs/FIRMWARE-CHANGES.md`.) RF-switch table (~line 279) is locked to Seeed's SKY13373 truth table: `MODE_STBY={0,0}` (revert, see below), `MODE_RX={1,0}`, `MODE_TX={1,1}`, `MODE_TX_HP={0,1}`; V1=DIO5/V2=DIO6.
- `platformio.ini` env `xiao_esp32s3`. R2 compile-time config: **906.875 MHz / BW 250 / SF11 / CR4-5 / 20 dBm / sync 0x2B**. Build flag **`-DR2_RX_ONLY_TEST` is currently SET** (see 0.6). `LORA_PREAMBLE_LEN=8`.

### 0.4 Current failing state — the ONE open bug
**R2 (Wio-LR1121) has a marginal sub-GHz RX deficit.** It is **alive and in continuous RX**; it **detects every preamble** (`irq` bit `0x10`); it **occasionally completes a packet** (decoded a real −68 dBm Meshtastic packet at SNR 10 on 2026-06-04); but it **completes only a small, unreliable fraction of packets — even strong ones** (a −42 dBm point-blank packet R1 decoded did NOT complete on R2). Symptom history across sessions: `isr` counter stuck low, `irq` latching `0x10` (preamble) or `0x50` (preamble+HEADER_ERR) without reaching `0x08` (RX_DONE). R1 (SX1262), same board/config, receives everything. **This deficit is the original problem from session 1 — it predates all the recent tests.**
**Open question driving #5:** is the deficit **small (≈5–15 dB → RSSI/image calibration → fixable via Seeed values #7)** or **large (structural → matching network / module / chip)?**

### 0.5 Experiments performed this session + RESULTS (chronological)
1. **R1 — MODE_STBY `{1,0}`→`{0,0}` revert** (`efbf88c`/`66dac8b`). Hypothesis: commit `949176a`'s `{1,0}` removed antenna isolation during TX→STBY, stressing the LNA. **Result: the revert is correct and committed, but was NOT the cause** (the `-20` later reproduced *with* `{0,0}`). Verified via UM §2.4 timing that the 20 µs SKY13373 shutdown recovery is absorbed by the chip's mode transitions (FS→TX 102 µs etc.), so `{0,0}` is safe.
2. **`{0,0}` 10-minute TX soak on suspect-GOOD module.** **Result: NO brick across full 600 s.** R1 RX healthy (−22…−85 dBm). R2 RX deaf (isr stuck at 1). R2 TX: every forward attempt failed `tx=-5` then `tx=-1`, `post-rx=-20`. **This killed the "cascade bricks the chip" story** — it just limps with per-TX errors.
3. **Decoded the error codes from RadioLib 7.7.0 `TypeDef.h`:** `-1`=`UNKNOWN`, `-5`=`TX_TIMEOUT`, `-7`=`CRC_MISMATCH`, **`-20`=`RADIOLIB_ERR_WRONG_MODEM`**. **The "`state=-20` SPI cascade / silicon damage" framing from sessions 1–3 is a MISDIAGNOSIS — `-20` is a software modem-state error.** First transmit waited the airtime timeout (~1.1 s, RadioLib logged "Timeout in 1108992 us") then failed; subsequent transmits failed instantly (~1 ms). (Earlier "38 s TX blocks" was a misread — that was idle between packets.)
4. **Added IRQ-status diagnostic** (`66dac8b`): `WioLR1121::debugIrqStatus()` reads the raw LR11x0 IRQ register via `getIrqFlags()`; printed each R2 heartbeat and post-`transmit()`. Lets us see, per power level, whether the chip reached preamble vs header vs RX_DONE.
5. **RX-only isolation test** (`R2_RX_ONLY_TEST` flag gates off R1→R2 forward so R2 just listens). **Result: R2 detects a preamble (`irq=0x10`, latched), `isr` frozen at 1, never reaches `RX_DONE`.** A −32 dBm (R1's RSSI) Heltec packet was NOT completed by R2 — but R2 had latched the preamble of *earlier* traffic before that packet arrived, so the read was partly contaminated.
6. **RadioLib IRQ-mask check:** `RADIOLIB_IRQ_RX_DEFAULT_MASK = (1<<RX_DONE)` only (`PhysicalLayer.h:24`); ISR is rising-edge (`LR_common.cpp:15`). **So DIO9 only fires on RX_DONE — the interrupt/DIO9 config is CORRECT. The "DIO9 wedged / IRQ-clear bug" hypothesis is REFUTED.**
7. **Point-blank tests** (Heltec antenna ~touching). **Result: R2 `isr` climbed 1→2 over ~7 min** (so it is NOT permanently hung — it completes rare packets), but a −42 dBm point-blank packet R1 decoded did not complete on R2. Concluded hand-sends can't characterize a "completes ~1-in-N" rate.
8. **Sync-word investigation (read RadioLib source):** `SX126x::setSyncWord(0x2B)` writes register `[0x24,0xB4]` = on-air `0x24B4` (nibble-expand with control byte 0x44). `LR11x0::setSyncWord(0x2B)` → `setLoRaSyncWord(0x2B)` sends the raw byte; the LR1121 expands it internally to the same `0x24B4` (Semtech SX126x-compat). **Plus** a T3S3 LR1121 interoperates on this mesh at 0x2B. **Sync word REFUTED** (and `ChirpChat` *does* have a sync-word field — it's gated behind setting Modulation=LoRa).
9. **HackRF/ChirpChat-as-source attempt** (`run-results/sweep-20260604-134553.log`; R2 cabled, KT3=0). **Results:** (a) R2 cleanly decoded a **real** Meshtastic packet at **−68 dBm SNR 10** → its demod + front end work; (b) on the ChirpChat packets: **R1 → `ERROR -7`** (full demod, payload CRC fail), **R2 → `irq=0x50`** (HEADER_ERR + preamble), **0 completions on either**. **Conclusion: SDRAngel ChirpChat is not Semtech-CRC/header-compatible → useless as a LoRa source. HackRF-as-source ABANDONED.**
10. **Safety analysis (datasheet-cited).** LR1121 datasheet **Table 3-1: abs-max RF input = +10 dBm** ("permanent device failure"); **Table 3-2: max operating input = 0 dBm**. Heltec V4 PA can emit **+30 dBm**. **Cabled** +30 dBm − ~7 dB chain = **+23 dBm at the LR1121 → destroyed.** **OTA** has 30–70 dB path loss even close (R1 saw the Heltec at −42 dBm during point-blank) → safe. **The damage hazard was the cabled HackRF/attenuator rig — which was never built. Front end confirmed intact (see 0.4/#9).**

### 0.6 Hypotheses RULED OUT (with evidence)
| Hypothesis | Verdict | Evidence |
|---|---|---|
| Silicon damage / `-20` SPI cascade | **REFUTED** | `-20`=`WRONG_MODEM` (software); reproduced on suspect-GOOD; no brick in 10-min soak |
| Front-end / LNA damaged by tests | **REFUTED** | decoded −68 dBm @ SNR 10 on 2026-06-04; a blown LNA can't |
| Sync-word mismatch | **REFUTED** | both chips → on-air `0x24B4`; T3S3 LR1121 interops at 0x2B |
| MODE_STBY `{1,0}` stressed the LNA | **NOT the cause** | `-20` occurs with `{0,0}` too; revert stands but didn't fix it |
| Interrupt / DIO9 / IRQ-clear bug | **REFUTED** | RX IRQ mask = `RX_DONE` only (correct); `isr` does climb |
| Hung receiver | **REFUTED** | `isr` climbed 1→2 over 7 min |
| Hand-send measurement | **INADEQUATE** | can't resolve a probabilistic "~1-in-N" completion rate → need calibrated levels |

### 0.7 Firmware & diagnostic state (on the board now, all committed)
- **Chip-EUI boot logging** (`8f29bed`): `[Radio2-Edge] chip EUI = 00:16:C0:01:F0:9B:37:D5` — identifies which physical module is mounted. Registry: `docs/testbed/MODULE-REGISTRY.md`.
- **IRQ-status heartbeat** (`66dac8b`): `[R2 HB] isr=N (+d/5s) rxFlag=x irq=0x________`. Decode the `irq` bits below.
- **`R2_RX_ONLY_TEST` build flag is ACTIVE** in `platformio.ini` — R2 is pure-listen, the R1→R2 forward is gated off (ideal for RX measurement; the bridge is half-live with it on). **To restore normal dual-radio bridging: delete `-D R2_RX_ONLY_TEST` and reflash.**
- **`MODE_STBY={0,0}`** committed and correct.

### 0.8 Reference data (so a new session need not re-derive)
- **RadioLib error codes:** `-1`=UNKNOWN, `-5`=TX_TIMEOUT, `-7`=CRC_MISMATCH, `-20`=WRONG_MODEM.
- **LR11x0 IRQ register bits** (`irq=` field): `0x08`=RX_DONE(bit3), `0x10`=PREAMBLE_DETECTED(bit4), `0x20`=SYNC_WORD/HEADER_VALID(bit5), `0x40`=HEADER_ERR(bit6), `0x80`=CRC_ERR(bit7). RX IRQ→DIO9 mask = RX_DONE only.
- **Sync word:** Meshtastic 0x2B → on-air `0x24B4` on both SX126x and LR11x0.
- **LR1121 RF-input limits (datasheet):** abs-max **+10 dBm** (T3-1), operating max **0 dBm** (T3-2).
- **Ports:** bridge=COM6, Heltec V4=COM11 (was T3S3/COM5 — re-enumerated/swapped).

### 0.9 Open experiments / NEXT STEPS (with rationale + decision trees)
**#5 — Quantify the R2 RX deficit (IMMEDIATE).** *Why:* the fix path forks on the *size* of the deficit; we must measure it. *Method (SAFE):* over-the-air **distance sweep** — distance is the attenuator, zero hardware risk (full runbook `docs/testbed/RX-DEFICIT-MEASUREMENT.md`):
  1. Both radios on **normal antennas** (no cable rig). T3S3 (COM11) **low power**: `meshtastic --port COM11 --set lora.tx_power 1` (**never 0 — in Meshtastic 0 = "use MAX"**).
  2. Capture COM6: `pio device monitor -p COM6 -b 115200 | Tee-Object -FilePath "docs\testbed\run-results\distance-$(Get-Date -f yyyyMMdd-HHmmss).log"`.
  3. Walk the T3S3 near→far, ~5 positions (P1 ~1 m → P5 edge-of-range), **10 labeled packets each**: `1..10 | %{ meshtastic --port COM11 --sendtext "P1 $_"; Start-Sleep 2 }`.
  4. Per position, count `[R1 decoded]`/`[R2 decoded]` /10 and R1 median RSSI. **Decision:** R2 tracks R1 → little deficit; **R2 collapses while R1 still hears → the deficit = R1's RSSI at that position.** Small (5–15 dB) ⇒ calibration ⇒ #7; large ⇒ structural ⇒ weight #8.
  *Do NOT* use the HackRF/ChirpChat source (abandoned, see #9). HACKRF-QUICKSTART/PLAN are **superseded** for #5.
**#7 — Seeed RSSI-calibration follow-up (parallel; the likely actual FIX).** *Why:* if the deficit is calibration-class, the Wio-LR1121-specific `SetRssiCalibration` byte values (UM §7.2.15 says RSSI "must be calibrated for each hardware type") are the remedy; reference Table 7-21 tunes already failed (RX-AUDIT Run 2). We asked Seeed (David Du) on **2026-05-28**; **awaiting reply** (also asked: FW-update path, HF_XOSC_START_ERR=0x0020 expected on POR). Follow up ~**2026-06-08**. Owner reviews all outbound. Attach #5 numbers if available.
**#8 — WaveShare Core1121 bring-up (GATED).** *Why:* the decisive **board-vs-chip control** — does a *different* LR1121 board show the same deficit? If the WaveShare (which **publishes a schematic**, unlike Seeed) receives cleanly where the Wio doesn't → **Seeed-board design flaw**; if it's equally deficient → LR1121-chip/firmware-level. Gated on hardware arrival (~2–10 days) and the #5 baseline. Full plan: `docs/WAVESHARE-CORE1121-HANDOFF.md`.
**#2 — Red-dot (suspect-BAD) module (LOW priority / near-moot).** *Why originally:* prove silicon damage. *Now:* the `-20` is software and reproduces on the GOOD module, so this is mostly overtaken — optional comparison only.

### 0.10 Key documents (read in this order)
1. **This file** (CLAUDE.md §0 handoff, §2 session blocks, §5 rules).
2. **`docs/testbed/RX-DEFICIT-MEASUREMENT.md`** — the experiment to run (#5), safety facts, why HackRF was dropped.
3. `docs/testbed/MODULE-REGISTRY.md` — module EUI ↔ sharpie mapping.
4. `docs/WAVESHARE-CORE1121-HANDOFF.md` — #8 bring-up.
5. `run-results/sweep-20260604-134553.log` — the front-end-intact + ChirpChat-fail evidence.
6. `LR1121-RX-INIT-AUDIT.md` — the 9-run firmware DOE (all refuted) from earlier sessions.
7. `SEEED_EMAIL_DRAFT.md` / `SEEED_EMAIL_REPLY_2026-05-28.md` — Seeed correspondence (#7).
8. `docs/REFERENCES.md` — datasheet index; LR1121 §-citations refer to PDFs in `docs/datasheets/`.
9. Superseded for #5 but retained: `docs/testbed/HACKRF-QUICKSTART.md`, `docs/testbed/HACKRF-DIAGNOSTIC-PLAN.md`.

### 0.11 Commit log — this session (newest first)
| Commit | What changed (file-level) |
|---|---|
| `4216f12` | CLAUDE.md: add this §0 handoff. |
| `26d9870` | **NEW** `docs/testbed/RX-DEFICIT-MEASUREMENT.md` (safe OTA distance method + safety + why-not-HackRF); ⛔ SUPERSEDED banners on `HACKRF-QUICKSTART.md` + `HACKRF-DIAGNOSTIC-PLAN.md`; CLAUDE.md §2 session-4 block; task #5 repointed. |
| `36a02c1` | `HACKRF-QUICKSTART.md` clarifications (now superseded): detection-rate def, 50%-floor, isr-delta counting, exact-count Repeat=N + Heltec-bookmark workflow, sync-word=LoRa-mode gotcha. |
| `71b9f50` | **NEW** `HACKRF-QUICKSTART.md` runbook + pointer from the full plan (now superseded). |
| `ae2f94f` | **NEW** `docs/WAVESHARE-CORE1121-HANDOFF.md` (#8 bring-up handoff). |
| `7868d63` | `CLAUDE.md` §2 session-3 reframe block (root cause = RX deficit, not silicon). |
| `66dac8b` | **firmware:** `WioLR1121.h/.cpp` add public `debugIrqStatus()` (reads public `getIrqFlags()`); `transmit()` logs post-TX `irq=`; `main.cpp` R2 heartbeat prints `irq=` (+RX_DONE flag); `radio1Task` R1→R2 forward wrapped in `#ifndef R2_RX_ONLY_TEST`; `setup()` banner; `platformio.ini` adds `-DR2_RX_ONLY_TEST` (currently active). Function-level diff: `docs/FIRMWARE-CHANGES.md`. |
| `8f29bed` | **firmware:** `WioLR1121.cpp` chip-EUI boot logging via `getChipEui`; **NEW** `docs/testbed/MODULE-REGISTRY.md`. |
| `1884e16` | `CLAUDE.md` §5 rule: electrical/timing/safety claims require datasheet citation (incl. reverts and "no-op" conclusions). |
| `efbf88c` | (owner) `WioLR1121.cpp` MODE_STBY `{1,0}`→`{0,0}` revert + comment; CLAUDE.md edits. (The revert; not the cause.) |

### 0.12 Rules of engagement (firm — see §5 and §7)
**Do not guess** — cite the datasheet/code for electrical/timing/safety claims; say "I don't know" otherwise. **One variable per experiment.** **Never cable a high-power source into an RX front end** (LR1121 abs-max +10 dBm). **No mid-experiment edits** that contaminate the record. **Accepted edits ship — don't re-ask.** **Owner reviews all outbound** (Seeed, PRs) — drafts only. **PowerShell + HEREDOC commits via Bash; cite the active bench state, don't assume it. Never force-push `main`** (snapshot tag is force-pushable; bump per branch commit). **Keep CLAUDE.md current as you go — don't let bench reasoning live only in chat.**

---

## 1. Ultimate goal

Phase 1 deliverable: **a single XIAO ESP32-S3 board hosting two LoRa radios (R1 = Wio-SX1262 sub-GHz, R2 = Wio-LR1121 sub-GHz + 2.4 GHz) bridging Meshtastic and MeshCore protocols across bands.** Both bands must work on R2 (Interpretation B locked in by owner — partial 2.4G-only does not ship).

Phase 0 (dual SX1262, sub-GHz only) ships at v8.1 on `main`. Phase 1 is the work happening on `lr1121-phase1`.

---

## 2. Current state (2026-05-30, end of bench session 2)

> ### ⭐ UPDATE — bench session 4 (2026-06-04): #5 method changed; front end OK
>
> **R2 front end is NOT damaged** — it decoded a −68 dBm packet at SNR 10 dB today
> (`run-results/sweep-20260604-134553.log`); a blown LNA can't. The deficit is the original
> pre-existing one, not new damage from OTA/point-blank tests.
>
> **HackRF/ChirpChat as a LoRa source: ABANDONED.** ChirpChat's CRC/header isn't
> Semtech-compatible (R1 `ERROR -7` payload-CRC, R2 `irq=0x50` HEADER_ERR, 0 clean decodes,
> while R2 decoded a *real* Meshtastic packet fine). Plus a cabled high-power source is a
> hardware-kill hazard: LR1121 abs-max input = **+10 dBm** (datasheet Table 3-1); +30 dBm
> cabled ≈ +23 dBm at the chip = destroyed.
>
> **New #5 method: over-the-air distance sweep with the T3S3 (real LoRa).** Walk the node
> near→far, both radios log decode+RSSI, the point where R2 quits but R1 still hears = the
> deficit. Zero hardware risk. Full runbook: **`docs/testbed/RX-DEFICIT-MEASUREMENT.md`**
> (supersedes HACKRF-QUICKSTART/PLAN for #5).

> ### ⭐ UPDATE — bench session 3 (2026-06-01): root cause reframed
>
> The session-2 "silicon damage / `-20` SPI cascade" framing below is a
> **MISDIAGNOSIS.** Decoded from RadioLib 7.7.0 `TypeDef.h`: **`-20` =
> `RADIOLIB_ERR_WRONG_MODEM`** (a software modem-state error), `-5` =
> `TX_TIMEOUT`. The `-20` reproduced on the **suspect-GOOD** module under
> `MODE_STBY={0,0}`, no brick across a full 10-min soak. So the module is
> almost certainly **not damaged**.
>
> **What R2 actually does:** it is alive and in continuous RX — it detects
> **every preamble** (`irq=0x10`) but **completes only a small, unreliable
> fraction of packets, even strong ones** (a −42 dBm point-blank Heltec
> packet that R1 decoded did **not** complete on R2; `isr` climbed only
> 1→2 over ~7 min). That is a **marginal RX sensitivity / demod deficit** —
> the project's original R2 finding, now cleanly isolated.
>
> **Eliminated with evidence this session:** silicon damage; the `-20`
> cascade (it's `WRONG_MODEM`); **sync word** (SX126x expands `0x2B`→`0x24B4`
> via nibble+control-0x44; LR11x0 `SetLoRaSyncWord` expands the raw byte the
> same way internally — and your T3S3 LR1121 interoperated on this mesh at
> `0x2B`, proving it); the **TX path**; the **interrupt/DIO9 config** (RX IRQ
> mask = `RX_DONE` only, correct — `PhysicalLayer.h:24`); a **hung receiver**
> (the 1→2 `isr` climb disproves it). The MODE_STBY `{1,0}→{0,0}` revert
> stands but was NOT the cause.
>
> **Resume here → Experiment R4/#5: HackRF + KT3 calibrated sweep.** Hand-sends
> can't characterise a "completes ~1-in-N" rate; inject stepped, known power
> levels and measure R2 completion-rate vs R1. That quantifies the deficit and
> produces the numbers for the Seeed RSSI-calibration follow-up.
>
> **Firmware state (commit `66dac8b`):** chip-EUI logging (kept); R2 IRQ-status
> heartbeat readout + `transmit()` `irq=` logging (kept — the per-power-level
> completion indicator for #5); **`R2_RX_ONLY_TEST` build flag is ACTIVE** —
> R2 is pure-listen (no R1→R2 forward). That's the *correct* config for the
> sensitivity sweep; **delete the `-D R2_RX_ONLY_TEST` in `platformio.ini` to
> restore normal dual-radio bridging.**
>
> **Bench mapping changed:** XIAO bridge = **COM6** (EUI `00:16:C0:01:F0:9B:37:D5`
> = suspect-GOOD, no sharpie dot); test source is now a **Heltec V4** (US
> LongFast) on **COM11** (was T3S3/COM5 — swapped to remove the T3S3's own
> 2.4↔subG switch as a confound). See `docs/testbed/MODULE-REGISTRY.md`.
> R1 (SX1262) RX is healthy — decoded dozens of packets all session, so the
> session-2 "R1 regression" looks moot.

### Hardware
- Original Wio-LR1121 module: **possibly silicon-damaged by accumulated TX-induced LNA stress** caused by commit `949176a` (see §4). Sub-GHz TX triggers `state=-20` SPI cascade after a stress threshold; chip becomes unresponsive until power cycle. Cascade observed at 35 s, 193 s, ~200 s, 408 s across multiple runs today, both bands.  Needs final futher tests to confirm.
- Fresh Wio-LR1121 module swapped in as a control. **Boots clean at 2.4 GHz, no cascade through 188 s**, but ALSO does not RX any T3S3 packets (isr=0 throughout). Either DIO9/jumper-wire contact issue on the new module, antenna mismatch, or Wio-LR1121 2.4 GHz path is design-deficient.  Need to confirm.
- T3S3 LR1121 reference radio currently reconfigured back to **US sub-GHz LongFast** (was 2.4 GHz earlier today). Owner reports R1 SX1262 is **no longer catching T3S3 sub-GHz packets** — unexplained at end of session, owner attributes to my code edits. Need to confirm.
- KT3-2N-90/1S step attenuator + HackRF One available for sensitivity testing per `docs/testbed/HACKRF-DIAGNOSTIC-PLAN.md` (test plan written, not yet executed).
- Bench: XIAO ESP32-S3 on COM6; T3S3 on COM5. See `docs/testbed/TESTBED.md` for layout + photos.

### Firmware
- Captive portal save path bugs (wideLora BW dropped, freq field clobber, Custom protocol BW-constrained) **all fixed today**. Portal saves 2.4 GHz Meshtastic LongFast configs correctly.
- `LoraConfigCheck.h` accepts both sub-GHz (150-960) and 2.4 GHz (2400-2500) for R2, both BW sets (sub-GHz 250/500/etc + wideLora 812.5/406.25/1625).
- `WioLR1121::begin()` correctly calls `LR11x0::begin(..., high=true)` for 2.4 GHz operation.
- `MODE_STBY` is currently `{1,0}` (RX-latched) per commit `949176a`. **This is the suspected silicon-damage cause and should be reverted to `{0,0}` (shutdown) — see §3 immediate action item.**
- platformio.ini currently has R2 hardcoded to 2.4 GHz Meshtastic LongFast (2404.46875 / 812.5 / SF11 / CR5 / 10 dBm / sync 0x2B). Owner accepted this edit at end of session; NVS overrides at runtime.

### Documentation
- `LR1121-RX-INIT-AUDIT.md` — 9-section DOE record (Runs 0–8) of firmware-side sub-GHz RX bring-up attempts. All failed; firmware hypothesis space exhausted prior to today's investigation.
- `SEEED_EMAIL_DRAFT.md` — original 2026-05-26 inquiry to Seeed engineering + their authoritative reply (SKY13373-460LF truth table, V1=DIO5, V2=DIO6).
- `SEEED_EMAIL_REPLY_2026-05-28.md` — **SENT 2026-05-28 to David Du. Awaiting reply.** Three open questions: (1) Wio-LR1121-specific `SetRssiCalibration` byte values, (2) LR1121 base FW update path (current FW 1.3), (3) whether `HF_XOSC_START_ERR=0x0020` on every POR is expected on TCXO-fitted modules. **Reply not yet received as of this handoff.** If a Seeed engineering reply arrives before next session, append it to `SEEED_EMAIL_DRAFT.md` under a new "Inbound replies received" subsection following the 2026-05-28 David Du reply pattern.
- `SEEED_SUPPORT_INQUIRY.md` — has per-question status badges (✅/🟡/⏳) reflecting David Du's reply.
- `docs/testbed/HACKRF-DIAGNOSTIC-PLAN.md` — dual-band Test 0/A/B/C procedure with discrete PowerShell steps. **Test 0a/0b not yet executed under clean conditions.**
- `docs/UPSTREAM-PR-CANDIDATES.md` — tracking ledger for upstream bug reports (Meshtastic region-change BW drift documented).
- **`docs/REFERENCES.md`** — canonical index of all authoritative reference material with both local paths and vendor URLs. Local PDFs live under `docs/datasheets/`:
  - `LR1121_V2_1_data_sheet.pdf` — Semtech LR1121 chip datasheet rev 2.1 (Dec 2023). RFSWx pin mapping (Table 4-1), sensitivity numbers, PA config
  - `LR1121_UM_V2.2.pdf` — Semtech LR1121 User Manual v2.2 (Apr 2026, 140 pages). Chip-level command spec; cited heavily in `LR1121-RX-INIT-AUDIT.md` (SetDioAsRfSwitch §4.2.1, SetRssiCalibration §7.2.15, CalibImage §2.1.3)
  - `Wio-LR1121_Module_Datasheet.pdf` — Seeed module-level datasheet. Antenna pads (SUBG_RF pad 23, 2.4G_RF pad 2), TCXO integration, pinout. **RF switch wiring incomplete in this doc**; David Du's 2026-05-28 reply (appended to `SEEED_EMAIL_DRAFT.md`) is the authoritative source for V1=DIO5, V2=DIO6
  - `310060742_SKYWORKS_SKY13373-460LF_Datasheet.pdf` — on-module SP3T antenna switch, provided by David Du with his reply
  - Plus vendor URLs (URL-only, no local PDF) for: Wio-SX1262 with XIAO ESP32-S3 product page; XIAO ESP32-S3 pin multiplexing wiki

---

## 3. Active roadblocks / next experiments

### IMMEDIATE (do before any other work)

**Experiment R1 — Revert `MODE_STBY={1,0}` → `{0,0}` in `src/WioLR1121.cpp`.**

- **Rationale:** Commit `949176a` changed `MODE_STBY` from `{0,0}` (SKY13373 shutdown, antenna isolated) to `{1,0}` (RX-latched, antenna connected to RFI_LF/LNA input). This removed electrical protection during TX→STBY→RX transitions. Residual PA-vent energy after every TX is now injected into the LNA. Cumulative damage across many TX cycles plausibly explains the `state=-20` cascade pattern observed today on the original module. Run 7/8 (May 28, 2-minute observations) worked; cascade only manifests after extended TX use, consistent with cumulative damage.
- **Evidence supporting this hypothesis:**
  - Same cascade signature on both sub-GHz AND 2.4 GHz on same module
  - Cascade started spontaneously during idle in one run (no immediate TX trigger), consistent with chip-internal degradation
  - Fresh module mounted with identical firmware did NOT cascade through 188 s
  - The OLD `{0,0}` shutdown had a 20 µs entry/exit penalty I dismissed as "optimization opportunity" without proper electrical analysis
- **Procedure:**
  1. Edit `src/WioLR1121.cpp` line containing `MODE_STBY` table entry, change `{1,0,0,0,0}` back to `{0,0,0,0,0}`
  2. Rebuild + flash + erase NVS
  3. Boot + run for 10+ minutes with periodic TX from R1 (which the bridge auto-bridges through R2)
  4. Watch for cascade or stability
- **Pass:** no `state=-20` events through 10 min of operation including 5+ bridge TX events
- **Fail:** cascade returns → `MODE_STBY` was not the cause, look elsewhere; possibly chip is permanently damaged from prior runs

### SHORT-TERM (after R1 outcome known)

**Experiment R2 — Determine whether fresh LR1121 module's 2.4 GHz RX is broken or just isn't being signaled.**

- **Rationale:** Fresh module installed today, boots clean at 2.4 GHz with BW 812.5 / SF11 / CR5 / sync 0x2B, but isr=0 during 3 T3S3 send attempts. Two possible causes:
  - (a) DIO9 IRQ wire (orange Kapton jumper on module pin 12 or 14) not properly soldered/contacting after the swap
  - (b) Wio-LR1121 module-level 2.4 GHz front-end design issue (matching network, antenna routing) — would affect all fresh modules
- **Procedure:**
  1. Visual + DMM continuity check on orange jumper wires (DIO9_INT specifically) on the new module
  2. Antenna touch-test: bring T3S3's 2.4 GHz antenna within 1 cm of Wio-LR1121's 2.4 GHz antenna, send T3S3 message
  3. Observe whether ANY isr increment occurs (proves DIO9 reaches MCU + chip demodulates)
- **Pass:** isr increments on touch test → wires fine, signal path works, sensitivity is the open question
- **Fail:** isr=0 even with antennas touching → either DIO9 wire broken OR module-level 2.4 GHz design issue. Verify wires first; if wires OK, escalate to Seeed.

**Experiment R3 — Diagnose R1 sub-GHz reception regression.**

- **Rationale:** Owner reports R1 SX1262 no longer catches T3S3 packets after T3S3 was reconfigured back to US sub-GHz. R1 IS still catching neighborhood Meshtastic traffic (Glasgow at -74 dBm, etc.) so R1 RX path is alive. T3S3-specific issue.
- **Procedure:**
  1. Verify T3S3 current `--get lora` — region US, BW 250, SF 11, CR 5, sync 0x2B, frequency 906.875 expected
  2. Verify R1 boot log shows `906.875 MHz BW 250.0 kHz SF11 CR4/5 ... sync 0x2B`
  3. Verify the bridge `[MT] channel="LongFast" hash=0x08` line is present at boot (else channel decode will fail)
  4. T3S3 sendtext → look for `[R1 RX]` lines AND `[R1 decoded] Meshtastic src=0x62D90E80` (T3S3 node ID)
- **Likely causes ranked:** T3S3 still has stale 2.4 GHz config → physical antenna issue → bridge MT channel mismatch → R1 modem param drift

### LONG-TERM (after Phase 1 ships)

- **Experiment R4 — HackRF + KT3 calibrated sensitivity sweep per `docs/testbed/HACKRF-DIAGNOSTIC-PLAN.md` Tests A/B/C** to produce numerical sensitivity-deficit data for Seeed engineering follow-up. Not gated by R1 sub-GHz vs 2.4 GHz outcome; useful regardless.
- **Experiment R5 — Follow up with David Du if Seeed reply has not arrived after ~7-10 business days.** The 2026-05-28 email was sent and is awaiting reply on three questions (RSSI cal values, FW update path, HF_XOSC_START_ERR expected behaviour). After Test A/B/C results land, owner may want to send a supplementary message attaching the numerical sensitivity-deficit data — but that's discretionary, not a re-prompt.
- **Feature — Add Meshtastic preset quick-buttons to captive portal** (Option A from session 2: "Apply Meshtastic US LongFast defaults" + "Apply Meshtastic 2.4G LongFast defaults"). Eliminates portal drift bug class for the two common configs.

---

## 4. Commit log (this session, weighted heavy on recent)

**🔴 Suspected-damaging commit (Experiment R1 reverts this):**

- `949176a` — **Lock RF switch table to Seeed-authoritative SKY13373 truth table.** Changed `MODE_TX` from `{0,1}` (HP path) → `{1,1}` (LP path, per Seeed authoritative). Changed `MODE_STBY` from `{0,0}` (shutdown) → `{1,0}` (RX-latched). **The MODE_STBY change is the suspected cumulative-damage cause** — removed protective antenna isolation during TX→STBY transitions, allowing residual PA-vent energy to repeatedly stress the LNA. Owner's analysis post-session attributes silicon damage on original module to this commit.  See Seeed Engineering email repy with authoritative truth table as a reference.

**🟢 Bug-fix commits this session (all needed for 2.4 GHz Meshtastic to work, leave in place):**

- `12d685d` — **fix: LR1121 2.4 GHz begin() rejected wideLora BW (RadioLib API gap).** Workaround in `WioLR1121.cpp`: for 2.4 GHz path, call `LR11x0::begin(bw, sf, cr, sync, preamble, high=true)` directly instead of `LR1120::begin(freq, bw, ...)` convenience overload, which drops the `high` flag and validates BW against sub-GHz range only → `-8 INVALID_BANDWIDTH` for 812.5 kHz. Then manually `setFrequency()` + `setOutputPower()`. RadioLib API design flaw worth upstreaming.
- `0df109a` — **fix: captive portal freq field clobbered user input mid-typing.** Removed `oninput=updAll()` reactive trigger on the freq input. The reactive recompute was overwriting user typing because `setF()` auto-fills the field when value equals previous computed value or is empty.
- `3b1ffe3` — **audit: full sub-GHz / 2.4 GHz / Custom protocol BW accommodation.** (1) `LoraConfigCheck.h` R2 freq static_assert now accepts both 150-960 MHz AND 2400-2500 MHz. (2) `CaptivePortal.cpp applyRadio()` Custom protocol no longer calls `bwAllowed()` — Custom is escape hatch for arbitrary RF experimentation. (3) JS preset table extended with PRE24 (wideLora BWs) + BAND24 range + is24() helper + chip-onchange + freq-onchange wiring.
- `6d78644` — **fix: captive portal saved wrong BW for LR1121 2.4 GHz Meshtastic configs.** Root-cause fix. `applyRadio()` at PROTO_MT branch now passes `wideLora = (chip == LR1121 && freq >= 2400.0f)` to `modemPresetParams()`. Previously dropped wideLora flag → saved 250 instead of 812.5 → silent BW mismatch with T3S3 → zero R2 RX. Also fixed: `LoraConfigCheck.h LORA_CHK_VALID_BW` macro adds 812.5/406.25/1625; `CaptivePortal.cpp ALLOWED_BW[]` runtime array adds same; `presetFromParams()` tries both `wideLora=true` and `false` for reverse lookup.

**🟡 Owner-accepted compile-time hardcode (last action of session):**

- platformio.ini R2 settings changed from `906.875f / 250.0f / 11 / 5 / 20 / 0x2B` to `2404.46875f / 812.5f / 11 / 5 / 10 / 0x2B`. This change applies after NVS erase or on first boot without saved config. Owner accepted this edit to bypass the portal for fresh-module 2.4 GHz testing.  Current state R2 manually edited by owner to `906.875f / 250.0f / 11 / 5 / 20 / 0x2B`.  Confirm before proceeding with intial testing.

**🔵 Earlier session commits (documentation, no functional impact):**

- `82ad470` — Track Meshtastic T3S3 region-change BW drift bug as upstream PR candidate
- `0171a10` — HackRF plan: add explicit PowerShell commands for execution + log capture
- `dfc621b` — HackRF plan: fix wrong channel-hash precondition in Test 0a/0b verification
- `59fee47` — HackRF plan: tighten Test 0 intro
- `cd26769` — HackRF plan: Test 0a/0b rewritten as discrete steps with per-step verification
- `a81e10d` — platformio.ini: Radio 2 defaults → Meshtastic LongFast (since superseded by 2.4 GHz hardcode)
- `4b58a8a` — HackRF plan: split Test 0 into Test 0a (sub-GHz) and Test 0b (2.4 GHz)
- `4e1e300` — HackRF plan: extend to dual-band coverage
- `c9370b2` — HackRF plan: switch Test B to fully-cabled KT3 step attenuator
- `8de16ac` — Add HackRF + SDRAngel diagnostic plan
- `96eff6c` — Use GrayHatGuy pseudonym in Seeed reply signature
- `3248d5e` — Update contact email to grayhatguyllc@protonmail.com + annotate Seeed inquiry questions
- `3823318` — Document Phase-1 LR1121 RX bring-up test bed (TESTBED.md + 5 bench photos)
- `7673fad` — Document Seeed reply chain + add SKY13373 datasheet to project

---

## 5. Critical rules of engagement for next session

These are firm. Violating them caused this session to derail:

- **No "optimization" edits without explicit electrical/behavioral analysis.** Commit `949176a`'s MODE_STBY change was made without understanding why the original value existed. Result: silicon damage. **Especially for RF switch tables, PA configs, and timing-sensitive sequences, require a written rationale citing the relevant datasheet section before any change.**
- **Verify electrical/timing claims against the datasheet — including reverts and "no action needed" conclusions.** A revert demands the same datasheet-cited justification as the original change: the `MODE_STBY {1,0}→{0,0}` revert was validated by confirming the 20 µs SKY13373 shutdown-recovery is fully absorbed by the chip's own mode-transition timings (UM v2.2 §2.4 Table 2-15: FS→TX 102 µs, STBY→TX 142–172 µs, FS→RX 39 µs — all exceed 20 µs) and that the chip sequences the switch internally via `SetDioAsRfSwitch` (§4.2.1), so firmware exposes no settling knob to mis-time. **"The chip probably handles it" is not analysis** — cite the section and the numbers, even when the conclusion is that nothing needs to change.
- **Do not propose hardware swaps when the root cause may be code.** Session 2 recommended swapping the LR1121 module before fully investigating whether commit `949176a` was the cumulative-damage cause. Hardware swaps are an expensive experimental tool — eliminate code as a variable first.
- **Do not guess.** If the data doesn't support a conclusion, say "I don't know" and ask for what you need. Speculation framed as analysis caused multiple iterations of wasted work.
- **Owner reviews all outbound communications (Seeed correspondence, upstream PRs).** Drafts only.
- **PowerShell shell, HEREDOC commit messages via Bash tool, `cd "<path>"` for shell context.** Never force-push `main`. Snapshot tag `lr1121-bringup-2026-05-26` is force-pushable; bump after each branch commit.

---

## 6. Quick context recovery for fresh session

If a new session loses context mid-task, read in this order:

1. This file (`CLAUDE.md`)
2. `git log --oneline -20 lr1121-phase1` to confirm HEAD state
3. `docs/REFERENCES.md` — index of all datasheets and vendor docs. **Any LR1121 §-citation in this project refers to the version checked into `docs/datasheets/`.**
4. `LR1121-RX-INIT-AUDIT.md` for the 9-run firmware DOE history (sub-GHz RX deficit refuted across every prescribed UM v2.2 remedy)
5. `SEEED_EMAIL_DRAFT.md` correspondence chain (includes the David Du SKY13373 truth-table reply)
6. `docs/testbed/HACKRF-DIAGNOSTIC-PLAN.md` for the unexecuted SDR test methodology
7. `docs/testbed/TESTBED.md` for the physical bench layout (referenced by all bench-result discussion)

**First action on resumption: confirm with owner what just happened on the bench since this handoff was written. Do NOT assume the firmware state matches what's documented here without verification.**

---

## 7. Owner context note

Owner has been actively debugging this with high engagement. End of session 2 was contentious because my edits this session contaminated the experimental record, the cumulative MODE_STBY change likely caused silicon damage that triggered an unnecessary hardware swap, and the bench session ended without a verified Phase 1 deliverable.

The owner is right to expect: rigorous separation of code changes from bench observations, electrical justification for any RF switch table modification, and direct action when the plan is agreed (no re-asking permission for accepted edits). The most useful thing the next session can do is execute Experiment R1 cleanly and report results without adding new variables.
