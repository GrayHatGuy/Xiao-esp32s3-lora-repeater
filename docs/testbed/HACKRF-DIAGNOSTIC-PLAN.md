# HackRF + SDRAngel Diagnostic Plan — Wio-LR1121 RX Bring-Up (Dual-Band)

> **▶ Just want to run task #5? Start with [`HACKRF-QUICKSTART.md`](HACKRF-QUICKSTART.md)** —
> a short linear runbook for the current bench (sub-GHz, HackRF-as-source, RX-only firmware).
> This document is the full reference (dual-band, every table/option); dip into it from the
> quick-start when a step needs detail.
>
> **Session-3 deltas (2026-06-01) vs this doc as written:** (1) OTA test source is now a
> **Heltec V4 on COM11**, not T3S3/COM5 — only matters for Test 0a; Test B/A use the HackRF as
> source. (2) Focus is **sub-GHz**; skip the 2.4 GHz arcs for #5. (3) Firmware already has the
> **IRQ-status heartbeat** (`irq=` field: `0x08`=RX_DONE, `0x10`=preamble-only) and
> **`R2_RX_ONLY_TEST`** active — ideal for Test B; remove the `-D` and reflash for Test A.
> (4) R2's failure is a **marginal sensitivity/demod deficit**, not total deafness — see
> `CLAUDE.md` §2 session-3 block.

**Purpose:** Localize the Wio-LR1121's RX failure mode that the firmware DOE in [`../../LR1121-RX-INIT-AUDIT.md`](../../LR1121-RX-INIT-AUDIT.md) could not resolve from the chip side. This document now covers **both** the bench-proven-broken **sub-GHz path** (LR1121 `RFI_P/N_LF` ↔ module pad 23 `SUBG_RF`, switched by the on-module SKY13373-460LF per V1=DIO5/V2=DIO6) **and** the **never-tested 2.4 GHz path** (LR1121 `RFIO_HF` ↔ module pad 2 `2.4G_RF`, NOT switched by the SKY13373).

The 2.4 GHz path is the **production-intended band per [`LR1121-SPEC.md`](../../LR1121-SPEC.md)** — Phase 1 design has the LR1121 operating at 2.4 GHz and the SX1262 sibling handling sub-GHz. Sub-GHz testing on the LR1121 was a debugging convenience (A/B comparison against the SX1262 reference) that surfaced a failure but does not gate Phase 1 shipping if 2.4 GHz works.

**Four tests, sequenced for highest information-per-minute first:**

| Test | Band | Equipment needed | ~Time | Information yield |
|---|---|---|---|---|
| **Test 0a — Sub-GHz Baseline** | Sub-GHz | Live Meshtastic neighborhood traffic | 5 min | Reproduces Run 7/8 failure with current firmware |
| **Test 0b — 2.4 GHz Baseline** | 2.4 GHz | T3S3 LR1121 (OTA source); 2.4 GHz IPEX antenna; firmware reconfig | 15 min | **Phase 1 deliverable status** |
| **Test C — Environment Sweep** | Both | HackRF as receiver | 15 min | Ambient RF noise floor characterization |
| **Test A — TX Power A/B** | Both | HackRF as receiver; SX1262 ref (sub-GHz) or T3S3 LR1121 ref (2.4 GHz) | 30 min/band | Matching-network-vs-chip-internal split |
| **Test B — Sensitivity Floor** | Both | HackRF as TX; KT3 step attenuator; SMA-N adapters | 30 min/band | Numerical sensitivity deficit in dB |

**Bench:** as documented in [`TESTBED.md`](TESTBED.md). The HackRF is added as an external instrument — the existing XIAO + Wio-SX1262 + Wio-LR1121 bridge stays running unmodified for sub-GHz tests. For 2.4 GHz tests the Wio-LR1121 IPEX pigtail physically moves from module pad 23 to module pad 2 (see Test 0b for details).

**Equipment owned and assumed available:**

- HackRF One SDR (1 MHz – 6 GHz, ±13 dBm TX max, 8-bit ADC, ~9 dB noise figure)
- SDRAngel (latest release) with both **ChirpChat Demodulator** and **ChirpChat Modulator** plugins enabled. The demod page documents the plugin's parameter coverage and confirms a matching modulator exists.
- **LilyGO T3S3 LR1121** — configured for 2.4 GHz Meshtastic, working RX + TX. Already on the bench. Used as the known-good OTA source for 2.4 GHz tests (Test 0b and Test B 2.4 GHz variant).
- **LilyGO T-Watch S3** — SX1262 + SX1280 dual radio, both bands working. Backup 2.4 GHz OTA source via the SX1280; also can act as an independent 2.4 GHz reference receiver.
- **2.4 GHz IPEX antenna** — any standard WiFi/Bluetooth IPEX (u.fl) antenna. Required for the Wio-LR1121 2.4 GHz tests; the current sub-GHz rubber-duck cannot be reused at 2.4 GHz.
- **KT3-2N-90/1S step attenuator** — 0 to 90 dB in 1 dB steps, N-female connectors both ends, typically DC–3 GHz. **Verify the actual upper frequency limit before Test B at 2.4 GHz** — some KT3 variants stop at 1 GHz; the 2N-90/1S spec sheet should confirm 2.5 GHz coverage. If it does not cover 2.4 GHz cleanly, Test B at 2.4 GHz falls back to HackRF gain control + fixed pad chain.
- **5 dB SMA fixed pad** — lives permanently on the HackRF TX port as VSWR protection during Test B.
- **2× SMA-to-N adapters** (one SMA-male → N-male; one N-male → SMA-female) — to interface the KT3 (N-type) with the rest of the SMA-based bench. ~$10–15 on Amazon, DC–6 GHz rated.
- **1× short SMA male/male jumper** (6–12 inches, RG-316 or better) — to connect the post-KT3 SMA-female adapter to an IPEX-SMA pigtail.
- **1× spare IPEX-to-SMA pigtail** — same type used for your normal antenna feeds, but a dedicated one for the test rig avoids having to repeatedly unplug the radio's antenna.
- HackRF stock telescoping whip antenna (for Tests A and C only — not used in Test B)
- USB cable for HackRF
- The bench from `TESTBED.md` (both radios powered, firmware running, serial monitor open)

**Cabled Test B chain layout:**

```
HackRF TX (SMA-f) → 5 dB SMA pad → SMA-m / N-m adapter
                                  ↓
                                  KT3-2N-90/1S step attenuator (sweep 0–90 dB)
                                  ↓
                                  N-m / SMA-f adapter → short SMA jumper → IPEX-SMA pigtail → DUT IPEX port
```

Total fixed insertion loss (excluding HackRF gain and KT3 setting): **~6 dB** = 5 dB pad + 2× adapters (~0.4 dB) + jumper (~0.5 dB) + IPEX-SMA pigtail (~0.5 dB).

**Dynamic range available:** HackRF IF gain swing (47 dB) + KT3 swing (90 dB) = **137 dB**. With HackRF TX at IF gain 47 dB (~+13 dBm) and KT3 at 90 dB, the power at the DUT antenna port is approximately **−83 dBm**. With HackRF IF gain 0 (~−40 dBm) and KT3 at 90 dB, it's approximately **−131 dBm** — at or below the LR1121 datasheet sensitivity spec of −134 dBm.

**Total time estimate:**

| Scenario | Time |
|---|---|
| SDRAngel one-time setup | ~15 min |
| Test 0a + 0b (dual-band baseline) | ~20 min |
| 0b PASS → optional sub-GHz characterization (Tests C/A/B sub-GHz) | ~70 min more |
| 0b PASS → optional 2.4 GHz characterization (Tests C/A/B 2.4 GHz) | ~70 min more |
| 0b FAIL → full dual-band characterization (all Tests at both bands) | ~150 min more |
| **Best-case session (0a EXPECTED + 0b PASS, ship sub-GHz documentation)** | **~35 min** |
| **Worst-case session (full dual-band characterization required)** | **~170 min** |

---

## Pre-flight — one-time SDRAngel setup (~15 min)

Do this once. The configuration persists between sessions.

### 1. Confirm plugins are enabled

1. Launch SDRAngel.
2. Top menu → **Preferences → Plugins**.
3. In the list, confirm these three are enabled (checkbox ON):
   - **HackRF Input** (devices/input/hackrfinput)
   - **HackRF Output** (devices/output/hackrfoutput)
   - **ChirpChat Demodulator** (channelrx/demodchirpchat)
   - **ChirpChat Modulator** (channeltx/modchirpchat)
4. Restart SDRAngel if you changed anything.

### 2. Create the RX device configuration (used by Tests A and C)

1. Top menu → **Devices → Add sample source**.
2. Select **HackRF Input**, click OK.
3. In the HackRF Input control panel that appears:
   - **Center frequency:** `906.875000 MHz`
   - **Sample rate:** `2000000 Sa/s` (2 MS/s — wide enough for the BW250 LoRa signal)
   - **LNA gain:** `16 dB`
   - **VGA gain:** `20 dB`
   - **AMP:** OFF (unchecked) — these gain values stay FIXED for the entire test session; do not auto-AGC anything.
   - **Bias-T:** OFF
