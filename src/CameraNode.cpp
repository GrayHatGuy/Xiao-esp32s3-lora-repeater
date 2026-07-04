// CameraNode.cpp — see CameraNode.h. OV2640 pin map from the Seeed XIAO ESP32-S3
// Sense CameraWebServer example (CAMERA_MODEL_XIAO_ESP32S3).

#include "CameraNode.h"
#if defined(BRIDGE_ROLE_CAMERA)

#include "esp_camera.h"
#include "SerialLog.h"
#include <SPI.h>
#include <FS.h>
#include <SD.h>
#include <stdio.h>
#include <string.h>

namespace CameraNode {

static bool s_ok = false;
static SemaphoreHandle_t s_camMutex = nullptr;   // serialize snap() vs MJPEG stream

// --- Phase 2b: microSD on the shared SPI bus --------------------------------
static const int      SD_CS = 21;               // Sense microSD chip-select (GPIO21)
static SemaphoreHandle_t s_busMutex = nullptr;  // the radios' spiMutex (shared bus)
static bool     s_sdOk      = false;
static uint32_t s_nextIdx   = 1;                // next /loracam/IMG_%05u.jpg
static uint8_t  s_sdFreePct = 0xFF;             // 0xFF sentinel = no card

static bool busLock(uint32_t ms) {
    return s_busMutex && xSemaphoreTake(s_busMutex, pdMS_TO_TICKS(ms)) == pdTRUE;
}
static void busUnlock() { xSemaphoreGive(s_busMutex); }

// Recompute the cached free-space percentage. Call with the bus held.
static void refreshFreePct() {
    uint64_t tot = SD.totalBytes();
    s_sdFreePct = tot ? (uint8_t)(((tot - SD.usedBytes()) * 100ULL) / tot) : 0;
}

void beginStorage(SemaphoreHandle_t busMutex) {
    s_busMutex = busMutex;
    if (!busLock(3000)) {
        SerialLog::logf("[CameraNode] SD: bus busy at mount — no card mounted\n");
        return;
    }
    // 4 MHz probe keeps the shared bus friendly; FAT/FAT32 only (exFAT fails here).
    if (!SD.begin(SD_CS, SPI, 4000000)) {
        busUnlock();
        SerialLog::logf("[CameraNode] no microSD (mount failed) — snaps stay in PSRAM\n");
        return;
    }
    if (!SD.exists("/loracam")) SD.mkdir("/loracam");

    // Resume numbering across reboots: highest existing IMG_<n> + 1.
    File dir = SD.open("/loracam");
    if (dir && dir.isDirectory()) {
        File e;
        while ((e = dir.openNextFile())) {
            const char *nm = e.name();
            const char *base = strrchr(nm, '/');
            base = base ? base + 1 : nm;
            unsigned idx;
            if (sscanf(base, "IMG_%u", &idx) == 1 && idx >= s_nextIdx)
                s_nextIdx = idx + 1;
            e.close();
        }
        dir.close();
    }
    refreshFreePct();
    uint64_t mb = SD.totalBytes() / (1024ULL * 1024ULL);   // read while bus is held
    s_sdOk = true;
    busUnlock();
    SerialLog::logf("[CameraNode] microSD mounted: %llu MB, %u%% free, next=IMG_%05u.jpg\n",
                    (unsigned long long)mb, (unsigned)s_sdFreePct, (unsigned)s_nextIdx);
}

bool    sdPresent() { return s_sdOk; }
uint8_t sdFreePct() { return s_sdFreePct; }

// --- Photos tab helpers (see CameraNode.h) -----------------------------------

static void photoPath(uint32_t idx, char *out, size_t cap) {
    snprintf(out, cap, "/loracam/IMG_%05u.jpg", (unsigned)idx);
}

size_t photoList(PhotoInfo *out, size_t cap) {
    if (!s_sdOk || !out || cap == 0 || !busLock(2000)) return 0;
    size_t n = 0;
    File dir = SD.open("/loracam");
    if (dir && dir.isDirectory()) {
        File e;
        while ((e = dir.openNextFile())) {
            const char *nm = e.name();
            const char *base = strrchr(nm, '/');
            base = base ? base + 1 : nm;
            unsigned idx;
            if (sscanf(base, "IMG_%u", &idx) == 1) {
                PhotoInfo pi = { (uint32_t)idx, (uint32_t)e.size() };
                if (n < cap) {
                    out[n++] = pi;
                } else {
                    // Full: replace the smallest index so the newest `cap` survive.
                    size_t mn = 0;
                    for (size_t i = 1; i < n; i++)
                        if (out[i].idx < out[mn].idx) mn = i;
                    if (pi.idx > out[mn].idx) out[mn] = pi;
                }
            }
            e.close();
        }
        dir.close();
    }
    busUnlock();
    // Newest first (insertion sort — n is small, <= cap).
    for (size_t i = 1; i < n; i++) {
        PhotoInfo k = out[i];
        size_t j = i;
        while (j > 0 && out[j - 1].idx < k.idx) { out[j] = out[j - 1]; j--; }
        out[j] = k;
    }
    return n;
}

size_t photoRead(uint32_t idx, uint8_t **bufOut) {
    if (bufOut) *bufOut = nullptr;
    if (!s_sdOk || !bufOut || !busLock(2000)) return 0;
    char path[32];
    photoPath(idx, path, sizeof(path));
    File f = SD.open(path, FILE_READ);
    size_t len = 0;
    if (f) {
        size_t sz = f.size();
        // A JPEG from this sensor is tens of KB; 2 MB is a sanity ceiling, not a
        // target. Buffer from PSRAM when present so big reads never squeeze heap.
        if (sz > 0 && sz <= 2 * 1024 * 1024) {
            uint8_t *buf = (uint8_t *)(psramFound() ? ps_malloc(sz) : malloc(sz));
            if (buf && f.read(buf, sz) == sz) {
                *bufOut = buf;
                len = sz;
            } else if (buf) {
                free(buf);
            }
        }
        f.close();
    }
    busUnlock();
    return len;
}

bool photoDelete(uint32_t idx) {
    if (!s_sdOk || !busLock(2000)) return false;
    char path[32];
    photoPath(idx, path, sizeof(path));
    bool ok = SD.remove(path);
    if (ok) refreshFreePct();
    busUnlock();
    if (ok) SerialLog::logf("[CameraNode] deleted %s\n", path);
    return ok;
}

bool lockCamera(uint32_t ms) {
    if (!s_camMutex) return true;                // not yet created (single-threaded init)
    return xSemaphoreTake(s_camMutex, pdMS_TO_TICKS(ms)) == pdTRUE;
}

void unlockCamera() {
    if (s_camMutex) xSemaphoreGive(s_camMutex);
}

void begin() {
    camera_config_t c = {};
    c.ledc_channel = LEDC_CHANNEL_0;
    c.ledc_timer   = LEDC_TIMER_0;
    // OV2640 DVP data + control pins (XIAO ESP32-S3 Sense).
    c.pin_d0 = 15; c.pin_d1 = 17; c.pin_d2 = 18; c.pin_d3 = 16;
    c.pin_d4 = 14; c.pin_d5 = 12; c.pin_d6 = 11; c.pin_d7 = 48;
    c.pin_xclk = 10; c.pin_pclk = 13; c.pin_vsync = 38; c.pin_href = 47;
    c.pin_sccb_sda = 40; c.pin_sccb_scl = 39;
    c.pin_pwdn = -1; c.pin_reset = -1;
    c.xclk_freq_hz = 20000000;
    c.pixel_format = PIXFORMAT_JPEG;
    c.frame_size   = FRAMESIZE_UXGA;       // pre-allocate the largest buffer
    c.fb_location  = CAMERA_FB_IN_PSRAM;
    c.grab_mode    = CAMERA_GRAB_LATEST;
    c.jpeg_quality = 12;
    c.fb_count     = 2;
    // NOTE: LED_GPIO_NUM (GPIO21) from the example is the microSD CS — never driven.

    if (!psramFound()) {                   // shouldn't happen on the Sense (8 MB PSRAM)
        c.frame_size  = FRAMESIZE_SVGA;
        c.fb_location = CAMERA_FB_IN_DRAM;
        c.fb_count    = 1;
        SerialLog::logf("[CameraNode] no PSRAM — falling back to SVGA/DRAM\n");
    }

    esp_err_t err = esp_camera_init(&c);
    if (err != ESP_OK) {
        SerialLog::logf("[CameraNode] esp_camera_init failed 0x%x — camera disabled "
                        "(no daughterboard?)\n", (unsigned)err);
        s_ok = false;
        return;
    }
    sensor_t *s = esp_camera_sensor_get();
    if (s) s->set_framesize(s, FRAMESIZE_SVGA);   // sensible default snap size (800x600)
    if (!s_camMutex) s_camMutex = xSemaphoreCreateMutex();
    s_ok = true;
    SerialLog::logf("[CameraNode] OV2640 ready (sensor PID=0x%x) default SVGA\n",
                    s ? (unsigned)s->id.PID : 0u);
}

bool ready() { return s_ok; }

size_t snap(char *nameOut, size_t nameCap, uint16_t *wOut, uint16_t *hOut) {
    if (nameCap) nameOut[0] = '\0';
    if (!s_ok) return 0;

    // Serialize with the portal's MJPEG stream (Phase 3) — both pull frame buffers
    // from the same sensor. A stream frame send is short; 1 s is generous headroom.
    if (!lockCamera(1000)) {
        SerialLog::logf("[CameraNode] snap: camera busy (stream) — capture skipped\n");
        return 0;
    }
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
        unlockCamera();
        SerialLog::logf("[CameraNode] esp_camera_fb_get() returned null — capture failed\n");
        return 0;
    }
    size_t len = fb->len;
    if (wOut) *wOut = (uint16_t)fb->width;
    if (hOut) *hOut = (uint16_t)fb->height;

