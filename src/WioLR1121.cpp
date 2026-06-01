// WioLR1121.cpp — see WioLR1121.h for design notes.
//
// Build-time flags (set in platformio.ini build_flags):
//
//   -DLR1121_DEBUG
//      Enable verbose Phase-B diagnostic prints — per-read pktLen/state,
//      getIrqFlags after begin(), transmit() return codes, startReceive()
//      return codes, re-arm warnings. ON by default while the RX block is
//      under investigation. Flip OFF (delete the flag) for quiet production
//      builds once the Wio-LR1121 RX path is resolved.
//
//   -DLR1121_BRUTEFORCE_RX_DIOMASK=<0..31>
//      Override the MODE_RX entry of the LR1121 RF switch table with an
//      explicit 5-bit DIO bitmask covering all 5 LR1121 RFSWx outputs:
//          bit 0 → DIO5 (RFSW0)
//          bit 1 → DIO6 (RFSW1)
//          bit 2 → DIO7 (RFSW2)
//          bit 3 → DIO8 (RFSW3)
//          bit 4 → DIO10 (RFSW4)
//      Per LR1121 user manual §4.5.2, the chip supports up to 5 external
//      RF switch / LNA control lines, all defaulting to High-Z until
//      SetDioAsRfSwitch is configured. The Wio-LR1121 module exposes
//      only DIO8 and DIO9 on user pads — DIO5/6/7 and DIO10 are internal
//      to the module and the candidate set for its integrated RF switch.
//      (DIO11 is not in RadioLib's RFSWx mapping.)
//
//      First-pass swept values 0..7 (DIO5/6/7 only) — all identical failure
//      on the Wio-LR1121. Extended sweep adds DIO8/DIO10 as candidates.
//      Examples:
//          0  = 00000 (passive, all LOW — femtofox config)
//          1  = 00001 (DIO5 HIGH — LilyGO T3S3 production)
//          8  = 01000 (DIO8 HIGH only — RFSW3)
//          16 = 10000 (DIO10 HIGH only — RFSW4, free for RFSW use because
//                       module has integrated TCXO, no 32k crystal needed)
//          24 = 11000 (DIO8 + DIO10 — most likely if module uses RFSW3+RFSW4
//                       per Semtech multi-band reference design)
//          25 = 11001 (T3S3-DIO5 + DIO8 + DIO10)
//          31 = 11111 (all 5 HIGH — catch-all)
//      Default: 1 (T3S3-style, DIO5 HIGH).
//
//   -DLR1121_RX_AUDIT_RUN=<0..5>
//      Select which DOE row in LR1121-RX-INIT-AUDIT.md to execute. Used for
//      the Phase-1 RX bring-up bench session — each value applies a
//      different combination of chip-level init treatments:
//          0 = Baseline (no extra treatments) + getErrors() diagnostic only
//          1 = + explicit _radio->standby(STANDBY_RC) before setRfSwitchTable
//              (verifies SetDioAsRfSwitch isn't silently failing per UM
//              v2.2 §4.2.1 "only works with the chip in Standby RC mode,
//              otherwise it returns a CMD_FAIL on the next GetStatus")
//          2 = + setRssiCalibration with UM v2.2 Table 7-21 "From 600 MHz
//              to 2 GHz" reference tunes — TOP CANDIDATE per UM §7.2.15:
//              "An incorrect gain can result in a missed detection
//              (packet loss)... The RSSI must be calibrated for each
//              hardware type. By default, the chip is calibrated for the
//              868-915MHz band on the LR1121 EVK."
//          3 = + explicit _radio->calibrateImageRejection(902.0f, 928.0f)
//              for the US sub-GHz band (UM §2.1.3: POR image cal "fails"
//              on chips with TCXO fitted — Wio-LR1121 has integrated TCXO,
//              so re-calibrating explicitly may be required)
//          4 = + setRxBoostedGainMode(true) — small ~2 dB benefit per UM
//              §7.2.12, not expected to solve alone
//          5 = Kitchen-sink stack (1+2+3+4 combined)
//      Default: 0 (baseline + diagnostic).
//
//      Walk this value sequentially 0,1,2,3,4,5 across bench runs per the
//      DOE table in LR1121-RX-INIT-AUDIT.md. The instrumentation below
//      prints each treatment's chip return code on the serial port so the
//      test outcome can be recorded per DOE row.

