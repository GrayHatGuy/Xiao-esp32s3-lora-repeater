// CaptivePortal.h
// ---------------------------------------------------------------------------
// First-flash / on-demand WiFi configuration portal for the bridge.
//
// When begin() is called the device:
//   1. Starts an open WiFi SoftAP named "LoRa-Bridge-<last hex byte of node ID>".
//   2. Brings up a DNSServer that resolves every name to the AP's IP, so any
//      HTTP request from a connected phone/laptop lands on the form.
//   3. Brings up a WebServer on :80 serving a single HTML config page
//      pre-filled from BridgeConfig.
//   4. Blocks forever, pumping handleClient() + processNextRequest(), until
//      the user submits the form. The submit handler writes the new values
//      into BridgeConfig, calls save(), prints a confirmation, and then
//      ESP.restart()s.
//
// Trigger is the caller's responsibility — main.cpp decides whether to
// invoke begin() based on (a) BridgeConfig::isConfigured() returning false,
// or (b) the BOOT button being pressed within the first ~3 s after boot.
// ---------------------------------------------------------------------------

#pragma once

namespace CaptivePortal {

// Block-and-serve. Does not return — exits via ESP.restart() once the user
// saves the form.
void begin();

}  // namespace CaptivePortal
