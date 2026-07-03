// CamStream.cpp — see CamStream.h. The :81 MJPEG server (esp_http_server).

#include "CamStream.h"
#if defined(BRIDGE_ROLE_CAMERA)

#include <esp_http_server.h>
#include <esp_camera.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <stdio.h>

#include "CameraNode.h"   // ready() / lockCamera()
#include "CamPortal.h"    // sessionValid()
#include "SerialLog.h"

namespace CamStream {

static constexpr uint16_t STREAM_PORT = 81;
static httpd_handle_t s_httpd = nullptr;

// Extract ?sid=<hex> from the query and validate it against the portal sessions.
static bool authed(httpd_req_t *req) {
    char q[64], sid[40] = {0};
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) != ESP_OK) return false;
    if (httpd_query_key_value(q, "sid", sid, sizeof(sid)) != ESP_OK) return false;
    return CamPortal::sessionValid(sid);
}

static esp_err_t streamHandler(httpd_req_t *req) {
    if (!authed(req)) {
        httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "login required");
        return ESP_FAIL;
    }
    static const char *PART = "\r\n--lcframe\r\nContent-Type: image/jpeg\r\n"
                              "Content-Length: %u\r\n\r\n";
    httpd_resp_set_type(req, "multipart/x-mixed-replace;boundary=lcframe");

    // Bench finding (P3-28): a snap contending for the camera can make fb_get
    // transiently fail; ending the stream on ONE null frame leaves the browser's
    // <img> frozen until a manual refresh. Retry instead — only give up after
    // ~3 s of consecutive failures (camera genuinely wedged, not a passing snap).
    int nullStreak = 0;
    while (true) {
        if (!CameraNode::lockCamera(1000)) continue;
        camera_fb_t *fb = esp_camera_fb_get();
        if (!fb) {
            CameraNode::unlockCamera();
            if (++nullStreak >= 30) break;
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        nullStreak = 0;
        char hdr[96];
        int hl = snprintf(hdr, sizeof(hdr), PART, (unsigned)fb->len);
        esp_err_t e = httpd_resp_send_chunk(req, hdr, hl);
        if (e == ESP_OK) e = httpd_resp_send_chunk(req, (const char *)fb->buf, fb->len);
        esp_camera_fb_return(fb);
        CameraNode::unlockCamera();
        if (e != ESP_OK) break;                       // client disconnected
        vTaskDelay(pdMS_TO_TICKS(100));               // ~10 fps cap
    }
    return ESP_OK;
}

static esp_err_t jpgHandler(httpd_req_t *req) {
    if (!authed(req)) {
        httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "login required");
        return ESP_FAIL;
    }
    if (!CameraNode::lockCamera(1000)) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "camera busy");
        return ESP_FAIL;
    }
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
        CameraNode::unlockCamera();
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "capture failed");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "image/jpeg");
    esp_err_t e = httpd_resp_send(req, (const char *)fb->buf, fb->len);
    esp_camera_fb_return(fb);
    CameraNode::unlockCamera();
    return e;
}

void start() {
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = STREAM_PORT;
    cfg.ctrl_port   = 32769;                          // distinct from the default 32768
    cfg.lru_purge_enable = true;
    if (httpd_start(&s_httpd, &cfg) != ESP_OK) {
        SerialLog::logf("[CamStream] failed to start on :%u\n", (unsigned)STREAM_PORT);
        s_httpd = nullptr;
        return;
    }
    httpd_uri_t stream = { "/stream", HTTP_GET, streamHandler, nullptr };
    httpd_uri_t jpg    = { "/jpg",    HTTP_GET, jpgHandler,    nullptr };
    httpd_register_uri_handler(s_httpd, &stream);
    httpd_register_uri_handler(s_httpd, &jpg);
    SerialLog::logf("[CamStream] MJPEG server up on :%u (/stream, /jpg)\n",
                    (unsigned)STREAM_PORT);
}

}  // namespace CamStream

#endif  // BRIDGE_ROLE_CAMERA