#include "WioLR1121.h"

#ifndef LR1121_BRUTEFORCE_RX_DIOMASK
  #define LR1121_BRUTEFORCE_RX_DIOMASK 1   // T3S3-style: DIO5 HIGH for RX
#endif

#ifndef LR1121_RX_AUDIT_RUN
  #define LR1121_RX_AUDIT_RUN 0            // baseline + getErrors() diagnostic
#endif

// Per-treatment compile-time booleans decoded from LR1121_RX_AUDIT_RUN.
// Each is true for its dedicated solo run AND for run 5 (kitchen-sink).
// See LR1121-RX-INIT-AUDIT.md "Run plan" table for the full DOE.
// RadioLib 7.7.0 marks LR11x0::getErrors() and LR11x0::setRssiCalibration() as
// protected, so they are unreachable through a plain LR1121* pointer. Use the
// standard access-promoting subclass idiom to re-expose them as public — this
// adds no data members and no virtuals, so static_cast<LR1121Access*>(_radio)
// is well-defined for any LR1121 instance.
struct LR1121Access : public LR1121 {
  using LR11x0::getErrors;
  using LR11x0::setRssiCalibration;
  // LR1121Access is never instantiated directly; this ctor exists only to
  // satisfy the compiler if it ever is.
  LR1121Access(Module* mod) : LR1121(mod) {}
};

namespace {
  constexpr bool RX_AUDIT_PRE_STANDBY      = (LR1121_RX_AUDIT_RUN == 1) || (LR1121_RX_AUDIT_RUN == 5);
  constexpr bool RX_AUDIT_RSSI_CAL         = (LR1121_RX_AUDIT_RUN == 2) || (LR1121_RX_AUDIT_RUN == 5);
  constexpr bool RX_AUDIT_CALIB_IMG        = (LR1121_RX_AUDIT_RUN == 3) || (LR1121_RX_AUDIT_RUN == 5);
  constexpr bool RX_AUDIT_RX_BOOSTED       = (LR1121_RX_AUDIT_RUN == 4) || (LR1121_RX_AUDIT_RUN == 5);
  // Run 6 — "Meshtastic-style" init, mirroring stock Meshtastic firmware's
  // working LR1121 path (proven on LilyGO T3S3 LR1121 on the same bench):
  //   - RF switch table declares only DIO5 + DIO6 (others = RADIOLIB_NC)
  //   - setRegulatorDCDC() called after begin() (RadioLib default is LDO)
  //   - setPreambleLength() called immediately before every startReceive()
  //     (per Meshtastic LR11x0Interface.cpp comment: "Solve RX ack fail
  //      after direct message sent. Not sure why this is needed.")
  //   - setRxBoostedGainMode stays OFF (Meshtastic default)
  // These three deltas are board-layout-INDEPENDENT and worth a final test
  // even though the T3S3 board's switch wiring differs from the Wio-LR1121.
  constexpr bool RX_AUDIT_MESHTASTIC_STYLE = (LR1121_RX_AUDIT_RUN == 6);

  // UM v2.2 Table 7-21 reference RSSI calibration tunes for the
  // "From 600 MHz to 2 GHz" band. Index order matches RadioLib's
  // setRssiCalibration buffer packing in LR11x0_commands.cpp:
  //   tune[0]=G4,  [1]=G5,  [2]=G6,  [3]=G7,   [4]=G8,   [5]=G9,
  //   tune[6]=G10, [7]=G11, [8]=G12, [9]=G13,  [10..16]=G13hp1..hp7,
  //   tune[17]=unused (high nibble of byte 8 set to 0).
  constexpr int8_t LR1121_RSSI_TUNE_600M_2G[18] = {
      2, 2, 2, 3, 3, 4, 5, 4, 4, 6,   // G4..G13
      5, 5, 6, 6, 6, 7, 6,            // G13hp1..hp7
      0,                              // unused, packed as high nibble of byte 8
  };
  constexpr int16_t LR1121_RSSI_GAIN_OFFSET_600M_2G = 0;
}

