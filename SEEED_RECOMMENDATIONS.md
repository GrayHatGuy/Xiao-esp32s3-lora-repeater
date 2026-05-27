# Recommendations to Seeed Studio for Wio-LR1121 (SKU 113991415)

**Companion document to `SEEED_SUPPORT_INQUIRY.md`.**
**Date:** 2026-05-26
**Project:** Xiao-ESP32-S3 Dual-Radio LoRa Mesh Bridge
**Repository:** <https://github.com/GrayHatGuy/Xiao-esp32s3-lora-repeater>

This document captures four tiers of recommendations for Seeed engineering
based on our Phase 1 LR1121 bring-up investigation. The Seeed support
inquiry (separate doc) focuses on specific questions / asks. This document
is the longer-form companion intended as constructive feedback should
Seeed engineering wish to engage further.

Tiers are ordered cheapest-to-most-expensive. Tier 1 is unilaterally
actionable by Seeed today with no engineering cost. Tier 4 is process /
policy. Tiers 2 and 3 require bench work and (potentially) hardware
revision respectively.

---

## Tier 1 — Documentation gaps (zero engineering cost, immediate value)

Even if the Wio-LR1121 hardware is healthy and we are simply missing a
config step, the documentation around this module makes it nearly
impossible for users to bring up RX correctly. Seeed can close these gaps
unilaterally without any bench work.

1. **Publish (a) which DIO pins drive the internal RF switch, and (b)
   the truth table for those DIOs.** The Wio-LR1121 Module Datasheet
   (v1.0, 2025-07-01) confirms "integrated TCXO and RF switch" but
   does not publish either piece of information.

   The **Semtech LR1121 v2.1 datasheet** (rev 2.1, Dec 2023, §4.5.1 /
   Table 4-1) establishes the chip-level RFSWx pin mapping:
   **RFSW0=DIO5, RFSW1=DIO6, RFSW2=DIO7, RFSW3=DIO8, RFSW4=DIO10**
   (DIO9 is IRQ-only; DIO11 has no DIO-mode alternate), with all RFSWx
   outputs defaulting to High-Z until `SetDioAsRfSwitch` (cmd 0x0112)
   is called. The Wio-LR1121 module datasheet should restate this
   chip-level mapping in the module's own reference section, then
   answer the module-specific question: *of those 5 chip-level RFSWx
   outputs, which one(s) does the Wio-LR1121 PCB actually wire to its
   integrated front-end switch — or does the module use a passive
   RF combining network with no software-controlled switch IC at all?*

   Our bench evidence strongly suggests the latter:

   - We exhaustively swept **all 5 chip-level RFSWx-capable DIOs**
     (DIO5/6/7/8/10) across 12 combinations covering every meaningful
     permutation. Every combination produced identical failure with
     self-echo RSSI invariant within ~7 dB.
   - The published Seeed KiCad library routes the chip's DIO5/6/7
     pins to bottom-side test pads named `MCU_DIO5/6/7` — the `MCU_`
     prefix consistently denoting host-MCU expansion lines elsewhere
     in the design. Seeed deliberately freeing these pins for user
     GPIO is **consistent with the module not using them as switch
     outputs** (because the module uses passive combining).

   Taken together this suggests the Wio-LR1121's RF front-end is
   passive: there is no software-controlled switch IC, and the chip's
   internal LNA/PA mode-selection logic is expected to handle path
   routing autonomously. If that is correct, then `SetDioAsRfSwitch`
   is irrelevant to this module and the RX failure root cause is
   something else (missing chip-level init step, chip-firmware bug,
   hardware fault on the LNA path).

   The documentation should therefore answer: (a) does the module
   have an active RF switch IC or is the front-end passive combining?
   (b) if active, which chip DIO drives it? (c) if passive, what
   chip-level initialization (LNA enable, RxBoosted, calibration
   sequence, etc.) is required for the LNA path to be active during
   RX? This is the single most impactful piece of documentation Seeed
   can publish for LR1121-based product development.

