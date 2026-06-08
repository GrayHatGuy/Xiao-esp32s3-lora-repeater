// Core1121.cpp — WaveShare Core1121-XF (Semtech LR1121) driver.
// See Core1121.h for the board-fact summary and design notes.
//
// Build-time flags (set in platformio.ini build_flags):
//
//   -DLR1121_DEBUG
//      Enable verbose bring-up diagnostic prints — per-read pktLen/state,
//      getErrors() after the RF-switch install, getIrqFlags() after begin(),
//      transmit() return codes + post-TX IRQ register, startReceive() return
//      codes, re-arm warnings. Useful while bringing the Core1121 up on the
//      bench; delete the flag for a quiet production build.
//
// History: this file replaces the Seeed Wio-LR1121 driver (WioLR1121.*) used on
// the lr1121-phase1 branch. The Seeed bring-up scaffolding — the
// LR1121_BRUTEFORCE_RX_DIOMASK DIO sweep and the LR1121_RX_AUDIT_RUN 0–6 DOE —
// was deleted: both existed only to hunt the Seeed module's *undocumented* RF
// switch wiring and its sub-GHz RX deficit. The Core1121 publishes a schematic,
// so its switch wiring is known exactly (see the truth table below) and that
// guess-and-check machinery is obsolete here. The still-useful instruments
// (chip-EUI boot logging, the DIO9 ISR counter, debugIrqStatus(), and the
// LR1121_DEBUG return-code logging) are retained.

#include "Core1121.h"
#include "SerialLog.h"   // logf() — serialise USB-CDC log across the two cores

// --- CORE1121_TCXO_RECAL — RX frequency-offset experiment (see CLAUDE.md §0) --
// The Core1121 (LR1121) RX center is bench-confirmed LOW by >30 kHz at the
// MeshCore BW62.5 config: a static, monotonic, frequency-linear offset (>33 ppm
// @ 910 MHz) that is fatal at narrow BW but tolerated at BW250. Leading cause:
// RadioLib's begin() runs setTCXO(3.0V, ~5 ms start budget) then immediately
// calibrate(CALIBRATE_ALL) in config() with NO confirmation that the 32 MHz
// TCXO/XOSC is up — and the LR1121 SKIPS its power-on calibration when a TCXO
// is fitted, requiring the host to "program the TCXO configuration and
// re-launch the calibrations before further usage" (LR1121 datasheet §1.2.4,
// p.8; UM v2.2 §3.6.1, p.32 — the HF_XOSC_START_ERR fix is "send SetTcxoMode and
// redo calibrations"). RadioLib never re-runs calibrate() after the XOSC
// settles, so a wrong-reference PLL trim is baked in and never refreshed.
//
// When CORE1121_TCXO_RECAL is defined, begin() re-issues setTCXO + a full
// calibrate() once the TCXO is warm, on the sub-GHz path only, and logs the
// device error register before/after so the owner can A/B it on the bench.
// DEFAULT OFF: with the flag undefined, begin() is byte-for-byte the baseline.
// This is BOTH the candidate fix AND the discriminator the investigation needs:
// if the bench nudge result is unchanged after a warm recalibration, the offset
// is a fixed hardware reference error (not driver-fixable); if it moves toward
// nominal, the cold-XOSC-calibration theory is confirmed.
//
// CORE1121_TCXO_DELAY_US is the SetTcxoMode startup-timeout argument (max time
// the chip allows the 32 MHz oscillator to start, UM v2.2 §6.3.2, p.49; RadioLib
// converts it to 30.52 us ticks). RadioLib's default is 5000 us (~5 ms); its
// LR2021 successor bumped the default to 1 s, noting first-startup timing "may
// be measured very inaccurately" — so a too-short budget can spuriously trip
// HF_XOSC_START_ERR. Sweep this (one value per build) as the SECOND experiment
// variable if the recal alone does not clear the error / move the offset.
#ifndef CORE1121_TCXO_DELAY_US
#define CORE1121_TCXO_DELAY_US 5000
#endif