// Static ISR trampolines — supports up to 2 simultaneous instances.
// Each ISR just sets a flag; SPI reads happen later in the polling task.
static WioLR1121 *_inst[2]  = {};
static uint8_t    _instCount = 0;

static void IRAM_ATTR _isr0() {
    if (_inst[0]) { _inst[0]->_rxFlag = true; _inst[0]->_isrCount++; }
}
static void IRAM_ATTR _isr1() {
    if (_inst[1]) { _inst[1]->_rxFlag = true; _inst[1]->_isrCount++; }
}

static void (* const _isrs[2])() = { _isr0, _isr1 };

// ---------------------------------------------------------------------------

WioLR1121::WioLR1121(int nss, int irq, int reset, int busy,
                     SPIClass &spi, SemaphoreHandle_t mutex,
                     const char *name, const LoraConfig &config)
    : _mutex(mutex), _reset(reset), _busy(busy), _name(name), _config(config)
{
    // Deassert CS immediately so this chip doesn't corrupt the shared bus
    // while the other radio's begin() runs its SPI init sequence.
    pinMode(nss, OUTPUT);
    digitalWrite(nss, HIGH);

    // RadioLib Module for LR11x0: (cs, irq, rst, busy, spi, spiSettings).
    _mod   = new Module(nss, irq, reset, busy, spi,
                        SPISettings(1000000, MSBFIRST, SPI_MODE0));
    _radio = new LR1121(_mod);
    if (_instCount < 2) {
        _inst[_instCount++] = this;
    }
}

WioLR1121::~WioLR1121()
{
    delete _radio;
    delete _mod;
}