2. **Publish or link to a known-good reference firmware example.** A
   simple Arduino / PlatformIO + RadioLib sketch that initializes the
   Wio-LR1121, transmits one packet, receives one packet, and prints
   both — verified by Seeed engineering on real hardware. The Wio-SX1262
   has examples like this in Seeed's wiki; the Wio-LR1121 should too.
   Even a minimal "ping-pong" between two Wio-LR1121 modules would be
   highly valuable.

3. **Document the exact TCXO voltage** the LR1121's `tcxoVoltage`
   configuration should be set to for this module. The datasheet is
   silent. We've bench-tested 1.6 V, 3.0 V, and 3.3 V — guessing at this
   value is unprofessional and a developer time-sink.

4. **Publish the module's internal RF schematic** (or at minimum a
   block diagram showing the antenna → switch → chip-RF-pin wiring).
   The published OPL KiCad library at `Seeed-Studio/OPL_Kicad_Library/
   Seeed Studio Wio LR1121 Module v0.9` shows the chip's DIO routing
   to test pads but **does not show the actual RF switch topology** —
   the section between the LR1121's RFI / RFO_LP / RFO_HP / RFIO_HF
   pins and the SUBG_RF / 2.4G_RF module pads is opaque. A block
   diagram answering at minimum:
   - Is there a discrete RF switch IC on the module (and if so, what
     part number)?
   - Or is the front-end a passive combining network (diplexer /
     matching) with the chip's internal mode handling everything?
   - If a switch IC, which DIO(s) control it, and what's the truth
     table?

   ...would let firmware authors verify behavior with a logic analyzer
   instead of guessing.

5. **Document any required chip-level initialization beyond
   `SetDioAsRfSwitch` (cmd 0x0112).** If the Wio-LR1121's integrated
   front-end requires additional commands — e.g. `SetLnaConfig`,
   `SetRxBoostedGainMode`, an internal calibration sequence, or a
   non-default `SetPaConfig` — please publish them in the datasheet or
   an application note.

6. **Document the supported LR1121 firmware version range.** Our modules
   report `Base FW version: 1.3` via `GET_VERSION` (cmd 0x0303). If
   Seeed knows that certain firmware versions have RX-path bugs that are
   fixed in later revisions, please publish the version compatibility
   matrix and a firmware-update procedure.

7. **Publish the recommended `SetRssiCalibration` (cmd 0x0229) values
   for the Wio-LR1121 PCB.** Per Semtech LR1121 User Manual v2.2
   §7.2.15, the chip's automatic LNA gain control uses RSSI to pick a
   gain level, and the UM explicitly warns: *"An incorrect gain can
   result in a missed detection (packet loss) or decreased resistance
   to interference... By default, the chip is calibrated for the
   868-915MHz band on the LR1121 EVK... The RSSI must be calibrated
   for each hardware type."* The Wio-LR1121's matching network is
   different from the LR1121 EVK; the default calibration is therefore
   incorrect for this module. Seeed should run the per-PCB
   calibration procedure (UM §7.2.15 step-by-step using an RF generator)
   once during the Wio-LR1121 design qualification, and publish the
   resulting Gain Tune values (G4..G13, G13hp1..hp7) and Gain Offset
   in the Wio-LR1121 datasheet. This is a one-time measurement that
   benefits **every** Wio-LR1121 customer; without it, all such
   customers ship products with potentially-degraded RX sensitivity
   due to wrong AGC gain selection.

8. **Document the bottom-side `MCU_DIO5/6/7` test pads.** The published
   KiCad layout brings DIO5, DIO6, DIO7 (chip pins 20, 19, 11 per
   Semtech UM Table 4-1) out to bottom-side pads named `MCU_DIO5`,
   `MCU_DIO6`, `MCU_DIO7` — but the v1.0 datasheet pinout (page 3)
   lists only the 24-pin perimeter, omitting these test pads entirely.
   This is a significant undocumented capability: users implementing
   multi-IO designs would benefit enormously from knowing these
   additional GPIOs are accessible on the module bottom side. Add them
   to the datasheet pinout table with their pad coordinates, intended
   use ("host-MCU expansion"), and any electrical limits / cautions
   (e.g. "internal pull state at boot," "max sink/source current,"
   "ESD rating"). The KiCad naming convention `MCU_DIO*` strongly
   suggests these are deliberately exposed for user use, not just
   production-test probe points — make that intent explicit.

