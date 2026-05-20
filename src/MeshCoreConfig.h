// MeshCoreConfig.h
// ---------------------------------------------------------------------------
// Runtime container for the MeshCore channel the bridge talks on. Replaces
// the previous hard-coded MESHCORE_PUBLIC_KEY / MESHCORE_PUBLIC_CHANNEL_HASH
// constants in MeshDecoderDebug.h so other channels (private, custom) can
// be selected at build time via platformio.ini, and later at runtime via
// the planned WiFi captive-portal config.
//
// Defaults reproduce the MeshCore public channel. Override with:
//   -DBRIDGE_MC_KEY_HEX="<32 lowercase hex chars>"
//   -DBRIDGE_MC_CHANNEL_NAME="<display name>"
//
// The channelHash is derived (not configured) — it's SHA-256(key)[0], the
// same single byte that arrives over the air in every MC packet. Computed
// once in begin().
//
// Thread-safety: read-mostly. begin() runs in setup(), before either radio
// task starts, so the data is stable by the time decode/encode reads it.
// ---------------------------------------------------------------------------

#pragma once

#include <stdint.h>

namespace MeshCoreConfig {

extern uint8_t      key[16];        // AES-128 key for the configured MC channel
extern uint8_t      channelHash;    // SHA-256(key)[0] — on-air channel selector
extern const char  *channelName;    // for diagnostic prints; default "public"

void begin();

}  // namespace MeshCoreConfig
