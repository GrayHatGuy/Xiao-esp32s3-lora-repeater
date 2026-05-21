// RegionPreset.h
// ---------------------------------------------------------------------------
// Region tables + Meshtastic Tier 2 channel-slot frequency computation for the
// v8 vanilla-firmware feature. Header-only.
//
// The region table, the djb2 channel-name hash, the slot-count / slot-centre
// formula, and the modem-preset BW/SF/CR bundle are all transcribed verbatim
// from meshtastic/firmware (src/mesh/RadioInterface.cpp + src/mesh/MeshRadio.h,
// master branch, verified 2026-05-21):
//
//   numChannels  = floor((freqEnd - freqStart) / (spacing + bw/1000))
//   channel_num  = djb2(channelName) % numChannels
//   freq         = freqStart + (bw/2000) + (channel_num * (bw/1000))
//
// spacing is 0 for every region we ship, so it is dropped here. bw is in kHz,
// frequencies in MHz.
//
// MeshCore / Reticulum / Custom radios are not slot-computed — they get a flat
// band-centre default (Tier 1); the portal frequency field stays editable.
// ---------------------------------------------------------------------------

#pragma once

#include <Arduino.h>
#include <stdint.h>
#include "BridgeConfig.h"   // BridgeConfig::Region enum

namespace RegionPreset {

// One row of the region table. powerLimit is the regulatory TX cap (dBm).
struct RegionInfo {
    const char *name;
    float       freqStart;   // MHz
    float       freqEnd;     // MHz
    int8_t      powerLimit;  // dBm
    uint8_t     dutyCyclePct;
};

// Indexed by BridgeConfig::Region. UNSET / CUSTOM carry zeroed RF bounds.
static const RegionInfo kRegions[] = {
    /* REGION_UNSET  */ { "UNSET",  0.0f,   0.0f,    0, 0   },
    /* REGION_US     */ { "US",     902.0f, 928.0f, 30, 100 },
    /* REGION_EU_868 */ { "EU_868", 869.4f, 869.65f,27, 10  },
    /* REGION_EU_433 */ { "EU_433", 433.0f, 434.0f, 10, 10  },
    /* REGION_ANZ    */ { "ANZ",    915.0f, 928.0f, 30, 100 },
    /* REGION_CN     */ { "CN",     470.0f, 510.0f, 19, 100 },
    /* REGION_JP     */ { "JP",     920.5f, 923.5f, 13, 100 },
    /* REGION_IN     */ { "IN",     865.0f, 867.0f, 30, 100 },
    /* REGION_KR     */ { "KR",     920.0f, 923.0f, 23, 100 },
    /* REGION_RU     */ { "RU",     868.7f, 869.2f, 20, 100 },
    /* REGION_CUSTOM */ { "Custom", 0.0f,   0.0f,    0, 0   },
};
static constexpr uint8_t kRegionCount =
    sizeof(kRegions) / sizeof(kRegions[0]);

// Meshtastic modem presets. Our portal exposes these for Meshtastic radios;
// the BW/SF/CR bundle resolves into BridgeConfig's per-radio RF fields.
enum ModemPreset : uint8_t {
    PRESET_SHORT_TURBO   = 0,
    PRESET_SHORT_FAST    = 1,
    PRESET_SHORT_SLOW    = 2,
    PRESET_MEDIUM_FAST   = 3,
    PRESET_MEDIUM_SLOW   = 4,
    PRESET_LONG_FAST     = 5,   // Meshtastic default
    PRESET_LONG_MODERATE = 6,
    PRESET_LONG_SLOW     = 7,
    PRESET_LONG_TURBO    = 8,
};

// --- Lookups ---------------------------------------------------------------

static inline const RegionInfo &regionInfo(uint8_t region) {
    if (region >= kRegionCount) region = BridgeConfig::REGION_UNSET;
    return kRegions[region];
}

// True when the region has a usable sub-GHz band (i.e. not UNSET/CUSTOM).
static inline bool regionHasBand(uint8_t region) {
    const RegionInfo &r = regionInfo(region);
    return r.freqEnd > r.freqStart;
}

// Resolve a modem preset to its non-wideLora BW(kHz)/SF/CR bundle. Verbatim
// from meshtastic/firmware MeshRadio.h modemPresetToParams().
static inline void modemPresetParams(uint8_t preset, float &bwKHz,
                                     uint8_t &sf, uint8_t &cr) {
    switch (preset) {
        case PRESET_SHORT_TURBO:   bwKHz = 500.0f; sf = 7;  cr = 5; break;
        case PRESET_SHORT_FAST:    bwKHz = 250.0f; sf = 7;  cr = 5; break;
        case PRESET_SHORT_SLOW:    bwKHz = 250.0f; sf = 8;  cr = 5; break;
        case PRESET_MEDIUM_FAST:   bwKHz = 250.0f; sf = 9;  cr = 5; break;
        case PRESET_MEDIUM_SLOW:   bwKHz = 250.0f; sf = 10; cr = 5; break;
        case PRESET_LONG_MODERATE: bwKHz = 125.0f; sf = 11; cr = 8; break;
        case PRESET_LONG_SLOW:     bwKHz = 125.0f; sf = 12; cr = 8; break;
        case PRESET_LONG_TURBO:    bwKHz = 500.0f; sf = 11; cr = 8; break;
        case PRESET_LONG_FAST:
        default:                   bwKHz = 250.0f; sf = 11; cr = 5; break;
    }
}

static inline const char *modemPresetName(uint8_t preset) {
    switch (preset) {
        case PRESET_SHORT_TURBO:   return "ShortTurbo";
        case PRESET_SHORT_FAST:    return "ShortFast";
        case PRESET_SHORT_SLOW:    return "ShortSlow";
        case PRESET_MEDIUM_FAST:   return "MediumFast";
        case PRESET_MEDIUM_SLOW:   return "MediumSlow";
        case PRESET_LONG_MODERATE: return "LongModerate";
        case PRESET_LONG_SLOW:     return "LongSlow";
        case PRESET_LONG_TURBO:    return "LongTurbo";
        case PRESET_LONG_FAST:
        default:                   return "LongFast";
    }
}

// --- Tier 2 channel-slot frequency -----------------------------------------

// djb2 string hash (Dan Bernstein) — the hash Meshtastic uses to pick a slot.
static inline uint32_t djb2(const char *s) {
    uint32_t h = 5381;
    int c;
    while (s && (c = (unsigned char)*s++) != 0)
        h = ((h << 5) + h) + (uint32_t)c;     // h * 33 + c
    return h;
}

// Compute the Meshtastic channel-slot centre frequency (MHz) for a given
// region + channel name + bandwidth. Returns 0.0f if the region has no band
// or the band is too narrow for one slot of this bandwidth.
static inline float slotFrequencyMHz(uint8_t region, const char *channelName,
                                     float bwKHz) {
    const RegionInfo &r = regionInfo(region);
    if (r.freqEnd <= r.freqStart || bwKHz <= 0.0f) return 0.0f;

    float bwMHz = bwKHz / 1000.0f;
    uint32_t numChannels = (uint32_t)floorf((r.freqEnd - r.freqStart) / bwMHz);
    if (numChannels == 0) return 0.0f;

    uint32_t channelNum = djb2(channelName) % numChannels;
    return r.freqStart + (bwMHz / 2.0f) + ((float)channelNum * bwMHz);
}

// Flat band-centre default (Tier 1) — used for MeshCore / Reticulum / Custom
// radios, which are not slot-computed. Returns 0.0f for a band-less region.
static inline float bandCenterMHz(uint8_t region) {
    const RegionInfo &r = regionInfo(region);
    if (r.freqEnd <= r.freqStart) return 0.0f;
    return r.freqStart + (r.freqEnd - r.freqStart) / 2.0f;
}

// Convenience: full RF resolution for one radio.
//   protocol == PROTO_MT  -> Tier 2 slot frequency from modem preset + name
//   otherwise             -> Tier 1 band-centre default, caller-supplied RF
// On success fills freqMHz/bwKHz/sf/cr and returns true. The caller (portal)
// keeps the frequency field editable so the user can override.
static inline bool resolveMeshtasticRf(uint8_t region, const char *channelName,
                                       uint8_t modemPreset,
                                       float &freqMHz, float &bwKHz,
                                       uint8_t &sf, uint8_t &cr) {
    modemPresetParams(modemPreset, bwKHz, sf, cr);
    freqMHz = slotFrequencyMHz(region, channelName, bwKHz);
    return freqMHz > 0.0f;
}

}  // namespace RegionPreset
