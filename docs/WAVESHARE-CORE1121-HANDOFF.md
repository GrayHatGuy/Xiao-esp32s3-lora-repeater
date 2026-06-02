# Handoff — WaveShare Core1121 (LR1121) bring-up as design-flaw control

**Created:** 2026-06-01 (end of bench session 3)
**For:** a future session, once the WaveShare hardware arrives (on order, ~2–10 days as of this writing — **NOT on the bench yet**).
**Tracks:** task #8 in the main task list.
**Repo / branch:** https://github.com/GrayHatGuy/Xiao-esp32s3-lora-repeater · `lr1121-phase1`
**Read first:** this file → `CLAUDE.md` (esp. the §2 "session 3, 2026-06-01" update) → `docs/REFERENCES.md` → `docs/testbed/MODULE-REGISTRY.md`.

---

## 1. Why this board exists in the project

The Seeed **Wio-LR1121** (radio R2 on the XIAO bridge) has a **marginal sub-GHz RX
sensitivity/demod deficit**: it is alive, in continuous RX, detects every preamble,
but completes only a small, unreliable fraction of packets — even strong ones a −42 dBm
point-blank packet R1 decoded did not complete on R2. (Full evidence + everything that
was *ruled out* — silicon damage, the `-20`/`WRONG_MODEM` "cascade", sync word, TX path,
interrupt/DIO9 config, hung receiver — is in `CLAUDE.md` §2 session-3 block.)

The owner purchased a **WaveShare Core1121** (LR1121 module) as an **independent control**
to answer the one question the Seeed module can't answer about itself:

> **Is the RX deficit a Seeed Wio-LR1121 *board* design flaw, or an LR1121 *chip/firmware*
> level issue that would affect any LR1121?**

- **WaveShare RX works cleanly** (completes packets the Seeed R2 drops, under identical
  conditions) → it's a **Seeed board design flaw** → WaveShare becomes the path forward.
