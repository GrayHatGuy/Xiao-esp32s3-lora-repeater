# R2 RX Deficit — Safe Measurement (task #5)

**Created session 4, 2026-06-04. This SUPERSEDES the HackRF/attenuator method** in
`HACKRF-QUICKSTART.md` and `HACKRF-DIAGNOSTIC-PLAN.md`. Those are kept for reference,
but the HackRF-as-LoRa-source approach was **abandoned** — see "Why not the HackRF" below.

---

## The question
Is R2 (Wio-LR1121) a *little* less sensitive than R1 (SX1262), or *a lot*?
- Small (≈5–15 dB) → likely **RSSI/image calibration** → pursue Seeed cal values (#7).
- Large (20+ dB) → structural (matching/module).

You do NOT need a calibrated lab number to answer that. A rough dB from listening is enough.

---

## The experiment — distance is the attenuator (zero risk)

Walk a real LoRa node from near to far; both bridge radios log every decode + RSSI.
The point where R2 quits while R1 still hears = the deficit. No cables, no attenuator,
no power aimed into a front end. Power *decreases* with distance — the opposite of risk.

### Setup
0. **Both radios on their normal antennas** (no test cable rig).
1. **T3S3 (COM11) = source**, low power: `meshtastic --port COM11 --set lora.tx_power 1`
   (never `0` — in Meshtastic `0` = "use max").
2. **Capture the bridge log:**
   ```powershell
   pio device monitor -p COM6 -b 115200 |
     Tee-Object -FilePath "docs\testbed\run-results\distance-$(Get-Date -f yyyyMMdd-HHmmss).log"
   ```

### Runs — 5 positions, 10 packets each, progressively farther
At each position (change the label `P1`→`P2`… each move):
```powershell
1..10 | ForEach-Object { meshtastic --port COM11 --sendtext "P1 $_"; Start-Sleep 2 }
```
- **P1** next to the bridge (~1 m) · **P2** across the room (~5 m) · **P3** one wall ·
  **P4** two walls / far room · **P5** edge of range (stop if R1 itself goes quiet).

### Analysis (per position)
Count from the log, filtered to the T3S3's node id:
- `[R1 decoded] … "P1 …"` → R1 decoded /10, with R1 median RSSI
- `[R2 decoded] … "P1 …"` → R2 decoded /10

| Pattern | Verdict |
|---|---|
| R2 tracks R1 to the same distance | little/no deficit |
| R2 collapses while R1 still catches most (e.g. P3: R1 10/10, R2 1/10) | **deficit** — read the dB off R1's RSSI at that position |

---

## Safety facts (LR1121 datasheet, cited)
- **Absolute-max RF input = +10 dBm** — Table 3-1, *"may cause permanent device failure."*
- **Max operating input = 0 dBm** — Table 3-2.
- **A cabled high-power source is lethal:** +30 dBm − ~7 dB chain = **+23 dBm at the LR1121** =
  +13 dB over the destroy line. **NEVER cable a high-power source at low attenuation.**
- **OTA at distance is safe:** even close range has 30–70 dB of path loss (air gap). The
  distance method operates ~50–100 dB below the damage threshold by construction.
- If a cabled rig is ever used again: source at **minimum power**, start at **max attenuation**,
  and use the radio's reported **RSSI as a live power meter** (stop if RSSI > −20 dBm).

## Front end confirmed INTACT (2026-06-04)
`run-results/sweep-20260604-134553.log` line 10–12: **R2 decoded a −68 dBm packet, SNR 10 dB.**
A blown/damaged LNA is deaf and could not do that. R2's deficit is the **original,
pre-existing** issue (present since session 1) — not new damage from any OTA/point-blank test.

## Why NOT the HackRF / ChirpChat source (abandoned)
Same log: SDRAngel ChirpChat is a from-scratch LoRa reimplementation and its **CRC/header
encoding is not Semtech-compatible**. Against the bridge's real Semtech radios:
- R1 demodulated the whole packet but **payload CRC failed** (`ERROR -7` = `CRC_MISMATCH`).
- R2 failed earlier at the **header** (`irq=0x50` = `HEADER_ERR | PREAMBLE`), 0 completions.
- Yet R2 cleanly decoded a *real* Meshtastic packet (−68 dBm) in the same window.

So ChirpChat packets don't cleanly decode on Semtech radios → no consistent "did it receive"
criterion → measurement invalid. Combined with the +30 dBm cabling hazard, the entire
HackRF-as-source approach was dropped in favor of a **real LoRa node (T3S3) over the air.**
