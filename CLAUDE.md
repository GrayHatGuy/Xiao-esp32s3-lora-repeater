# CLAUDE.md — Project Handoff

**Project:** Xiao ESP32-S3 dual-radio LoRa mesh bridge (sub-GHz ↔ 2.4 GHz cross-band)
**Repo:** https://github.com/GrayHatGuy/Xiao-esp32s3-lora-repeater
**Local path:** `C:\Users\6r4yh\workspace\Platformio\Projects\xiao esp32 wio sx1262 dual repeater`
**Active branch:** `lr1121-phase1` (HEAD: `e07e39f` as of this handoff — see §4 for last-touched functional code commit `12d685d`; later commits are doc-only)
**Owner identity:** `GrayHatGuy` (pseudonym), `grayhatguyllc@protonmail.com`. **Real name is ephemeral, do not persist or write to docs.**
**Shell:** PowerShell (Windows). Use HEREDOC via Bash tool for commit messages; no inline newlines in shell commands.

---

## 1. Ultimate goal

Phase 1 deliverable: **a single XIAO ESP32-S3 board hosting two LoRa radios (R1 = Wio-SX1262 sub-GHz, R2 = Wio-LR1121 sub-GHz + 2.4 GHz) bridging Meshtastic and MeshCore protocols across bands.** Both bands must work on R2 (Interpretation B locked in by owner — partial 2.4G-only does not ship).

Phase 0 (dual SX1262, sub-GHz only) ships at v8.1 on `main`. Phase 1 is the work happening on `lr1121-phase1`.

---

## 2. Current state (2026-05-30, end of bench session 2)

### Hardware
- Original Wio-LR1121 module: **possibly silicon-damaged by accumulated TX-induced LNA stress** caused by commit `949176a` (see §4). Sub-GHz TX triggers `state=-20` SPI cascade after a stress threshold; chip becomes unresponsive until power cycle. Cascade observed at 35 s, 193 s, ~200 s, 408 s across multiple runs today, both bands.
- Fresh Wio-LR1121 module swapped in as a control. **Boots clean at 2.4 GHz, no cascade through 188 s**, but ALSO does not RX any T3S3 packets (isr=0 throughout). Either DIO9/jumper-wire contact issue on the new module, antenna mismatch, or Wio-LR1121 2.4 GHz path is design-deficient.
- T3S3 LR1121 reference radio currently reconfigured back to **US sub-GHz LongFast** (was 2.4 GHz earlier today). Owner reports R1 SX1262 is **no longer catching T3S3 sub-GHz packets** — unexplained at end of session, owner attributes to my code edits.
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

- `949176a` — **Lock RF switch table to Seeed-authoritative SKY13373 truth table.** Changed `MODE_TX` from `{0,1}` (HP path) → `{1,1}` (LP path, per Seeed authoritative). Changed `MODE_STBY` from `{0,0}` (shutdown) → `{1,0}` (RX-latched). **The MODE_STBY change is the suspected cumulative-damage cause** — removed protective antenna isolation during TX→STBY transitions, allowing residual PA-vent energy to repeatedly stress the LNA. Owner's analysis post-session attributes silicon damage on original module to this commit.

**🟢 Bug-fix commits this session (all needed for 2.4 GHz Meshtastic to work, leave in place):**

- `12d685d` — **fix: LR1121 2.4 GHz begin() rejected wideLora BW (RadioLib API gap).** Workaround in `WioLR1121.cpp`: for 2.4 GHz path, call `LR11x0::begin(bw, sf, cr, sync, preamble, high=true)` directly instead of `LR1120::begin(freq, bw, ...)` convenience overload, which drops the `high` flag and validates BW against sub-GHz range only → `-8 INVALID_BANDWIDTH` for 812.5 kHz. Then manually `setFrequency()` + `setOutputPower()`. RadioLib API design flaw worth upstreaming.
- `0df109a` — **fix: captive portal freq field clobbered user input mid-typing.** Removed `oninput=updAll()` reactive trigger on the freq input. The reactive recompute was overwriting user typing because `setF()` auto-fills the field when value equals previous computed value or is empty.
- `3b1ffe3` — **audit: full sub-GHz / 2.4 GHz / Custom protocol BW accommodation.** (1) `LoraConfigCheck.h` R2 freq static_assert now accepts both 150-960 MHz AND 2400-2500 MHz. (2) `CaptivePortal.cpp applyRadio()` Custom protocol no longer calls `bwAllowed()` — Custom is escape hatch for arbitrary RF experimentation. (3) JS preset table extended with PRE24 (wideLora BWs) + BAND24 range + is24() helper + chip-onchange + freq-onchange wiring.
- `6d78644` — **fix: captive portal saved wrong BW for LR1121 2.4 GHz Meshtastic configs.** Root-cause fix. `applyRadio()` at PROTO_MT branch now passes `wideLora = (chip == LR1121 && freq >= 2400.0f)` to `modemPresetParams()`. Previously dropped wideLora flag → saved 250 instead of 812.5 → silent BW mismatch with T3S3 → zero R2 RX. Also fixed: `LoraConfigCheck.h LORA_CHK_VALID_BW` macro adds 812.5/406.25/1625; `CaptivePortal.cpp ALLOWED_BW[]` runtime array adds same; `presetFromParams()` tries both `wideLora=true` and `false` for reverse lookup.

**🟡 Owner-accepted compile-time hardcode (last action of session):**

- platformio.ini R2 settings changed from `906.875f / 250.0f / 11 / 5 / 20 / 0x2B` to `2404.46875f / 812.5f / 11 / 5 / 10 / 0x2B`. This change applies after NVS erase or on first boot without saved config. Owner accepted this edit to bypass the portal for fresh-module 2.4 GHz testing.

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

- **No mid-experiment code edits.** If the bench is running a test, do NOT modify firmware or docs in ways that could affect the observation. Edits contaminate the experimental record.
- **No "optimization" edits without explicit electrical/behavioral analysis.** Commit `949176a`'s MODE_STBY change was made without understanding why the original value existed. Result: silicon damage. **Especially for RF switch tables, PA configs, and timing-sensitive sequences, require a written rationale citing the relevant datasheet section before any change.**
- **Do not propose hardware swaps when the root cause may be code.** Session 2 recommended swapping the LR1121 module before fully investigating whether commit `949176a` was the cumulative-damage cause. Hardware swaps are an expensive experimental tool — eliminate code as a variable first.
- **Do not guess.** If the data doesn't support a conclusion, say "I don't know" and ask for what you need. Speculation framed as analysis caused multiple iterations of wasted work.
- **Edits accepted in plan = ship the edit.** Do not re-ask permission after the owner has accepted the approach. Asking again is friction; the owner has decision-making authority and will revert if wrong.
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
