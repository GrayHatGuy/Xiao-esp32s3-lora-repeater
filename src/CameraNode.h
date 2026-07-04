// CameraNode.h
// ---------------------------------------------------------------------------
// LoRaCam Phase 2: the OV2640 camera on the XIAO ESP32-S3 Sense daughterboard.
// Gated BRIDGE_ROLE_CAMERA so it is absent from the commander / stock builds.
//
// The camera lives on the rear B2B 40-pin connector (GPIO10-18,38-40,47,48) —
// disjoint from the edge radio (R2) pins, so it coexists with the LoRa radio.
// The example's LED-flash pin (GPIO21) is the microSD chip-select and is NEVER
// driven here. begin() degrades gracefully if no daughterboard is attached
// (ready()==false), and executeCommand returns RES_NOCAM.
//
// Phase 2a (this file): snap a single JPEG into PSRAM and report its size/dims.
// Phase 2b (next): persist to microSD on the shared SPI bus (spiMutex) + return
// the real filename; N-second clip recording.
// ---------------------------------------------------------------------------

#pragma once
#if defined(BRIDGE_ROLE_CAMERA)

#include <Arduino.h>
#include <stdint.h>

namespace CameraNode {

void   begin();        // esp_camera_init for the XIAO S3 Sense; sets ready()
bool   ready();        // true if the OV2640 initialised OK

// Phase 2b: mount the microSD (CS=GPIO21) on the SHARED SPI bus. Every SD
// operation holds busMutex (the radios' spiMutex) — the card and the SX1262
// coexist on SCK/MISO/MOSI. Call from setup() AFTER SPI.begin() + the mutex
// exist. No card / mount failure degrades gracefully (snaps stay in PSRAM).
void    beginStorage(SemaphoreHandle_t busMutex);
bool    sdPresent();   // true if a card is mounted
uint8_t sdFreePct();   // cached free % (0-100), or 0xFF = no card (status sentinel)

// Phase 2b — portal Photos tab. Photos are addressed by their NUMBER (the %05u in
// /loracam/IMG_%05u.jpg), never by a caller-supplied path, so the web layer cannot
// be steered outside /loracam. All three hold the bus mutex internally.
struct PhotoInfo { uint32_t idx; uint32_t bytes; };
size_t photoList(PhotoInfo *out, size_t cap);      // newest-first; returns count
// Read photo #idx into a heap/PSRAM buffer (caller free()s *bufOut). 0 = not found.
size_t photoRead(uint32_t idx, uint8_t **bufOut);
bool   photoDelete(uint32_t idx);

// Capture one JPEG. Returns the byte length (0 on failure). Fills nameOut with a
// filename (a real SD path once Phase 2b lands, else a synthetic descriptor) and
// the frame dimensions.
size_t snap(char *nameOut, size_t nameCap, uint16_t *wOut, uint16_t *hOut);

// Phase 3: serialize esp_camera access between a C2 snap() and the portal's MJPEG
// stream (both call esp_camera_fb_get/return on the same sensor). snap() takes the
// lock internally; the stream handler brackets its own fb_get/return with these.
// lockCamera() returns false if the lock can't be taken within ms (caller skips).
bool lockCamera(uint32_t ms);
void unlockCamera();

}  // namespace CameraNode

#endif  // BRIDGE_ROLE_CAMERA