bool WioLR1121::begin()
{
    xSemaphoreTake(_mutex, portMAX_DELAY);

    // --- Manual reset + extended BUSY-wait ---------------------------------
    // LA-confirmed on the Wio-LR1121 module: BUSY stays HIGH for ~120-150 ms
    // after NRESET rises (the boot ROM takes that long to release). RadioLib's
    // internal waitForBusy times out well before that, so the driver starts
    // pumping SPI commands while the chip is still asserting BUSY — chip
    // ignores them and findChip() reports "not found". We drive reset
    // manually and poll BUSY directly before handing off to RadioLib.
    if (_reset >= 0) {
        pinMode(_reset, OUTPUT);
        digitalWrite(_reset, LOW);
        delay(2);
        digitalWrite(_reset, HIGH);
    }
    delay(50);                          // initial boot-ROM margin

    if (_busy >= 0) {
        pinMode(_busy, INPUT);
        uint32_t t0 = millis();
        while (digitalRead(_busy) == HIGH && (millis() - t0) < 1000) {
            delay(1);
        }
        uint32_t busyElapsed = millis() - t0;
        if (digitalRead(_busy) == HIGH) {
            Serial.printf("[%s] BUSY stuck HIGH after %lu ms — aborting begin()\n",
                          _name, (unsigned long)busyElapsed);
            xSemaphoreGive(_mutex);
            return false;
        }
        Serial.printf("[%s] BUSY released after %lu ms (post 50ms boot margin)\n",
                      _name, (unsigned long)busyElapsed);
    }
    // --- end pre-init wait -------------------------------------------------

    // ----- RX-init-audit Run 1 / 5: Pre-standby treatment -------------------
    // Per UM v2.2 §4.2.1: "SetDioAsRfSwitch ... only works with the chip in
    // Standby RC mode, otherwise it returns a CMD_FAIL on the next GetStatus
    // command." Per UM §2.2 the chip ends startup in STBY_RC, so the post-
    // reset path SHOULD be in STBY_RC by the time we call setRfSwitchTable.
    // But RadioLib's setRfSwitchTable discards the return value, so silent
    // CMD_FAIL would be invisible to us. This treatment explicitly forces
    // STBY_RC before the switch table install to remove all doubt.
    if (RX_AUDIT_PRE_STANDBY) {
        int16_t sbState = _radio->standby(RADIOLIB_LR11X0_STANDBY_RC);
        Serial.printf("[%s] [RX-AUDIT Run %d] pre-setRfSwitchTable standby(STBY_RC) = %d\n",
                      _name, (int)LR1121_RX_AUDIT_RUN, (int)sbState);
    }

    // ----- Install the LR1121 RF switch truth table BEFORE begin() -------
    // Per LR1121 user manual §4.5.2 the chip supports up to 5 RFSWx outputs
    // mappable to DIO5 (RFSW0), DIO6 (RFSW1), DIO7 (RFSW2), DIO8 (RFSW3),
    // DIO10 (RFSW4). All outputs default to High-Z until SetDioAsRfSwitch
    // is called via setRfSwitchTable(). The Wio-LR1121 module exposes only
    // DIO8 and DIO9 on user pads; DIO5/6/7 and DIO10 are internal to the
    // module and the candidate set for its integrated RF switch.
    //
    // DIO10 is dual-purposed with the 32k crystal (32k_N) on the chip, but
    // the Wio-LR1121 has an integrated TCXO (per datasheet) — no 32k
    // crystal needed, so DIO10 is free for RFSW4 use.
    //
    // MODE_RX is the variable under test (see LR1121_BRUTEFORCE_RX_DIOMASK
    // build flag). Other modes use the LilyGO T3S3 production reference
    // for DIO5/6/7 with DIO8/DIO10 held LOW:
    //   - STBY:  all LOW
    //   - TX:    DIO6 HIGH (low power PA)
    //   - TX_HP: DIO6 HIGH (high power PA, same as low — T3S3 reference)
    //   - TX_HF: all LOW (2.4 GHz, unused on this module)
    //   - GNSS:  all LOW (not exercised)
    //   - WIFI:  all LOW (not exercised)
    // Pin arrays for the RF switch table. RadioLib's setRfSwitchTable takes
    // a reference to a fixed-size 5-array, so we cannot select via pointer —
    // each branch passes its own array directly to setRfSwitchTable below.
    //
    // Run 6 (Meshtastic-style): declare only DIO5 + DIO6 as switch outputs;
    // leave DIO7/8/10 as RADIOLIB_NC so the chip never drives them. This
    // matches Meshtastic's stock T3S3 LR1121 rfswitch.h. If the Wio module
    // uses DIO7/8/10 internally for any purpose (LNA bias, etc.), driving
    // them as outputs in the 5-pin table could be commandeering them.
    static const uint32_t rfswitch_pins_meshtastic[Module::RFSWITCH_MAX_PINS] = {
        RADIOLIB_LR11X0_DIO5, RADIOLIB_LR11X0_DIO6,
        RADIOLIB_NC, RADIOLIB_NC, RADIOLIB_NC
    };
    static const uint32_t rfswitch_pins_5pin[Module::RFSWITCH_MAX_PINS] = {
        RADIOLIB_LR11X0_DIO5, RADIOLIB_LR11X0_DIO6, RADIOLIB_LR11X0_DIO7,
        RADIOLIB_LR11X0_DIO8, RADIOLIB_LR11X0_DIO10
    };
    // Decode the 5-bit brute-force MODE_RX bitmask into per-DIO values.
    // bit 0=DIO5 / bit 1=DIO6 / bit 2=DIO7 / bit 3=DIO8 / bit 4=DIO10
    constexpr uint8_t RX_D5  = (LR1121_BRUTEFORCE_RX_DIOMASK >> 0) & 1;
    constexpr uint8_t RX_D6  = (LR1121_BRUTEFORCE_RX_DIOMASK >> 1) & 1;
    constexpr uint8_t RX_D7  = (LR1121_BRUTEFORCE_RX_DIOMASK >> 2) & 1;
    constexpr uint8_t RX_D8  = (LR1121_BRUTEFORCE_RX_DIOMASK >> 3) & 1;
    constexpr uint8_t RX_D10 = (LR1121_BRUTEFORCE_RX_DIOMASK >> 4) & 1;
    // 2026-05-28: Seeed (David Du, Sensecap Support) confirmed the SKY13373-460LF
    // truth table for the Wio-LR1121 integrated RF switch:
    //   V1 (DIO5)  V2 (DIO6)  Path
    //       0          0      Shutdown (>20 us → 20 us recovery to active)
    //       1          0      RFI_P_LF & RFI_N_LF  (RX)
    //       0          1      RFO_HP_LF            (TX high-power)
    //       1          1      RFO_LP_LF            (TX low-power)
    // DIO7/DIO8/DIO10 are NOT connected to the switch on this module — only
    // DIO5/DIO6 (V1/V2) are wired. RX_D5..RX_D10 macros remain in place so the
    // archived LR1121_BRUTEFORCE_RX_DIOMASK sweep machinery still compiles, but
    // the table itself is now locked to Seeed's authoritative values. The
    // MODE_RX entry under the default mask (=1) matches the confirmed RX path.
    // MODE_STBY is (0,0) shutdown: the antenna is isolated from the LNA input
    // during TX->STBY transitions, restoring the protection that 949176a's
    // (1,0) RX-latched value removed. The (1,0) value parked the antenna on the
    // LNA path during standby; the steady state is benign (identical to RX-idle),
    // but it removed isolation during the TX->STBY window, leaving residual PA
    // energy a path into the LNA. Reverted per Experiment R1 (CLAUDE.md §3). The
    // 20 us shutdown->active re-arm penalty is accepted; it is negligible for a
    // continuous-listen repeater and not worth trading antenna isolation for.
    static const Module::RfSwitchMode_t rfswitch_table[] = {
        // mode                        DIO5    DIO6    DIO7    DIO8    DIO10
        { LR11x0::MODE_STBY,         { 0,      0,      0,      0,      0      } },
        { LR11x0::MODE_RX,           { RX_D5,  RX_D6,  RX_D7,  RX_D8,  RX_D10 } },
        { LR11x0::MODE_TX,           { 1,      1,      0,      0,      0      } },
        { LR11x0::MODE_TX_HP,        { 0,      1,      0,      0,      0      } },
        { LR11x0::MODE_TX_HF,        { 0,      0,      0,      0,      0      } },
        { LR11x0::MODE_GNSS,         { 0,      0,      0,      0,      0      } },
        { LR11x0::MODE_WIFI,         { 0,      0,      0,      0,      0      } },
        { LR11x0::MODE_END_OF_TABLE, {} },
    };
    Serial.printf("[%s] installing RF switch table: MODE_RX = "
                  "D5=%u D6=%u D7=%u D8=%u D10=%u (BRUTEFORCE_RX_DIOMASK=%u)%s\n",
                  _name, RX_D5, RX_D6, RX_D7, RX_D8, RX_D10,
                  (unsigned)LR1121_BRUTEFORCE_RX_DIOMASK,
                  RX_AUDIT_MESHTASTIC_STYLE ? " [Run 6: D7/D8/D10 forced NC]" : "");
    if (RX_AUDIT_MESHTASTIC_STYLE) {
        _radio->setRfSwitchTable(rfswitch_pins_meshtastic, rfswitch_table);
    } else {
        _radio->setRfSwitchTable(rfswitch_pins_5pin, rfswitch_table);
    }

#ifdef LR1121_DEBUG
    // RX-init-audit diagnostic (always on under LR1121_DEBUG): check the
    // chip's error register after setRfSwitchTable. Per UM v2.2 §4.2.1
    // SetDioAsRfSwitch returns CMD_FAIL if not in STBY_RC mode — RadioLib's
    // setRfSwitchTable discards the return value. If devErrors has the
    // CMD_FAIL bit set, the entire 12-iteration RFSWx sweep evidence is
    // invalidated and we need to re-run with the pre-standby treatment.
    {
        uint16_t devErrors = 0;
        int16_t errState = static_cast<LR1121Access*>(_radio)->getErrors(&devErrors);
        Serial.printf("[%s] [RX-AUDIT diag] post-setRfSwitchTable getErrors() "
                      "state=%d errors=0x%04X\n",
                      _name, (int)errState, (unsigned)devErrors);
    }
#endif

    // RadioLib 7.7.0 LR1120::begin(freq, bw, sf, cr, sync, power, preamble,
    // tcxo) convenience overload calls LR11x0::begin(bw, sf, cr, sync,
    // preamble) -- note: the `high` parameter is DROPPED and defaults to
    // false. setBandwidth(bw, high=false) then validates bw against the
    // sub-GHz range [0, 510] kHz and REJECTS the 2.4 GHz wideLora values
    // (203.125/406.25/812.5/1625) with RADIOLIB_ERR_INVALID_BANDWIDTH (-8).
    //
    // Workaround: for 2.4 GHz operation, bypass the LR1120 convenience
    // overload. Call LR11x0::begin() directly with high=true so the
    // setBandwidth call inside accepts the wideLora BWs, then call
    // setFrequency()/setOutputPower() manually for the remaining config.
    //
    // RadioLib API design flaw worth upstreaming -- the LR1120 overloads
    // should auto-derive `high` from freq, or expose `high` in the signature.
    // Logged in docs/UPSTREAM-PR-CANDIDATES.md.
    bool is2g4 = (_config.frequency > 1000.0f);
    int16_t state;

    if (is2g4) {
        // 2.4 GHz path: call LR11x0::begin() with high=true to allow the
        // wideLora bandwidths. _radio is an LR1121* which inherits the
        // public LR11x0::begin() overload via LR1120; access it explicitly.
        state = _radio->LR11x0::begin(
            _config.bandwidth,
            _config.spreadFactor,
            _config.codingRate,
            _config.syncWord,
            _config.preambleLen,
            true   // high — selects the 2.4 GHz BW validation range
        );
        if (state == RADIOLIB_ERR_NONE) {
            state = _radio->setFrequency(_config.frequency);
        }
        if (state == RADIOLIB_ERR_NONE) {
            state = _radio->setOutputPower(_config.txPower);
        }
        // TCXO voltage is applied during modSetup inside LR11x0::begin()
        // when the chip was constructed with the TCXO arg; the LR1121
        // constructor in this project passes _config.tcxoVoltage already.
    } else {
        // Sub-GHz path: use the LR1120 convenience overload (RadioLib's
        // existing happy path). setBandwidth(bw, high=false) accepts the
        // sub-GHz bandwidths (62.5/125/250/500 kHz).
        state = _radio->begin(
            _config.frequency,
            _config.bandwidth,
            _config.spreadFactor,
            _config.codingRate,
            _config.syncWord,
            _config.txPower,
            _config.preambleLen,
            _config.tcxoVoltage
        );
    }

    if (state != RADIOLIB_ERR_NONE) {
        Serial.printf("[%s] begin() failed: %d (is2g4=%d freq=%.3f bw=%.3f)\n",
                      _name, state, (int)is2g4,
                      _config.frequency, _config.bandwidth);
        xSemaphoreGive(_mutex);
        return false;
    }

    Serial.printf("[%s] ready — %.3f MHz  BW %.1f kHz  SF%d  CR4/%d  %d dBm  "
                  "sync 0x%02X  %s\n",
                  _name,
                  _config.frequency, _config.bandwidth,
                  _config.spreadFactor, _config.codingRate,
                  _config.txPower, _config.syncWord,
                  is2g4 ? "2.4GHz" : "sub-GHz");

    // ----- RX-init-audit Runs 2/3/4/5: post-begin() chip-level treatments --
    // Each treatment is enabled by the LR1121_RX_AUDIT_RUN build flag (see
    // doc block at top of file). The treatments apply in defined order so
    // that the kitchen-sink Run 5 composes them deterministically.

    if (RX_AUDIT_RSSI_CAL) {
        // UM v2.2 §7.2.15: per-PCB calibration via Table 7-21 "From 600 MHz
        // to 2 GHz" reference tunes. Top candidate root cause for the RX
        // failure — the chip ships calibrated for the LR1121 EVK, not the
        // Wio-LR1121's matching network.
        int16_t rssiState = static_cast<LR1121Access*>(_radio)->setRssiCalibration(
                                LR1121_RSSI_TUNE_600M_2G,
                                LR1121_RSSI_GAIN_OFFSET_600M_2G);
        Serial.printf("[%s] [RX-AUDIT Run %d] setRssiCalibration(UM 600M-2G) = %d\n",
                      _name, (int)LR1121_RX_AUDIT_RUN, (int)rssiState);
    }

    if (RX_AUDIT_CALIB_IMG) {
        // UM v2.2 §2.1.3: POR image calibration "fails" on chips with TCXO
        // fitted. The Wio-LR1121 has integrated TCXO, so explicit re-cal
        // for the application band may be required. Sub-GHz US ISM band.
        int16_t imgState = _radio->calibrateImageRejection(902.0f, 928.0f);
        Serial.printf("[%s] [RX-AUDIT Run %d] calibrateImageRejection(902, 928) = %d\n",
                      _name, (int)LR1121_RX_AUDIT_RUN, (int)imgState);
    }

    if (RX_AUDIT_RX_BOOSTED) {
        // UM v2.2 §7.2.12: ~2 dB boost. Small effect; included for
        // completeness — not expected to solve the RX block alone.
        int16_t boostState = _radio->setRxBoostedGainMode(true);
        Serial.printf("[%s] [RX-AUDIT Run %d] setRxBoostedGainMode(true) = %d\n",
                      _name, (int)LR1121_RX_AUDIT_RUN, (int)boostState);
    }

    if (RX_AUDIT_MESHTASTIC_STYLE) {
        // Meshtastic LR11x0Interface.cpp calls setRegulatorDCDC() right after
        // begin(). RadioLib's begin() defaults to LDO via setRegulatorLDO().
        // The Wio-LR1121 module datasheet does not specify which regulator
        // mode it expects — Seeed's reference assumes the chip default.
        int16_t dcdcState = _radio->setRegulatorDCDC();
        Serial.printf("[%s] [RX-AUDIT Run 6] setRegulatorDCDC() = %d\n",
                      _name, (int)dcdcState);
    }

    Serial.printf("[%s] RX-AUDIT run = %d (pre_standby=%d rssi_cal=%d calib_img=%d rx_boosted=%d mt_style=%d)\n",
                  _name, (int)LR1121_RX_AUDIT_RUN,
                  (int)RX_AUDIT_PRE_STANDBY, (int)RX_AUDIT_RSSI_CAL,
                  (int)RX_AUDIT_CALIB_IMG, (int)RX_AUDIT_RX_BOOSTED,
                  (int)RX_AUDIT_MESHTASTIC_STYLE);

#ifdef LR1121_DEBUG
    // Dump the chip's IRQ flags right after begin() so we can see what (if
    // any) bits are pending before we ever ask it to RX. Latched bits here
    // might mask future RX_DONE events.
    uint32_t irqAfterBegin = _radio->getIrqFlags();
    Serial.printf("[%s] getIrqFlags() post-begin = 0x%08lX\n",
                  _name, (unsigned long)irqAfterBegin);
#endif

    // Wire the DIO9 interrupt to this instance's ISR trampoline.
    for (uint8_t i = 0; i < _instCount; i++) {
        if (_inst[i] == this) {
            _radio->setPacketReceivedAction(_isrs[i]);
            break;
        }
    }

    xSemaphoreGive(_mutex);
    return true;
}