// --- CORE1121_FREQ_OFFSET_HZ — fixed RX/TX carrier-offset correction ---------
// This board's 32 MHz reference runs low, so the LR1121's synthesized carrier
// (both RX LO and TX) lands ~45-50 kHz below the commanded frequency — a FIXED
// hardware offset (recalibration does not move it; bench-confirmed). Since the
// reference error scales every synthesized frequency equally, commanding the
// chip HIGHER by the same amount lands the actual RX/TX on the configured
// frequency. This adds CORE1121_FREQ_OFFSET_HZ to every sub-GHz frequency the
// driver programs, so a config of 910.525 MHz transmits and receives on
// 910.525 MHz. Default 0 = no correction (unchanged baseline); set the bench
// value (e.g. 50000) in platformio.ini. 2.4 GHz path is left uncorrected.
#ifndef CORE1121_FREQ_OFFSET_HZ
#define CORE1121_FREQ_OFFSET_HZ 0
#endif

// --- CORE1121_RX_SYNC_OVERRIDE — debug: force the LoRa sync word after begin() -
// "Stuck at preamble (irq 0x10), never advances to header" is the textbook
// fingerprint of a sync-word mismatch — so this flag lets the bench force R2's
// LoRa sync word to an arbitrary value to test that directly. NOTE: the OEM
// Semtech-driver demo for this exact board uses sync 0x12 — identical to our
// config — so this is expected to confirm sync is NOT the cause; provided for
// empirical proof / sweeping. Undefined = use the configured sync word (no-op).
//   e.g. -DCORE1121_RX_SYNC_OVERRIDE=0x12   (or 0x34 public, 0x2B meshtastic)
// (No default define — absence leaves begin() at the configured sync word.)

// RadioLib 7.7.0 marks several LR11x0 methods (getErrors/getChipEui and, for the
// CORE1121_TCXO_RECAL experiment, calibrate/clearErrors) as protected, so they
// are unreachable through a plain LR1121* pointer. Use the standard
// access-promoting subclass idiom to re-expose them as public — this adds no
// data members and no virtuals, so static_cast<LR1121Access*>(_radio) is
// well-defined for any LR1121 instance.
struct LR1121Access : public LR1121 {
  using LR11x0::getErrors;
  using LR11x0::getChipEui;
  using LR11x0::calibrate;          // CORE1121_TCXO_RECAL — redo block calibration
  using LR11x0::clearErrors;        // CORE1121_TCXO_RECAL — clear HF_XOSC_START_ERR
  using LR11x0::setRssiCalibration; // CORE1121_RSSI_CAL — program the AGC gain table
  // LR1121Access is never instantiated directly; this ctor exists only to
  // satisfy the compiler if it ever is.
  LR1121Access(Module* mod) : LR1121(mod) {}
};

// Static ISR trampolines — supports up to 2 simultaneous instances.
// Each ISR just sets a flag; SPI reads happen later in the polling task.
static Core1121 *_inst[2]  = {};
static uint8_t   _instCount = 0;

static void IRAM_ATTR _isr0() {
    if (_inst[0]) { _inst[0]->_rxFlag = true; _inst[0]->_isrCount++; }
}
static void IRAM_ATTR _isr1() {
    if (_inst[1]) { _inst[1]->_rxFlag = true; _inst[1]->_isrCount++; }
}

static void (* const _isrs[2])() = { _isr0, _isr1 };

// ---------------------------------------------------------------------------

