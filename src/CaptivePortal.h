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
// or (b) within a short post-boot window, the BOOT button being pressed OR
// any character arriving on the serial monitor (the serial route covers
// hardware stacks where the BOOT button is hidden under the radio shield).
// ---------------------------------------------------------------------------

#pragma once

#if defined(BRIDGE_ROLE_CAMERA)
#include <WebServer.h>
#endif

namespace CaptivePortal {

// Block-and-serve. Does not return — exits via ESP.restart() once the user
// saves the form.
void begin();

#if defined(BRIDGE_ROLE_CAMERA)
// Phase 3 reuse: the always-on portal (CamPortal) serves the SAME config form
// without the blocking first-flash flow. serverRef() is the file-static :80
// WebServer instance — CamPortal makes it the active server, registers its own
// routes on it, and pumps handleClient(). serveConfigForm()/serveConfigSave()
// render + parse the existing form (one source of truth); CamPortal registers
// them behind the portal login. A save still ESP.restart()s (radios init once at
// boot), which is correct. These are camera-only additions — stock builds compile
// this file identically (do-no-harm).
WebServer &serverRef();
void serveConfigForm();   // GET: render + send the config form
void serveConfigSave();   // POST: validate, persist, reboot (== the captive save)
#endif

}  // namespace CaptivePortal
