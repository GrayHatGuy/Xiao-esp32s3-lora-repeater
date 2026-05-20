// MeshCoreConfig.h
// ---------------------------------------------------------------------------
// MeshCore channel resolver. As of v2 this is a stateless helper, not a
// singleton — resolve() fills a caller-owned RadioChannel from a 32-char
// hex key + channel name, so each radio slot can carry its own MC channel.
//
// The channelHash is derived (not configured): SHA-256(key)[0], the single
// byte that arrives over the air in every MeshCore packet.
// ---------------------------------------------------------------------------

#pragma once

#include "RadioChannel.h"

namespace MeshCoreConfig {

// Resolve a MeshCore channel into `out`:
//   - parse keyHex (exactly 32 lowercase/uppercase hex chars) into out.key,
//     falling back to the public-channel key on any malformed input,
//   - out.keyLen = 16, out.protocol = 0x12 (SYNC_WORD_MESHCORE),
//   - out.channelHash = SHA-256(key)[0],
//   - out.name = name (truncated to fit).
void resolve(const char *keyHex, const char *name, RadioChannel &out);

}  // namespace MeshCoreConfig
