// MeshtasticConfig.h
// ---------------------------------------------------------------------------
// Runtime container for the Meshtastic channel the bridge talks on. The
// symmetric counterpart to MeshCoreConfig — replaces the hard-coded
// MESHTASTIC_DEFAULT_KEY / MESHTASTIC_LONGFAST_CHANNEL_HASH constants that
// used to pin the bridge to the public LongFast channel.
//
// Defaults reproduce LongFast. Override via BridgeConfig (which itself
// defaults to the BRIDGE_MT_CHANNEL_NAME / BRIDGE_MT_PSK_B64 build flags,
// and can be set at runtime through the captive portal).
//
// PSK handling — Meshtastic channel keys arrive as a base64 string that
// decodes to:
//   - 0 bytes  : default/primary channel  -> defaultpsk, AES-128
//   - 1 byte   : short key index N        -> defaultpsk with last byte
//                                            bumped by (N-1), AES-128
//   - 16 bytes : full key                 -> used verbatim, AES-128
//   - 32 bytes : full key                 -> used verbatim, AES-256
// begin() runs that expansion and computes the on-air channel-hash byte.
//
// Thread-safety: read-mostly. begin() runs in setup() before the radio
// tasks start, so key/keyLen/channelHash are stable by the time any
// decode/encode reads them.
// ---------------------------------------------------------------------------

#pragma once

#include <stdint.h>

namespace MeshtasticConfig {

extern uint8_t      key[32];        // expanded AES key; first keyLen bytes valid
extern uint8_t      keyLen;         // 16 (AES-128) or 32 (AES-256)
extern uint8_t      channelHash;    // XOR-fold(name) ^ XOR-fold(key) — on-air selector
extern const char  *channelName;    // for diagnostics; default "LongFast"

void begin();

}  // namespace MeshtasticConfig