bool WioLR1121::available()
{
    return _rxFlag;
}

int16_t WioLR1121::read(uint8_t *buf, size_t &len, float *rssi, float *snr)
{
    _rxFlag = false;

    xSemaphoreTake(_mutex, portMAX_DELAY);

    size_t  pktLen = _radio->getPacketLength();
    size_t  toRead = (pktLen < len) ? pktLen : len;
    int16_t state  = _radio->readData(buf, toRead);
    len = (state == RADIOLIB_ERR_NONE) ? toRead : 0;

#ifdef LR1121_DEBUG
    // Dump every read attempt so we can see why some ISR fires don't
    // translate into an [R2 RX] line.
    Serial.printf("[%s] read: pktLen=%u state=%d len=%u\n",
                  _name, (unsigned)pktLen, (int)state, (unsigned)len);
#endif

    if (state == RADIOLIB_ERR_NONE) {
        if (rssi) *rssi = _radio->getRSSI();
        if (snr)  *snr  = _radio->getSNR();
    }

    // Defensive re-arm: guarantee return to continuous RX before releasing the
    // mutex. RadioLib's readData() already calls clearIrqState() internally,
    // but bench-observed behavior on the Wio-LR1121 showed that without this
    // belt-and-braces sequence the chip stops receiving after one packet.
    int16_t clrSt = _radio->clearIrqFlags(RADIOLIB_LR11X0_IRQ_ALL);
    int16_t sbSt  = _radio->standby();
    if (RX_AUDIT_MESHTASTIC_STYLE) {
        // Meshtastic LR11x0Interface.cpp:272 — "Solve RX ack fail after
        // direct message sent. Not sure why this is needed." Undocumented
        // workaround that Meshtastic relies on. Run 6 tests it on the Wio.
        _radio->setPreambleLength(_config.preambleLen);
    }
    int16_t rxSt  = _radio->startReceive();
#ifdef LR1121_DEBUG
    if (clrSt != RADIOLIB_ERR_NONE || sbSt != RADIOLIB_ERR_NONE ||
        rxSt != RADIOLIB_ERR_NONE) {
        Serial.printf("[%s] re-arm warn: clr=%d sb=%d rx=%d\n",
                      _name, clrSt, sbSt, rxSt);
    }
#else
    (void)clrSt; (void)sbSt; (void)rxSt;
#endif

    xSemaphoreGive(_mutex);
    return state;
}