4. Add a channel: **Channels → Add channel → ChirpChat Demodulator**.
5. In the ChirpChat Demodulator panel:
   - **Δf (channel offset):** `0 Hz`
   - **BW:** `250000 Hz` (250 kHz)
   - **SF:** `11`
   - **CR:** `4/5`
   - **DE (low data rate optimization):** `2` (required for SF11/BW250 per the plugin's documentation — matches Meshtastic LongFast)
   - **Sync word:** `0x2B` (Meshtastic public)
   - **CRC:** ON
   - **Preamble chirps:** `8` (LoRa preamble is 8 symbols; the firmware-side `preambleLen=16` setting includes the sync detection symbols)
   - **Packet length:** `Implicit` OFF (use explicit header — Meshtastic default)
6. Also add: **Channels → Add channel → Spectrum Analyzer**. This gives a clean waterfall + peak-hold display alongside the ChirpChat decoder.
7. **Save preset:** top menu → **Preset → Save as → "LR1121 RX 906875"**.

### 3. Create the TX device configuration (used by Test B)

1. Top menu → **Devices → Add sample sink**.
2. Select **HackRF Output**, click OK.
3. In the HackRF Output control panel:
   - **Center frequency:** `906.875000 MHz`
   - **Sample rate:** `2000000 Sa/s`
   - **IF gain:** `30 dB` (starting value — you'll sweep this in Test B)
   - **AMP:** OFF (unchecked)
   - **Bias-T:** OFF
4. Add channel: **Channels → Add channel → ChirpChat Modulator**.
5. Set identical modem params to the demodulator (BW 250 / SF 11 / CR 4/5 / DE 2 / sync 0x2B / CRC ON / preamble 8).
6. **Message generator** section of the modulator panel:
   - Mode: `Continuous` (TX repeats automatically)
   - Period: `1000 ms`
   - Payload: `HACKRF-TEST-{counter}` (literal string with %d incrementing counter — the exact bytes don't matter, just that they decode to something recognizable in your firmware logs)
7. **Save preset:** **Preset → Save as → "LR1121 TX 906875"**.

### 4. Verify HackRF basic operation

1. Load the RX preset.
2. Click **Start** (RX). You should see noise on the waterfall.
3. Trigger an SX1262 TX from your firmware (e.g., let the bench bridge do its periodic NodeInfo TX). You should see a chirp burst on the waterfall and the ChirpChat decoder should print a packet to its message log. If yes, your SDRAngel setup is working.
4. **Stop** RX before moving on.

---

## Test 0 — Dual-Band Baseline (~20 min total, NO HackRF required)

**What each test does:**

- **Test 0a (sub-GHz, ~5 min):** verify the Wio-LR1121 is still deaf at 906.875 MHz Meshtastic LongFast (same failure mode as prior 8 DOE runs — see [`LR1121-RX-INIT-AUDIT.md`](../../LR1121-RX-INIT-AUDIT.md) for what each prior run did and why they all failed).
- **Test 0b (2.4 GHz, ~15 min):** first-ever 2.4 GHz test on the Wio-LR1121. Determines if Phase 1's intended production band works.

**Code changes required: NONE.** Build current HEAD, flash, run.

**Compile-time config: ALREADY DONE.** Commit `a81e10d` set Radio 2 platformio.ini defaults to Meshtastic LongFast (906.875 / 250 / SF 11 / 0x2B). For Test 0b, Radio 2 gets reconfigured to 2.4 GHz at runtime via the captive portal (not via code edits).

**OTA traffic source:**

- Test 0a: you actively send 3 messages from the T3S3 on COM5 via `meshtastic --sendtext` (controlled source), plus any neighborhood Meshtastic packets that arrive naturally during the window (passive bonus).
- Test 0b: you actively send 3 messages from the T3S3 via `meshtastic --sendtext`. No ambient 2.4 GHz Meshtastic traffic expected. All OTA is driven by you.

**Order:** Test 0a first, then 0b. If 0a fails to reproduce the prior 8-run failure, stop and figure out what changed before swapping antennas for 0b.

---

### Test 0a — Sub-GHz Baseline (~5 min execution + ~5 min setup verification)

**Goal:** confirm Runs 0–8 sub-GHz failure mode still reproduces with current committed firmware. Reference: T3S3 LR1121 (independent LR1121 at same Meshtastic LongFast preset) and R1 SX1262 (sub-GHz reference radio).

#### Procedure (PowerShell commands; do each step in order; verify before proceeding)

**Notation:** `$ts` is a timestamp variable used in log filenames. Set it once at start of session so all files for one run share a stamp.

```powershell
# === SESSION INITIALIZATION (run once before starting) ===
cd "C:\Users\6r4yh\workspace\Platformio\Projects\xiao esp32 wio sx1262 dual repeater"
$ts = Get-Date -Format "yyyyMMdd-HHmmss"
$logdir = "docs\testbed\run-results"
New-Item -ItemType Directory -Force -Path $logdir | Out-Null
Write-Host "Session timestamp: $ts"
```

```
STEP 1.  Build current firmware
  COMMAND:
            pio run 2>&1 | Tee-Object -FilePath "$logdir\test-0a-$ts-build.log"
            git rev-parse HEAD | Out-File "$logdir\test-0a-$ts-commit.txt"
  VERIFY:   Build ends with "SUCCESS". Note the commit hash.
  RECORD:   Commit hash from commit.txt:  ____________

STEP 2.  Flash device
  COMMAND:
            pio run -t upload 2>&1 | Tee-Object -FilePath "$logdir\test-0a-$ts-flash.log"
  VERIFY:   "[SUCCESS] Took N seconds" appears at the end.

STEP 3.  Capture T3S3 reference radio info (one-off; before opening bridge monitor)
  COMMAND:
            meshtastic --port COM5 --info 2>&1 | Out-File "$logdir\test-0a-$ts-t3s3-info.txt"
            Select-String -Path "$logdir\test-0a-$ts-t3s3-info.txt" -Pattern "myNodeNum|^Owner|user.{0,200}id"
  VERIFY:   T3S3 connects (no "Could not connect" error in the file).
            Select-String surfaces the node ID line.
  RECORD:   T3S3 node ID (8-char hex from "user id" or hex of myNodeNum):  !________

STEP 4.  Open the bridge serial monitor and capture boot log to file
  COMMAND:
            pio device monitor --port COM6 --filter direct 2>&1 |
              Tee-Object -FilePath "$logdir\test-0a-$ts-bridge.log"
  ACTION:   Once monitor is connected, press the XIAO RESET button (or briefly
            unplug/replug USB) to power-cycle the bridge.
  VERIFY:   Boot log streams to terminal AND to file. Wait for "Bridge active."
            before proceeding to Step 5. (Do NOT Ctrl-C the monitor yet.)

STEP 5.  Verify firmware preconditions in the boot log
  ACTION:   With the monitor still running, open a SECOND PowerShell window
            in the same project directory.
  COMMAND (in the second window):
            $ts = "<paste your session timestamp from Step 1 here>"
            $logdir = "docs\testbed\run-results"
            Select-String -Path "$logdir\test-0a-$ts-bridge.log" `
              -Pattern "Radio1 = .*, Radio2 = .*|\[MT\] channel=|Radio1-B2B\] ready|installing RF switch table|RX-AUDIT diag.*getErrors|Radio2-Edge\] ready"

  VERIFY:   Six matching lines appear (one per precondition):

   [5a] "[setup] Radio1 = SX1262, Radio2 = LR1121"
        → R2 chip-detect must be LR1121. If not, NVS chip-type override is
          wrong. STOP, fix via portal.

   [5b] One or more "[MT] channel=" lines, and at least ONE MUST be:
            channel="LongFast" hash=0x08 keyLen=16
              key=d4f1bb3a20290759f0bcffabcf4e6901
        → If LongFast hash 0x08 is NOT registered, the bridge can't decode
          neighborhood LongFast packets even if R2 demodulates them.

   [5c] "[Radio1-B2B] ready — 906.875 MHz  BW 250.0 kHz  SF11  CR4/5  20 dBm
        sync 0x2B"
        → R1 SX1262 on Meshtastic LongFast.

   [5d] "[Radio2-Edge] installing RF switch table: MODE_RX = D5=1 D6=0
        D7=0 D8=0 D10=0 (BRUTEFORCE_RX_DIOMASK=1)"
        → Seeed-authoritative locked switch table.

   [5e] "[Radio2-Edge] [RX-AUDIT diag] post-setRfSwitchTable getErrors()
        state=0 errors=0x0020"
        → state=0, errors=0x0020 (HF_XOSC_START_ERR sticky on TCXO chips).
        → Any other errors value: STOP. Modality changed since Run 8.

   [5f] "[Radio2-Edge] ready — 906.875 MHz  BW 250.0 kHz  SF11  CR4/5
        20 dBm  sync 0x2B  sub-GHz"
        → R2 on Meshtastic LongFast sub-GHz. "sub-GHz" tag confirms
          is2g4 branch did NOT trigger.

  RECORD:   All six precondition lines confirmed Y/N: ____
            If any N: STOP. Do not proceed to Step 6.

STEP 6.  Start a 5-minute observation window
  ACTION:   Note start time. Bridge monitor stays running (Step 4 window).
  COMMAND (in the second PowerShell window, executed once per minute over 3 minutes):
            meshtastic --port COM5 --sendtext "Test 0a #1"
            Start-Sleep -Seconds 60
            meshtastic --port COM5 --sendtext "Test 0a #2"
            Start-Sleep -Seconds 60
            meshtastic --port COM5 --sendtext "Test 0a #3"
            Start-Sleep -Seconds 120
            # Total elapsed: 5 minutes from start of Send #1

  VERIFY:   T3S3 CLI prints "Sending Text Message..." for each of the 3 sends
            without error.
  RECORD:   Start timestamp (note when you ran Send #1):  _________
            End timestamp (after final Sleep):            _________

STEP 7.  Stop bridge monitor and confirm log captured
  ACTION:   In the FIRST PowerShell window (the one running pio device monitor),
            press Ctrl-C to exit. Tee-Object flushes the buffer to file.
  COMMAND:
            Get-ChildItem "$logdir\test-0a-$ts-bridge.log" | Format-List Length, LastWriteTime
            (Get-Content "$logdir\test-0a-$ts-bridge.log" | Measure-Object -Line).Lines
  VERIFY:   File exists. Length > 0. Line count is more than ~50 (boot log +
            5 min of operation should be at least that many lines).
  RECORD:   Log path:  $logdir\test-0a-$ts-bridge.log
            Line count:  _________
```

#### Step verification — fill in BEFORE analyzing data

This catches DoE violations before they contaminate analysis. If any row is
not VERIFIED, the test is INVALID and must be re-run.

| Step | Precondition | VERIFIED? (Y/N) | Notes |
|---|---|---|---|
| 1 | Firmware built clean | _____ | commit `__________` |
| 2 | Flash succeeded | _____ | |
| 3 | T3S3 info captured to file | _____ | T3S3 node ID: `!________` |
| 4 | Bridge monitor running, "Bridge active." seen | _____ | log file: `test-0a-$ts-bridge.log` |
| 5a | R2 chip = LR1121 | _____ | |
| 5b | "LongFast" hash=0x08 channel registered | _____ | other channels also seen: `_______` |
| 5c | R1 at Meshtastic LongFast 906.875 | _____ | |
| 5d | R2 switch table = D5=1 D6=0 ... | _____ | |
| 5e | getErrors state=0 errors=0x0020 | _____ | actual: `0x____` |
| 5f | R2 at Meshtastic LongFast 906.875 sub-GHz | _____ | |
| 6 | 3 T3S3 sendtext commands succeeded | _____ | start time: `_______` end: `_______` |
| 7 | Log file saved with ≥50 lines | _____ | line count: `_____` |
| 5 | T3S3 node ID captured | _____ | `!________` |
| 6 | 3 T3S3 messages sent during window | _____ | |
| 7 | Complete log saved | _____ | path: `__________` |

**If ANY row above is N → STOP. Test 0a is INVALID. Do not proceed to data
analysis. Fix the failed precondition and re-execute from Step 1.**

#### Data analysis (only run after every Step-verification row above is Y)

PowerShell commands; run from project root with `$ts` set to your session timestamp:

```powershell
# Re-set $ts if your shell history was lost between steps
$ts = "<paste your session timestamp>"
$log = "docs\testbed\run-results\test-0a-$ts-bridge.log"

# ANALYSIS 1 — Count R1 (SX1262) RX events
$r1_rx = Select-String -Path $log -Pattern "\[R1 RX\]"
"R1 RX count: $($r1_rx.Count)"
$r1_rx | ForEach-Object { $_.Line } | Select-String -Pattern "RSSI -?\d+\.\d+" |
  ForEach-Object { $_.Matches[0].Value } | Sort-Object -Unique

# ANALYSIS 2 — Count R2 (LR1121) RX events broken out by src
$r2_rx = Select-String -Path $log -Pattern "\[R2 decoded\] Meshtastic src=0x[0-9A-Fa-f]{8}"
$r2_self_echo  = $r2_rx | Where-Object { $_.Line -match "src=0x75D7AC1C" }
$r2_from_t3s3  = $r2_rx | Where-Object { $_.Line -match "src=0x<T3S3_HEX_NODENUM>" }  # paste T3S3 hex here
$r2_other_ext  = $r2_rx | Where-Object {
                          $_.Line -notmatch "src=0x75D7AC1C" -and
                          $_.Line -notmatch "src=0x<T3S3_HEX_NODENUM>"
                       }
"R2 total RX events:        $($r2_rx.Count)"
"R2 self-echo (own bridge): $($r2_self_echo.Count)"
"R2 from T3S3:              $($r2_from_t3s3.Count)"
"R2 from other external:    $($r2_other_ext.Count)"
"R2 total EXTERNAL:         $($r2_from_t3s3.Count + $r2_other_ext.Count)"

# ANALYSIS 3 — Compute deficit
$deficit = $r1_rx.Count - ($r2_from_t3s3.Count + $r2_other_ext.Count)
"DEFICIT: R1 caught $($r1_rx.Count) external packets; R2 caught $($r2_from_t3s3.Count + $r2_other_ext.Count). Deficit = $deficit"
```

#### Record analysis results

| Quantity | Value |
|---|---|
| R1 (SX1262) total external RX count | _____ |
| R1 RSSI range observed (dBm) | ____ to ____ |
| R1 distinct source node IDs | `___________________________` |
| R2 (LR1121) total RX events | _____ |
| R2 self-echo (src=0x75D7AC1C) | _____ |
| R2 from T3S3 (src=T3S3 node ID) | _____ |
| R2 from other external sources | _____ |
| R2 total external (T3S3 + other) | _____ |
| Deficit (R1 external − R2 external) | _____ |

#### Pass / fail determination

| R2 external RX events | R1 external RX events | Outcome | Action |
|---|---|---|---|
| 0 | ≥ 3 | **EXPECTED** — reproduces Run 7/8 failure | Proceed to Test 0b |
| 1–2 | ≥ 5 | **PARTIAL** — marginal improvement vs Run 7/8 | Flag for review; investigate firmware delta vs commit 8de16ac before proceeding |
| ≥ 3 | ≥ 3 | **UNEXPECTED** — sub-GHz appears to work | STOP. Compare firmware delta vs commit 8de16ac. Find what changed. |
| 0 | 0 | **INDETERMINATE** — no OTA traffic in window | Re-run Test 0a at a different time of day with confirmed traffic |

---

### Test 0b — 2.4 GHz Baseline (~15 min execution + ~10 min setup)

**Goal:** answer whether the Wio-LR1121 receives at all on its production-intended 2.4 GHz band. Reference: T3S3 LR1121 reconfigured for 2.4 GHz Meshtastic (or T-Watch S3 SX1280 alternate).

**Prerequisite:** Test 0a completed with EXPECTED or PARTIAL outcome.

#### Procedure (PowerShell commands; do each step in order; verify before proceeding)

```powershell
# === SESSION INITIALIZATION (run once before starting Test 0b) ===
cd "C:\Users\6r4yh\workspace\Platformio\Projects\xiao esp32 wio sx1262 dual repeater"
$ts = Get-Date -Format "yyyyMMdd-HHmmss"
$logdir = "docs\testbed\run-results"
New-Item -ItemType Directory -Force -Path $logdir | Out-Null
Write-Host "Session timestamp: $ts"
```

```
STEP 1.  Reconfigure T3S3 for 2.4 GHz Meshtastic LongFast
  COMMAND:
            meshtastic --port COM5 --set lora.region 4 `
                                   --set lora.use_preset true `
                                   --set lora.modem_preset LONG_FAST 2>&1 |
              Tee-Object -FilePath "$logdir\test-0b-$ts-t3s3-reconfig.log"
            Start-Sleep -Seconds 30   # let T3S3 reboot and re-tune

  VERIFY:   reconfig log shows no errors, "Writing modified preferences" appears.

STEP 2.  Capture T3S3 post-reconfig state and discover actual frequency
  COMMAND:
            meshtastic --port COM5 --get lora 2>&1 |
              Out-File "$logdir\test-0b-$ts-t3s3-lora.txt"
            meshtastic --port COM5 --info 2>&1 |
              Out-File "$logdir\test-0b-$ts-t3s3-info.txt"

            Select-String -Path "$logdir\test-0b-$ts-t3s3-lora.txt" `
              -Pattern "region|bandwidth|spread_factor|coding_rate|channel_num|override_frequency"

  VERIFY:   lora.region = 4
            lora.bandwidth ≈ 812.5
            lora.spread_factor = 11
            lora.coding_rate = 5

  RECORD:   T3S3 channel_num:           _____
            T3S3 override_frequency:    _____ MHz (if 0, compute below)
            T3S3 node ID:               !________
            T3S3 actual operating freq: _____ MHz
              (if override_frequency is 0, compute:
               2400.0 + ((channel_num - 0.5) * 0.8125)
               example: channel_num=20 → 2400.0 + 19.5*0.8125 = 2415.84375 MHz)

STEP 3.  Power down Wio-LR1121 bridge for antenna swap
  ACTION:   Disconnect XIAO USB cable from the host PC.
  VERIFY:   XIAO power LED off.

STEP 4.  Physical antenna swap on the Wio-LR1121 module
  ACTION:   (a) Carefully un-click the existing IPEX pigtail from module
                pad 23 (SUBG_RF port).
            (b) Click a fresh IPEX pigtail (or move the same one) onto
                module pad 2 (2.4G_RF port).
            (c) Screw a 2.4 GHz IPEX antenna onto the SMA end of the pigtail.
  VERIFY:   Pigtail audibly clicks into pad 2.
            R1 (SX1262) antenna untouched.
  RECORD:   Antenna swap confirmed visually: Y/N: ____

STEP 5.  Reconfigure R2 to 2.4 GHz via captive portal
  ACTION:   (a) Reconnect XIAO USB. Device powers up.
            (b) On your PC, connect WiFi to the XIAO's captive portal AP
                (SSID printed in bridge boot log, viewable at COM6 monitor).
            (c) Open http://192.168.4.1 in browser.
            (d) Set Radio 2 fields:
                  Chip:              LR1121
                  Frequency (MHz):   <T3S3 actual operating freq from Step 2>
                  Bandwidth (kHz):   812.5
                  Spreading factor:  11
                  Coding rate:       5
                  Sync word (hex):   0x2B
                  TX power (dBm):    13
                  MT channel name:   LongFast    (matches the T3S3's channel
                                                  so decoded packets register)
            (e) Click Save. Device reboots automatically.

  VERIFY:   Portal shows "Configuration saved" without validation errors.
  RECORD:   Portal save success Y/N: ____
            If N, error message: _________________________________

STEP 6.  Capture bridge boot log and verify 2.4 GHz preconditions
  COMMAND:
            pio device monitor --port COM6 --filter direct 2>&1 |
              Tee-Object -FilePath "$logdir\test-0b-$ts-bridge.log"
  ACTION:   Once monitor connects, press XIAO RESET (or unplug/replug USB).
            Wait for "Bridge active." line, then in a SECOND PowerShell window:

  COMMAND (in second window):
            $ts = "<paste session timestamp>"
            $logdir = "docs\testbed\run-results"
            Select-String -Path "$logdir\test-0b-$ts-bridge.log" `
              -Pattern "Radio2-Edge\] ready|RX-AUDIT diag.*getErrors|startReceive|\[MT\] channel"

  VERIFY:   Four matching lines appear:

   [6a] "[Radio2-Edge] ready — <FREQ> MHz  BW 812.5 kHz  SF11  CR4/5
        13 dBm  sync 0x2B  2.4GHz"
        → FREQ must match T3S3 operating frequency from Step 2 (±0.001 MHz).
        → "2.4GHz" tag at end confirms is2g4 branch DID trigger.
        → If "sub-GHz" tag appears OR frequency differs, portal save did not
          apply. STOP, redo Step 5.

   [6b] "[Radio2-Edge] [RX-AUDIT diag] post-setRfSwitchTable getErrors()
        state=0 errors=0x____"
        → state=0 required.
        → Errors value may differ from sub-GHz 0x0020 since the chip's RX
          path is now on RFIO_HF, not LF. Note actual value.

   [6c] "[Radio2-Edge] startReceive() = 0"
        → Required.

   [6d] "[MT] channel=\"<NAME>\" hash=0x<HH>" — at least one [MT] channel
        line whose channel matches what the T3S3 reported in Step 2.
        → If hash differs from what T3S3 uses, R2's decoded packets won't
          surface as "[R2 decoded]" events. (Demod will still work, but
          you won't see decoded payload.)

  RECORD:   All four preconditions Y/N: ____
            R2 actual frequency in boot log:  _____ MHz
            R2 actual errors value:           `0x____`
            Bridge [MT] channel name/hash:    `_______` / `0x__`

STEP 7.  Confirm T3S3 is alive on 2.4 GHz with a sanity ping
  COMMAND:
            meshtastic --port COM5 --sendtext "Test 0b sanity check" 2>&1 |
              Tee-Object -FilePath "$logdir\test-0b-$ts-t3s3-sends.log"
  VERIFY:   T3S3 CLI prints "Sending Text Message..." without error.
  RECORD:   T3S3 TX confirmed: Y/N: ____

STEP 8.  Start the 5-minute observation window
  ACTION:   Note start time. Bridge monitor stays running (Step 6 window).
  COMMAND (in the second PowerShell window):
            meshtastic --port COM5 --sendtext "Test 0b #1" 2>&1 |
              Add-Content "$logdir\test-0b-$ts-t3s3-sends.log"
            Start-Sleep -Seconds 60
            meshtastic --port COM5 --sendtext "Test 0b #2" 2>&1 |
              Add-Content "$logdir\test-0b-$ts-t3s3-sends.log"
            Start-Sleep -Seconds 60
            meshtastic --port COM5 --sendtext "Test 0b #3" 2>&1 |
              Add-Content "$logdir\test-0b-$ts-t3s3-sends.log"
            Start-Sleep -Seconds 120
            # Total elapsed: 5 minutes

  RECORD:   Start timestamp (when Send #1 ran):  _________
            End timestamp:                       _________

STEP 9.  Stop bridge monitor and confirm log captured
  ACTION:   In the FIRST PowerShell window (the one running pio device monitor),
            press Ctrl-C to exit.
  COMMAND:
            Get-ChildItem "$logdir\test-0b-$ts-bridge.log" | Format-List Length, LastWriteTime
            (Get-Content "$logdir\test-0b-$ts-bridge.log" | Measure-Object -Line).Lines
  VERIFY:   File exists, line count >50.
  RECORD:   Log path:  $logdir\test-0b-$ts-bridge.log
            Line count: _________
```

#### Step verification — fill in BEFORE analyzing data

| Step | Precondition | VERIFIED? (Y/N) | Notes |
|---|---|---|---|
| 1 | T3S3 reconfig command succeeded | _____ | |
| 2 | T3S3 actual operating freq known | _____ | freq `____` MHz, node `!____` |
| 3 | XIAO powered down for antenna swap | _____ | |
| 4 | Antenna physically swapped pad 23 → pad 2 | _____ | |
| 5 | Portal save accepted R2 = 2.4 GHz | _____ | |
| 6a | R2 boot log shows expected freq, "2.4GHz" tag | _____ | freq in log: `____` MHz |
| 6b | getErrors state=0 | _____ | actual errors: `0x____` |
| 6c | startReceive() = 0 | _____ | |
| 6d | T3S3's 2.4 GHz channel registered on bridge | _____ | T3S3 hash=`0x__` bridge hash=`0x__` |
| 7 | T3S3 confirmed alive (sanity-check sendtext) | _____ | |
| 8 | 3 T3S3 messages sent during window | _____ | start: `____` end: `____` |
| 9 | Log file saved with ≥50 lines | _____ | line count: `____` |

**If ANY row above is N → STOP. Test 0b is INVALID. Do not proceed to data
analysis. Fix the failed precondition and re-execute from the failed step.**

#### Data analysis (only run after every verification row is Y)

PowerShell commands; run from project root with `$ts` set to your session timestamp:

```powershell
$ts = "<paste your session timestamp>"
$log = "docs\testbed\run-results\test-0b-$ts-bridge.log"

# ANALYSIS 1 — Count R2 (LR1121) RX events from T3S3 at 2.4 GHz
$r2_demod = Select-String -Path $log -Pattern "Radio2-Edge\] read: pktLen=\d+"
$r2_decoded = Select-String -Path $log -Pattern "\[R2 decoded\] Meshtastic src=0x[0-9A-Fa-f]{8}"
$r2_crc_err = Select-String -Path $log -Pattern "Radio2-Edge\] read: pktLen=\d+ state=-7"

# Replace <T3S3_HEX> with the hex representation of T3S3 node ID
$T3S3_HEX = "<paste T3S3 8-char hex here (no ! prefix)>"
$r2_from_t3s3   = $r2_decoded | Where-Object { $_.Line -match "src=0x$T3S3_HEX" }
$r2_self_echo   = $r2_decoded | Where-Object { $_.Line -match "src=0x75D7AC1C" }
$r2_other       = $r2_decoded | Where-Object {
                                $_.Line -notmatch "src=0x$T3S3_HEX" -and
                                $_.Line -notmatch "src=0x75D7AC1C"
                              }

"R2 total demod events (pktLen>0):           $($r2_demod.Count)"
"R2 successful decodes:                       $($r2_decoded.Count)"
"  from T3S3 (src=0x$T3S3_HEX):               $($r2_from_t3s3.Count)"
"  self-echo (src=0x75D7AC1C):                $($r2_self_echo.Count)"
"  other:                                     $($r2_other.Count)"
"R2 CRC mismatches (state=-7):                $($r2_crc_err.Count)"
```

#### Record analysis results

| Quantity | Value |
|---|---|
| R2 total demod events (pktLen > 0) | _____ |
| R2 successful decodes total | _____ |
| R2 decodes from T3S3 | _____ |
| R2 self-echo decodes | _____ |
| R2 other-source decodes | _____ |
| R2 CRC mismatches (state=-7) | _____ |

#### Pass / fail determination

| R2 RX from T3S3 | R2 RX with CRC mismatch | Outcome | Implication |
|---|---|---|---|
| ≥ 1 | any | **PASS** | 2.4 GHz works. Phase 1 unblocked. |
| 0 | ≥ 1 | **INCONCLUSIVE** | Demod fires but payload bad → modem-param mismatch. Re-check Step 1 matches Step 4. |
| 0 | 0 | **FAIL** | 2.4 GHz also broken. Proceed to Tests A/B at 2.4 GHz with HackRF. |

#### Combined Test 0 decision branches

| 0a outcome | 0b outcome | Next action |
|---|---|---|
| EXPECTED | PASS | Phase 1 unblocked. Ship LR1121 production-locked to 2.4 GHz. Sub-GHz HackRF tests become optional Seeed-evidence work. |
| EXPECTED | FAIL | Both bands broken. Proceed to full Tests A/B at both bands. Reframe Seeed follow-up to include 2.4 GHz failure question. |
| EXPECTED | INCONCLUSIVE | Fix modem-param mismatch between Wio-LR1121 and T3S3; re-run 0b. |
| PARTIAL | any | Stop, investigate firmware delta vs commit 8de16ac before trusting 0b result. |
| UNEXPECTED | n/a | Sub-GHz drift detected. Stop, investigate, do not proceed to 0b. |
| INDETERMINATE | n/a | Re-run 0a at different time. |

---

## Test C — Bench RF Environment Sweep (~10 min)

**Goal:** rule out ambient RF interference at 906.875 MHz as a contributor to the LR1121 RX deficit.

**Setup:** both radios POWERED DOWN (or disconnect USB to the XIAO). Only the HackRF is running.

### Procedure

1. Load SDRAngel **"LR1121 RX 906875"** preset.
2. Change center frequency to `915.000 MHz` and sample rate to `20000000 Sa/s` (20 MS/s — wide span). This shows the whole US 902–928 MHz ISM band in one view.
3. Open the **Spectrum Analyzer** view, set:
   - FFT size: `4096`
   - Averaging: `Max-hold` (this captures any intermittent signal that pops up during the observation window)
4. Click **Start**. Let it run for **3 minutes**.
5. Save a screenshot of the max-hold spectrum to `docs/testbed/sdrangel-captures/test-c-ambient-902-928.png`.
6. Look for any narrowband peak near **906.875 MHz ±2 MHz**. Note its peak amplitude vs the noise floor.

### Result table — fill in

| Observation | Value |
|---|---|
| Noise floor away from 906.875 MHz (dBFS, average) | _____ |
| Peak amplitude at 906.875 MHz ±2 MHz over 3 min (dBFS) | _____ |
| Delta peak − noise (dB) | _____ |
| Subjective interferer description (cellular? WiFi splatter? unknown narrowband?) | _____ |

### Pass / fail

- **PASS** (bench is RF-clean): peak − noise delta < 10 dB anywhere in 904–910 MHz. Test C is decorative; move on.
- **FAIL** (bench is RF-hostile): persistent signal >10 dB above noise within 2 MHz of 906.875. This **could** be partly responsible for desensitizing the LR1121 (especially if it has weaker AGC than the SX1262). Document but continue — Tests A and B will quantify the actual deficit regardless.

### Test C 2.4 GHz extension

Repeat the environment sweep at the 2.4 GHz Meshtastic band:

1. Change SDRAngel HackRF input center frequency to `2440 MHz` (covers 2402–2480 MHz, the main 2.4 GHz LoRa range).
2. Sample rate `20000000 Sa/s`, max-hold averaging, 3 minute observation.
3. Save screenshot to `docs/testbed/sdrangel-captures/test-c-ambient-2400-2480.png`.
4. **Expect** to see significant ambient activity — Wi-Fi, Bluetooth, microwave ovens, etc. The 2.4 GHz ISM band is normally very noisy. The question is not "is it clean?" but "is there a strong narrowband interferer specifically at the Meshtastic 2.4G center frequency (2403.59375 MHz) that would specifically desensitize the LR1121?"

#### 2.4 GHz environment result — fill in

| Observation | Value |
|---|---|
| Noise floor across 2402–2480 MHz (dBFS, median) | _____ |
| Peak amplitude at 2403.59375 MHz ±5 MHz over 3 min (dBFS) | _____ |
| WiFi channels observed (CH1 = 2412, CH6 = 2437, CH11 = 2462) | _____ |
| Subjective overall noisiness of the 2.4 GHz band | low / moderate / high |

This data is mostly for the record — the 2.4 GHz band is always noisy and that noise floor is something both the T3S3 LR1121 and the Wio-LR1121 see equally. The A/B comparison in Tests A and B nullifies it.

---

## Test A — TX Power A/B Comparison (~30 min)

**Goal:** decide whether the LR1121's RF front end (matching network, switch, antenna routing) is intact or damaged. This invokes the **reciprocity argument**: a passive RF network has the same loss in both directions, so if the LR1121's TX power at the antenna port matches the SX1262's, the front end is mechanically healthy and the RX failure is **chip-internal only** (LNA, RFI_LF input, or FW errata).

**Setup:** both radios powered and running normal bridge firmware. HackRF on the bench at a **FIXED position** ~50 cm from both radio antennas. Don't move anything during the test.

### Procedure — Phase 1: SX1262 reference TX measurement

1. Load **"LR1121 RX 906875"** preset in SDRAngel.
2. Confirm gain settings exactly as configured: **LNA 16 dB, VGA 20 dB, AMP OFF**. Do not change these for the entire test.
3. Open Spectrum Analyzer view, set:
   - FFT size: `4096`
   - Averaging: `None` (single-shot — you want peak per burst, not averaged)
   - Trace: `Max-hold` with manual reset between bursts (press the reset button before each TX event)
4. Click **Start** RX.
5. Watch the bench serial monitor for `[R1 NodeInfo TX]` events (these are the SX1262 transmitting at +20 dBm — visible in your existing serial logs).
6. For each TX burst observed:
   - Reset the max-hold trace.
   - Wait for the next `[R1 NodeInfo TX]` event.
   - Read the peak amplitude in dBFS off the spectrum display at the burst's center frequency.
   - Record below. Confirm the ChirpChat demodulator decoded the packet (sanity check on which radio TXed).
7. Capture **at least 5 SX1262 bursts.** This usually takes 5–10 minutes given how often the bridge transmits NodeInfos. If too slow, you can speed it up by power-cycling the XIAO to trigger a fresh NodeInfo TX.
8. Take a screenshot of one representative burst's spectrum at `docs/testbed/sdrangel-captures/test-a-sx1262-burst.png`.

#### SX1262 TX result table — fill in

| Burst # | Peak amplitude (dBFS) | Decoded src ID (from ChirpChat log) | Timestamp |
|---|---|---|---|
| 1 | _____ | _____ | _____ |
| 2 | _____ | _____ | _____ |
| 3 | _____ | _____ | _____ |
| 4 | _____ | _____ | _____ |
| 5 | _____ | _____ | _____ |
| **Median** | **_____** | — | — |

### Procedure — Phase 2: LR1121 DUT TX measurement

1. Without moving the HackRF, without changing its gain settings, without touching anything: trigger an LR1121 TX burst. The LR1121 only transmits when bridging a Meshtastic packet from the SX1262, so:
   - On your phone running Meshtastic, send a message to the public channel ("HackRF Test A 1", "HackRF Test A 2", etc.).
   - Each message → R1 RX → bridge → R2 (LR1121) TX. Watch the serial monitor for `[Radio2-Edge] transmit(N B)` events confirming the LR1121 transmitted.
2. For each LR1121 TX burst, follow the same procedure as Phase 1 — reset max-hold, wait for the event, read peak amplitude.
3. Capture at least **5 LR1121 bursts.**
4. Take a screenshot at `docs/testbed/sdrangel-captures/test-a-lr1121-burst.png`.

#### LR1121 TX result table — fill in

| Burst # | Peak amplitude (dBFS) | MT message that triggered the bridge | Timestamp |
|---|---|---|---|
| 1 | _____ | _____ | _____ |
| 2 | _____ | _____ | _____ |
| 3 | _____ | _____ | _____ |
| 4 | _____ | _____ | _____ |
| 5 | _____ | _____ | _____ |
| **Median** | **_____** | — | — |

### Compute the delta

```
Test A delta = LR1121 median peak dBFS − SX1262 median peak dBFS
             = _____ dB
```

### Interpretation table

| LR1121 vs SX1262 delta | Conclusion | Implication for the Seeed inquiry |
|---|---|---|
| **+3 dB to −3 dB** | LR1121 RF front end is healthy. Failure is **RX-only chip-internal** (LNA, RFI_LF input, AGC bug, or FW 1.3 errata in the RX demod path). | Rewrite the Seeed follow-up to focus exclusively on chip-internal RX path and FW update availability. Switch table / matching network / module hardware are off the table. |
| **−5 dB to −15 dB** | Marginal RF front end — partial matching network mistune or partial switch damage. Probably contributes to RX deficit but unlikely to be the full 30+ dB story. | Continue with Test B to quantify the RX deficit; share both numbers with Seeed. |
| **−15 dB or more** | RF front end is broken — matching network, switch, or RF trace fault. Both TX and RX are affected; the RX deficit is dominated by this. | RMA conversation. The module's hardware is electrically defective. |

**Test A is the single most diagnostic experiment available to you. The result alone redirects the entire investigation.**

### Test A 2.4 GHz extension

**Run only if Test 0b PASSED** (proves Wio-LR1121 can at least RX at 2.4 GHz, which implies TX is also working at 2.4 GHz). If Test 0b FAILED, skip Test A 2.4 GHz — the Wio-LR1121 has no demonstrated 2.4 GHz functionality to characterize against.

The SX1262 sibling radio cannot operate at 2.4 GHz, so the sub-GHz A/B methodology doesn't apply. Substitute the **T3S3 LR1121** as the reference radio — both DUT and reference are then LR1121 chips, so the comparison is directly meaningful.

**Setup changes from sub-GHz Test A:**

1. SDRAngel HackRF input center frequency: change to the 2.4 GHz Meshtastic frequency the Wio-LR1121 and T3S3 are configured for (e.g., **2403.59375 MHz** for LongFast 2.4G slot 0).
2. Sample rate: **2 MS/s** (covers the 812.5 kHz Meshtastic 2.4G LongFast bandwidth with room to spare).
3. ChirpChat Demodulator settings: **BW 812500 Hz, SF 11, CR 4/5, DE 2, sync 0x2B**.
4. HackRF gain stages: **LNA 24 dB, VGA 20 dB, AMP OFF** (slightly higher LNA at 2.4 GHz because the 2.4 GHz ambient noise floor is generally higher and the HackRF's noise figure is slightly worse). These stay FIXED for the entire 2.4 GHz Test A.
5. HackRF antenna position: **fixed**, equidistant from the Wio-LR1121's 2.4 GHz antenna and the T3S3 LR1121's antenna (both transmitters need to be at approximately equal RF distance from the HackRF for the comparison to be meaningful).

**Procedure:**

1. **Phase 1 — T3S3 reference TX:** Trigger T3S3 LR1121 TX bursts (Meshtastic message from its phone client). For each burst observed in SDRAngel: reset max-hold, capture peak dBFS, confirm ChirpChat decoded the packet, record. Capture at least 5 bursts. Note the T3S3's configured TX power.
2. **Phase 2 — Wio-LR1121 DUT TX:** Trigger Wio-LR1121 TX bursts (send Meshtastic messages from any source that arrives via the bridge — same as sub-GHz Test A but at 2.4 GHz). Capture peak dBFS for at least 5 bursts. Confirm both radios were configured for the same TX power (`+13 dBm` if both at the 2.4 GHz region-exempt cap).
3. Compute the delta: **`(Wio-LR1121 median peak dBFS) − (T3S3 LR1121 median peak dBFS)`**.

#### 2.4 GHz TX A/B result tables — fill in

**T3S3 LR1121 reference:**

| Burst # | Peak dBFS | T3S3 TX power setting | Timestamp |
|---|---|---|---|
| 1 | _____ | _____ dBm | _____ |
| 2 | _____ | _____ dBm | _____ |
| 3 | _____ | _____ dBm | _____ |
| 4 | _____ | _____ dBm | _____ |
| 5 | _____ | _____ dBm | _____ |
| **Median** | **_____** | — | — |

**Wio-LR1121 DUT:**

| Burst # | Peak dBFS | Wio-LR1121 TX power setting | Timestamp |
|---|---|---|---|
| 1 | _____ | _____ dBm | _____ |
| 2 | _____ | _____ dBm | _____ |
| 3 | _____ | _____ dBm | _____ |
| 4 | _____ | _____ dBm | _____ |
| 5 | _____ | _____ dBm | _____ |
| **Median** | **_____** | — | — |

**Test A 2.4 GHz delta = Wio-LR1121 median − T3S3 median = _____ dB**

Interpretation table same as sub-GHz Test A: ±3 dB = healthy 2.4 GHz front end, 5–15 dB low = marginal, 15+ dB low = broken. **Apply the equal-TX-power correction** if the two radios are configured for different TX powers (subtract the configured TX-power difference from the dBFS delta).

---

## Test B — RX Sensitivity Floor A/B Comparison, Fully Cabled (~30 min)

**Goal:** measure the LR1121's RX sensitivity floor relative to the SX1262 on the same bench. Produces the numerical "deficit in dB" that goes into the Seeed email.

**Methodology:** the KT3-2N-90/1S step attenuator provides a calibrated 0–90 dB sweep in 1 dB increments between the HackRF TX output and the DUT antenna port. Combined with HackRF IF gain control (0–47 dB), the full path attenuation can be set anywhere from ~5 dB (KT3=0, gain=47, just the fixed losses) to ~137 dB (KT3=90, gain=0). This spans the entire useful sensitivity envelope of both radios at SF11/BW250. The setup is **fully conducted** — no over-the-air radiation, no multipath, no antenna-position dependency.

**Critical principle:** the chain's fixed insertion losses (~6 dB total — see equipment section) are **identical** in the SX1262 and LR1121 phases. They cancel out of the subtraction. The result is a clean relative dB delta with 1 dB precision.

### Cabled chain — physical setup

1. Disconnect the LR1121 module's IPEX antenna pigtail from its rubber-duck antenna. Leave the pigtail connected to the LR1121 module (IPEX side).
2. Disconnect the SX1262 module's IPEX antenna pigtail from its rubber-duck antenna similarly.
3. Build the test chain:
   ```
   HackRF TX (SMA-f) → 5 dB SMA pad → SMA-m / N-m adapter → KT3 input
                                                                  ↓
                                                                  KT3 output → N-m / SMA-f adapter → short SMA jumper
                                                                                                               ↓
                                                                                                               → DUT IPEX-SMA pigtail (SMA-male side)
   ```
4. The "DUT IPEX-SMA pigtail" SMA-male is what you'll **physically swap** between the LR1121 pigtail and the SX1262 pigtail when changing which radio is under test. Or — easier — use a spare IPEX-SMA pigtail dedicated to the test chain, and just swap which radio's IPEX connector you click onto.
5. Set KT3 to **0 dB** initially (loudest signal). Confirm all cables are snug.

### Calibration — anchor HackRF output power

Optional but useful. Skip if you don't have a power meter — the A/B comparison still works without it; you just won't know absolute dBm numbers as precisely.

1. If you have a power meter: insert it between the 5 dB pad and the first SMA-N adapter, set HackRF IF gain to **30 dB**, run ChirpChat modulator continuously. Record measured power at the 5 dB pad output: ______ dBm. (Typical HackRF at IF gain 30 dB outputs ~0 dBm. After the 5 dB pad it's ~−5 dBm.)
2. If no power meter: assume the HackRF datasheet curve — at 906 MHz, IF gain 30 dB ≈ 0 dBm output; IF gain 47 ≈ +13 dBm; each 1 dB of IF gain change ≈ 1 dB output change in the 10–40 dB region. Accuracy ±3 dB.

**Power at DUT antenna port at any sweep point:**

```
P_DUT (dBm) ≈ P_HackRF_at_gain_G − KT3_setting_dB − 6 dB (fixed chain losses)
```

Example with HackRF IF gain 30 dB (~0 dBm out), KT3 at 70 dB:
```
P_DUT ≈ 0 − 70 − 6 = −76 dBm
```

### Procedure — Phase 1: SX1262 sensitivity floor sweep

1. Connect the test chain SMA-male output to the **SX1262**'s IPEX pigtail.
2. Load SDRAngel **"LR1121 TX 906875"** preset (the modulator).
3. Set HackRF IF gain to **30 dB**. (Reference point. Don't change during the sweep.)
4. Start HackRF TX with ChirpChat modulator continuous, period 1000 ms, payload `HACKRF-CAL`.
5. Confirm SX1262 is reporting RX events in the serial monitor with KT3 at 0 dB. These will appear as `[R1 RX]` events with `state=-7` (CRC mismatch — the payload isn't Meshtastic-formatted, so RadioLib will reject it post-CRC, but the **RX_DONE interrupt fired and the payload was demodulated** — that's what counts for sensitivity measurement).
6. **Coarse sweep** — increment KT3 in **10 dB steps** (0 → 10 → 20 → 30 → 40 → 50 → 60 → 70 → 80 → 90). At each step, wait **30 seconds** and count `[R1 RX]` events. ChirpChat is sending ~30 packets per 30 s window.
7. **Find the cutoff zone** — the KT3 setting at which RX rate drops below ~10% (3 of 30 packets).
8. **Fine sweep** — back off 10 dB into the working zone, then **increment KT3 in 1 dB steps** to find the exact cutoff KT3 setting. Take 60-second observation windows in this zone for better statistics.

#### SX1262 floor sweep result table — fill in

**Coarse sweep:**

| KT3 setting (dB) | Implied P_DUT (dBm) | Packets sent (~30) | Packets received | Detection rate | Pass/Fail (≥10% = pass) |
|---|---|---|---|---|---|
| 0 | _____ | ~30 | _____ | _____% | _____ |
| 10 | _____ | ~30 | _____ | _____% | _____ |
| 20 | _____ | ~30 | _____ | _____% | _____ |
| 30 | _____ | ~30 | _____ | _____% | _____ |
| 40 | _____ | ~30 | _____ | _____% | _____ |
| 50 | _____ | ~30 | _____ | _____% | _____ |
| 60 | _____ | ~30 | _____ | _____% | _____ |
| 70 | _____ | ~30 | _____ | _____% | _____ |
| 80 | _____ | ~30 | _____ | _____% | _____ |
| 90 | _____ | ~30 | _____ | _____% | _____ |

**Fine sweep around the cutoff** (fill in based on where the coarse sweep dropped out):

| KT3 setting (dB) | Implied P_DUT (dBm) | Packets sent (~60) | Packets received | Detection rate | Pass/Fail |
|---|---|---|---|---|---|
| ___ | _____ | ~60 | _____ | _____% | _____ |
| ___ | _____ | ~60 | _____ | _____% | _____ |
| ___ | _____ | ~60 | _____ | _____% | _____ |
| ___ | _____ | ~60 | _____ | _____% | _____ |
| ___ | _____ | ~60 | _____ | _____% | _____ |

**SX1262 sensitivity floor = highest KT3 setting where detection rate ≥ 10% = ______ dB**
**Implied P_DUT at SX1262 floor = ______ dBm**

9. **Stop** HackRF TX. Disconnect the test chain SMA from the SX1262 pigtail.

### Procedure — Phase 2: LR1121 sensitivity floor sweep

1. Connect the test chain SMA-male to the **LR1121**'s IPEX pigtail (the only thing that changes between phases — the rest of the chain stays untouched).
2. Restart HackRF TX with identical settings (IF gain 30 dB, ChirpChat continuous).
3. Confirm LR1121 is reporting RX events in the serial monitor with KT3 at 0 dB. Look for `[Radio2-Edge] read: pktLen=N` events where N > 0. (Like Phase 1, the payload will fail CRC, but the demod event is what counts.)
4. Repeat the coarse-then-fine KT3 sweep.

#### LR1121 floor sweep result table — fill in

**Coarse sweep:**

| KT3 setting (dB) | Implied P_DUT (dBm) | Packets sent (~30) | Packets received | Detection rate | Pass/Fail (≥10% = pass) |
|---|---|---|---|---|---|
| 0 | _____ | ~30 | _____ | _____% | _____ |
| 10 | _____ | ~30 | _____ | _____% | _____ |
| 20 | _____ | ~30 | _____ | _____% | _____ |
| 30 | _____ | ~30 | _____ | _____% | _____ |
| 40 | _____ | ~30 | _____ | _____% | _____ |
| 50 | _____ | ~30 | _____ | _____% | _____ |
| 60 | _____ | ~30 | _____ | _____% | _____ |
| 70 | _____ | ~30 | _____ | _____% | _____ |
| 80 | _____ | ~30 | _____ | _____% | _____ |
| 90 | _____ | ~30 | _____ | _____% | _____ |

**Fine sweep around the cutoff:**

| KT3 setting (dB) | Implied P_DUT (dBm) | Packets sent (~60) | Packets received | Detection rate | Pass/Fail |
|---|---|---|---|---|---|
| ___ | _____ | ~60 | _____ | _____% | _____ |
| ___ | _____ | ~60 | _____ | _____% | _____ |
| ___ | _____ | ~60 | _____ | _____% | _____ |
| ___ | _____ | ~60 | _____ | _____% | _____ |
| ___ | _____ | ~60 | _____ | _____% | _____ |

**LR1121 sensitivity floor = highest KT3 setting where detection rate ≥ 10% = ______ dB**
**Implied P_DUT at LR1121 floor = ______ dBm**

### Compute the delta

The relative sensitivity delta is the difference in **KT3 settings** at the cutoff — fixed chain losses cancel:

```
LR1121 sensitivity deficit vs SX1262 reference = (SX1262 floor KT3 setting) − (LR1121 floor KT3 setting)
                                                = _____ dB − _____ dB
                                                = _____ dB
```

(Equivalently, if KT3 of N dB allows the SX1262 to receive but the LR1121 needs KT3 backed off to M < N dB, the deficit is N − M dB. A higher KT3 setting = lower P_DUT = more sensitive receiver = better.)

### Absolute floor comparison

If you established an HackRF output power calibration above:

| Radio | Floor P_DUT (dBm, measured) | Floor P_DUT (dBm, datasheet spec) | Bench deficit vs spec |
|---|---|---|---|
| SX1262 | _____ | ~−134 | _____ dB |
| LR1121 | _____ | ~−134 | _____ dB |

The SX1262's deficit vs datasheet absorbs all the non-radio losses on the bench (HackRF noise figure, multipath through the IPEX-SMA pigtail, ambient interference). The LR1121's deficit vs datasheet, **minus the SX1262's deficit vs datasheet**, equals the LR1121-specific deficit you'd quote to Seeed — that's the same number as the relative-KT3 delta above, just sanity-checked from a different direction.

### Interpretation

| Test B delta (LR1121 deficit) | Conclusion |
|---|---|
| **0–5 dB** | LR1121 RX is essentially as sensitive as SX1262 on this bench. The "30 dB deficit" perception from earlier bench runs is probably an artifact of OTA-vs-self-echo geometry. Re-test live OTA reception with corrected expectations. |
| **5–15 dB** | Modest deficit — likely the `SetRssiCalibration` delta + minor matching network mistune. Strong evidence for pursuing Wio-LR1121-specific cal values from Seeed. |
| **15–30 dB** | Significant deficit, matching your earlier OTA observations. Hardware (matching network) or chip-FW errata. Hard numerical evidence for Seeed. |
| **30+ dB** | Catastrophic, matching the original DOE conclusion. Cross-reference with Test A — if Test A also shows TX deficit, RMA; if Test A is clean, chip-internal RX fault confirmed. |

### Sanity checks during the sweep

- **At KT3 = 0**, both radios MUST be receiving 100% of packets if the chain is good. If they're not, the chain has a continuity problem — check connectors and adapter genders.
- **At KT3 = 90 dB**, both radios should be below their floor (P_DUT around −96 dBm at HackRF IF gain 30 dB). If a radio still receives reliably at KT3=90, it means it has more headroom than 90 dB and you need to **drop HackRF IF gain** to find the actual floor. Re-run Phase 1 or Phase 2 at HackRF IF gain 20 or 10 dB to extend the reach.
- **At the cutoff**, the receiver behavior should be **graded** — gradually losing packets as KT3 increases, not a sharp wall. A sharp wall suggests something else (e.g., a chip-internal threshold or an interference issue) is dominating.

### Test B 2.4 GHz extension

**Run if** (a) Test 0b PASSED and you want numerical 2.4 GHz characterization, OR (b) Test 0b FAILED and you need to characterize the 2.4 GHz failure.

**Prerequisite check on the KT3:** verify the KT3-2N-90/1S spec sheet (or the AliExpress listing) for upper frequency limit. The "2N" likely indicates DC–3 GHz coverage (good for 2.4 GHz), but the "90/1S" variant might be limited differently. If the KT3 is rated only to 1 GHz, **skip the cabled procedure and use the HackRF-gain-only variant below**.

**If KT3 covers 2.4 GHz — cabled procedure:**

1. Reconfigure the test chain exactly as for sub-GHz Test B (HackRF TX → 5 dB pad → SMA-N adapter → KT3 → N-SMA adapter → SMA jumper → IPEX-SMA pigtail), but connect the IPEX-male end to **module pad 2 (`2.4G_RF`)** instead of pad 23.
2. Load **"LR1121 TX 906875"** preset in SDRAngel, then:
   - Change HackRF Output center frequency to **2403.59375 MHz**
   - Change ChirpChat Modulator parameters to **BW 812500 Hz, SF 11, CR 4/5, DE 2, sync 0x2B**
   - Save as new preset **"LR1121 TX 2403"**
3. Reference radio for the floor sweep: the **T3S3 LR1121** (already on the bench, receives Meshtastic 2.4G). The T3S3 will report RSSI for each received packet via its Meshtastic interface (phone app or serial). Use this as the 2.4 GHz reference floor.
4. Procedure identical to sub-GHz Test B: coarse 10 dB sweep, fine 1 dB sweep, compute delta. The T3S3 LR1121's floor is your **2.4 GHz bench-reference floor**. The Wio-LR1121's floor minus the T3S3's floor is the Wio-LR1121's 2.4 GHz-specific deficit (or zero, if they match).

**If KT3 does NOT cover 2.4 GHz — HackRF-gain-only variant:**

1. Skip the cabled chain. Set up like sub-GHz Test C: HackRF whip antenna at a fixed 3 m position from both radios' 2.4 GHz antennas.
2. SDRAngel HackRF Output: 2403.59375 MHz center, ChirpChat Modulator with 2.4G Meshtastic params, continuous TX.
3. Establish T3S3 RSSI anchor at HackRF IF gain 30 dB (read T3S3 RSSI from Meshtastic app).
4. Sweep HackRF IF gain in 5 dB steps from 47 down to 0. Count packets received at each step by both T3S3 and Wio-LR1121 (the Wio-LR1121's RX is read from the bridge firmware serial monitor).
5. Floor for each radio = lowest IF gain step with ≥10% packet detection. Delta in IF gain steps = sensitivity delta in dB.

#### 2.4 GHz floor sweep result tables — fill in

**T3S3 LR1121 reference (whichever method):**

| KT3 (cabled) or HackRF gain (free-space) | Implied P_DUT (dBm) | Packets sent | Packets received | Detection rate | Pass/Fail |
|---|---|---|---|---|---|
| (coarse sweep — fill 5 rows) | | | | | |

**Wio-LR1121 DUT:**

| KT3 (cabled) or HackRF gain (free-space) | Implied P_DUT (dBm) | Packets sent | Packets received | Detection rate | Pass/Fail |
|---|---|---|---|---|---|
| (coarse sweep — fill 5 rows) | | | | | |

**T3S3 floor = _____ ; Wio-LR1121 floor = _____ ; 2.4 GHz deficit = _____ dB**

Interpretation: same table as sub-GHz Test B (0–5 dB ≈ healthy, 5–15 = modest deficit, 15+ = significant, 30+ = catastrophic).

---

## Decision tree — what to do with the numbers

### Decision branch 1 — Test 0a/0b result drives everything

```
Test 0a (sub-GHz baseline):
├── EXPECTED — R2 deaf to distant OTA, only self-echo (matches Runs 0-8)
│   │   Proceed to Test 0b.
│   │
│   └── Test 0b (2.4 GHz baseline):
│       ├── PASS — Wio-LR1121 receives T3S3 OTA at 2.4 GHz
│       │   │
│       │   └─► Phase 1 essentially unblocked. Pivot project to ship LR1121
│       │       production-locked to 2.4 GHz. Sub-GHz investigation continues only
│       │       for documentation and Seeed evidence — no longer critical path.
│       │       │
│       │       ├─► Optionally run sub-GHz Tests C/A/B (~70 min) for Seeed numerical evidence
│       │       ├─► Optionally run 2.4 GHz Tests C/A/B (~70 min) for 2.4 GHz characterization
│       │       └─► Update LR1121-SPEC.md, CHANGELOG.md, and v9.1 release notes to reflect
│       │           "Phase 1: LR1121 at 2.4 GHz, sub-GHz known-issue documented"
│       │
│       ├── FAIL — Wio-LR1121 also deaf at 2.4 GHz
│       │   │
│       │   └─► Both bands broken. RX chain has a fault that transfers across
│       │       analog front ends → most likely chip-internal (LNA shared logic,
│       │       AGC, FW 1.3 errata) or fundamental manufacturing defect.
│       │       │
│       │       ├─► Run sub-GHz Tests A/B for full characterization (~1 hr)
│       │       ├─► Run 2.4 GHz Tests A/B for full characterization (~1 hr)
│       │       ├─► Update Seeed follow-up to add 2.4 GHz failure as new question
│       │       └─► If both bands' Test A show TX deficit → RMA conversation
│       │
│       └── INCONCLUSIVE — RX events but CRC mismatches
│           └─► Modem params mismatch; fix Wio-LR1121 ↔ T3S3 config and retry 0b.
│
└── UNEXPECTED — R2 starts receiving distant sub-GHz OTA
    │
    └─► STOP. The failure mode is no longer reproducing with the current
        committed firmware. Investigate what changed since commit 8de16ac
        (Run 8 reference state) before proceeding to any HackRF testing.
        Did a recent firmware change unblock sub-GHz? A hardware tweak?
        Document the working state thoroughly, then re-baseline the audit.
```

### Decision branch 2 — Test A interpretation (per band)

```
Test A delta (Wio-LR1121 TX minus reference TX, both at same configured power):
├── −3 to +3 dB (RF front end healthy on this band)
│   │
│   ├── Test B delta < 5 dB
│   │   └─► RX is also healthy on this band. Reassess earlier failure observations.
│   │
│   ├── Test B delta 5–15 dB
│   │   └─► RSSI calibration / matching network mistune. Strongly push question 1
│   │       of the Seeed follow-up for band-specific calibration values.
│   │       (Sub-GHz uses UM Table 7-21 "600 MHz – 2 GHz" row;
│   │        2.4 GHz uses UM Table 7-21 "Above 2 GHz" row with GainOffset=2030
│   │        — verify Table 7-19 byte encoding before flashing.)
│   │
│   └── Test B delta 15+ dB
│       └─► Chip-internal RX fault confirmed for this band. Seeed conversation
│           pivots to FW 1.3 RX-path errata and FW update availability.
│
├── −5 to −15 dB (marginal TX deficit)
│   └─► Band-specific matching network issue. Both TX and RX suffer.
│       Seeed: "is matching properly tuned for this band on production units?"
│
└── −15 dB or more (broken RF front end on this band)
    └─► RMA conversation for this band. Hardware defect.
```

---

## Data archival

After completing the tests:

1. Save all SDRAngel screenshots to `docs/testbed/sdrangel-captures/` with the filenames suggested above.
2. Fill in this document's result tables with measured values.
3. Append a `## Run 9 — HackRF + SDRAngel characterization (2026-MM-DD)` section to [`../../LR1121-RX-INIT-AUDIT.md`](../../LR1121-RX-INIT-AUDIT.md) referencing this plan, summarizing the deltas, and updating the refuted-hypotheses table (Tests A and B will close 1–2 more hypotheses each, regardless of outcome).
4. Update [`../../SEEED_EMAIL_REPLY_2026-05-28.md`](../../SEEED_EMAIL_REPLY_2026-05-28.md) Section 3 to fold the new numerical evidence into the existing questions. (Or, depending on outcome, replace the questions entirely — e.g., if Test A reveals a broken RF front end, the email becomes an RMA request, not an engineering question.)
5. Commit and push the captures, the filled-in plan, and the updated audit + email docs in a single commit titled `HackRF + SDRAngel diagnostic results (Run 9)`.

## Safety + sanity notes

- **No over-the-air HackRF TX outside this bench.** Even at +13 dBm into a whip antenna at ~3 m, you are well within Part 15 ISM-band rules at 906 MHz. Don't transmit if you move outside Part 15-permitted bands; don't transmit on aviation, cellular, or licensed amateur frequencies without proper authorization.
- **Protect the HackRF TX port** — the 5 dB pad lives there as a permanent VSWR absorber whenever TX is active. Never TX into an open SMA port.
- **Don't change SDRAngel gains mid-test.** Every gain change invalidates the A/B comparison for that test. If you must change a gain to get useful range, restart the entire test phase from scratch.
- **Power-cycle the XIAO between Test A Phase 1 and Phase 2 if needed** — only to trigger a fresh SX1262 NodeInfo TX. Don't unplug or move any antenna or cable.
- **Take notes as you go.** The result tables are the deliverable, not the experience.

## What you'll have at the end

- A definitive in-house answer to "is the LR1121 module RF front end healthy?" (Test A)
- A numerical sensitivity-deficit delta in dB vs the SX1262 reference (Test B)
- A clean RF environment characterization (Test C)
- 3+ screenshots documenting the measurements
- An evidence package strong enough to either close the investigation locally or hand Seeed engineering unanswerable data
