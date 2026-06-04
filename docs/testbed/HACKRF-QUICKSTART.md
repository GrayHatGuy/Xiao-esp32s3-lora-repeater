# HackRF #5 — Quick-Start Runbook (START HERE)

**This is the digestible runbook for task #5.** The full reference with every
table and option is `HACKRF-DIAGNOSTIC-PLAN.md` — open that only when you need
detail on a step. This page is the linear "do this, then this."

**One-sentence goal:** measure how much *weaker* a signal R1 (SX1262) can still
hear versus R2 (LR1121), on the same bench, in dB. That single number tells us
whether R2's deficit is calibration, demod, or hardware.

**Scope for now:** **sub-GHz only** (906.875 MHz). Ignore all the 2.4 GHz
sections in the big doc — not needed for this. The test source is the **HackRF
itself** (it generates the LoRa signal); you do **not** need the Heltec for this.

---

## The plan in three phases

1. **PREP** — confirm firmware, set up SDRAngel as a LoRa transmitter, build one cable chain. *(~20 min, one-time.)*
2. **SWEEP** — feed the signal into R1, step the attenuator down until R1 goes deaf; repeat into R2. *(~30 min — this is the measurement.)*
3. **READ** — subtract the two cutoff points = the deficit in dB. *(~2 min.)*

You can stop after Phase 3 and have the #5 deliverable. (Test A — TX comparison —
is optional and noted at the bottom.)

---

## PHASE 1 — PREP

### 1a. Firmware — nothing to flash
The board on **COM6** is already running the right build (`66dac8b`):
chip-EUI logging + the IRQ-status heartbeat + **R2 RX-only mode** (which is
*exactly* what you want for an RX measurement — R2 just listens).

Confirm by opening the monitor and resetting the board once:
```powershell
cd "C:\Users\6r4yh\workspace\Platformio\Projects\xiao esp32 wio sx1262 dual repeater"
pio device monitor -p COM6 -b 115200
```
You should see, near the top:
- `[Radio2-Edge] chip EUI = 00:16:C0:01:F0:9B:37:D5`  ← suspect-GOOD module
- `*** R2_RX_ONLY_TEST: R1->R2 forward DISABLED ***`
- `[Radio2-Edge] ready — 906.875 MHz  BW 250.0 kHz  SF11  CR4/5 ... sub-GHz`

If those three lines are there, firmware is correct. Leave the monitor open.

### 1b. SDRAngel — make the HackRF transmit a LoRa signal
Follow **"Create the TX device configuration"** in `HACKRF-DIAGNOSTIC-PLAN.md`
(Pre-flight §3). In short: HackRF Output @ **906.875 MHz**, 2 MS/s, **IF gain 30 dB**,
AMP off; add a **ChirpChat Modulator** set to **BW 250k / SF11 / CR4/5 / DE 2 /
sync 0x2B / CRC on / preamble 8**.

> **⚠️ Sync-word gotcha:** the Sync word box (and FEC/CRC/Header) is a **LoRa-specific**
> control — it's greyed out / hidden unless the **Modulation scheme = LoRa**. Set
> Modulation = **LoRa** first, *then* the 2-nibble hex **Sync word** field appears —
> set it to **`2B`**. A sync mismatch would make both radios detect the preamble but
> never decode, which masquerades as the exact deficit we're measuring — so this MUST match.