    // Phase 2b: persist to microSD when mounted, returning the REAL path in the
    // ACK. The camera mutex is already held; nest the shared-bus mutex under it
    // (lock order is always camera -> bus, and only snap() ever nests, so no
    // deadlock with the radios, which take the bus mutex alone).
    bool wrote = false;
    if (s_sdOk && busLock(2000)) {
        char path[32];
        snprintf(path, sizeof(path), "/loracam/IMG_%05u.jpg", (unsigned)s_nextIdx);
        File f = SD.open(path, FILE_WRITE);
        if (f) {
            size_t w = f.write(fb->buf, len);
            f.close();
            if (w == len) {
                snprintf(nameOut, nameCap, "%s", path);
                s_nextIdx++;
                refreshFreePct();
                wrote = true;
                SerialLog::logf("[CameraNode] saved %s (%u B, %u%% free)\n",
                                path, (unsigned)len, (unsigned)s_sdFreePct);
            } else {
                SD.remove(path);   // short write (card full?) — don't leave a stub
                SerialLog::logf("[CameraNode] SD short write %u/%u B — card full? kept in PSRAM\n",
                                (unsigned)w, (unsigned)len);
            }
        } else {
            SerialLog::logf("[CameraNode] SD open %s failed — kept in PSRAM\n", path);
        }
        busUnlock();
    }

    // No card (or the write failed): report a PSRAM descriptor so the ACK still
    // proves a real capture happened, exactly as in Phase 2a.
    if (!wrote)
        snprintf(nameOut, nameCap, "snap_%ux%u_%uB.jpg",
                 (unsigned)fb->width, (unsigned)fb->height, (unsigned)len);

    esp_camera_fb_return(fb);
    unlockCamera();
    return len;
}

}  // namespace CameraNode

#endif  // BRIDGE_ROLE_CAMERA
