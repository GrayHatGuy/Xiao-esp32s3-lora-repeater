// CamPortalConfig.h
// ---------------------------------------------------------------------------
// LoRaCam Phase 3 — credentials for the always-on web portal: the WPA2 AP
// passphrase and the portal login (username + salted SHA-256 password hash).
//
// Mirrors the LoRaWANConfig / CamC2Config pattern: a small blob in its OWN NVS
// namespace ("camportal"), so there is NO BridgeConfig schema bump and the stock
// bridge is unaffected. Self-gated BRIDGE_ROLE_CAMERA (the portal is camera-only).
//
// First boot (nothing provisioned) seeds defaults from build flags
// (BRIDGE_PORTAL_USER / _PASS / _AP_PASS) if present, else hard defaults — and
// logs a loud warning to change them via the portal's "Portal & WiFi security"
// section. The AP passphrase is stored in the clear (the SoftAP needs it to come
// up); the login password is never stored in the clear (salt + SHA-256 only).
// ---------------------------------------------------------------------------

#pragma once
#if defined(BRIDGE_ROLE_CAMERA)

#include <Arduino.h>
#include <stdint.h>

namespace CamPortalConfig {

constexpr size_t USER_MAX    = 23;   // login username (chars, + NUL)
constexpr size_t AP_PASS_MAX = 63;   // WPA2 passphrase (8..63 chars per 802.11)
constexpr size_t AP_PASS_MIN = 8;

void begin();          // load (or first-boot seed) from NVS "camportal"

// True once the operator has saved non-default credentials via the portal.
bool isProvisioned();
// True while the live credentials are still the build-flag/hard defaults
// (drives the portal's "change your password" warning banner + a boot log line).
bool usingDefaults();

// --- AP ---
const char *apPass();                // WPA2 passphrase used to start the SoftAP

// --- login ---
const char *user();                  // current login username
// Constant-time check of a submitted username+password against the stored hash.
bool        checkLogin(const char *u, const char *p);

// --- setters (portal "Portal & WiFi security" form) ---
// Returns nullptr on success (and persists), else an error string to flash back.
const char *setApPass(const char *pass);
const char *setLogin(const char *u, const char *pass);

}  // namespace CamPortalConfig

#endif  // BRIDGE_ROLE_CAMERA