- **WaveShare shows the same deficit** → it's **chip-level or our RadioLib usage** →
  affects all LR1121, escalate accordingly (and the Seeed module is exonerated as "normal
  for the part").

This is gated: only worth doing **after** the Seeed RX deficit is characterised
(HackRF #5) so there's a baseline to compare against — but the schematic/demo homework
below can be done any time.

---

## 2. The part, and the one big advantage over Seeed

Owner purchased the **Core1121-HF** variant. WaveShare's wiki family page is **Core1121-XF**
and lists HF/LF band variants. **The single most important fact:**

> **WaveShare PUBLISHES A SCHEMATIC** — `Core1121_XF_Sch.pdf`. The entire Seeed saga
> happened because Seeed's module datasheet had **incomplete RF-switch wiring**, and we only
> learned `V1=DIO5 / V2=DIO6` from a support email (David Du, 2026-05-28). With WaveShare we
> can **derive the RF-switch DIO wiring + truth table straight from the schematic** before
> flashing anything. That removes the "undocumented switch wiring" risk entirely.

**⚠️ First verification when parts arrive:** confirm whether the purchased **HF** variant
uses the same wiring as the **XF** schematic, or whether there's an HF-specific schematic /
RF-switch part. Don't assume HF == XF for the switch table.

### Confirmed from the WaveShare wiki (spotpear mirror)
- **Clock:** TCXO 32 MHz (same as the Seeed Wio — one less variable).
- **IRQ line:** DIO9 (wiki hardware table shows `GPIO40 DIO9` for the ESP32 reference).
- **Bands (HF):** 850–930 MHz, 1900–2100 MHz, 2400–2500 MHz (dual-band, satisfies Phase-1
  Interpretation B).
- **Antenna:** IPEX-4 or castellated, with TVS protection.
- **Demos:** 14 demos (LoRa CAD, PER, ping-pong, LoRaWAN Class A) for **ESP32, Raspberry Pi,
  Pico, STM32** — `Core1121_XF_Demo.zip`. ESP32 demo is the directly-relevant reference.
- **NOT on the wiki:** RF-switch truth table, switch part number — **but the schematic has them.**

### Resources to download (do this any time, pre-arrival)
- Schematic: `https://files.waveshare.com/wiki/Core1121/Core1121_XF_Sch.pdf`
- Demo zip:  `https://files.waveshare.com/wiki/Core1121/Core1121_XF_Demo.zip`
- Wiki (waveshare.com 403'd WebFetch; use the spotpear mirror or a browser):
  `https://spotpear.com/wiki/Core1121-HF-LF-LoRa-LR1121-Sub-GHz-2.4GHz.html`
- Owner also has **example code sets** locally — review alongside the demo zip.
- Owner-supplied chip UM already in `~/Downloads/WaveShare_UserManual_LR1121_v1_2.pdf` —
  note this is the **Semtech LR1121 chip User Manual** (same `§4.2.1 SetDioAsRfSwitch` /
  `§9.4.2 RF Switch Implementation` as the Seeed-side UM), **NOT** a board document. It does
  **not** give the WaveShare board's switch wiring — the **schematic** does.

Suggested home for downloads: `docs/datasheets/waveshare/`.

---

## 3. The key reuse insight

**It's the same LR1121 chip**, so the existing RadioLib driver does most of the work:
- `src/WioLR1121.cpp` / `.h` is effectively a **generic LR1121 driver** (named for the Seeed
  Wio but chip-generic). It should work for the WaveShare module **as-is**, except for
  **board-specific** bits.
- The chip-level init, the `LR1121Access` protected-method idiom, chip-EUI logging, and the
  IRQ-status heartbeat diagnostic (added session 3, commit `66dac8b`) all carry over and are
  exactly the instruments you want for the comparison.

**What differs per board (the whole bring-up surface):**
1. **RF switch table** — `rfswitch_table[]` in `WioLR1121.cpp` (~line 279) is locked to the
   Seeed **SKY13373** truth table: `MODE_STBY={0,0}`, `MODE_RX={1,0}`, `MODE_TX={1,1}`,
   `MODE_TX_HP={0,1}`, with V1=DIO5/V2=DIO6. **Derive the WaveShare equivalent from its
   schematic** (which DIOs drive its switch, what part, what truth table) and substitute.
   Per the rules of engagement: **any RF-switch table change needs written, datasheet/
   schematic-cited justification** — the schematic is that justification here.
2. **Pinout** — SPI (MISO/MOSI/SCK/CS), RESET, BUSY, DIO9=IRQ. Map the WaveShare module's
   pads to XIAO ESP32-S3 GPIOs (the firmware passes `nss, irqDio, reset, busy` into the
   driver via `makeRadio()` in `main.cpp:676`). WaveShare's ESP32 demo shows their reference
   GPIO mapping (e.g. DIO9→GPIO40) — adapt to the XIAO.
3. **TCXO** — both 32 MHz TCXO, so the existing TCXO handling should transfer; verify the
   reference voltage / `setTcxoMode` matches.

Decision on structure: either reuse `WioLR1121` with a board-select build flag, or fork a
thin `WaveShareLR1121` that differs only in the RF-switch table + pin defaults. Keep it
minimal — the diff from the Seeed driver should be small.

---

## 4. Bring-up plan (when hardware arrives)

1. **Desk work (pre-arrival OK):** download + read `Core1121_XF_Sch.pdf`. Extract and
   **write down** the RF-switch wiring + truth table. Confirm HF-vs-XF applicability.
2. **Wire** the Core1121 to a XIAO ESP32-S3 (or its own host per the demo) — SPI + RESET +
   BUSY + DIO9. Record the GPIO mapping in `MODULE-REGISTRY.md`.
3. **Firmware:** add a board variant (RF-switch table from the schematic + pin map). Keep
   the chip-EUI logging and the IRQ-status heartbeat readout. Start in the same
   **`R2_RX_ONLY_TEST`** mode (pure-listen) for a clean RX comparison.
4. **Capture the EUI** at boot → add a `pristine-WaveShare` row to
   `docs/testbed/MODULE-REGISTRY.md` so logs are unambiguous.
5. **Run the identical RX test** the Seeed module ran:
   - Same Heltec V4 source (US LongFast, sync 0x2B), same channel hash 0x08.
   - Watch `isr` climb + `[R2 RX]` + the heartbeat `irq=` (`0x10` preamble vs `0x08` RX_DONE).
   - Then the **HackRF + KT3 calibrated sweep (#5)**: completion-rate vs stepped power,
     side-by-side with the Seeed module's curve.
6. **Compare** completion rate under matched conditions and decide per the matrix in §1.

---

## 5. Pitfalls / rules carried over

- **One variable at a time.** Don't change RF-switch table, PA config, and pinout in one
  untested step. The schematic-derived switch table is the highest-risk item — get it right
  and justify it in writing (schematic-cited) before flashing TX.
- **TX power:** `platformio.ini` `LORA_RADIO2_TX_POWER=20` is asserted as "SX1262 PA range";
  for the LR1121 that needs the HP PA path. Not relevant for pure-RX comparison, but flag it
  before any WaveShare TX.
- **Don't repeat the Seeed mistake:** the schematic is the authoritative switch source —
  read it first, don't guess from the chip UM.
- **Bench identity:** always confirm which module is mounted via the boot chip-EUI line and
  `MODULE-REGISTRY.md`. Seeed suspect-GOOD = `00:16:C0:01:F0:9B:37:D5`.
- **PowerShell + HEREDOC commits via the Bash tool. Never force-push `main`** (the snapshot
  tag `lr1121-bringup-2026-05-26` is force-pushable and bumped per branch commit).

---

## 6. Success criteria

- **RF-switch wiring + truth table for the WaveShare board recorded** from its schematic.
- WaveShare LR1121 brought up on the bench, EUI logged, boots clean.
- **A side-by-side RX completion-rate comparison** (Heltec source, ideally HackRF-calibrated)
  vs the Seeed Wio module under identical conditions.
- **A verdict:** Seeed-board-design-flaw vs LR1121-chip/firmware-level — which decides
  whether Phase 1 ships on the WaveShare module or the deficit is escalated as chip-level.