## Tier 2 — Engineering verification (1–2 days of bench work)

If Seeed engineering can reproduce our setup with their own bench tools,
these tests will localize the root cause:

1. **Verify RX functionality on a freshly-pulled production unit.** Connect
   a Wio-LR1121 to a host MCU running RadioLib's stock
   `examples/LR11x0/LR11x0_Receive_Interrupt` example, configured for
   906–915 MHz / BW 250 kHz / SF 11 / sync 0x2B, with the antenna
   plugged in. Use a calibrated signal generator (or a known-good
   LR1121-based device like a LilyGO T3S3) to inject a -80 dBm signal
   at the antenna port. Confirm `RX_DONE` fires on DIO9 and the packet
   decodes. **If this works for Seeed:** the working firmware reveals
   what step we are missing — please share. **If this does not work for
   Seeed either:** there is a Wio-LR1121 production issue affecting all
   units of the current revision.

2. **Probe DIO5, DIO6, DIO7 on a powered unit during TX → RX transitions**
   with a logic analyzer — using the bottom-side `MCU_DIO5/6/7` test
   pads already present on the module (per Tier 1 item 7). Confirm
   whether the LR1121 actually toggles those pins on mode transitions
   when `SetDioAsRfSwitch` has been issued (RadioLib 7.7.0 calls this
   command via `setRfSwitchTable()`). Given the KiCad naming
   convention strongly suggests DIO5/6/7 are *not* the switch
   controls, the expected result is **observable toggling that
   doesn't affect the antenna path** — confirming the antenna routing
   uses some other mechanism. If the DIOs *do* affect RF behavior on
   Seeed's bench (and don't on ours), that would point to a unit
   variance worth investigating.

3. **Replicate the dual-radio bench setup** — Wio-SX1262 + Wio-LR1121 on
   the same carrier ~10 cm apart, both transmitting at +20 dBm — and
   confirm whether the LR1121's RX desensitizes / blocks when the
   SX1262's PA is active. Our observation is that R2 receives the
   bridge's own R1 NodeInfo TX at -52 to -56 dBm **regardless of what we
   drive DIO5/6/7 to** (8/8 brute-force sweep confirmed), suggesting the
   receive signal is not travelling through the proper antenna → LNA
   chain at all but through PCB substrate / supply / ground coupling
   from R1's PA. This is symptomatic of either a permanently-
   disconnected LNA path or persistent desensitization.

4. **Inspect production-test coverage for RX sensitivity.** It is
   plausible that production test verifies TX (straightforward — measure
   radiated power) but does not verify RX sensitivity (harder —
   requires injecting a known-low-power signal and confirming demod). If
   two independently-sourced units from different orders both fail RX
   but pass current production test, the test coverage has a gap.

5. **Verify the module's antenna feedline integrity** with a network
   analyzer (S11 return loss) on a representative production unit. Our
   bench DMM check showed continuous connectivity from antenna tip
   through IPEX → pad 23 (SUBG_RF), but DC continuity does not guarantee
   RF integrity. An S11 sweep across 850–960 MHz would catch matching
   network defects, missing capacitors, or solder shorts that pass DC
   checks.

## Tier 3 — Hardware design improvements (future module revisions)

If Seeed determines the current Wio-LR1121 design has a structural
limitation that an existing-stock fix cannot address:

1. **Add LNA bias decoupling capacitor if missing.** The LR1121's LNA
   input typically requires a small decoupling cap close to the chip
   pin. A missing or under-spec cap allows ground bounce or RF
   re-radiation onto the LNA input, killing sensitivity. This is a
   common one-component fix.

2. **Verify the RF switch IC's LNA-path matching network.** The switch →
   LNA path likely has a Π or T matching network on the module. A wrong
   component value, a missing component, or a stuffing error in
   production can leave the LNA poorly matched, reducing sensitivity to
   where the module appears to "not receive" even though TX still works
   (TX has its own matching).

3. **Add a control-pin bias network if the RF switch control DIOs require
   defined pull-down / pull-up resistors.** If DIO5/6/7 boot floating
   (high-impedance) before the host MCU issues `SetDioAsRfSwitch`, the
   switch state at power-on is indeterminate. Defined pull-downs ensure
   RX is the safe default state.

