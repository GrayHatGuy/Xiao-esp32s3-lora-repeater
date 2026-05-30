# HackRF + SDRAngel Diagnostic Plan — Wio-LR1121 RX Failure

**Purpose:** Use a HackRF One software-defined radio with SDRAngel's ChirpChat plugins to localize the Wio-LR1121's RX failure mode that the firmware DOE in [`../../LR1121-RX-INIT-AUDIT.md`](../../LR1121-RX-INIT-AUDIT.md) could not resolve from the chip side. Three tests, run in order, with concrete pass/fail criteria and result tables.

**Bench:** as documented in [`TESTBED.md`](TESTBED.md). The HackRF is added as an external instrument — the existing XIAO + Wio-SX1262 + Wio-LR1121 bridge stays running unmodified.

**Equipment owned and assumed available:**

- HackRF One SDR (1 MHz – 6 GHz, ±13 dBm TX max, 8-bit ADC, ~9 dB noise figure)
- SDRAngel (latest release) with both **ChirpChat Demodulator** and **ChirpChat Modulator** plugins enabled. The demod page documents the plugin's parameter coverage and confirms a matching modulator exists.
- A single **5 dB SMA attenuator pad** (used as HackRF VSWR protection only)
- HackRF stock telescoping whip antenna
- USB cable for HackRF
- The bench from `TESTBED.md` (both radios powered, firmware running, serial monitor open)

**Equipment NOT owned and worked around:**

- A proper SMA attenuator kit (would enable Test B as a clean cabled experiment). Worked around by using HackRF TX-gain control + free-space path loss as the variable attenuator. **Recommendation: order a 6-piece SMA pad kit (1/2/3/6/10/20 dB) for ~$30 on eBay — Test B becomes much cleaner.**

**Total time estimate:** ~100 minutes for all three tests with note-taking, plus a one-time ~15 minutes for SDRAngel setup.

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

---

## Test B — RX Sensitivity Floor A/B Comparison (~60 min)

**Goal:** measure the LR1121's RX sensitivity floor relative to the SX1262 on the same bench. Produces the numerical "deficit in dB" that goes into the Seeed email.

**Methodology with only a 5 dB pad:** use the HackRF's TX-gain control (0–47 dB, 1 dB steps) as the variable attenuator. Combined with free-space path loss across a fixed 3 m bench separation, this gives roughly 65 dB of dynamic range — enough to span from "loud RX" to "below floor" on a healthy receiver. The 5 dB pad's only job is protecting the HackRF TX port from VSWR; it's a constant in every measurement and drops out of the A/B comparison.

**Critical principle:** all absolute losses (FSPL, antenna gains, the 5 dB pad, cable insertion loss, indoor multipath) are **identical** between the SX1262 measurement and the LR1121 measurement, as long as **nothing physical moves between the two phases**. They cancel out of the subtraction. The result is a clean relative dB delta even though the absolute numbers are uncertain.

### Setup

1. Place the HackRF with its stock whip antenna at a **fixed position** approximately **3 m from the bench**. Don't move it for the entire test. Tape it down if necessary.
2. Connect the **5 dB pad directly to the HackRF TX SMA port**. Connect the whip antenna to the other end of the pad.
3. Both Wio radios remain on the bench with their antennas in their normal positions. Don't move them.
4. The XIAO + bridge firmware runs normally — you'll be reading RX events from the serial monitor.

### Calibration — establish the SX1262 RSSI anchor

Before sweeping, establish a known reference point.

1. Load SDRAngel **"LR1121 TX 906875"** preset (the TX/modulator side).
2. Set HackRF IF gain to **30 dB**.
3. In the ChirpChat Modulator: payload `"HACKRF-CAL"`, continuous TX at **1000 ms period**.
4. Click **Start** TX.
5. Watch the serial monitor for `[R1 RX]` events from the HackRF-generated packets. They will decode as ChirpChat-generated (no Meshtastic header — RadioLib will report a CRC mismatch or a `read: pktLen=N state=-7` event, but the **RSSI value reported by R1 for these packets is the calibrated anchor**).
6. Record:
   - SX1262 RSSI at HackRF gain 30: ______ dBm

This anchor point lets you read any other HackRF gain as a delta against this:

```
Received power at gain G = (anchor RSSI) − (30 − G) dB
```

So gain 20 = anchor − 10 dB, gain 10 = anchor − 20 dB, etc.

7. **Stop** TX.

### Procedure — Phase 1: SX1262 sensitivity floor sweep

1. Start HackRF TX at gain **30 dB**. Confirm SX1262 is decoding the ChirpChat packets at the anchor RSSI you recorded.
2. **Decrement HackRF IF gain by 5 dB** (gain → 25, 20, 15, 10, 5, 0). After each decrement, wait **30 seconds** to observe how many of the ~30 ChirpChat packets sent at that level are actually received. Record:

#### SX1262 floor sweep result table — fill in

