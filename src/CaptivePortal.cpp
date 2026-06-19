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
#include "LoRaWANConfig.h"   // v8.4 ABP device section (encoder builds only)

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
    const char *chName = BridgeConfig::radioChannelName(idx);
    const char *chKey  = BridgeConfig::radioChannelKey(idx);

    char buf[16];

    page += F("<h2>Radio ");
    page += n;
    page += F("</h2>");
    if (n >= 3)
        page += F("<div class=\"hint\">On the SECOND XIAO (radio co-processor), "
                  "reached over the UART crossover link.</div>");

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
        { BridgeConfig::PROTO_LORAWAN,"LoRaWAN"    },
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
    page += F("fld mt mc rns custom lw\"><label>Channel name</label>"
              "<input type=\"text\" id=\"r");
    page += n;
    page += F("name\" name=\"r");
    page += n;
    page += F("ChannelName\" maxlength=\"23\" oninput=\"updAll()\" value=\"");
    page += htmlEscape(chName);
    page += F("\"></div>");

    // CO-7: a custom Meshtastic PSK only decodes a private channel whose NAME
    // matches; surfaced as an MT-only hint right under the channel name.
    page += F("<div class=\"r");
    page += n;
    page += F("fld mt\"><div class=\"hint\">If you enter a custom PSK below, the "
              "Channel name must exactly match the private channel you are "
              "joining.</div></div>");

    // Channel key (MT/MC/Custom).
    page += F("<div class=\"r");
    page += n;
    page += F("fld mt mc custom\"><label>Channel key</label>"
              "<input type=\"text\" name=\"r");
    page += n;
    page += F("ChannelKey\" maxlength=\"47\" oninput=\"updAll()\" value=\"");
    // CO-4: a Meshtastic radio with no custom key shows the LongFast default AQ==.
    if (proto == BridgeConfig::PROTO_MT && (!chKey || chKey[0] == '\0'))
        page += F("AQ==");
    else
        page += htmlEscape(chKey);
    page += F("\"><div class=\"hint\">Meshtastic: base64 PSK \xe2\x80\x94 "
              "AQ== is the LongFast default (blank works too). "
              "MeshCore: 32 hex chars \xe2\x80\x94 the public key starts 8b... "
              "(enter your own for a private channel). "
              "Custom: per your decoder.</div></div>");

    // LoRaWAN region + channel-slot picker (CO-9/10/11). Region is persisted in
    // radio[].lwRegion; choosing a region/slot auto-fills Frequency/SF/Bandwidth
    // below (all stay editable). Coding rate is fixed at 4/5 for LoRaWAN.
    page += F("<div class=\"r");
    page += n;
    page += F("fld lw\"><label>LoRaWAN region</label><select id=\"r");
    page += n;
    page += F("lwreg\" name=\"r");
    page += n;
    page += F("lwreg\" onchange=\"lwReg(");
    page += n;
    page += F(")\">");
    {
        uint8_t curLw = BridgeConfig::radioLwRegion(idx);
        static const struct { uint8_t v; const char *name; } kLwReg[] = {
            { 0, "\xe2\x80\x94 select region \xe2\x80\x94" },
            { 1, "US915 (FSB2)" },
            { 2, "AU915 (FSB2)" },
            { 3, "AS923" },
            { 4, "EU868" },
        };
        for (auto &lr : kLwReg) {
            page += F("<option value=\"");
            page += lr.v;
            page += F("\"");
            if (lr.v == curLw) page += F(" selected");
            page += F(">");
            page += lr.name;
            page += F("</option>");
        }
    }
    page += F("</select><label>Channel slot</label><select id=\"r");
    page += n;
    page += F("lwslot\" onchange=\"lwSlot(");
    page += n;
    page += F(")\"></select>"
              "<div class=\"hint\">Pick your network's region + channel to fill "
              "Frequency / SF / Bandwidth below (still editable). Coding rate is "
              "fixed at 4/5 for LoRaWAN.</div></div>");

    // Frequency (MT/MC/RNS/Custom) — editable; JS pre-fills + flags override.
    snprintf(buf, sizeof(buf), "%.3f", freq);
    page += F("<div class=\"r");
    page += n;
    page += F("fld mt mc rns custom lw\"><label>Frequency (MHz)</label>"
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
    page += F("fld mt mc rns custom lw\"><label>TX power (dBm)</label>"
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

    // BW / SF / CR. Editable for MeshCore / Custom / LoRaWAN / Reticulum; for
    // Meshtastic they are shown READ-ONLY because the modem preset sets them
    // (CO-5). Reticulum is now user-editable, defaulting to 250/11/5 (CO-13) —
    // both RNS endpoints must match. MeshCore has no universal presets: each
    // community picks its own RF (e.g. 62.5 kHz / SF7, or 250 kHz / SF11).
    page += F("<div class=\"r");
    page += n;
    page += F("fld custom mc lw mt rns\">");
    page += F("<div class=\"hint r");
    page += n;
    page += F("fld custom mc lw rns\">Set BW/SF/CR to match the exact LoRa settings "
              "of the network you are bridging (MeshCore: your community's; "
              "LoRaWAN: your channel's; Reticulum: both ends must match).</div>");
    page += F("<div class=\"hint r");
    page += n;
    page += F("fld mt\">Bandwidth / SF / CR are set by the modem preset above "
              "(shown for reference).</div>");

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

    // Routing matrix (v8.5) — which other radios this one bridges its RX to.
    // Hidden when the radio is disabled (shares the protocol-class show/hide).
    page += F("<div class=\"r");
    page += n;
    page += F("fld mt mc rns custom lw\"><label>Bridge received traffic to</label>");
    uint8_t routeMask = BridgeConfig::radioRouteMask(idx);
    for (int j = 1; j <= BridgeConfig::NUM_RADIOS; j++) {
        if (j == n) continue;
        page += F("<label style=\"font-weight:400;display:inline-block;margin-right:1em\">"
                  "<input type=\"checkbox\" name=\"r");
        page += n;
        page += F("routeTo");
        page += j;
        page += F("\" value=\"1\"");
        if (routeMask & (uint8_t)(1u << (j - 1))) page += F(" checked");
        page += F(">R");
        page += j;
        page += F("</label>");
    }
    page += F("<div class=\"hint\">Cross-protocol translation is automatic per "
              "destination; loops are dropped by content-hash dedup.</div></div>");
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
    // Preset RF table: { presetVal: [bw_kHz, sf, cr] }. Drives the read-only
    // Meshtastic BW/SF/CR display + preset-change auto-fill (CO-5/CO-6).
    page += F("var PRE={");
    for (uint8_t p = 0; p <= RegionPreset::PRESET_LONG_TURBO; p++) {
        float pbw; uint8_t psf, pcr;
        RegionPreset::modemPresetParams(p, pbw, psf, pcr);
        char b[32];
        snprintf(b, sizeof(b), "%u:[%.1f,%u,%u],", p, pbw, (unsigned)psf, (unsigned)pcr);
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
    // LoRaWAN region -> default channel slots (CO-9/10/11). Each slot = [freqMHz,
    // SF, BW_kHz] at the region's common uplink data rate; CR is always 5.
    // US915/AU915 = FSB2 (ChirpStack us915_1/au915_1); EU868/AS923 = the
    // mandatory default channels. SF/BW stay editable for other data rates.
    page += F("var LW={"
      "1:{s:[['903.9',7,125],['904.1',7,125],['904.3',7,125],['904.5',7,125],"
            "['904.7',7,125],['904.9',7,125],['905.1',7,125],['905.3',7,125],"
            "['904.6',8,500]]},"
      "2:{s:[['916.8',7,125],['917.0',7,125],['917.2',7,125],['917.4',7,125],"
            "['917.6',7,125],['917.8',7,125],['918.0',7,125],['918.2',7,125],"
            "['917.5',8,500]]},"
      "3:{s:[['923.2',7,125],['923.4',7,125]]},"
      "4:{s:[['868.1',7,125],['868.3',7,125],['868.5',7,125]]}"
      "};");
    // Auto-fill RF defaults on protocol switch (bench 2026-06-16, owner request).
    // Switching a radio's Protocol dropdown used to keep the PRIOR protocol's
    // freq/BW/SF/CR + channel key, so the operator hand-fixed every field — which
    // caused a real bench slip: R1->MeshCore left the freq stale, it got retyped
    // as 910.575 vs the correct 910.525 (50 kHz off) -> rx-error rc=-7 -> a long
    // debug. upd(n) below now fills the new protocol's defaults on an ACTUAL
    // dropdown change (tracked via LASTP), guarded so a value the user typed for
    // the current protocol is never clobbered:
    //   - MeshCore: public key 8b3387...cd72 (BRIDGE_MC_KEY_HEX), 910.525 /
    //     BW62.5 / SF7 / CR5 (the build-flag MeshCore defaults; operator overrides
    //     freq per community).
    //   - Meshtastic: blank PSK (LongFast); freq stays preset-computed.
    //   - Custom / LoRaWAN: left manual (community/channel-specific by design).
    page += F(
      "var PC=[0,0,0,0];"                    // last computed freq per radio
      "var LASTP=[null,null,null,null];"     // last protocol per radio (autofill-on-change)
      "var LR=[null,null,null,null];"        // last LoRaWAN region per radio (slot-list sync)
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
      "function lwFill(n,reg,slot){if(!LW[reg]||!LW[reg].s[slot])return;"
        "var s=LW[reg].s[slot];"
        "var ff=document.getElementById('r'+n+'freq');"
        "var bw=document.getElementsByName('r'+n+'Bw')[0];"
        "var sf=document.getElementsByName('r'+n+'Sf')[0];"
        "var cr=document.getElementsByName('r'+n+'Cr')[0];"
        "if(ff){ff.value=s[0];PC[n-1]=s[0];}"
        "if(sf)sf.value=s[1];if(bw)bw.value=s[2].toFixed(1);if(cr)cr.value='5';}"
      "function lwOpts(n){var reg=gv('r'+n+'lwreg');"
        "var sl=document.getElementById('r'+n+'lwslot');if(!sl)return;"
        "sl.innerHTML='';"
        "var o=document.createElement('option');o.value='-1';"
        "o.text='\\u2014 keep current \\u2014';sl.appendChild(o);"
        "if(LW[reg]){var a=LW[reg].s;for(var i=0;i<a.length;i++){"
          "var op=document.createElement('option');op.value=i;"
          "op.text=a[i][0]+' MHz \\u00b7 SF'+a[i][1]+'/'+a[i][2]+'k';"
          "sl.appendChild(op);}}}"
      "function lwReg(n){lwOpts(n);LR[n-1]=gv('r'+n+'lwreg');"
        "var sl=document.getElementById('r'+n+'lwslot');"
        "if(sl&&sl.options.length>1){sl.selectedIndex=1;lwFill(n,gv('r'+n+'lwreg'),0);}}"
      "function lwSlot(n){var s=parseInt(gv('r'+n+'lwslot'),10);"
        "if(s>=0)lwFill(n,gv('r'+n+'lwreg'),s);}"
      "function upd(n){var p=gv('r'+n+'proto');"
        "var ck=document.getElementsByName('r'+n+'ChannelKey')[0];"
        "var bw=document.getElementsByName('r'+n+'Bw')[0];"
        "var sf=document.getElementsByName('r'+n+'Sf')[0];"
        "var cr=document.getElementsByName('r'+n+'Cr')[0];"
        "var nm=document.getElementsByName('r'+n+'ChannelName')[0];"
        "var ff=document.getElementById('r'+n+'freq');"
        "var ps=gv('r'+n+'preset');"
        // Fill the new protocol's RF defaults ONLY when the dropdown actually
        // changes (not on page load or channel-name keystrokes), so a value the
        // user typed for the current protocol is never overwritten.
        "if(LASTP[n-1]!==null&&LASTP[n-1]!==p){"
          "if(p==='2'){"                       // MeshCore: build-flag defaults
            "if(ck)ck.value='8b3387e9c5cdea6ac9e5edbaa115cd72';"
            "if(bw)bw.value='62.5';if(sf)sf.value='7';if(cr)cr.value='5';"
            "if(ff){ff.value='910.525';PC[n-1]='910.525';}"   // sync PC so a later MT switch recomputes
          "}else if(p==='1'){"                 // Meshtastic: default LongFast PSK (CO-4)
            "if(ck)ck.value='AQ==';"
          "}else if(p==='3'){"                 // Reticulum: RNode defaults (914.875/125/8/5)
            "if(bw)bw.value='125.0';if(sf)sf.value='8';if(cr)cr.value='5';"
            "if(ff){ff.value='914.875';PC[n-1]='914.875';}"
          "}"
        "}"
        "LASTP[n-1]=p;"
        // CO-5/CO-6: Meshtastic BW/SF/CR follow the modem preset and are shown
        // read-only; every other protocol keeps them editable.
        "var mt=(p==='1');"
        "if(mt&&PRE[ps]){if(bw)bw.value=PRE[ps][0].toFixed(1);"
          "if(sf)sf.value=PRE[ps][1];if(cr)cr.value=PRE[ps][2];}"
        "if(bw)bw.readOnly=mt;if(sf)sf.readOnly=mt;if(cr)cr.readOnly=mt;"
        // CO-8/CO-14: channel name is locked to N/A for LoRaWAN and defaults to
        // N/A (editable) for Custom; cleared when returning to a named protocol.
        "if(nm){var ckv=ck?ck.value:'';"
          "if(p==='5'){nm.value='N/A';nm.readOnly=true;nm.classList.add('na');}"
          "else if(p==='1'){"        // Meshtastic: name follows the preset until the key is custom
            "if(ckv===''||ckv==='AQ=='){nm.value=PN[ps];nm.readOnly=true;nm.classList.add('na');}"
            "else{nm.readOnly=false;nm.classList.remove('na');}}"
          "else if(p==='2'){"        // MeshCore: name locked to 'public' until the key is custom
            "if(ckv.toLowerCase()==='8b3387e9c5cdea6ac9e5edbaa115cd72'){"
              "nm.value='public';nm.readOnly=true;nm.classList.add('na');}"
            "else{nm.readOnly=false;nm.classList.remove('na');}}"
          "else{nm.readOnly=false;nm.classList.remove('na');"
            "if(nm.value==='N/A'&&p!=='4')nm.value='';"
            "if(p==='4'&&nm.value==='')nm.value='N/A';}"
        "}"
        "var tok={'1':'mt','2':'mc','3':'rns','4':'custom','5':'lw','0':'none'}[p];"
        "var fl=document.querySelectorAll('.r'+n+'fld');"
        "for(var i=0;i<fl.length;i++){"
          "var sh=(p!=='0')&&fl[i].classList.contains(tok);"
          "fl[i].style.display=sh?'':'none';}"
        "var fh=document.getElementById('r'+n+'fhint');"
        "if(p==='1'){setF(n,slot(gv('region'),PN[ps],PRE[ps][0]),'computed');}"
        "else if(p==='3'){fh.textContent="
          "'Reticulum: enter the exact frequency / SF / BW \\u2014 both ends must match.';}"
        "else if(p==='2'){fh.textContent="
          "'MeshCore: enter the exact frequency your community uses.';}"
        "else if(p==='5'){fh.textContent="
          "'LoRaWAN: enter the exact channel frequency + SF your devices use.';"
          "if(LR[n-1]!==gv('r'+n+'lwreg')){lwOpts(n);LR[n-1]=gv('r'+n+'lwreg');}}"
        "else{fh.textContent='';}}"
      "function updAll(){upd(1);upd(2);upd(3);upd(4);}"
      "window.addEventListener('load',updAll);"
      "</script>");
}

#if defined(BRIDGE_LW_ENCODE) && BRIDGE_LW_ENCODE
// --- v8.4 LoRaWAN ABP device section (only in encoder-enabled builds) -------
static int lwHexNib(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
static bool lwHexToBytes(const String &s, uint8_t *out, size_t n) {
    if (s.length() != n * 2) return false;
    for (size_t i = 0; i < n; i++) {
        int hi = lwHexNib(s[2 * i]); int lo = lwHexNib(s[2 * i + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return true;
}
static void lwHexFromBytes(const uint8_t *b, size_t n, char *out) {
    for (size_t i = 0; i < n; i++) snprintf(out + 2 * i, 3, "%02x", b[i]);
}

static void appendLoRaWANDevices(String &page) {
    page += F("<h2>LoRaWAN ABP devices (v8.4 encoder)</h2>"
              "<div class=\"hint\">Mesh / Custom traffic routed to a LoRaWAN radio is "
              "encoded as an ABP uplink under the first matching device below. Provision "
              "the SAME DevAddr + keys in your LNS (ChirpStack: MAC 1.0.x, ABP, Class A, "
              "ADR off, disable frame-counter validation or persist). Disabled slots are "
              "skipped; build-flag creds are the fallback when no slot matches.</div>"
              "<div class=\"hint\">Each slot maps a traffic <b>source</b> \xe2\x80\x94 any "
              "source, one Meshtastic node id, or one source protocol \xe2\x80\x94 to its own "
              "ChirpStack device; the first enabled slot that matches a packet wins. The four "
              "slots let you forward different senders as different LoRaWAN devices. To use a "
              "slot, tick Enabled and fill DevAddr + NwkSKey + AppSKey. See the user manual "
              "for a full walkthrough.</div>");
    char hx[40];
    for (size_t i = 0; i < LoRaWANConfig::deviceCount(); i++) {
        const LoRaWANConfig::Device &d = LoRaWANConfig::device((int)i);

        page += F("<h3 style=\"margin:.9em 0 .2em;font-size:.95em\">Device ");
        page += (int)i;
        page += F("</h3>");

        page += F("<label><input type=\"checkbox\" name=\"lw");
        page += (int)i;
        page += F("en\" value=\"1\"");
        if (d.inUse) page += F(" checked");
        page += F(">Enabled</label>");

        page += F("<label>Applies to source</label><select name=\"lw");
        page += (int)i;
        page += F("sel\">");
        static const char *kSel[] = { "Any source (default)",
                                      "Meshtastic node id", "Source protocol" };
        for (uint8_t s = 0; s < 3; s++) {
            page += F("<option value=\"");
            page += (int)s;
            page += F("\"");
            if (s == d.srcSel) page += F(" selected");
            page += F(">");
            page += kSel[s];
            page += F("</option>");
        }
        page += F("</select>");

        snprintf(hx, sizeof(hx), "%lu", (unsigned long)d.srcMatch);
        page += F("<label>Match value</label><input type=\"text\" name=\"lw");
        page += (int)i;
        page += F("match\" value=\"");
        if (d.srcSel != LoRaWANConfig::SRC_ANY) page += hx;
        page += F("\"><div class=\"hint\">node-id: 8 hex (e.g. b16b00b5); "
                  "protocol: 1=MT 2=MC 3=RNS 4=Custom; ignored for Any.</div>");

        snprintf(hx, sizeof(hx), "%08lX", (unsigned long)d.devAddr);
        page += F("<label>DevAddr (8 hex)</label><input type=\"text\" name=\"lw");
        page += (int)i;
        page += F("addr\" maxlength=\"8\" value=\"");
        if (d.devAddr) page += hx;
        page += F("\">");

        lwHexFromBytes(d.nwkSKey, 16, hx);
        page += F("<label>NwkSKey (32 hex)</label><input type=\"text\" name=\"lw");
        page += (int)i;
        page += F("nwk\" maxlength=\"32\" value=\"");
        if (d.inUse) page += hx;
        page += F("\">");

        lwHexFromBytes(d.appSKey, 16, hx);
        page += F("<label>AppSKey (32 hex)</label><input type=\"text\" name=\"lw");
        page += (int)i;
        page += F("app\" maxlength=\"32\" value=\"");
        if (d.inUse) page += hx;
        page += F("\">");

        page += F("<label>FPort (1-223)</label><input type=\"text\" name=\"lw");
        page += (int)i;
        page += F("port\" value=\"");
        page += (unsigned)(d.fport ? d.fport : 13);
        page += F("\">");

        page += F("<label><input type=\"checkbox\" name=\"lw");
        page += (int)i;
        page += F("tag\" value=\"1\"");
        if (d.flags & LoRaWANConfig::FLAG_TAG_SRC) page += F(" checked");
        page += F(">Prepend source tag [proto][srcId] to FRMPayload (multiplexed codec)</label>");
    }
}

// Parse + validate the ABP device form into LoRaWANConfig. Returns nullptr on
// success (and persists the table), or an error string to flash back.
static const char *applyLoRaWANDevices() {
    for (size_t i = 0; i < LoRaWANConfig::deviceCount(); i++) {
        String pre = String("lw") + (int)i;
        if (s_http.arg(pre + "en") != "1") { LoRaWANConfig::clearDevice((int)i); continue; }

        LoRaWANConfig::Device d = {};
        d.inUse  = 1;
        d.srcSel = (uint8_t)s_http.arg(pre + "sel").toInt();

        String mv = s_http.arg(pre + "match"); mv.trim();
        if (d.srcSel == LoRaWANConfig::SRC_MT_NODE) {
            if (mv.startsWith("0x") || mv.startsWith("0X")) mv = mv.substring(2);
            if (!isHexString(mv, 8)) return "ABP node-id match must be 8 hex chars.";
            d.srcMatch = (uint32_t)strtoul(mv.c_str(), nullptr, 16);
        } else if (d.srcSel == LoRaWANConfig::SRC_PROTO) {
            d.srcMatch = (uint32_t)mv.toInt();
            if (d.srcMatch < 1 || d.srcMatch > 4) return "ABP protocol match must be 1-4.";
        } else {
            d.srcMatch = 0;
        }

        String addr = s_http.arg(pre + "addr"); addr.trim();
        if (addr.startsWith("0x") || addr.startsWith("0X")) addr = addr.substring(2);
        if (!isHexString(addr, 8)) return "ABP DevAddr must be 8 hex chars.";
        d.devAddr = (uint32_t)strtoul(addr.c_str(), nullptr, 16);

        String nwk = s_http.arg(pre + "nwk"); nwk.trim();
        String app = s_http.arg(pre + "app"); app.trim();
        if (!lwHexToBytes(nwk, d.nwkSKey, 16)) return "ABP NwkSKey must be 32 hex chars.";
        if (!lwHexToBytes(app, d.appSKey, 16)) return "ABP AppSKey must be 32 hex chars.";

        int fp = s_http.arg(pre + "port").toInt();
        if (fp < 1 || fp > 223) return "ABP FPort must be 1-223.";
        d.fport = (uint8_t)fp;
        if (s_http.arg(pre + "tag") == "1") d.flags |= LoRaWANConfig::FLAG_TAG_SRC;

        LoRaWANConfig::setDevice((int)i, d);
    }
    LoRaWANConfig::saveTable();
    return nullptr;
}
#endif  // BRIDGE_LW_ENCODE

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
              "input.na{background:#eee;color:#777;cursor:not-allowed}"
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

    // Bridge behaviour — Meshtastic-specific, so grouped with identity/region in
    // the top frame (CO-1), with a note about its scope (CO-2).
    page += F("<h2>Bridge behaviour</h2>");
    page += F("<div class=\"hint\">Applies only to radios set to the Meshtastic "
              "protocol.</div>");
    page += F("<label><input type=\"checkbox\" name=\"positionEnabled\" value=\"1\"");
    if (BridgeConfig::positionEnabled()) page += F(" checked");
    page += F(">Bridge Meshtastic POSITION_APP packets</label>");
    page += F("<label><input type=\"checkbox\" name=\"telemetryEnabled\" value=\"1\"");
    if (BridgeConfig::telemetryEnabled()) page += F(" checked");
    page += F(">Bridge Meshtastic TELEMETRY_APP packets</label>");

    for (int n = 1; n <= BridgeConfig::NUM_RADIOS; n++)
        appendRadio(page, n);
#if defined(BRIDGE_LW_ENCODE) && BRIDGE_LW_ENCODE
    appendLoRaWANDevices(page);
#endif

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
    if (proto == BridgeConfig::PROTO_LORAWAN) return 0x34;
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
    if (proto == BridgeConfig::PROTO_CUSTOM || proto == BridgeConfig::PROTO_MC ||
        proto == BridgeConfig::PROTO_LORAWAN || proto == BridgeConfig::PROTO_RNS) {
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
    } else if (proto == BridgeConfig::PROTO_LORAWAN) {
        // Keyless: sync fixed at the LoRaWAN public value; BW/SF/CR were read
        // above; no channel key. Channel name is an optional display label.
        sync  = 0x34;
        chKey = "";
        // CO-9: persist the chosen LoRaWAN region (the region/slot picker already
        // filled Frequency/SF/BW, validated above).
        uint8_t lwReg = (uint8_t)s_http.arg(String("r") + n + "lwreg").toInt();
        if (lwReg > BridgeConfig::LW_REGION_EU868) lwReg = BridgeConfig::LW_REGION_UNSET;
        BridgeConfig::setRadioLwRegion(idx, lwReg);
    } else {                                  // Reticulum
        // CO-13: BW/SF/CR are now taken from the form (read + validated above),
        // defaulting to 250/11/5. Both RNS endpoints must use the same plan.
        sync = 0x42;
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
    BridgeConfig::setRadioChannelName(idx, chName.c_str());
    BridgeConfig::setRadioChannelKey(idx, chKey.c_str());

    // Routing matrix (v8.5): which other radios this one bridges its RX to.
    uint8_t routeMask = 0;
    for (int j = 1; j <= BridgeConfig::NUM_RADIOS; j++) {
        if (j == n) continue;
        if (s_http.arg(String("r") + n + "routeTo" + j) == "1")
            routeMask |= (uint8_t)(1u << (j - 1));
    }
    BridgeConfig::setRadioRouteMask(idx, routeMask);
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

    // --- per-radio protocol / RF / channel / routing (all NUM_RADIOS radios) ---
    for (int n = 1; n <= BridgeConfig::NUM_RADIOS; n++) {
        const char *e = applyRadio(n, region);
        if (e) { fail(e); return; }
    }

    // At least one radio must be active.
    int activeCount = 0;
    for (int i = 0; i < BridgeConfig::NUM_RADIOS; i++)
        if (BridgeConfig::radioProtocol(i) != BridgeConfig::PROTO_NONE) activeCount++;
    if (activeCount == 0) {
        fail("At least one radio must have a protocol (all set to None).");
        return;
    }

    // Same-protocol SELF-bridge guard (freq-aware — V8.2-SPEC §2 A4 / §5.1),
    // routeMask-aware: reject a ROUTED pair (i -> j) of same-protocol MT/MC radios
    // only when they share channel name+key AND frequency — i.e. literally the
    // same RF channel bridged to itself. Same channel on a DIFFERENT frequency is
    // a valid cross-frequency relay (loop-safe via the content-hash dedup), and
    // cross-band same-protocol relay between two distinct radios is allowed.
    for (int i = 0; i < BridgeConfig::NUM_RADIOS; i++) {
        uint8_t pi = BridgeConfig::radioProtocol(i);
        if (pi != BridgeConfig::PROTO_MT && pi != BridgeConfig::PROTO_MC) continue;
        uint8_t mask = BridgeConfig::radioRouteMask(i);
        for (int j = 0; j < BridgeConfig::NUM_RADIOS; j++) {
            if (j == i) continue;
            if (!(mask & (uint8_t)(1u << j))) continue;
            if (BridgeConfig::radioProtocol(j) != pi) continue;
            float df = BridgeConfig::radioFrequency(i) - BridgeConfig::radioFrequency(j);
            if (df < 0) df = -df;
            if (df < 0.001f &&
                strcmp(BridgeConfig::radioChannelName(i),
                       BridgeConfig::radioChannelName(j)) == 0 &&
                strcmp(BridgeConfig::radioChannelKey(i),
                       BridgeConfig::radioChannelKey(j)) == 0) {
                char sbMsg[128];
                snprintf(sbMsg, sizeof(sbMsg),
                         "Radio %d routes to Radio %d on the same channel AND "
                         "frequency \xe2\x80\x94 that bridges a channel to itself.",
                         i + 1, j + 1);
                fail(sbMsg);
                return;
            }
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

#if defined(BRIDGE_LW_ENCODE) && BRIDGE_LW_ENCODE
    const char *eLw = applyLoRaWANDevices();   // validates + persists the ABP table
    if (eLw) { fail(eLw); return; }
#endif

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
