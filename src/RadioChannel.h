// RadioChannel.h
// ---------------------------------------------------------------------------
// Per-radio channel context. Two of these (one per radio slot) replace the
// old MeshCoreConfig / MeshtasticConfig singletons, so each radio can run a
// different channel — including the SAME protocol on both slots (MC↔MC or
// MT↔MT relay between two channels: private↔private, private↔public).
//
// Resolved once at boot by MeshCoreConfig::resolve() / MeshtasticConfig::
// resolve() from each radio's BridgeConfig channel strings + its build-flag
// protocol. Decoders and encoders take a `const RadioChannel&` argument.
// ---------------------------------------------------------------------------

#pragma once

#include <stdint.h>

struct RadioChannel {
    uint8_t protocol;       // LoRa sync word: 0x12 MeshCore, 0x2B Meshtastic, 0x42 Reticulum
    uint8_t key[32];        // AES key — MeshCore + AES-128 Meshtastic use [0..15]
    uint8_t keyLen;         // 16 (AES-128) or 32 (AES-256); 0 for Reticulum (no channel key)
    uint8_t channelHash;    // on-air channel selector byte
    char    name[24];       // channel display name (null-terminated)
};
