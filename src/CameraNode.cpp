// CameraNode.cpp — see CameraNode.h. OV2640 pin map from the Seeed XIAO ESP32-S3
// Sense CameraWebServer example (CAMERA_MODEL_XIAO_ESP32S3).

#include "CameraNode.h"
#if defined(BRIDGE_ROLE_CAMERA)

#include "esp_camera.h"
#include "SerialLog.h"

namespace CameraNode {

static bool s_ok = false;

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
    s_ok = true;
    SerialLog::logf("[CameraNode] OV2640 ready (sensor PID=0x%x) default SVGA\n",
                    s ? (unsigned)s->id.PID : 0u);
}

bool ready() { return s_ok; }

bool sdPresent() { return false; }   // Phase 2b: mount microSD on the shared SPI bus

size_t snap(char *nameOut, size_t nameCap, uint16_t *wOut, uint16_t *hOut) {
    if (nameCap) nameOut[0] = '\0';
    if (!s_ok) return 0;

    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
        SerialLog::logf("[CameraNode] esp_camera_fb_get() returned null — capture failed\n");
        return 0;
    }
    size_t len = fb->len;
    if (wOut) *wOut = (uint16_t)fb->width;
    if (hOut) *hOut = (uint16_t)fb->height;

    // Phase 2a: no SD yet — the JPEG stays in PSRAM; report a descriptor + size so the
    // ACK proves a real capture happened. Phase 2b writes fb->buf to microSD and
    // returns the real filename here (taking the shared-bus spiMutex around SD I/O).
    snprintf(nameOut, nameCap, "snap_%ux%u_%uB.jpg",
             (unsigned)fb->width, (unsigned)fb->height, (unsigned)len);

    esp_camera_fb_return(fb);
    return len;
}

}  // namespace CameraNode

#endif  // BRIDGE_ROLE_CAMERA
