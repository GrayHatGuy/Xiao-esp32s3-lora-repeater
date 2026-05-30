# HackRF + SDRAngel Diagnostic Plan — Wio-LR1121 RX Failure

**Purpose:** Use a HackRF One software-defined radio with SDRAngel's ChirpChat plugins to localize the Wio-LR1121's RX failure mode that the firmware DOE in [`../../LR1121-RX-INIT-AUDIT.md`](../../LR1121-RX-INIT-AUDIT.md) could not resolve from the chip side. Three tests, run in order, with concrete pass/fail criteria and result tables.

**Bench:** as documented in [`TESTBED.md`](TESTBED.md). The HackRF is added as an external instrument — the existing XIAO + Wio-SX1262 + Wio-LR1121 bridge stays running unmodified.

**Equipment owned and assumed available:**

- HackRF One SDR (1 MHz – 6 GHz, ±13 dBm TX max, 8-bit ADC, ~9 dB noise figure)
- SDRAngel (latest release) with both **ChirpChat Demodulator** and **ChirpChat Modulator** plugins enabled. The demod page documents the plugin's parameter coverage and confirms a matching modulator exists.
- **KT3-2N-90/1S step attenuator** — 0 to 90 dB in 1 dB steps, N-female connectors both ends, DC–3 GHz typical. Primary instrument for Test B.
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

**Total time estimate:** ~80 minutes for all three tests with note-taking, plus a one-time ~15 minutes for SDRAngel setup. (Test B is faster than the earlier free-space version because the KT3 sweep is 1 dB-resolution and immune to multipath.)

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
