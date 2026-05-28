# Seeed Reply — SKY13373 Truth Table Confirmed, RX Deficit Persists

**To:** David Du (Sensecap Support, Application Engineer)
**CC:** sensecap@seeed.cc, iot@seeed.cc, techsupport@seeed.cc
**Subject:** Re: Wio-LR1121 SKY13373 truth table — confirmed correct, RX deficit persists; follow-up questions

---

Hello David,

Thank you very much for the SKY13373-460LF truth table and the confirmation that V1/V2 are wired to LR1121 DIO5/DIO6 on the Wio-LR1121 module. That was exactly the authoritative information we needed to close the switch-routing hypothesis. We appreciate the fast turnaround.

## 1. Truth table integrated and bench-validated

We implemented your truth table verbatim in our LR1121 driver:

| Mode | DIO5 (V1) | DIO6 (V2) | Path |
|---|---|---|---|
| Standby (RX-latched, see note) | 1 | 0 | RFI_P_LF & RFI_N_LF |
| RX | 1 | 0 | RFI_P_LF & RFI_N_LF |
| TX (low-power) | 1 | 1 | RFO_LP_LF |
| TX (high-power) | 0 | 1 | RFO_HP_LF |

Note on standby: per your warning about the 20 µs shutdown timer at V1=V2=0 plus 20 µs recovery, we elected to latch the standby state to the RX path (1,0) so that every re-arm avoids the recovery penalty. Our application is a continuous-listen mesh repeater, so the switch idle current at (1,0) is negligible compared to the LR1121 chip's own STBY_RC current.

The corrected switch table installed cleanly on bench (Run 7, 2026-05-28 — full bench evidence with serial-log excerpts is captured in our public audit document at [LR1121-RX-INIT-AUDIT.md § Run 7 — Authoritative Seeed reply received](https://github.com/GrayHatGuy/Xiao-esp32s3-lora-repeater/blob/lr1121-phase1/LR1121-RX-INIT-AUDIT.md#run-7--authoritative-seeed-reply-received-2026-05-28)):

- `setRfSwitchTable()` returned without error
- Subsequent `getErrors()` returned `state=0 errors=0x0020` (HF_XOSC_START_ERR sticky bit — see question 3 below)
- One real OTA RX event was successfully demodulated: the bridge's own NodeInfo from the SX1262 sibling radio at **-63 dBm, SNR 11.0 dB**, decoded cleanly
- All TX paths (HP and bridged transmissions) continue to work flawlessly

## 2. RX sensitivity deficit remains — ~30+ dB below SX1262 reference

Over the same 136-second bench window, with both radios on the same antenna chain and tuned to identical Meshtastic LongFast parameters (906.875 MHz, BW 250 kHz, SF11, CR 4/5, sync 0x2B, preamble 16):

| Radio | OTA packets demodulated | RSSI range captured |
|---|---|---|
| **SX1262 reference** | ~12 packets (text messages, telemetry, NodeInfos from 4 distinct distant sources) | -53 to **-80 dBm** |
| **Wio-LR1121** | **1** (the bridge's own near-field self-echo NodeInfo) | -63 dBm only |

The Wio-LR1121 missed every single OTA packet that the SX1262 demodulated during this window, including a -80 dBm NodeInfo from a remote Meshpoint node. The asymmetry indicates the LR1121's effective RX sensitivity is degraded by roughly 30 dB or more relative to its datasheet specification and relative to the SX1262 reference on the same antenna chain.

We have now completed and documented 13 separate hypotheses across 8 bench runs (see attached `LR1121-RX-INIT-AUDIT.md`). Every firmware-side hypothesis defined by the Semtech UM v2.2 and by comparison against stock Meshtastic's working LR1121 firmware (LilyGO T3S3 reference platform) has been refuted. The remaining hypothesis space is confined to hardware design or chip-firmware errata, which are outside our application scope.

## 3. Three specific questions to advance the investigation

We would be very grateful for any of the following:

**Q1 — Wio-LR1121-specific `SetRssiCalibration` values.** Per UM v2.2 §7.2.15, the chip ships calibrated for the Semtech LR1121 EVK matching network, and "*the RSSI must be calibrated for each hardware type*." We tested UM Table 7-21's generic 600 MHz–2 GHz reference tunes (gain offset = 0); the call returned success and shifted self-echo RSSI by +4 dB, but did not recover OTA RX. Does Seeed have measured `SetRssiCalibration` byte values for the production Wio-LR1121 PCB? Even approximate values from an EVT/DVT run would let us validate whether AGC mis-tune is contributing.

**Q2 — LR1121 base firmware update path.** Our modules report `Base FW version: 1.3`. Has Semtech released a newer base firmware that addresses any known sub-GHz RX-path issues? If so, can Seeed provide the binary blob and the recommended update procedure for the Wio-LR1121? We can drive `lr1121_bootloader_*` commands via RadioLib if Seeed can confirm a known-good FW image and the brick-recovery procedure.

**Q3 — `HF_XOSC_START_ERR` (errors register bit 5, mask 0x0020).** This bit is set on every POR of every Wio-LR1121 unit we have tested (we have two units, both behave identically). UM v2.2 §2.1.3 notes that POR image calibration "fails" on TCXO-fitted chips, so we suspect this is a known benign symptom of the integrated TCXO and not the root cause of the RX deficit. Can Seeed confirm whether this bit is expected to be set on every boot of the Wio-LR1121, or whether it indicates a real fault we should be investigating further?

## 4. Test data package

Attached:

- `LR1121-RX-INIT-AUDIT.md` — full DOE document with all 8 bench runs, return codes, sample serial logs, and the hypothesis refutation table
- Build environment: PlatformIO with Espressif32 6.13.0, RadioLib 7.7.0
- Hardware under test: two independent Seeed Wio-LR1121 modules (SKU 113991415, IPEX variant), both showing identical behaviour, paired with Seeed XIAO ESP32S3 host MCUs and Seeed XIAO Wio-SX1262 reference radio modules

We are happy to run additional bench experiments if Seeed engineering wants specific data captured. Our test bench is set up and stable; any new treatment is roughly a 15-minute round trip from "suggested change" to "bench result documented."

Thank you again for your continued support.

Best regards,

GrayHatGuy
grayhatguyllc@protonmail.com
Seeed/Meshtastic Build-Off 2026 entry — Cross-Protocol Dual-Radio Bridge