Core1121::Core1121(int nss, int irq, int reset, int busy,
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

Core1121::~Core1121()
{
    delete _radio;
    delete _mod;
}

bool Core1121::begin()
{
    xSemaphoreTake(_mutex, portMAX_DELAY);

    // --- Manual reset + extended BUSY-wait ---------------------------------
    // LA-confirmed on the Wio-LR1121 module: BUSY stays HIGH for ~120-150 ms
    // after NRESET rises (the boot ROM takes that long to release). RadioLib's
    // internal waitForBusy times out well before that, so the driver starts
    // pumping SPI commands while the chip is still asserting BUSY — chip
    // ignores them and findChip() reports "not found". We drive reset
    // manually and poll BUSY directly before handing off to RadioLib. This is
    // an LR1121 boot-ROM behaviour (chip-level, not Seeed-specific), so it is
    // kept for the Core1121.
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
            logf("[%s] BUSY stuck HIGH after %lu ms — aborting begin()\n",
                          _name, (unsigned long)busyElapsed);
            xSemaphoreGive(_mutex);
            return false;
        }
        logf("[%s] BUSY released after %lu ms (post 50ms boot margin)\n",
                      _name, (unsigned long)busyElapsed);
    }
    // --- end pre-init wait -------------------------------------------------

    // ----- Install the Core1121 RF-switch truth table BEFORE begin() --------
    // Authoritative source: WaveShare Core1121-XF schematic `Core1121_XF_Sch.pdf`
    // (U1 = pSemi PE4259 SPDT switch). Recorded in full in
    // docs/datasheets/waveshare/CORE1121-RF-SWITCH.md. This satisfies the
    // project rule that any RF-switch table needs written, schematic-cited
    // justification.
    //
    // Schematic wiring:
    //   - PE4259 VDD  (pin 6) = RFSW0_V1 = LR1121 DIO5
    //   - PE4259 CTRL (pin 4) = RFSW1_V2 = LR1121 DIO6
    //   - PE4259 RFC  (pin 5) = LORA_ANT (ANT2, the sub-GHz antenna)
    //   - RF1 (pin 1) = RFO_HP_LF / RFO_LP_LF PA output (TX). R3=0R wires the
    //                   HIGH-POWER PA output (RFO_HP_LF); R4 (LP) = NC, so only
    //                   the HP PA is physically connected.
    //   - RF2 (pin 3) = RFI_P_LF0 / RFI_N_LF0 LNA input (RX).
    //
    // Schematic truth table (printed on the sheet):
    //   V1(DIO5)=0, V2(DIO6)=1 → RFC→RF1  (transmit path)
    //   V1(DIO5)=1, V2(DIO6)=0 → RFC→RF2  (receive path)
    //   V1=0, V2=0             → switch isolated (standby)
    //
    // Mapped to RadioLib LR11x0 RF-switch modes:
    //   STBY  : {0,0}  switch isolated
    //   RX    : {1,0}  RFC→RF2 (LNA)              — both bands; harmless for 2.4G
    //   TX    : {0,1}  RFC→RF1 (PA)
    //   TX_HP : {0,1}  RFC→RF1 (PA) — operative path at our TX power (HP PA)
    //   TX_HF : {0,0}  2.4 GHz uses RFIO_HF → ANT1, bypassing the PE4259
    //   GNSS  : {0,0}  unused
    //   WIFI  : {0,0}  unused
    //
    // Only DIO5 + DIO6 are switch outputs; DIO7/DIO8 are broken out to the
    // module header but unused by the switch, and DIO10/DIO11 carry the
    // 32.768 kHz crystal (Y2) on this board — so all three remain RADIOLIB_NC
    // and the chip never drives them. (This is the key Core1121-vs-Seeed
    // difference: the Seeed Wio had no 32k crystal and a different SP3T switch
    // whose MODE_TX was {1,1}; the Core1121 PE4259 uses {0,1} for TX.)
    static const uint32_t rfswitch_pins[Module::RFSWITCH_MAX_PINS] = {
        RADIOLIB_LR11X0_DIO5, RADIOLIB_LR11X0_DIO6,
        RADIOLIB_NC, RADIOLIB_NC, RADIOLIB_NC
    };
    static const Module::RfSwitchMode_t rfswitch_table[] = {
        // mode                        DIO5  DIO6  (DIO7/DIO8/DIO10 = NC)
        { LR11x0::MODE_STBY,         { 0,    0,    0,   0,   0 } },
        { LR11x0::MODE_RX,           { 1,    0,    0,   0,   0 } },
        { LR11x0::MODE_TX,           { 0,    1,    0,   0,   0 } },
        { LR11x0::MODE_TX_HP,        { 0,    1,    0,   0,   0 } },
        { LR11x0::MODE_TX_HF,        { 0,    0,    0,   0,   0 } },
        { LR11x0::MODE_GNSS,         { 0,    0,    0,   0,   0 } },
        { LR11x0::MODE_WIFI,         { 0,    0,    0,   0,   0 } },
        { LR11x0::MODE_END_OF_TABLE, {} },
    };
    logf("[%s] installing Core1121 PE4259 RF switch table "
                  "(RX={D5=1,D6=0} TX={D5=0,D6=1})\n", _name);
    _radio->setRfSwitchTable(rfswitch_pins, rfswitch_table);