4. **Improve RF shielding / grounding around the antenna port.** Our
   PCB-coupling observation — the chip "hears" only signals coming
   through the substrate from a near PA — suggests poor isolation
   between the antenna pin and the chip's substrate / supply. A proper
   RF guard ring with stitching vias around the antenna trace and a
   solid ground pour under the RF section would block substrate paths.

5. **Document the existing bottom-side `MCU_DIO5/6/7` test pads** —
   they are already present on the module per the KiCad layout (see
   Tier 1 item 7). The Tier 3 ask here is twofold:
   (a) Move "document this" from "nice-to-have" to "mandatory in the
   datasheet pinout table" for future revisions, and
   (b) Confirm whether these pads are intentionally exposed
   (host-MCU expansion) or unintentionally exposed (production-test
   probe points). If intentional, the module's positioning shifts from
   "minimal pinout sub-GHz / 2.4 GHz module" to "compact sub-GHz /
   2.4 GHz module with 3 bonus host GPIOs" — a real differentiator for
   product designers. Either answer is fine; just publish it.

6. **Add a hardware revision marker.** The KiCad library mentions
   "Wio LR1121 Module v0.9". If our modules are an early-revision and
   there's a v1.x in the pipeline with this issue fixed, users have no
   way to tell from the unit. A visible revision marker plus a
   published change-log fixes this.

7. **Consider re-evaluating the integrated TCXO voltage choice** if the
   module ships with the LR1121's `tcxoVoltage` configured for a value
   that is unusual or non-default. Some LR1121 reference designs use
   1.8 V, others 3.0 V — picking a non-standard value forces users to
   guess unless documented.

## Tier 4 — Process improvements

1. **Add Wio-LR1121 to Seeed's CI / nightly test rig** so any future
   firmware reference, production-revision change, or chip-firmware
   update is automatically validated against a real RX test. This
   catches regressions before they ship to customers.

2. **Maintain a public errata page** for the Wio-LR1121 — even a
   one-line "v0.9 modules: RX path is sensitive to X, fixed in v1.0" is
   far more useful than silence. Developers spend significant time
   chasing issues that Seeed engineering may already know are
   revision-specific.

3. **Provide a swap policy** for users who purchased early-revision units
   with known issues. We bought two units in good faith for product
   development; if v1.0 fixes our issue, a clearly-published swap path
   would be appreciated and would build customer trust.

4. **Consider publishing the Wio-LR1121's design files** (schematic, BoM,
   PCB layout) under the same open-hardware terms as Seeed's other Wio
   modules. Closed reference designs force the developer community to
   reverse-engineer behavior. Open hardware accelerates adoption and
   surfaces issues faster.

5. **Expand the Wio-LR1121 wiki page** with an integration guide that
   covers at least:
   - Pinout and minimum required external components for a typical
     Arduino-style host.
   - Antenna selection guidance (the IPEX variant ships without an
     antenna; what gain / impedance characteristics are recommended for
     each band the module supports?)
   - Power supply requirements during TX peaks (the datasheet shows
     124.5 mA at 22 dBm — recommend appropriate bulk capacitance on
     VDD_RF to handle this transient).
   - Multi-radio coexistence guidance — what minimum separation between
     a Wio-LR1121 and an adjacent strong-TX radio is recommended to
     avoid desensitization?

---

## Closing note

The Wio-LR1121 is a compelling module — compact, capable, broadband
(sub-GHz + 2.4 GHz + S-band + GNSS + WiFi-scan), and well-priced. We
want to use it in production. The TX path works. The chip and host
plumbing all work. With Tier 1 documentation in place — even just the
switch truth table and a verified reference example — most of the
developer bring-up pain would disappear. We hope this feedback is taken
in the constructive spirit in which it is offered.

If Seeed engineering would like to set up a direct conversation,
inspect our serial logs and source code in detail, or have us run any
additional bench tests, please reach out via
<jrussell328@gmail.com> or via the project repository:
<https://github.com/GrayHatGuy/Xiao-esp32s3-lora-repeater>.

Thank you for your time.
