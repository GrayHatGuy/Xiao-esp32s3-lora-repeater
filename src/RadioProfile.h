// RadioProfile.h
// ---------------------------------------------------------------------------
// Build-profile selection for the radio chips (the three profiles are below).
//
//   (default) MIXED            Radio 1 = SX1262, Radio 2 = SX1262 or LR1121.
//                              Radio 2 chip is portal-selectable; both
//                              drivers are linked.
//   RADIO_PROFILE_DUAL_SX1262  Both radios SX1262 — compile-time locked.
//   RADIO_PROFILE_DUAL_LR1121  Both radios LR1121 — compile-time locked.
//
// Set the profile from platformio.ini, e.g.:
//   build_flags = -DRADIO_PROFILE_DUAL_LR1121
// Define at most one; absence of both = MIXED.
// ---------------------------------------------------------------------------

#pragma once

#if defined(RADIO_PROFILE_DUAL_SX1262) && defined(RADIO_PROFILE_DUAL_LR1121)
  #error "Define at most one RADIO_PROFILE_* build flag"
#endif

#if !defined(RADIO_PROFILE_DUAL_SX1262) && !defined(RADIO_PROFILE_DUAL_LR1121)
  #define RADIO_PROFILE_MIXED 1
#endif

// Chip codes — must match BridgeConfig::Chip.
#define RADIO_CHIP_SX1262 0
#define RADIO_CHIP_LR1121 1

// Radio 2 default chip in the MIXED profile (first-boot default; the portal
// can override it). Ignored by the DUAL_* profiles.
#ifndef RADIO2_CHIP
  #define RADIO2_CHIP RADIO_CHIP_SX1262
#endif
