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
sync 0x2B / CRC on / preamble 8**, message generator **Continuous, 1000 ms,**
payload `HACKRF-CAL`. Save the preset. **Put the 5 dB pad on the HackRF TX port now
and leave it there** (protects the TX port — never transmit into an open SMA).

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

The signal is identical the whole time; you only change **(a) which radio** the
chain feeds and **(b) the KT3 dial**. At each KT3 setting you watch the COM6
monitor for ~30 s and **count** how many packets that radio caught.

**What "caught a packet" looks like in the monitor:**
- **R1 (SX1262):** lines beginning `[  ... ms][R1 RX]`
- **R2 (LR1121):** lines beginning `[Radio2-Edge] read: pktLen=N` with **N > 0**
  *(the payload will fail CRC — that's fine; a demod with pktLen>0 counts as "caught")*
- **Bonus for R2:** the heartbeat `irq=` field — `0x08` = full decode (RX_DONE),
  `0x10` = heard-the-preamble-but-didn't-finish. Note where it flips from 0x08 to 0x10-only.

### Step 1 — Reference radio: R1 (SX1262)
1. Plug the chain's IPEX end onto **R1's** pigtail. Unplug R1's normal antenna.
2. Start the HackRF transmitting (SDRAngel **Start**). With **KT3 = 0**, you should
   see a steady stream of `[R1 RX]` lines (~30 per 30 s). **If you see nothing at
   KT3=0, stop — the cable chain has a bad connector.** Fix before continuing.
3. **Coarse sweep:** set KT3 to 0, 10, 20, 30, … 90 dB. At each, watch 30 s and
   tally `[R1 RX]` count. Somewhere it drops toward zero — that's the cutoff zone.
4. **Fine sweep:** back up 10 dB into the working zone, then step **1 dB at a time**
   until R1 catches fewer than ~3 of ~30 packets.
5. **Write down the highest KT3 dB at which R1 still caught ≥10%** → call it **R1_floor**.

### Step 2 — Device under test: R2 (LR1121)
1. Move the chain's IPEX end from R1's pigtail to **R2's** pigtail. **Change nothing
   else** (HackRF gain, SDRAngel, KT3 start all the same).
2. Repeat the exact coarse-then-fine sweep, counting `[Radio2-Edge] read: pktLen=N`
   (N>0) this time.
3. **Write down R2's cutoff** → **R2_floor**.

> Tip: if R1 still hears the signal even at KT3 = 90 dB, it has more than 90 dB of
> range — drop the HackRF IF gain to 20 dB and redo that radio's sweep (and the
> other radio's too, so both use the same gain).

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