**Message generator** (this is what actually transmits):
- **Payload `HACKRF-CAL` IS the message** — arbitrary text; the radios just need to
  *receive* it (it won't decode as Meshtastic — the reception event is what we count).
- **Continuous + 1000 ms = it auto-repeats every ~1 s, forever** until you Stop →
  **~30 packets per 30 s window** (that's your denominator).
- **Keep the payload ≤ ~20 chars.** At SF11/BW250 a short packet is ~0.5–0.7 s on air;
  a long one could exceed the 1 s period and packets collide.

Save the preset. **Put the 5 dB pad on the HackRF TX port now and leave it there**
(protects the TX port — never transmit into an open SMA).

> Don't change the HackRF IF gain (30 dB) for the whole sweep. The KT3 does the sweeping.

### 1c. Build the one cable chain
```
HackRF TX --[5 dB pad]--[SMA→N adapter]--> KT3 input
KT3 output --[N→SMA adapter]--[short SMA jumper]--> (spare IPEX-SMA pigtail) --> DUT
```
The DUT end (the IPEX connector) is what you'll **move between R1 and R2**. Start
with **KT3 = 0 dB**.

---

## PHASE 2 — THE SWEEP

The signal is identical the whole time; you change only **(a) which radio** the
chain feeds and **(b) the KT3 dial**. At each KT3 setting you hold ~30 s and count
how many of the ~30 transmitted packets that radio caught.

### Key definitions — read once
- **Detection rate = packets received ÷ packets SENT in the window (~30).**
  So "90% detection" = caught ~27 of the ~30 *sent* — **NOT** 90% of the 0 dB count.
  The denominator is always "packets transmitted during the window."
- **Floor = the KT3 dB where detection crosses 50%** (the steepest, most repeatable
  point of the curve). **Use the same threshold for both radios** — the deficit is the
  dB gap between their curves at that threshold. Best practice: **record the % at every
  dB** so you can read the deficit at any threshold afterward (50% is just the cleanest).

### How to count — no payload parsing needed
- **R1 (SX1262):** count `[R1 RX]` lines in the window.
- **R2 (LR1121):** read the heartbeat **`isr=`** value at the window's start and end →
  **`isr_end − isr_start` = packets R2 completed** that step. (Per-packet you'll also see
  `[Radio2-Edge] read: pktLen=N` with N>0; and the `irq=` field — `0x08`=full decode,
  `0x10`=preamble-only — note where it flips. The CRC will fail on the non-Meshtastic
  payload; the demod/RX_DONE event is what counts, not the decode.)

### Capture-and-review workflow — log it, don't count live
Live tallying is error-prone. Capture to a file, sweep on a stopwatch, count afterward.
1. **Tee the monitor to a file:**
   ```powershell
   pio device monitor -p COM6 -b 115200 |
     Tee-Object -FilePath "docs\testbed\run-results\sweep-$(Get-Date -f yyyyMMdd-HHmmss).log"
   ```
2. Start HackRF continuous TX (short payload).
3. **At each KT3 change, drop a bookmark so the log self-labels each window** — fire ONE
   Heltec send (it *does* decode, so it prints a labeled, timestamped line):
   ```powershell
   meshtastic --port COM11 --sendtext "MARK R1 30dB"
   ```
   → appears as `[R1 decoded] text: "MARK R1 30dB"`. The HackRF packets between two
   bookmarks belong to that step. (Every bridge line is also stamped `[NNNNN ms]`, so a
   stopwatch + timestamps works even without bookmarks — the bookmark just makes slicing trivial.)
4. Hold each step ~30 s. **After** the sweep, slice the log by bookmark/timestamp and
   count `[R1 RX]` (R1) or `isr`-delta (R2) per step.

### Step 1 — Reference radio: R1 (SX1262)
1. Plug the chain's IPEX end onto **R1's** pigtail; unplug R1's normal antenna.
2. Start HackRF TX. At **KT3 = 0** you should get ~100% (~30/30). **Nothing at KT3=0 ⇒
   bad connector in the chain — fix before continuing.**
3. **Coarse (10 dB steps, 0→90):** find the *cliff zone* — the 10 dB step where detection
   collapses (e.g. 70 dB = 100%, 80 dB = 10% ⇒ the cliff is between 70 and 80). You're not
   looking for "drops below 100%", you're finding where it falls off.
4. **Fine (1 dB steps):** sweep that 10 dB window one dB at a time to map the curve.
5. **R1_floor = the dB where R1's detection crosses 50%.**

### Step 2 — Device under test: R2 (LR1121)
1. Move the chain's IPEX end from R1 to **R2's** pigtail. **Change NOTHING else**
   (HackRF gain, SDRAngel, KT3 all the same).
2. Repeat the same coarse→fine sweep, counting via the **`isr`-delta**.
3. **R2_floor = the dB where R2's detection crosses 50%** (same threshold as R1).

> Tip: if R1 still hears it at KT3 = 90 dB, it has more than 90 dB of range — drop the
> HackRF IF gain to 20 dB and redo **both** radios at that gain (always the same gain for both).

---

## PHASE 3 — READ THE RESULT

```
Deficit (dB) = R1_floor − R2_floor
```
A higher KT3 floor = hears a weaker signal = more sensitive. So if R1 still works
at KT3=80 but R2 quits at KT3=65, R2 is **15 dB** less sensitive.

**What the number means:**

| Deficit | Meaning | Next |
|---|---|---|
| **0–5 dB** | R2 is basically as good as R1. The old "deaf" impression was bench geometry. | Re-baseline expectations; #9 was a near-miss. |
| **5–15 dB** | Modest deficit → most likely **RSSI / image calibration**. | **Push Seeed (#7) for the Wio-LR1121 `SetRssiCalibration` values** — likely the fix. |
| **15–30 dB** | Significant → matching network or FW errata. | Strong numeric evidence for Seeed; consider Test A. |
| **30+ dB** | Catastrophic → run Test A; if TX is also low, it's hardware (RMA). | |

**Calibration vs. modulation (your question) — read the *shape*, not just the number:**
- **Gradual** fade as KT3 rises (packets thin out over several dB) → **calibration/sensitivity** → Seeed RSSI values.
- **Sharp cliff** or ragged "works/doesn't" with no clean threshold → **demod/modulation** issue, not simple sensitivity.
- The R2 `irq=` field confirms it: a clean `0x08 → 0x10-only` transition at one KT3 level = a real sensitivity threshold (calibration); never reaching `0x08` even at KT3=0 = demod.

**Record the numbers** in the result tables of `HACKRF-DIAGNOSTIC-PLAN.md`
(Test B section) so the full doc stays the system of record.

---

## OPTIONAL — Test A (only if you want the front-end health answer)

Test A measures R2's *transmit* power vs R1's to tell **passive-RF-fault (matching/
switch) vs chip-internal (calibration/demod)**. It needs R2 to transmit, so:

1. In `platformio.ini`, **delete the `-D R2_RX_ONLY_TEST` line**, then
   `pio run -t upload --upload-port COM6`. (This restores R1→R2 forwarding so R2 TXes.)
2. Follow Test A in the big doc (HackRF as **receiver** this time; LNA 16 / VGA 20).
3. **±3 dB** R2-vs-R1 = front end healthy → deficit is chip-internal (calibration).
   **−15 dB or worse** = broken front end → RMA.
4. When done, put the `-D R2_RX_ONLY_TEST` back if you want pure-RX mode again.

---

## If you only remember three things
1. **HackRF transmits; you sweep the KT3 dial; you count RX lines on COM6.**
2. **R1 first (reference), then R2 (DUT), nothing else changed between them.**
3. **R1_floor − R2_floor = the deficit.** 5–15 dB graded ⇒ Seeed calibration values.
</content>
