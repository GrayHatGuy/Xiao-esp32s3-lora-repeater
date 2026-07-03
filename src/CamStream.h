// CamStream.h
// ---------------------------------------------------------------------------
// LoRaCam Phase 3 — the :81 MJPEG stream server (esp_http_server). Lives in its
// OWN translation unit because esp_http_server.h and WebServer.h both define an
// HTTP_GET enumerator and cannot be included together. The stream + single-frame
// handlers are gated behind the portal login via CamPortal::sessionValid().
// Self-gated BRIDGE_ROLE_CAMERA.
// ---------------------------------------------------------------------------

#pragma once
#if defined(BRIDGE_ROLE_CAMERA)

namespace CamStream {

void start();   // httpd_start on :81 + register /stream and /jpg

}  // namespace CamStream

#endif  // BRIDGE_ROLE_CAMERA