| HackRF IF gain (dB) | Implied received power (dBm) at SX1262 | ChirpChat TX packets sent (≈30) | Packets received by SX1262 (count `[R1 RX]` events in log) | Packet error rate | Pass/Fail (≥10% PER = fail) |
|---|---|---|---|---|---|
| 30 (anchor) | _____ | ~30 | _____ | _____% | _____ |
| 25 | _____ | ~30 | _____ | _____% | _____ |
| 20 | _____ | ~30 | _____ | _____% | _____ |
| 15 | _____ | ~30 | _____ | _____% | _____ |
| 10 | _____ | ~30 | _____ | _____% | _____ |
| 5 | _____ | ~30 | _____ | _____% | _____ |
| 0 | _____ | ~30 | _____ | _____% | _____ |

**SX1262 sensitivity floor = lowest gain step where PER < 10% = HackRF gain ______ dB = implied received power ______ dBm**

3. **Stop** TX between Phase 1 and Phase 2. Do not move anything physical.

### Procedure — Phase 2: LR1121 sensitivity floor sweep

1. **Without moving any radio, antenna, or the HackRF**, restart HackRF TX at gain **30 dB**.
2. Watch the serial monitor for `[Radio2-Edge] read:` events showing LR1121 RX of the ChirpChat packets. (These will likely error as CRC mismatches or empty reads, just like the SX1262 packets did — but each one represents a successful preamble + header detection by the LR1121.)
3. Repeat the same gain sweep:

#### LR1121 floor sweep result table — fill in

| HackRF IF gain (dB) | Implied received power (dBm) at LR1121 | ChirpChat TX packets sent (≈30) | Packets detected by LR1121 (count `[Radio2-Edge] read:` events with `pktLen > 0` in log) | Packet detection rate | Pass/Fail (≥10% detection = pass) |
|---|---|---|---|---|---|
| 30 | _____ | ~30 | _____ | _____% | _____ |
| 25 | _____ | ~30 | _____ | _____% | _____ |
| 20 | _____ | ~30 | _____ | _____% | _____ |
| 15 | _____ | ~30 | _____ | _____% | _____ |
| 10 | _____ | ~30 | _____ | _____% | _____ |
| 5 | _____ | ~30 | _____ | _____% | _____ |
| 0 | _____ | ~30 | _____ | _____% | _____ |

**LR1121 sensitivity floor = lowest gain step where detection rate ≥ 10% = HackRF gain ______ dB = implied received power ______ dBm**

### Compute the delta

```
LR1121 sensitivity deficit vs SX1262 reference = (SX1262 floor power) − (LR1121 floor power)
                                                = _____ dBm − _____ dBm
                                                = _____ dB
```

### Multipath wobble mitigation

If your bench is in a small room, multipath at 906 MHz means RSSI can wobble ±10 dB just from someone moving. Mitigations:

- **Run during a still period** — no walking past the bench, no doors opening, no other people in the room.
- **Repeat each gain step** — if results look noisy, do each gain level THREE times (3 × 30-second windows) and use the median packet count.
- **Use a longer integration window** — bump the per-step observation time from 30 s to 60 s or 120 s for the borderline gain steps near the floor.

### Interpretation

| Test B delta (LR1121 deficit) | Conclusion |
|---|---|
| **0–5 dB** | LR1121 RX is essentially as sensitive as SX1262 on this bench. The "30 dB deficit" perception is probably a self-echo / proximity artifact from the original bench geometry. Re-test live OTA reception with the corrected understanding. |
| **5–15 dB** | Modest deficit — possibly the matching network + RSSI calibration delta. Worth pursuing the Wio-LR1121-specific `SetRssiCalibration` values question with Seeed. |
| **15–30 dB** | Significant deficit, in the range your earlier OTA observations suggested. Hardware (matching network) or chip-FW errata. Strong evidence for Seeed. |
| **30+ dB** | Catastrophic. Module is essentially deaf to anything but near-field signals. Cross-reference with Test A — if Test A also shows TX deficit, RMA; if Test A is clean, chip-internal RX fault confirmed. |

---

## Decision tree — what to do with the numbers

```
Test A delta:
├── −3 to +3 dB (RF front end healthy)
│   │
│   ├── Test B delta < 5 dB
│   │   └─► Reassess earlier "OTA RX failure" claim — may be measurement artifact.
│   │       Bench re-test live MT traffic with corrected expectations.
│   │
│   ├── Test B delta 5–15 dB
│   │   └─► Wio-LR1121-specific RSSI calibration is the likely cause.
│   │       Strongly push question 1 of the Seeed follow-up; ask for measured cal values.
│   │
│   └── Test B delta 15+ dB
│       └─► Chip-internal RX fault confirmed. Update Seeed email to focus on:
│           - LR1121 base FW 1.3 → newer FW availability (question 2 of the follow-up)
│           - Whether Semtech has known RX-path errata in FW 1.3
│           - Whether Seeed has bench-measured Wio-LR1121 sensitivity numbers
│
├── −5 to −15 dB (marginal TX deficit)
│   └─► Partial RF front end issue. Test B numbers will be ~similar to the TX deficit.
│       Seeed conversation pivots to "is the matching network properly tuned for
│       906–915 MHz on this production unit?"
│
└── −15 dB or more (broken RF front end)
    └─► RMA conversation. The module is electrically defective. Both TX and RX
        suffer. Don't waste Seeed engineering's time on RSSI cal or FW errata —
        ask for replacement modules under warranty.
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