int16_t WioLR1121::transmit(const uint8_t *buf, size_t len)
{
    xSemaphoreTake(_mutex, portMAX_DELAY);
    int16_t txSt = _radio->transmit(const_cast<uint8_t *>(buf), len);
    // Auto-return to RX so the bridge keeps listening without the caller
    // having to explicitly call startReceive() on the transmitting radio.
    _rxFlag = false;
    if (RX_AUDIT_MESHTASTIC_STYLE) {
        _radio->setPreambleLength(_config.preambleLen);
    }
    int16_t rxSt = _radio->startReceive();
#ifdef LR1121_DEBUG
    Serial.printf("[%s] transmit(%u B) tx=%d post-rx=%d\n",
                  _name, (unsigned)len, (int)txSt, (int)rxSt);
#else
    (void)rxSt;
#endif
    xSemaphoreGive(_mutex);
    return txSt;
}

void WioLR1121::startReceive()
{
    _rxFlag = false;
    xSemaphoreTake(_mutex, portMAX_DELAY);
    if (RX_AUDIT_MESHTASTIC_STYLE) {
        _radio->setPreambleLength(_config.preambleLen);
    }
    int16_t st = _radio->startReceive();
#ifdef LR1121_DEBUG
    Serial.printf("[%s] startReceive() = %d\n", _name, (int)st);
#else
    (void)st;
#endif
    xSemaphoreGive(_mutex);
}
