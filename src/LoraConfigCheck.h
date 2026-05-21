// LoraConfigCheck.h
// ---------------------------------------------------------------------------
// Compile-time validation of the optional LORA_RADIO* build-flag defaults.
//
// As of v8.0 the LORA_RADIO*_* flags in platformio.ini only seed BridgeConfig's
// first-boot defaults — the captive portal owns the live config. But a garbage
// default flag would still ship a bad first-boot config, so each flag that IS
// defined is range-checked here at build time. A bad flag fails `pio run` with
// a clear message instead of producing misbehaving firmware.
//
// This complements the portal-side runtime validation in CaptivePortal.cpp's
// handleSave(): that checks values typed into the form; this checks the
// compile-time defaults. The two are not redundant.
//
// Each guard is wrapped in #ifdef, so a flag that is simply not set (the
// vanilla-bin case) is skipped — not an error.
//
// Include once from a translation unit (main.cpp).
// ---------------------------------------------------------------------------

#pragma once

// Valid SX1262 LoRa bandwidths (kHz). Used by the static_assert chains below.
#define LORA_CHK_VALID_BW(x) \
    ((x) == 7.8f  || (x) == 10.4f || (x) == 15.6f || (x) == 20.8f || \
     (x) == 31.25f|| (x) == 41.7f || (x) == 62.5f || (x) == 125.0f|| \
     (x) == 250.0f|| (x) == 500.0f)

// --- Radio 1 ---------------------------------------------------------------

#ifdef LORA_RADIO1_FREQUENCY
static_assert((LORA_RADIO1_FREQUENCY) >= 150.0f && (LORA_RADIO1_FREQUENCY) <= 960.0f,
  "LORA_RADIO1_FREQUENCY must be within the SX1262 tuning range (150-960 MHz)");
#endif

#ifdef LORA_RADIO1_BANDWIDTH
static_assert(LORA_CHK_VALID_BW(LORA_RADIO1_BANDWIDTH),
  "LORA_RADIO1_BANDWIDTH must be a valid SX1262 bandwidth in kHz "
  "(7.8/10.4/15.6/20.8/31.25/41.7/62.5/125.0/250.0/500.0)");
#endif

#ifdef LORA_RADIO1_SPREAD_FACTOR
static_assert((LORA_RADIO1_SPREAD_FACTOR) >= 5 && (LORA_RADIO1_SPREAD_FACTOR) <= 12,
  "LORA_RADIO1_SPREAD_FACTOR must be 5-12");
#endif

#ifdef LORA_RADIO1_CODING_RATE
static_assert((LORA_RADIO1_CODING_RATE) >= 5 && (LORA_RADIO1_CODING_RATE) <= 8,
  "LORA_RADIO1_CODING_RATE must be 5-8 (denominator of 4/5..4/8)");
#endif

#ifdef LORA_RADIO1_SYNC_WORD
static_assert((LORA_RADIO1_SYNC_WORD) >= 0x00 && (LORA_RADIO1_SYNC_WORD) <= 0xFF,
  "LORA_RADIO1_SYNC_WORD must be a single byte (0x00-0xFF)");
#endif

#ifdef LORA_RADIO1_TX_POWER
static_assert((LORA_RADIO1_TX_POWER) >= -9 && (LORA_RADIO1_TX_POWER) <= 22,
  "LORA_RADIO1_TX_POWER must be -9..22 dBm (SX1262 PA range)");
#endif

// --- Radio 2 ---------------------------------------------------------------

#ifdef LORA_RADIO2_FREQUENCY
static_assert((LORA_RADIO2_FREQUENCY) >= 150.0f && (LORA_RADIO2_FREQUENCY) <= 960.0f,
  "LORA_RADIO2_FREQUENCY must be within the SX1262 tuning range (150-960 MHz)");
#endif

#ifdef LORA_RADIO2_BANDWIDTH
static_assert(LORA_CHK_VALID_BW(LORA_RADIO2_BANDWIDTH),
  "LORA_RADIO2_BANDWIDTH must be a valid SX1262 bandwidth in kHz "
  "(7.8/10.4/15.6/20.8/31.25/41.7/62.5/125.0/250.0/500.0)");
#endif

#ifdef LORA_RADIO2_SPREAD_FACTOR
static_assert((LORA_RADIO2_SPREAD_FACTOR) >= 5 && (LORA_RADIO2_SPREAD_FACTOR) <= 12,
  "LORA_RADIO2_SPREAD_FACTOR must be 5-12");
#endif

#ifdef LORA_RADIO2_CODING_RATE
static_assert((LORA_RADIO2_CODING_RATE) >= 5 && (LORA_RADIO2_CODING_RATE) <= 8,
  "LORA_RADIO2_CODING_RATE must be 5-8 (denominator of 4/5..4/8)");
#endif

#ifdef LORA_RADIO2_SYNC_WORD
static_assert((LORA_RADIO2_SYNC_WORD) >= 0x00 && (LORA_RADIO2_SYNC_WORD) <= 0xFF,
  "LORA_RADIO2_SYNC_WORD must be a single byte (0x00-0xFF)");
#endif

#ifdef LORA_RADIO2_TX_POWER
static_assert((LORA_RADIO2_TX_POWER) >= -9 && (LORA_RADIO2_TX_POWER) <= 22,
  "LORA_RADIO2_TX_POWER must be -9..22 dBm (SX1262 PA range)");
#endif
