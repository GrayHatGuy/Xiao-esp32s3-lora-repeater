// MeshtasticConfig.h
// ---------------------------------------------------------------------------
// Meshtastic channel resolver. As of v2 this is a stateless helper, not a
// singleton — resolve() fills a caller-owned RadioChannel from a base64 PSK
// + channel name, so each radio slot can carry its own MT channel.
//
// PSK handling — the base64 PSK decodes to:
//   - 0 bytes  : default/primary channel  -> defaultpsk, AES-128
//   - 1 byte   : short key index N        -> defaultpsk, last byte +(N-1)
//   - 16 bytes : full key                 -> verbatim, AES-128
//   - 32 bytes : full key                 -> verbatim, AES-256
// channelHash = XOR-fold(name) ^ XOR-fold(expanded key).
// ---------------------------------------------------------------------------

#pragma once

#include "RadioChannel.h"

namespace MeshtasticConfig {

// Resolve a Meshtastic channel into `out`:
//   - base64-decode pskB64 and run the expansion above into out.key,
//   - out.keyLen = 16 or 32, out.protocol = 0x2B (SYNC_WORD_MESHTASTIC),
//   - out.channelHash = XOR-fold(name) ^ XOR-fold(key),
//   - out.name = name (truncated to fit).
// Falls back to the LongFast default channel on any malformed PSK.
void resolve(const char *pskB64, const char *name, RadioChannel &out);

}  // namespace MeshtasticConfig