#ifdef LR1121_DEBUG
    // Check the chip's error register after setRfSwitchTable. Per UM v2.2
    // §4.2.1 SetDioAsRfSwitch returns CMD_FAIL if not in STBY_RC mode, and
    // RadioLib's setRfSwitchTable discards the return value — so read the
    // error register directly to confirm the install took.
    {
        uint16_t devErrors = 0;
        int16_t errState = static_cast<LR1121Access*>(_radio)->getErrors(&devErrors);
        logf("[%s] [diag] post-setRfSwitchTable getErrors() "
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
    // Commanded frequency = configured + the fixed sub-GHz reference correction
    // (see CORE1121_FREQ_OFFSET_HZ above). 2.4 GHz is left uncorrected.
    float cmdFreq = is2g4 ? _config.frequency
                          : _config.frequency + (CORE1121_FREQ_OFFSET_HZ / 1.0e6f);
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
            cmdFreq,
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
        logf("[%s] begin() failed: %d (is2g4=%d freq=%.3f bw=%.3f)\n",
                      _name, state, (int)is2g4,
                      _config.frequency, _config.bandwidth);
        xSemaphoreGive(_mutex);
        return false;
    }

    logf("[%s] ready — %.3f MHz  BW %.1f kHz  SF%d  CR4/%d  %d dBm  "
                  "sync 0x%02X  %s  (cfg %.3f MHz, offset %+ld Hz)\n",
                  _name,
                  cmdFreq, _config.bandwidth,
                  _config.spreadFactor, _config.codingRate,
                  _config.txPower, _config.syncWord,
                  is2g4 ? "2.4GHz" : "sub-GHz",
                  _config.frequency,
                  (long)(is2g4 ? 0 : CORE1121_FREQ_OFFSET_HZ));

#ifdef CORE1121_RX_SYNC_OVERRIDE
    // Debug: force the LoRa sync word (see the note at the top of this file).
    if (!is2g4) {
        int16_t syncSt = _radio->setSyncWord((uint8_t)CORE1121_RX_SYNC_OVERRIDE);
        logf("[%s] [debug] LoRa sync override -> 0x%02X (setSyncWord=%d)\n",
             _name, (unsigned)(uint8_t)CORE1121_RX_SYNC_OVERRIDE, (int)syncSt);
    }
#endif

#ifdef CORE1121_RSSI_CAL
    // Program the LR1121's per-band RSSI/AGC gain-tune table — the calibration
    // RadioLib NEVER sends (it only reads getRSSI()). Without it the receiver
    // selects the wrong LNA/AGC gain, which the LR1121 UM (§7.2.15) says "can
    // result in missed detection (packet loss)" — a candidate for R2 detecting
    // the preamble but failing to lock the header/data demod. Values are the
    // OEM Semtech-driver 600 MHz–2 GHz table (covers our 910 MHz), verbatim from
    // the WaveShare Core1121 demo (src/lr1121_common/lr1121_common.c:35-55):
    //   gain_offset=0, gain_tune g4..g13hp7 = {2,2,2,3,3,4,5,4,4,6,5,5,6,6,6,7,6}.
    // RadioLib's setRssiCalibration packs 18 nibbles (tune[0..17]); the 17 OEM
    // gains fill tune[0..16] and tune[17]=0 mirrors the OEM's unused high nibble.
    // Sub-GHz only — the >2 GHz table differs (offset 2030).
    if (!is2g4) {
        static const int8_t rssiTune[18] = {
            2, 2, 2, 3, 3, 4, 5, 4, 4, 6, 5, 5, 6, 6, 6, 7, 6, 0
        };
        int16_t rssiSt =
            static_cast<LR1121Access*>(_radio)->setRssiCalibration(rssiTune, 0);
        logf("[%s] [rssi-cal] OEM 600M-2G AGC table applied (setRssiCalibration=%d)\n",
             _name, (int)rssiSt);
    }
#endif

    // ----- Module identity: LR1121 unique 64-bit chip EUI ------------------
    // Print the chip EUI at boot so every bridge run self-identifies which
    // silicon produced the log — record it in the module registry (kept on the
    // lr1121-phase1 branch, docs/testbed/MODULE-REGISTRY.md).
    // This is the unambiguous way to tell the Core1121 apart from the Seeed
    // Wio-LR1121 modules in the captured logs for the #8 board-vs-chip control.
    // Read via the LR1121Access idiom because LR11x0::getChipEui() is protected
    // in RadioLib 7.7.0.
    uint8_t chipEui[8] = {0};
    int16_t euiState = static_cast<LR1121Access*>(_radio)->getChipEui(chipEui);
    if (euiState == RADIOLIB_ERR_NONE) {
        logf("[%s] chip EUI = %02X:%02X:%02X:%02X:%02X:%02X:%02X:%02X "
                      "(record this EUI to identify the module)\n",
                      _name, chipEui[0], chipEui[1], chipEui[2], chipEui[3],
                      chipEui[4], chipEui[5], chipEui[6], chipEui[7]);
    } else {
        logf("[%s] chip EUI read FAILED: %d (module unidentified)\n",
                      _name, (int)euiState);
    }

#ifdef LR1121_DEBUG
    // Dump the chip's IRQ flags right after begin() so we can see what (if
    // any) bits are pending before we ever ask it to RX. Latched bits here
    // might mask future RX_DONE events.
    uint32_t irqAfterBegin = _radio->getIrqFlags();
    logf("[%s] getIrqFlags() post-begin = 0x%08lX\n",
                  _name, (unsigned long)irqAfterBegin);
#endif

#ifdef CORE1121_TCXO_RECAL
    // ----- Warm TCXO re-config + full recalibration (sub-GHz only) ----------
    // See the CORE1121_TCXO_RECAL note at the top of this file. begin() has
    // already run RadioLib's setTCXO + calibrate(ALL) once (cold TCXO); by now
    // the 32 MHz TCXO has been powered for the rest of begin(), so re-issuing
    // setTCXO + calibrate(ALL) here runs the synthesizer/PLL/image trim against
    // a SETTLED reference. Datasheet-sanctioned remedy for HF_XOSC_START_ERR
    // with a TCXO fitted (DS §1.2.4 p.8; UM v2.2 §3.6.1 p.32, §6.3.2 p.49).
    //
    // This runs INSIDE the begin() critical section (the _mutex taken at the
    // top is still held) and BEFORE RX is armed — so it must NOT take/give the
    // mutex (it is non-recursive) and must NOT startReceive(). The only blocking
    // calls are delay()/BUSY polls, mirroring RadioLib's own config() wait; at
    // boot begin() is single-threaded (the radio tasks are spawned later), so
    // holding the mutex across these waits starves nothing.
    if (!is2g4) {
        LR1121Access *acc = static_cast<LR1121Access*>(_radio);

        _radio->standby();                                  // SetTcxoMode needs STBY_RC
        int16_t tcxoSt = _radio->setTCXO(_config.tcxoVoltage, CORE1121_TCXO_DELAY_US);
        delay(10);                                          // let the warm XOSC settle

        uint16_t errPre = 0;
        acc->getErrors(&errPre);
        if (errPre & RADIOLIB_LR11X0_ERROR_STAT_HF_XOSC_START_ERR) {
            acc->clearErrors();                             // clear the latch pre-cal
            delay(5);
        }

        int16_t calSt = acc->calibrate(RADIOLIB_LR11X0_CALIBRATE_ALL);
        delay(5);
        if (_busy >= 0) {                                   // mirror config()'s cal wait
            uint32_t t0 = millis();
            while (digitalRead(_busy) == HIGH && (millis() - t0) < 100) delay(1);
        }

        // Re-lock the synth on the fresh calibration. freqMHz already equals
        // _config.frequency, so this is a <20 MHz move and does NOT re-trigger
        // image calibration (LR1120.cpp) — it only re-sends SetRfFrequency.
        int16_t freqSt = _radio->setFrequency(cmdFreq);

        uint16_t errPost = 0;
        acc->getErrors(&errPost);
        logf("[%s] [recal] tcxoDelay=%u us  setTCXO=%d cal=%d setFreq=%d  "
             "errPre=0x%04X errPost=0x%04X%s\n",
             _name, (unsigned)CORE1121_TCXO_DELAY_US,
             (int)tcxoSt, (int)calSt, (int)freqSt,
             (unsigned)errPre, (unsigned)errPost,
             (errPost & RADIOLIB_LR11X0_ERROR_STAT_HF_XOSC_START_ERR)
                 ? "  <-- HF_XOSC_START_ERR still set" : "  (XOSC err clear)");
    }
#endif // CORE1121_TCXO_RECAL

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

bool Core1121::available()
{
    return _rxFlag;
}

int16_t Core1121::read(uint8_t *buf, size_t &len, float *rssi, float *snr)
{
    _rxFlag = false;

    xSemaphoreTake(_mutex, portMAX_DELAY);

    size_t  pktLen = _radio->getPacketLength();
    size_t  toRead = (pktLen < len) ? pktLen : len;
    int16_t state  = _radio->readData(buf, toRead);
    len = (state == RADIOLIB_ERR_NONE) ? toRead : 0;

#ifdef LR1121_DEBUG
    // Dump every read attempt (R2-deaf-at-BW62.5 investigation, see CLAUDE.md §0).
    // NOTE — there is deliberately NO freqErr here: RadioLib's
    // LR11x0::getFrequencyError() is a stub that always returns 0
    // (RadioLib LR11x0.cpp:1203 "// TODO implement this"; no LR1120/LR1121
    // override, no frequency-error register in the LR11x0 map). So, unlike R1's
    // SX1262, the LR1121 exposes NO in-firmware carrier-offset readout — a
    // logged "freqErr" would be a constant 0 and mislead. Characterize the R2
    // offset with the BW62.5 RX-frequency nudge + the post-begin getErrors()
    // read (the CORE1121_TCXO_RECAL [recal] line), not from here.
    logf("[%s] read: pktLen=%u state=%d len=%u\n",
                  _name, (unsigned)pktLen, (int)state, (unsigned)len);
#endif

    if (state == RADIOLIB_ERR_NONE) {
        if (rssi) *rssi = _radio->getRSSI();
        if (snr)  *snr  = _radio->getSNR();
    }

    // Defensive re-arm: guarantee return to continuous RX before releasing the
    // mutex. RadioLib's readData() already calls clearIrqState() internally,
    // but bench-observed LR1121 behaviour showed that without this
    // belt-and-braces sequence the chip stops receiving after one packet.
    int16_t clrSt = _radio->clearIrqFlags(RADIOLIB_LR11X0_IRQ_ALL);
    int16_t sbSt  = _radio->standby();
    int16_t rxSt  = _radio->startReceive();
#ifdef LR1121_DEBUG
    if (clrSt != RADIOLIB_ERR_NONE || sbSt != RADIOLIB_ERR_NONE ||
        rxSt != RADIOLIB_ERR_NONE) {
        logf("[%s] re-arm warn: clr=%d sb=%d rx=%d\n",
                      _name, clrSt, sbSt, rxSt);
    }
#else
    (void)clrSt; (void)sbSt; (void)rxSt;
#endif

    xSemaphoreGive(_mutex);
    return state;
}

int16_t Core1121::transmit(const uint8_t *buf, size_t len)
{
    xSemaphoreTake(_mutex, portMAX_DELAY);
    int16_t txSt = _radio->transmit(const_cast<uint8_t *>(buf), len);
    // Auto-return to RX so the bridge keeps listening without the caller
    // having to explicitly call startReceive() on the transmitting radio.
    _rxFlag = false;
    int16_t rxSt = _radio->startReceive();
#ifdef LR1121_DEBUG
    uint32_t irqAfterTx = _radio->getIrqFlags();   // raw LR11x0 IRQ register
    logf("[%s] transmit(%u B) tx=%d post-rx=%d irq=0x%08lX\n",
                  _name, (unsigned)len, (int)txSt, (int)rxSt,
                  (unsigned long)irqAfterTx);
#else
    (void)rxSt;
#endif
    xSemaphoreGive(_mutex);
    return txSt;
}

void Core1121::startReceive()
{
    _rxFlag = false;
    xSemaphoreTake(_mutex, portMAX_DELAY);
    int16_t st = _radio->startReceive();
#ifdef LR1121_DEBUG
    logf("[%s] startReceive() = %d\n", _name, (int)st);
#else
    (void)st;
#endif
    xSemaphoreGive(_mutex);
}

uint32_t Core1121::debugIrqStatus()
{
    xSemaphoreTake(_mutex, portMAX_DELAY);
    uint32_t irq = _radio->getIrqFlags();   // raw LR11x0 IRQ register
    xSemaphoreGive(_mutex);
    return irq;
}
