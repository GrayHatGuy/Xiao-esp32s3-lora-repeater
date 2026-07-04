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
static uint32_t s_nextVid   = 1;                // next /loracam/VID_%05u.avi
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

    // Resume numbering across reboots: highest existing IMG_<n> / VID_<n> + 1.
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
            else if (sscanf(base, "VID_%u", &idx) == 1 && idx >= s_nextVid)
                s_nextVid = idx + 1;
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

static void mediaPath(uint32_t idx, bool vid, char *out, size_t cap) {
    snprintf(out, cap, vid ? "/loracam/VID_%05u.avi" : "/loracam/IMG_%05u.jpg",
             (unsigned)idx);
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
            uint8_t vid = 0;
            if (sscanf(base, "IMG_%u", &idx) == 1)      vid = 0;
            else if (sscanf(base, "VID_%u", &idx) == 1) vid = 1;
            else { e.close(); continue; }
            PhotoInfo pi = { (uint32_t)idx, (uint32_t)e.size(), vid };
            if (n < cap) {
                out[n++] = pi;
            } else {
                // Full: replace the smallest index so the newest `cap` survive.
                size_t mn = 0;
                for (size_t i = 1; i < n; i++)
                    if (out[i].idx < out[mn].idx) mn = i;
                if (pi.idx > out[mn].idx) out[mn] = pi;
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

// Streaming read (portal task is the only caller — one open file at a time).
static File s_rd;

size_t photoOpen(uint32_t idx, bool vid) {
    if (!s_sdOk || !busLock(2000)) return 0;
    char path[32];
    mediaPath(idx, vid, path, sizeof(path));
    if (s_rd) s_rd.close();
    s_rd = SD.open(path, FILE_READ);
    size_t sz = s_rd ? s_rd.size() : 0;
    busUnlock();
    return sz;
}

size_t photoReadChunk(uint8_t *dst, size_t cap) {
    if (!s_rd || !dst || !busLock(2000)) return 0;
    size_t n = s_rd.read(dst, cap);
    busUnlock();
    return n;
}

void photoClose() {
    if (!s_rd) return;
    if (busLock(2000)) { s_rd.close(); busUnlock(); }
}

bool photoDelete(uint32_t idx, bool vid) {
    if (!s_sdOk || !busLock(2000)) return false;
    char path[32];
    mediaPath(idx, vid, path, sizeof(path));
    bool ok = SD.remove(path);
    if (ok) refreshFreePct();
    busUnlock();
    if (ok) SerialLog::logf("[CameraNode] deleted %s\n", path);
    return ok;
}

// --- Phase 2b: MJPEG-AVI video recorder ---------------------------------------
// A minimal single-stream AVI: RIFF header (placeholders patched at finalize) +
// 'movi' 00dc chunks (one JPEG frame each, even-padded) + an idx1 index. Plays in
// VLC / desktop players / phones. Frames are paced at ~5 fps — comfortable for a
// 4 MHz shared SPI bus with the radios and the portal stream still live.

static const uint32_t REC_FRAME_MS  = 200;      // ~5 fps target
static const size_t   REC_MAX_FRAMES = 3000;    // hard cap (~10 min at 5 fps)

static volatile bool s_recActive = false;
static volatile bool s_recStop   = false;
static uint16_t  s_recSecs = 30;
static uint32_t  s_recIdx  = 0;                 // VID number being written

struct IdxEnt { uint32_t off, len; };           // '00dc' offset (rel. 'movi') + size

static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static void putFcc(uint8_t *p, const char *f) { memcpy(p, f, 4); }

// Fixed 224-byte header up to and including the 'movi' LIST fourcc. Placeholder
// fields (sizes / counts / timing) are patched in recFinalize().
static void writeAviHeader(File &f, uint16_t w, uint16_t h) {
    uint8_t hd[224];
    memset(hd, 0, sizeof(hd));
    putFcc(hd + 0, "RIFF");                       // [4] riffSize @4 (patched)
    putFcc(hd + 8, "AVI ");
    putFcc(hd + 12, "LIST"); put32(hd + 16, 192); putFcc(hd + 20, "hdrl");
    putFcc(hd + 24, "avih"); put32(hd + 28, 56);
    // avih: usPerFrame@32 (patch), flags@44=HASINDEX, totalFrames@48 (patch),
    // streams@56=1, width@64, height@68.
    put32(hd + 44, 0x10);
    put32(hd + 56, 1);
    put32(hd + 64, w); put32(hd + 68, h);
    putFcc(hd + 88, "LIST"); put32(hd + 92, 116); putFcc(hd + 96, "strl");
    putFcc(hd + 100, "strh"); put32(hd + 104, 56);
    putFcc(hd + 108, "vids"); putFcc(hd + 112, "MJPG");
    // strh: dwScale@128 (patch = usPerFrame), dwRate@132 = 1e6 -> fps = rate/scale,
    // dwLength@140 (patch = frames), rcFrame right/bottom @ 160/162.
    put32(hd + 132, 1000000);
    hd[160] = (uint8_t)w; hd[161] = (uint8_t)(w >> 8);
    hd[162] = (uint8_t)h; hd[163] = (uint8_t)(h >> 8);
    putFcc(hd + 164, "strf"); put32(hd + 168, 40);
    put32(hd + 172, 40);                          // biSize
    put32(hd + 176, w); put32(hd + 180, h);
    hd[184] = 1;                                  // biPlanes
    hd[186] = 24;                                 // biBitCount
    putFcc(hd + 188, "MJPG");                     // biCompression
    put32(hd + 192, (uint32_t)w * h * 3);         // biSizeImage (nominal)
    putFcc(hd + 212, "LIST");                     // [216] moviSize (patched)
    putFcc(hd + 220, "movi");
    f.write(hd, sizeof(hd));
}

static void recTask(void *) {
    char path[32];
    mediaPath(s_recIdx, true, path, sizeof(path));

    IdxEnt *idx = (IdxEnt *)(psramFound() ? ps_malloc(REC_MAX_FRAMES * sizeof(IdxEnt))
                                          : malloc(REC_MAX_FRAMES * sizeof(IdxEnt)));
    File f;
    bool open = false;
    if (idx && busLock(3000)) {
        f = SD.open(path, FILE_WRITE);
        open = (bool)f;
        busUnlock();
    }
    if (!open) {
        SerialLog::logf("[CameraNode] REC: open %s failed\n", path);
        if (idx) free(idx);
        s_recActive = false;
        vTaskDelete(NULL);
        return;
    }

    uint32_t frames = 0, moviBytes = 4;           // 'movi' fourcc counts in the LIST
    uint16_t fw = 0, fh = 0;
    bool hdrDone = false;
    const uint32_t t0 = millis();
    const uint32_t deadline = t0 + (uint32_t)s_recSecs * 1000UL;
    SerialLog::logf("[CameraNode] REC start %s (%us)\n", path, (unsigned)s_recSecs);

    while (!s_recStop && millis() < deadline && frames < REC_MAX_FRAMES) {
        uint32_t tf = millis();
        if (lockCamera(500)) {
            camera_fb_t *fb = esp_camera_fb_get();
            if (fb) {
                if (busLock(2000)) {
                    if (!hdrDone) {
                        fw = (uint16_t)fb->width; fh = (uint16_t)fb->height;
                        writeAviHeader(f, fw, fh);
                        hdrDone = true;
                    }
                    uint32_t clen = fb->len;
                    uint8_t ch[8];
                    putFcc(ch, "00dc"); put32(ch + 4, clen);
                    idx[frames].off = moviBytes;              // rel. 'movi' fourcc
                    idx[frames].len = clen;
                    f.write(ch, 8);
                    f.write(fb->buf, clen);
                    if (clen & 1) { uint8_t z = 0; f.write(&z, 1); clen++; }
                    moviBytes += 8 + clen;
                    frames++;
                    busUnlock();
                }
                esp_camera_fb_return(fb);
            }
            unlockCamera();
        }
        uint32_t spent = millis() - tf;
        if (spent < REC_FRAME_MS) vTaskDelay(pdMS_TO_TICKS(REC_FRAME_MS - spent));
    }

    // Finalize: patch sizes/counts, append idx1, close. Zero frames -> delete.
    // NEVER use f.size() mid-write here: it can lag the stdio write buffer
    // (bench finding — the recorded riffSize came out short and VLC rejected
    // the file). Every byte written is counted arithmetically instead, and the
    // write position is already at EOF for the idx1 append (no seek needed).
    uint32_t elapsedMs = millis() - t0;
    if (busLock(5000)) {
        if (frames == 0) {
            f.close();
            SD.remove(path);
        } else {
            uint32_t usPerFrame = (elapsedMs * 1000UL) / frames;
            uint8_t e16[16];
            putFcc(e16, "idx1"); put32(e16 + 4, frames * 16);
            f.write(e16, 8);
            for (uint32_t i = 0; i < frames; i++) {
                putFcc(e16, "00dc"); put32(e16 + 4, 0x10);    // AVIIF_KEYFRAME
                put32(e16 + 8, idx[i].off); put32(e16 + 12, idx[i].len);
                f.write(e16, 16);
            }
            // header 224 + movi chunks (moviBytes counts the 'movi' fourcc's 4) +
            // idx1 header 8 + 16 per entry.
            uint32_t total = 224 + (moviBytes - 4) + 8 + frames * 16;
            uint8_t v4[4];
            put32(v4, total - 8);       f.seek(4);   f.write(v4, 4);   // riffSize
            put32(v4, usPerFrame);      f.seek(32);  f.write(v4, 4);   // avih usPerFrame
            put32(v4, frames);          f.seek(48);  f.write(v4, 4);   // avih totalFrames
            put32(v4, usPerFrame);      f.seek(128); f.write(v4, 4);   // strh dwScale
            put32(v4, frames);          f.seek(140); f.write(v4, 4);   // strh dwLength
            put32(v4, moviBytes);       f.seek(216); f.write(v4, 4);   // movi LIST size
            f.close();
            refreshFreePct();
        }
        busUnlock();
    }
    if (idx) free(idx);
    SerialLog::logf("[CameraNode] REC done %s: %u frames in %lu ms (%.1f fps) %s\n",
                    path, (unsigned)frames, (unsigned long)elapsedMs,
                    frames ? (frames * 1000.0f) / elapsedMs : 0.0f,
                    frames ? "" : "— empty, removed");
    s_recActive = false;
    vTaskDelete(NULL);
}

bool recordActive() { return s_recActive; }
void recordStop()   { s_recStop = true; }

bool recordStart(uint16_t seconds, char *nameOut, size_t nameCap) {
    if (nameCap) nameOut[0] = '\0';
    if (!s_ok || !s_sdOk || s_recActive) return false;
    if (seconds < 5)   seconds = 5;
    if (seconds > 300) seconds = 300;
    s_recSecs = seconds;
    s_recIdx  = s_nextVid++;
    s_recStop = false;
    s_recActive = true;
    char path[32];
    mediaPath(s_recIdx, true, path, sizeof(path));
    if (xTaskCreatePinnedToCore(recTask, "CamRec", 6144, nullptr, 1, nullptr, 0) != pdPASS) {
        s_recActive = false;
        return false;
    }
    snprintf(nameOut, nameCap, "%s", path);
    return true;
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
