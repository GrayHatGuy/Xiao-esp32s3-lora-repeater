// CamPortal.h
// ---------------------------------------------------------------------------
// LoRaCam Phase 3 — the always-on web portal on the camera node. A WPA2 SoftAP
// plus a login-gated web UI for: live MJPEG video, the full radio/identity config
// (reused from CaptivePortal, one source of truth), camera command (snap / record
// / stop / status), operator messaging over the C2 channel, and pairing
// (whitelist peer provisioning) + WiFi/login credential management.
//
// Two HTTP servers: a synchronous WebServer on :80 (pumped from loop() — the
// camera's loop() is otherwise idle) for the UI, and an esp_http_server on :81
// dedicated to the MJPEG stream so streaming never blocks UI access. Both run
// alongside the bridge's radio tasks; neither touches the SPI bus or spiMutex.
//
// Local-trust model: the portal calls CameraNode/CamC2 actuation DIRECTLY, gated
// by the portal login (+ WPA2) rather than the LoRa-path crypto self-test latch —
// the camera hardware works regardless of C2 crypto state (see LORACAM-SPEC §Phase3).
//
// Self-gated BRIDGE_ROLE_CAMERA — absent from stock / commander builds (do-no-harm).
// ---------------------------------------------------------------------------

#pragma once
#if defined(BRIDGE_ROLE_CAMERA)

namespace CamPortal {

void begin();    // start the AP + both HTTP servers (call once, after radios are up)
void handle();   // pump the :80 server + DNS — call from loop()

// Validate a session token (hex) — used by the :81 stream server (a separate
// translation unit, because esp_http_server.h and WebServer.h both define HTTP_GET
// and cannot share a TU) to gate the MJPEG stream behind the portal login.
bool sessionValid(const char *hexTok);

}  // namespace CamPortal

#endif  // BRIDGE_ROLE_CAMERA
