// CaptivePortal.cpp — see CaptivePortal.h for design notes.

#include "CaptivePortal.h"
#include "BridgeConfig.h"

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <string.h>
#include <ctype.h>
#include <mbedtls/base64.h>

namespace CaptivePortal {

static const byte    DNS_PORT  = 53;
static const uint8_t HTTP_PORT = 80;
static const IPAddress AP_IP(192, 168, 4, 1);
static const IPAddress AP_NETMASK(255, 255, 255, 0);

static DNSServer  s_dns;
static WebServer  s_http(HTTP_PORT);

// --- HTML escaping for safely interpolating BridgeConfig strings into the
// form. Only the four characters that matter in attribute/text contexts.
static String htmlEscape(const char *s) {
    String out;
    if (!s) return out;
    while (*s) {
        char c = *s++;
        switch (c) {
            case '&':  out += "&amp;";  break;
            case '<':  out += "&lt;";   break;
            case '>':  out += "&gt;";   break;
            case '"':  out += "&quot;"; break;
            case '\'': out += "&#39;";  break;
            default:   out += c;
        }
    }
    return out;
}

// Lowercase a string in place. Used to normalise the MC key hex before
// committing it; the parser elsewhere accepts both cases but persisted
// data is friendlier when consistent.
static void toLower(String &s) {
    for (size_t i = 0; i < s.length(); i++) s.setCharAt(i, (char)tolower(s[i]));
}

static bool isHexString(const String &s, size_t len) {
    if (s.length() != len) return false;
    for (size_t i = 0; i < len; i++) {
        char c = s[i];
        if (!((c >= '0' && c <= '9') ||
              (c >= 'a' && c <= 'f') ||
              (c >= 'A' && c <= 'F'))) return false;
    }
    return true;
}

// --- HTML form rendering ---------------------------------------------------
//
// Single-page form. Pre-fills every input from BridgeConfig. Uses inline
// style for portability (no external CSS the AP can't reach).
static String renderForm(const char *flash = nullptr) {
    char nodeIdBuf[12];
    snprintf(nodeIdBuf, sizeof(nodeIdBuf), "0x%08lX",
             (unsigned long)BridgeConfig::mtNodeId());

    String page;
    page.reserve(4096);
    page += F("<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\">"
              "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
              "<title>LoRa Bridge config</title><style>"
              "body{font-family:system-ui,Arial,sans-serif;max-width:560px;margin:1em auto;padding:0 1em;color:#111}"
              "h1{font-size:1.2em;margin:.4em 0}"
              "h2{font-size:1em;margin:1.4em 0 .3em;border-bottom:1px solid #ccc}"
              "label{display:block;margin:.6em 0 .15em;font-weight:600}"
              "input[type=text]{width:100%;padding:.4em;font-family:ui-monospace,Consolas,monospace;font-size:.95em;box-sizing:border-box}"
              "input[type=checkbox]{transform:scale(1.2);margin-right:.4em}"
              ".hint{font-size:.85em;color:#555;margin-top:.1em}"
              ".flash{padding:.6em;margin:.6em 0;background:#fffae6;border:1px solid #e0c070;border-radius:4px}"
              "button{margin-top:1.2em;padding:.6em 1.2em;font-size:1em;background:#0066cc;color:#fff;border:0;border-radius:4px;cursor:pointer}"
              "</style></head><body>");
    page += F("<h1>LoRa Bridge \xe2\x80\x94 Configuration</h1>");
    if (flash) {
        page += F("<div class=\"flash\">");
        page += htmlEscape(flash);
        page += F("</div>");
    }
    page += F("<form method=\"POST\" action=\"/save\">");

    page += F("<h2>Meshtastic identity</h2>");
    page += F("<label>Node ID (uint32, hex)</label>"
              "<input type=\"text\" name=\"mtNodeId\" value=\"");
    page += htmlEscape(nodeIdBuf);
    page += F("\" required>"
              "<div class=\"hint\">Must match the \"!hex\" string below.</div>");

    page += F("<label>Node ID string (\"!\" + 8 hex)</label>"
              "<input type=\"text\" name=\"mtNodeIdStr\" maxlength=\"15\" value=\"");
    page += htmlEscape(BridgeConfig::mtNodeIdStr());
    page += F("\" required>");

    page += F("<label>Long name</label>"
              "<input type=\"text\" name=\"mtLongName\" maxlength=\"39\" value=\"");
    page += htmlEscape(BridgeConfig::mtLongName());
    page += F("\" required>");

    page += F("<label>Short name</label>"
              "<input type=\"text\" name=\"mtShortName\" maxlength=\"8\" value=\"");
    page += htmlEscape(BridgeConfig::mtShortName());
    page += F("\" required>");

    page += F("<h2>Meshtastic channel</h2>");
    page += F("<label>Channel name</label>"
              "<input type=\"text\" name=\"mtChannelName\" maxlength=\"23\" value=\"");
    page += htmlEscape(BridgeConfig::mtChannelName());
    page += F("\" required>");

    page += F("<label>PSK (base64, blank = LongFast default)</label>"
              "<input type=\"text\" name=\"mtPskBase64\" maxlength=\"47\" value=\"");
    page += htmlEscape(BridgeConfig::mtPskBase64());
    page += F("\">"
              "<div class=\"hint\">Leave blank for the public LongFast channel. "
              "Otherwise paste the channel PSK as base64 — decodes to 1, 16 "
              "(AES-128) or 32 (AES-256) bytes.</div>");

    page += F("<h2>MeshCore channel</h2>");
    page += F("<label>Key (32 hex characters)</label>"
              "<input type=\"text\" name=\"mcKeyHex\" maxlength=\"32\" pattern=\"[0-9a-fA-F]{32}\" value=\"");
    page += htmlEscape(BridgeConfig::mcKeyHex());
    page += F("\" required>"
              "<div class=\"hint\">Public channel default: 8b3387e9c5cdea6ac9e5edbaa115cd72</div>");

    page += F("<label>Channel display name</label>"
              "<input type=\"text\" name=\"mcChannelName\" maxlength=\"23\" value=\"");
    page += htmlEscape(BridgeConfig::mcChannelName());
    page += F("\" required>");

    page += F("<h2>Bridge behaviour</h2>");
    page += F("<label><input type=\"checkbox\" name=\"positionEnabled\" value=\"1\"");
    if (BridgeConfig::positionEnabled()) page += F(" checked");
    page += F(">Bridge Meshtastic POSITION_APP packets to MC</label>");

    page += F("<label><input type=\"checkbox\" name=\"telemetryEnabled\" value=\"1\"");
    if (BridgeConfig::telemetryEnabled()) page += F(" checked");
    page += F(">Bridge Meshtastic TELEMETRY_APP packets to MC</label>");

    page += F("<button type=\"submit\">Save &amp; reboot</button>"
              "</form>"
              "<p class=\"hint\">After saving, the device reboots and starts bridging. "
              "Press the BOOT button within ~3 s of any future reset to re-enter this form.</p>"
              "</body></html>");
    return page;
}

// --- Request handlers ------------------------------------------------------

static void handleRoot() {
    s_http.send(200, "text/html; charset=utf-8", renderForm());
}

// Catch-all for captive-portal probes (e.g. /generate_204, /hotspot-detect.html).
// Returning a redirect to the config page is what triggers most OS-level
// captive-portal UIs to pop the browser open automatically.
static void handleCaptive() {
    s_http.sendHeader("Location", "/", true);
    s_http.send(302, "text/plain", "redirect");
}

static void handleSave() {
    // Parse + validate every field. On any failure, re-render the form with
    // a flash message; on full success, commit to BridgeConfig and reboot.
    String mtNodeIdRaw     = s_http.arg("mtNodeId");
    String mtNodeIdStr     = s_http.arg("mtNodeIdStr");
    String mtLongName      = s_http.arg("mtLongName");
    String mtShortName     = s_http.arg("mtShortName");
    String mtChannelName   = s_http.arg("mtChannelName");
    String mtPskBase64     = s_http.arg("mtPskBase64");
    String mcKeyHex        = s_http.arg("mcKeyHex");
    String mcChannelName   = s_http.arg("mcChannelName");
    bool   positionEnabled = (s_http.arg("positionEnabled")  == "1");
    bool   telemetryEnabled= (s_http.arg("telemetryEnabled") == "1");

    // Strip a leading "0x"/"0X" so users can paste either form.
    mtNodeIdRaw.trim();
    if (mtNodeIdRaw.startsWith("0x") || mtNodeIdRaw.startsWith("0X"))
        mtNodeIdRaw = mtNodeIdRaw.substring(2);

    if (!isHexString(mtNodeIdRaw, 8)) {
        s_http.send(200, "text/html; charset=utf-8",
                    renderForm("Node ID must be exactly 8 hex characters."));
        return;
    }
    uint32_t mtNodeId = (uint32_t)strtoul(mtNodeIdRaw.c_str(), nullptr, 16);

    mtNodeIdStr.trim();
    if (mtNodeIdStr.length() != 9 || mtNodeIdStr[0] != '!' ||
        !isHexString(mtNodeIdStr.substring(1), 8)) {
        s_http.send(200, "text/html; charset=utf-8",
                    renderForm("Node ID string must be \"!\" followed by 8 hex chars."));
        return;
    }
    {
        // Cross-check that the string and numeric ID encode the same value.
        uint32_t fromStr = (uint32_t)strtoul(mtNodeIdStr.substring(1).c_str(), nullptr, 16);
        if (fromStr != mtNodeId) {
            s_http.send(200, "text/html; charset=utf-8",
                        renderForm("Node ID and Node ID string must encode the same 32-bit value."));
            return;
        }
    }

    mcKeyHex.trim();
    toLower(mcKeyHex);
    if (!isHexString(mcKeyHex, 32)) {
        s_http.send(200, "text/html; charset=utf-8",
                    renderForm("MC key must be exactly 32 hex characters."));
        return;
    }

    if (mtLongName.length() == 0 || mtShortName.length() == 0 ||
        mtChannelName.length() == 0 || mcChannelName.length() == 0) {
        s_http.send(200, "text/html; charset=utf-8",
                    renderForm("Long name, short name, and channel names cannot be empty."));
        return;
    }

    // MT PSK: empty is allowed (== LongFast). If present it must base64-
    // decode to 1, 16 or 32 bytes — the only lengths MeshtasticConfig
    // accepts. Catch typos here rather than silently falling back at boot.
    mtPskBase64.trim();
    if (mtPskBase64.length() > 0) {
        uint8_t  dec[48];
        size_t   decLen = 0;
        int rc = mbedtls_base64_decode(dec, sizeof(dec), &decLen,
                                        (const unsigned char *)mtPskBase64.c_str(),
                                        mtPskBase64.length());
        if (rc != 0 || !(decLen == 1 || decLen == 16 || decLen == 32)) {
            s_http.send(200, "text/html; charset=utf-8",
                        renderForm("Meshtastic PSK must be blank, or base64 "
                                   "decoding to 1, 16, or 32 bytes."));
            return;
        }
    }

    BridgeConfig::setMtNodeId(mtNodeId);
    BridgeConfig::setMtNodeIdStr(mtNodeIdStr.c_str());
    BridgeConfig::setMtLongName(mtLongName.c_str());
    BridgeConfig::setMtShortName(mtShortName.c_str());
    BridgeConfig::setMtChannelName(mtChannelName.c_str());
    BridgeConfig::setMtPskBase64(mtPskBase64.c_str());
    BridgeConfig::setMcKeyHex(mcKeyHex.c_str());
    BridgeConfig::setMcChannelName(mcChannelName.c_str());
    BridgeConfig::setPositionEnabled(positionEnabled);
    BridgeConfig::setTelemetryEnabled(telemetryEnabled);
    BridgeConfig::save();

    Serial.println("[CaptivePortal] config saved, rebooting...");
    BridgeConfig::debugDump();

    s_http.send(200, "text/html; charset=utf-8",
                F("<!doctype html><meta http-equiv=\"refresh\" content=\"3\"><body>"
                  "<h2>Saved. Rebooting&hellip;</h2></body>"));

    delay(800);          // let the response actually flush
    ESP.restart();
}

void begin() {
    // SSID includes the last byte of the bridge MT node ID so two bridges
    // in earshot don't collide on the air.
    char ssid[24];
    snprintf(ssid, sizeof(ssid), "LoRa-Bridge-%02X",
             (unsigned)(BridgeConfig::mtNodeId() & 0xFFu));

    Serial.printf("\n[CaptivePortal] starting SoftAP \"%s\" @ %s\n",
                  ssid, AP_IP.toString().c_str());

    WiFi.mode(WIFI_AP);
    WiFi.softAPConfig(AP_IP, AP_IP, AP_NETMASK);
    WiFi.softAP(ssid);     // open AP — no password

    // Resolve every DNS query to the AP IP so any URL the browser tries
    // (apple-captive-network-assistant.com, msftconnecttest.com, etc.)
    // routes back to our form.
    s_dns.start(DNS_PORT, "*", AP_IP);

    s_http.on("/",                          HTTP_GET,  handleRoot);
    s_http.on("/save",                      HTTP_POST, handleSave);
    s_http.on("/generate_204",              HTTP_GET,  handleCaptive);
    s_http.on("/gen_204",                   HTTP_GET,  handleCaptive);
    s_http.on("/hotspot-detect.html",       HTTP_GET,  handleCaptive);
    s_http.on("/library/test/success.html", HTTP_GET,  handleCaptive);
    s_http.on("/ncsi.txt",                  HTTP_GET,  handleCaptive);
    s_http.on("/connecttest.txt",           HTTP_GET,  handleCaptive);
    s_http.onNotFound(handleCaptive);
    s_http.begin();

    Serial.println("[CaptivePortal] connect to the SSID above with a phone, "
                   "any browser request will redirect to the config form.");

    // Block here forever; handleSave() will ESP.restart() when the user
    // commits the form.
    for (;;) {
        s_dns.processNextRequest();
        s_http.handleClient();
        delay(2);
    }
}

}  // namespace CaptivePortal
