// CaptivePortal.cpp — see CaptivePortal.h for design notes.
//
// v8: the portal is now the full configuration surface. Per radio it picks a
// PROTOCOL (Meshtastic / MeshCore / Reticulum / Custom / None) and resolves
// the RF plan; a global REGION governs the sub-GHz band. Meshtastic radios
// get a Tier 2 channel-slot frequency (region + modem-preset + channel-name)
// pre-filled into an editable field; MeshCore / Reticulum get a flat band
// default; Custom exposes the full RF plan behind a warning banner.

#include "CaptivePortal.h"
#include "BridgeConfig.h"
#include "RegionPreset.h"

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <string.h>
#include <ctype.h>
#include <stdlib.h>
#include <math.h>
#include <mbedtls/base64.h>

namespace CaptivePortal {

static const byte    DNS_PORT  = 53;
static const uint8_t HTTP_PORT = 80;
static const IPAddress AP_IP(192, 168, 4, 1);
static const IPAddress AP_NETMASK(255, 255, 255, 0);

// SX1262 RF limits used to clamp/validate the Custom path.
static const float   SX1262_FREQ_MIN = 150.0f;   // MHz
static const float   SX1262_FREQ_MAX = 960.0f;   // MHz
static const int8_t  SX1262_TX_MIN   = -9;       // dBm
static const int8_t  SX1262_TX_MAX   = 22;       // dBm
static const float   ALLOWED_BW[] = { 7.8f, 10.4f, 15.6f, 20.8f, 31.25f,
                                      41.7f, 62.5f, 125.0f, 250.0f, 500.0f };

static DNSServer  s_dns;
static WebServer  s_http(HTTP_PORT);

// --- small helpers ---------------------------------------------------------

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

static bool bwAllowed(float bw) {
    for (float v : ALLOWED_BW)
        if (fabsf(bw - v) < 0.05f) return true;
    return false;
}

// Best-effort reverse lookup: which modem preset matches a stored BW/SF/CR.
// Falls back to LongFast so the form select always has a sensible default.
static uint8_t presetFromParams(float bw, uint8_t sf, uint8_t cr) {
    for (uint8_t p = 0; p <= RegionPreset::PRESET_LONG_TURBO; p++) {
        float pbw; uint8_t psf, pcr;
        RegionPreset::modemPresetParams(p, pbw, psf, pcr);
        if (fabsf(pbw - bw) < 0.05f && psf == sf && pcr == cr) return p;
    }
    return RegionPreset::PRESET_LONG_FAST;
}

// --- form sections ---------------------------------------------------------

static void appendRegionSelect(String &page) {
    page += F("<h2>Device region</h2>"
              "<label>Region (governs the sub-GHz band)</label>"
              "<select id=\"region\" name=\"region\" onchange=\"updAll()\">");
    uint8_t cur = BridgeConfig::region();
    for (uint8_t r = 0; r < RegionPreset::kRegionCount; r++) {
        page += F("<option value=\"");
        page += r;
        page += F("\"");
        if (r == cur) page += F(" selected");
        page += F(">");
        page += RegionPreset::regionInfo(r).name;
        page += F("</option>");
    }
    page += F("</select>"
              "<div class=\"hint\">2.4 GHz radios are region-exempt. "
              "Custom/UNSET leaves frequency entirely manual.</div>");
}

// One radio's full config section: protocol picker + modem preset + channel
// + frequency + Custom RF block. JS (updAll) shows/hides rows by protocol.
static void appendRadio(String &page, int n) {
    int idx = n - 1;
    uint8_t  proto = BridgeConfig::radioProtocol(idx);
    float    freq  = BridgeConfig::radioFrequency(idx);
    float    bw    = BridgeConfig::radioBandwidth(idx);
    uint8_t  sf    = BridgeConfig::radioSf(idx);
    uint8_t  cr    = BridgeConfig::radioCr(idx);
    uint8_t  sync  = BridgeConfig::radioSyncWord(idx);
    int8_t   txp   = BridgeConfig::radioTxPower(idx);
    uint8_t  preset= presetFromParams(bw, sf, cr);
    const char *chName = (n == 1) ? BridgeConfig::radio1ChannelName()
                                  : BridgeConfig::radio2ChannelName();
    const char *chKey  = (n == 1) ? BridgeConfig::radio1ChannelKey()
                                  : BridgeConfig::radio2ChannelKey();

    char buf[16];

    page += F("<h2>Radio ");
    page += n;
    page += F("</h2>");

    // Protocol picker.
    page += F("<label>Protocol</label><select id=\"r");
    page += n;
    page += F("proto\" name=\"r");
    page += n;
    page += F("proto\" onchange=\"updAll()\">");
    static const struct { uint8_t v; const char *name; } kProto[] = {
        { BridgeConfig::PROTO_MT,     "Meshtastic" },
        { BridgeConfig::PROTO_MC,     "MeshCore"   },
        { BridgeConfig::PROTO_RNS,    "Reticulum"  },
        { BridgeConfig::PROTO_CUSTOM, "Custom"     },
        { BridgeConfig::PROTO_NONE,   "None (disable radio)" },
    };
    for (auto &p : kProto) {
        page += F("<option value=\"");
        page += p.v;
        page += F("\"");
        if (p.v == proto) page += F(" selected");
        page += F(">");
        page += p.name;
        page += F("</option>");
    }
    page += F("</select>");

    // Modem preset (Meshtastic only).
    page += F("<div class=\"r");
    page += n;
    page += F("fld mt\"><label>Modem preset</label><select id=\"r");
    page += n;
    page += F("preset\" name=\"r");
    page += n;
    page += F("preset\" onchange=\"updAll()\">");
    for (uint8_t p = 0; p <= RegionPreset::PRESET_LONG_TURBO; p++) {
        page += F("<option value=\"");
        page += p;
        page += F("\"");
        if (p == preset) page += F(" selected");
        page += F(">");
        page += RegionPreset::modemPresetName(p);
        page += F("</option>");
    }
    page += F("</select></div>");

    // Channel name (MT/MC/RNS/Custom).
    page += F("<div class=\"r");
    page += n;
    page += F("fld mt mc rns custom\"><label>Channel name</label>"
              "<input type=\"text\" id=\"r");
    page += n;
    page += F("name\" name=\"r");
    page += n;
    page += F("ChannelName\" maxlength=\"23\" oninput=\"updAll()\" value=\"");
    page += htmlEscape(chName);
    page += F("\"></div>");

    // Channel key (MT/MC/Custom).
    page += F("<div class=\"r");
    page += n;
    page += F("fld mt mc custom\"><label>Channel key</label>"
              "<input type=\"text\" name=\"r");
    page += n;
    page += F("ChannelKey\" maxlength=\"47\" value=\"");
    page += htmlEscape(chKey);
    page += F("\"><div class=\"hint\">Meshtastic: base64 PSK (blank = LongFast). "
              "MeshCore: 32 hex chars. Custom: per your decoder.</div></div>");

    // Frequency (MT/MC/RNS/Custom) — editable; JS pre-fills + flags override.
    snprintf(buf, sizeof(buf), "%.3f", freq);
    page += F("<div class=\"r");
    page += n;
    page += F("fld mt mc rns custom\"><label>Frequency (MHz)</label>"
              "<input type=\"text\" id=\"r");
    page += n;
    page += F("freq\" name=\"r");
    page += n;
    page += F("Freq\" value=\"");
    page += buf;
    page += F("\"><div class=\"hint\" id=\"r");
    page += n;
    page += F("fhint\"></div></div>");

    // TX power (all active protocols).
    page += F("<div class=\"r");
    page += n;
    page += F("fld mt mc rns custom\"><label>TX power (dBm)</label>"
              "<input type=\"text\" name=\"r");
    page += n;
    page += F("Tx\" value=\"");
    page += (int)txp;
    page += F("\"><div class=\"hint\">Region cap applied on save "
              "(US +30, EU +27 / less).</div></div>");

    // Custom-only warning banner.
    page += F("<div class=\"r");
    page += n;
    page += F("fld custom\"><div class=\"warn\">Custom RF: you are entering the "
              "full radio plan by hand. A wrong frequency can put the radio "
              "out of band. The bridge derives its decoder from the sync word "
              "(0x2B Meshtastic, 0x12 MeshCore, 0x42 Reticulum); an "
              "unrecognised sync word receives RF but cannot be decoded.</div></div>");

    // BW / SF / CR — editable for MeshCore AND Custom. MeshCore has no
    // universal regional presets: each mesh community picks its own RF
    // (e.g. 62.5 kHz / SF7, or 250 kHz / SF11). These MUST match the
    // MeshCore network you are bridging or nothing decodes.
    page += F("<div class=\"r");
    page += n;
    page += F("fld custom mc\">");
    page += F("<div class=\"hint\">MeshCore: set these to match your local "
              "MeshCore community's exact LoRa settings.</div>");

    page += F("<label>Bandwidth (kHz)</label><input type=\"text\" name=\"r");
    page += n;
    snprintf(buf, sizeof(buf), "%.1f", bw);
    page += F("Bw\" value=\"");
    page += buf;
    page += F("\">");

    page += F("<label>Spreading factor (5-12)</label><input type=\"text\" name=\"r");
    page += n;
    page += F("Sf\" value=\"");
    page += (unsigned)sf;
    page += F("\">");

    page += F("<label>Coding rate (5-8)</label><input type=\"text\" name=\"r");
    page += n;
    page += F("Cr\" value=\"");
    page += (unsigned)cr;
    page += F("\"></div>");

    // Sync word — Custom only (MeshCore's sync is fixed at 0x12).
    page += F("<div class=\"r");
    page += n;
    page += F("fld custom\"><label>Sync word (hex, e.g. 2B)</label>"
              "<input type=\"text\" name=\"r");
    page += n;
    snprintf(buf, sizeof(buf), "%02X", sync);
    page += F("Sync\" value=\"");
    page += buf;
    page += F("\"></div>");
}

// Inline JS: region table, preset bandwidths, djb2 + slot formula, show/hide.
static void appendScript(String &page) {
    page += F("<script>");
    // Region table: { val: [startMHz, endMHz] }.
    page += F("var REG={");
    for (uint8_t r = 0; r < RegionPreset::kRegionCount; r++) {
        const RegionPreset::RegionInfo &ri = RegionPreset::regionInfo(r);
        char b[48];
        snprintf(b, sizeof(b), "%u:[%.3f,%.3f],", r, ri.freqStart, ri.freqEnd);
        page += b;
    }
    page += F("};");
    // Preset bandwidth (kHz) table.
    page += F("var PRE={");
    for (uint8_t p = 0; p <= RegionPreset::PRESET_LONG_TURBO; p++) {
        float pbw; uint8_t psf, pcr;
        RegionPreset::modemPresetParams(p, pbw, psf, pcr);
        char b[24];
        snprintf(b, sizeof(b), "%u:%.1f,", p, pbw);
        page += b;
    }
    page += F("};");
    // Preset canonical name table. Meshtastic computes the radio frequency
    // from the PRIMARY channel's name, which conventionally equals the modem
    // preset name (LongFast, etc.). A bridged private/secondary channel rides
    // on that same frequency, so the slot is hashed from the preset name —
    // NOT from the bridged channel name.
    page += F("var PN={");
    for (uint8_t p = 0; p <= RegionPreset::PRESET_LONG_TURBO; p++) {
        char b[24];
        snprintf(b, sizeof(b), "%u:'%s',", p, RegionPreset::modemPresetName(p));
        page += b;
    }
    page += F("};");
    page += F(
      "var PC=[0,0];"                        // last computed freq per radio
      "function gv(i){return document.getElementById(i).value;}"
      "function djb2(s){var h=5381;for(var i=0;i<s.length;i++)"
        "h=((h*33)+s.charCodeAt(i))>>>0;return h;}"
      "function slot(rg,nm,bw){var r=REG[rg];if(!r||r[1]<=r[0])return 0;"
        "var bm=bw/1000;var n=Math.floor((r[1]-r[0])/bm);if(n<1)return 0;"
        "var cn=djb2(nm)%n;return r[0]+bm/2+cn*bm;}"
      "function setF(n,f,lbl){var fh=document.getElementById('r'+n+'fhint');"
        "var ff=document.getElementById('r'+n+'freq');"
        "if(f<=0){fh.textContent='';return;}"
        "var fs=f.toFixed(3);"
        "if(ff.value===''||ff.value===PC[n-1])ff.value=fs;"
        "PC[n-1]=fs;"
        "fh.textContent=lbl+': '+fs+(ff.value!==fs?' \\u2014 overridden':'');}"
      "function upd(n){var p=gv('r'+n+'proto');"
        "var tok={'1':'mt','2':'mc','3':'rns','4':'custom','0':'none'}[p];"
        "var fl=document.querySelectorAll('.r'+n+'fld');"
        "for(var i=0;i<fl.length;i++){"
          "var sh=(p!=='0')&&fl[i].classList.contains(tok);"
          "fl[i].style.display=sh?'':'none';}"
        "var fh=document.getElementById('r'+n+'fhint');"
        "if(p==='1'){var ps=gv('r'+n+'preset');"
          "setF(n,slot(gv('region'),PN[ps],PRE[ps]),'computed');}"
        "else if(p==='3'){var r=REG[gv('region')];"
          "setF(n,(r&&r[1]>r[0])?(r[0]+(r[1]-r[0])/2):0,'band default');}"
        "else if(p==='2'){fh.textContent="
          "'MeshCore: enter the exact frequency your community uses.';}"
        "else{fh.textContent='';}}"
      "function updAll(){upd(1);upd(2);}"
      "window.addEventListener('load',updAll);"
      "</script>");
}

static String renderForm(const char *flash = nullptr) {
    char nodeIdBuf[12];
    snprintf(nodeIdBuf, sizeof(nodeIdBuf), "0x%08lX",
             (unsigned long)BridgeConfig::mtNodeId());

    String page;
    page.reserve(12288);
    page += F("<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\">"
              "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
              "<title>LoRa Bridge config</title><style>"
              "body{font-family:system-ui,Arial,sans-serif;max-width:560px;margin:1em auto;padding:0 1em;color:#111}"
              "h1{font-size:1.2em;margin:.4em 0}"
              "h2{font-size:1em;margin:1.4em 0 .3em;border-bottom:1px solid #ccc}"
              "label{display:block;margin:.6em 0 .15em;font-weight:600}"
              "input[type=text],select{width:100%;padding:.4em;font-family:ui-monospace,Consolas,monospace;font-size:.95em;box-sizing:border-box}"
              "input[type=checkbox]{transform:scale(1.2);margin-right:.4em}"
              ".hint{font-size:.85em;color:#555;margin-top:.1em}"
              ".warn{font-size:.85em;padding:.5em;margin:.5em 0;background:#fff0f0;border:1px solid #d08080;border-radius:4px;color:#a00}"
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
              "<div class=\"hint\">Must match the \"!hex\" string below. "
              "Default is derived from this device's MAC.</div>");

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

    appendRegionSelect(page);
    appendRadio(page, 1);
    appendRadio(page, 2);

    page += F("<h2>Bridge behaviour</h2>");
    page += F("<label><input type=\"checkbox\" name=\"positionEnabled\" value=\"1\"");
    if (BridgeConfig::positionEnabled()) page += F(" checked");
    page += F(">Bridge Meshtastic POSITION_APP packets</label>");

    page += F("<label><input type=\"checkbox\" name=\"telemetryEnabled\" value=\"1\"");
    if (BridgeConfig::telemetryEnabled()) page += F(" checked");
    page += F(">Bridge Meshtastic TELEMETRY_APP packets</label>");

    page += F("<button type=\"submit\">Save &amp; reboot</button>"
              "</form>"
              "<p class=\"hint\">After saving, the device reboots and starts "
              "bridging. Press BOOT or send any serial character within ~5 s "
              "of reset to re-enter this form.</p>");
    appendScript(page);
    page += F("</body></html>");
    return page;
}

// --- request handlers ------------------------------------------------------

static void handleRoot() {
    s_http.send(200, "text/html; charset=utf-8", renderForm());
}

static void handleCaptive() {
    s_http.sendHeader("Location", "/", true);
    s_http.send(302, "text/plain", "redirect");
}

static void fail(const char *msg) {
    s_http.send(200, "text/html; charset=utf-8", renderForm(msg));
}

// Sync word a preset protocol uses.
static uint8_t syncForProtocol(uint8_t proto) {
    if (proto == BridgeConfig::PROTO_MT)  return 0x2B;
    if (proto == BridgeConfig::PROTO_MC)  return 0x12;
    if (proto == BridgeConfig::PROTO_RNS) return 0x42;
    return 0x00;
}

// Resolve + validate one radio's protocol/RF/channel from the POSTed form,
// writing straight into BridgeConfig. Returns nullptr on success, or an
// error string to flash back.
static const char *applyRadio(int n, uint8_t region) {
    int idx = n - 1;
    String pStr = s_http.arg(String("r") + n + "proto");
    uint8_t proto = (uint8_t)pStr.toInt();

    if (proto == BridgeConfig::PROTO_NONE) {
        BridgeConfig::setRadioProtocol(idx, BridgeConfig::PROTO_NONE);
        return nullptr;                       // disabled — no RF to validate
    }

    String chName = s_http.arg(String("r") + n + "ChannelName");
    String chKey  = s_http.arg(String("r") + n + "ChannelKey");
    chName.trim();
    chKey.trim();

    String freqStr = s_http.arg(String("r") + n + "Freq");
    float  freq    = freqStr.toFloat();
    if (freq < SX1262_FREQ_MIN || freq > SX1262_FREQ_MAX)
        return "Frequency out of the SX1262 range (150-960 MHz).";

    String txStr = s_http.arg(String("r") + n + "Tx");
    int    txp   = txStr.toInt();

    float   bw = 250.0f;
    uint8_t sf = 11, cr = 5;
    uint8_t sync;

    // BW/SF/CR are read from the form for Custom AND MeshCore (MeshCore has
    // no universal presets — community-specific RF).
    if (proto == BridgeConfig::PROTO_CUSTOM || proto == BridgeConfig::PROTO_MC) {
        bw = s_http.arg(String("r") + n + "Bw").toFloat();
        sf = (uint8_t)s_http.arg(String("r") + n + "Sf").toInt();
        cr = (uint8_t)s_http.arg(String("r") + n + "Cr").toInt();
        if (!bwAllowed(bw))
            return "Bandwidth is not a valid SX1262 value.";
        if (sf < 5 || sf > 12) return "SF must be 5-12.";
        if (cr < 5 || cr > 8)  return "CR must be 5-8.";
    }

    if (proto == BridgeConfig::PROTO_CUSTOM) {
        String syncStr = s_http.arg(String("r") + n + "Sync");
        syncStr.trim();
        if (!isHexString(syncStr, 2))
            return "Custom sync word must be exactly 2 hex characters.";
        sync = (uint8_t)strtoul(syncStr.c_str(), nullptr, 16);
    } else if (proto == BridgeConfig::PROTO_MT) {
        uint8_t preset = (uint8_t)s_http.arg(String("r") + n + "preset").toInt();
        RegionPreset::modemPresetParams(preset, bw, sf, cr);
        sync = 0x2B;
        if (chName.length() == 0)
            return "Meshtastic channel name cannot be empty.";
        if (chKey.length() > 0) {
            uint8_t dec[48]; size_t decLen = 0;
            int rc = mbedtls_base64_decode(dec, sizeof(dec), &decLen,
                        (const unsigned char *)chKey.c_str(), chKey.length());
            if (rc != 0 || !(decLen == 1 || decLen == 16 || decLen == 32))
                return "Meshtastic PSK must be blank or base64 of 1/16/32 bytes.";
        }
    } else if (proto == BridgeConfig::PROTO_MC) {
        sync = 0x12;
        toLower(chKey);
        if (chName.length() == 0)
            return "MeshCore channel name cannot be empty.";
        if (!isHexString(chKey, 32))
            return "MeshCore channel key must be exactly 32 hex characters.";
    } else {                                  // Reticulum
        sync = 0x42;
        bw = 250.0f; sf = 11; cr = 5;
    }

    // Region-aware TX power cap.
    int8_t cap = SX1262_TX_MAX;
    if (RegionPreset::regionHasBand(region)) {
        int8_t rc = RegionPreset::regionInfo(region).powerLimit;
        if (rc < cap) cap = rc;
    }
    if (txp > cap) txp = cap;
    if (txp < SX1262_TX_MIN) txp = SX1262_TX_MIN;

    BridgeConfig::setRadioProtocol(idx, proto);
    BridgeConfig::setRadioFrequency(idx, freq);
    BridgeConfig::setRadioBandwidth(idx, bw);
    BridgeConfig::setRadioSf(idx, sf);
    BridgeConfig::setRadioCr(idx, cr);
    BridgeConfig::setRadioSyncWord(idx, sync);
    BridgeConfig::setRadioTxPower(idx, txp);
    if (n == 1) {
        BridgeConfig::setRadio1ChannelName(chName.c_str());
        BridgeConfig::setRadio1ChannelKey(chKey.c_str());
    } else {
        BridgeConfig::setRadio2ChannelName(chName.c_str());
        BridgeConfig::setRadio2ChannelKey(chKey.c_str());
    }
    return nullptr;
}

static void handleSave() {
    // --- Meshtastic identity ---
    String mtNodeIdRaw  = s_http.arg("mtNodeId");
    String mtNodeIdStr  = s_http.arg("mtNodeIdStr");
    String mtLongName   = s_http.arg("mtLongName");
    String mtShortName  = s_http.arg("mtShortName");

    mtNodeIdRaw.trim();
    if (mtNodeIdRaw.startsWith("0x") || mtNodeIdRaw.startsWith("0X"))
        mtNodeIdRaw = mtNodeIdRaw.substring(2);
    if (!isHexString(mtNodeIdRaw, 8)) {
        fail("Node ID must be exactly 8 hex characters.");
        return;
    }
    uint32_t mtNodeId = (uint32_t)strtoul(mtNodeIdRaw.c_str(), nullptr, 16);

    mtNodeIdStr.trim();
    if (mtNodeIdStr.length() != 9 || mtNodeIdStr[0] != '!' ||
        !isHexString(mtNodeIdStr.substring(1), 8)) {
        fail("Node ID string must be \"!\" followed by 8 hex chars.");
        return;
    }
    if ((uint32_t)strtoul(mtNodeIdStr.substring(1).c_str(), nullptr, 16) != mtNodeId) {
        fail("Node ID and Node ID string must encode the same 32-bit value.");
        return;
    }
    if (mtLongName.length() == 0 || mtShortName.length() == 0) {
        fail("Long name and short name cannot be empty.");
        return;
    }

    // --- region ---
    uint8_t region = (uint8_t)s_http.arg("region").toInt();
    if (region >= RegionPreset::kRegionCount) region = BridgeConfig::REGION_UNSET;

    // --- per-radio protocol / RF / channel ---
    const char *e1 = applyRadio(1, region);
    if (e1) { fail(e1); return; }
    const char *e2 = applyRadio(2, region);
    if (e2) { fail(e2); return; }

    uint8_t p1 = BridgeConfig::radioProtocol(0);
    uint8_t p2 = BridgeConfig::radioProtocol(1);

    // At least one radio must be active.
    if (p1 == BridgeConfig::PROTO_NONE && p2 == BridgeConfig::PROTO_NONE) {
        fail("At least one radio must have a protocol (both set to None).");
        return;
    }

    // Same-protocol relay must be between two DIFFERENT channels.
    if (p1 != BridgeConfig::PROTO_NONE && p1 == p2 &&
        (p1 == BridgeConfig::PROTO_MT || p1 == BridgeConfig::PROTO_MC)) {
        if (strcmp(BridgeConfig::radio1ChannelName(),
                   BridgeConfig::radio2ChannelName()) == 0 &&
            strcmp(BridgeConfig::radio1ChannelKey(),
                   BridgeConfig::radio2ChannelKey()) == 0) {
            fail("Both radios run the same protocol \xe2\x80\x94 their two "
                 "channels must differ (name or key).");
            return;
        }
    }
    (void)syncForProtocol;   // reserved for future per-protocol checks

    // --- commit ---
    BridgeConfig::setMtNodeId(mtNodeId);
    BridgeConfig::setMtNodeIdStr(mtNodeIdStr.c_str());
    BridgeConfig::setMtLongName(mtLongName.c_str());
    BridgeConfig::setMtShortName(mtShortName.c_str());
    BridgeConfig::setRegion(region);
    BridgeConfig::setPositionEnabled(s_http.arg("positionEnabled")  == "1");
    BridgeConfig::setTelemetryEnabled(s_http.arg("telemetryEnabled") == "1");
    BridgeConfig::save();

    Serial.println("[CaptivePortal] config saved, rebooting...");
    BridgeConfig::debugDump();

    s_http.send(200, "text/html; charset=utf-8",
                F("<!doctype html><meta http-equiv=\"refresh\" content=\"3\"><body>"
                  "<h2>Saved. Rebooting&hellip;</h2></body>"));

    delay(800);
    ESP.restart();
}

void begin() {
    char ssid[24];
    snprintf(ssid, sizeof(ssid), "LoRa-Bridge-%02X",
             (unsigned)(BridgeConfig::mtNodeId() & 0xFFu));

    Serial.printf("\n[CaptivePortal] starting SoftAP \"%s\" @ %s\n",
                  ssid, AP_IP.toString().c_str());

    WiFi.mode(WIFI_AP);
    WiFi.softAPConfig(AP_IP, AP_IP, AP_NETMASK);
    WiFi.softAP(ssid);

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

    for (;;) {
        s_dns.processNextRequest();
        s_http.handleClient();
        delay(2);
    }
}

}  // namespace CaptivePortal
