// CamPortal.cpp — see CamPortal.h.

#include "CamPortal.h"
#if defined(BRIDGE_ROLE_CAMERA)

#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <string.h>

#include "CaptivePortal.h"     // serverRef() + serveConfigForm()/serveConfigSave()
#include "CamPortalConfig.h"   // AP passphrase + login
#include "CamC2.h"             // executeCommand / messaging / Cmd enum
#include "CamC2Config.h"       // peer whitelist (pairing)
#include "CameraNode.h"        // ready() / lockCamera()
#include "CamStream.h"         // :81 MJPEG server (own TU — HTTP_GET enum clash)
#include "BridgeConfig.h"      // mtNodeId() for the SSID + the C2 radio index
#include "SerialLog.h"

namespace CamPortal {

// The C2 channel rides Radio 2 (the edge Custom-0x33 radio) — R1 is disabled in a
// camera build. Index 1 == "R2" for emit/sendMessage/sendCommand.
static constexpr int    C2_RADIO   = 1;
static constexpr uint32_t SESS_TTL = 1800;      // session idle lifetime (seconds)

static const IPAddress AP_IP(192, 168, 4, 1);
static const IPAddress AP_NETMASK(255, 255, 255, 0);

static DNSServer     s_dns;
static bool          s_up = false;

// --- sessions --------------------------------------------------------------
struct Session { uint8_t tok[16]; uint32_t expSec; bool used; };
static Session s_sess[2];

static inline uint32_t nowSec() { return (uint32_t)(millis() / 1000UL); }

static bool ctEqual(const uint8_t *a, const uint8_t *b, size_t n) {
    uint8_t d = 0;
    for (size_t i = 0; i < n; i++) d |= (uint8_t)(a[i] ^ b[i]);
    return d == 0;
}

static int hexNib(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
static bool hexToBytes(const char *s, uint8_t *out, size_t n) {
    if (!s || strlen(s) != n * 2) return false;
    for (size_t i = 0; i < n; i++) {
        int hi = hexNib(s[2 * i]), lo = hexNib(s[2 * i + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return true;
}
static void bytesToHex(const uint8_t *b, size_t n, char *out) {
    static const char *H = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) { out[2 * i] = H[b[i] >> 4]; out[2 * i + 1] = H[b[i] & 0xF]; }
    out[2 * n] = '\0';
}

// Find a live session by its hex token (refreshing its expiry). Used by both the
// :80 cookie path and the :81 stream ?sid= query.
static Session *findSession(const char *hextok) {
    uint8_t tok[16];
    if (!hexToBytes(hextok, tok, 16)) return nullptr;
    uint32_t now = nowSec();
    for (Session &s : s_sess) {
        if (s.used && s.expSec > now && ctEqual(s.tok, tok, 16)) {
            s.expSec = now + SESS_TTL;
            return &s;
        }
    }
    return nullptr;
}

static Session *pickSlot() {
    uint32_t now = nowSec();
    Session *oldest = &s_sess[0];
    for (Session &s : s_sess) {
        if (!s.used || s.expSec <= now) return &s;
        if (s.expSec < oldest->expSec) oldest = &s;
    }
    return oldest;   // all live → evict the soonest-to-expire
}

// --- :80 helpers -----------------------------------------------------------
static WebServer &srv() { return CaptivePortal::serverRef(); }

static String htmlEsc(const String &in) {
    String o; o.reserve(in.length() + 8);
    for (size_t i = 0; i < in.length(); i++) {
        char c = in[i];
        switch (c) {
            case '&': o += "&amp;"; break;
            case '<': o += "&lt;";  break;
            case '>': o += "&gt;";  break;
            case '"': o += "&quot;"; break;
            case '\'': o += "&#39;"; break;
            default: o += c;
        }
    }
    return o;
}

// Pull the sid hex out of the request Cookie header ("" if none).
static String cookieSid() {
    String c = srv().header("Cookie");
    int i = c.indexOf("sid=");
    if (i < 0) return String();
    i += 4;
    int j = c.indexOf(';', i);
    String hx = (j < 0) ? c.substring(i) : c.substring(i, j);
    hx.trim();
    return hx;
}

static Session *currentSession() {
    String hx = cookieSid();
    if (hx.length() != 32) return nullptr;
    return findSession(hx.c_str());
}

static const char *STYLE =
    "<style>body{font-family:system-ui,Arial,sans-serif;max-width:640px;margin:1em auto;"
    "padding:0 1em;color:#111}h1{font-size:1.2em}h2{font-size:1em;border-bottom:1px solid #ccc;"
    "margin-top:1.4em}a{color:#06c}label{display:block;margin:.5em 0 .15em;font-weight:600}"
    "input,select{width:100%;padding:.4em;box-sizing:border-box;font-family:ui-monospace,monospace}"
    "input[type=checkbox]{width:auto}button{margin:.3em .3em .3em 0;padding:.5em 1em;background:#06c;"
    "color:#fff;border:0;border-radius:4px;cursor:pointer}.nav a{margin-right:1em}.warn{padding:.5em;"
    "background:#fff0f0;border:1px solid #d08080;border-radius:4px;color:#a00;font-size:.9em}"
    ".ok{padding:.5em;background:#eef7ee;border:1px solid #8c8;border-radius:4px}"
    "img{max-width:100%;border:1px solid #999;border-radius:4px}pre{white-space:pre-wrap}"
    ".msg{padding:.3em .5em;border-bottom:1px solid #eee;font-size:.9em}.t{color:#888}</style>";

static void sendPage(const String &bodyInner) {
    String p; p.reserve(bodyInner.length() + 1024);
    p += F("<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\">"
           "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
           "<title>LoRaCam</title>");
    p += STYLE;
    p += F("</head><body>");
    p += bodyInner;
    p += F("</body></html>");
    srv().send(200, "text/html; charset=utf-8", p);
}

// Redirect to /login if not authenticated. Returns true when a live session exists.
static bool requireAuth() {
    if (currentSession()) return true;
    srv().sendHeader("Location", "/login");
    srv().send(302, "text/plain", "login");
    return false;
}

// Auth for media GETs the browser may fetch via its download manager, which can
// drop the session cookie (a .avi forces a download handoff — observed on Android:
// the cookie-less request 302'd to /login and the login HTML got saved as the
// file). Accept EITHER a live cookie session OR a valid ?sid= query token — the
// same scheme the :81 stream uses. On failure, 401 (not a 302) so a bad download
// fails loudly instead of silently saving the login page.
static bool requireAuthMedia() {
    if (currentSession()) return true;
    String q = srv().arg("sid");
    if (q.length() == 32 && sessionValid(q.c_str())) return true;
    srv().send(401, "text/plain", "login required");
    return false;
}

static String navBar() {
    return F("<div class=\"nav\"><a href=\"/\">Home</a><a href=\"/photos\">Photos</a>"
             "<a href=\"/config\">Config</a>"
             "<a href=\"/peers\">Pairing</a><a href=\"/sec\">Security</a>"
             "<a href=\"/logout\">Log out</a></div><hr>");
}

// --- login -----------------------------------------------------------------
static void sendLoginForm(const char *flash) {
    String b = F("<h1>LoRaCam \xe2\x80\x94 sign in</h1>");
    if (flash) { b += F("<div class=\"warn\">"); b += flash; b += F("</div>"); }
    b += F("<form method=\"POST\" action=\"/login\">"
           "<label>Username</label><input name=\"user\" autocomplete=\"username\">"
           "<label>Password</label><input name=\"pass\" type=\"password\" autocomplete=\"current-password\">"
           "<button type=\"submit\">Sign in</button></form>");
    sendPage(b);
}
static void handleLoginForm() { sendLoginForm(nullptr); }

static void handleLoginPost() {
    String u = srv().arg("user"), p = srv().arg("pass");
    if (!CamPortalConfig::checkLogin(u.c_str(), p.c_str())) {
        SerialLog::logf("[CamPortal] login FAIL user=%s\n", u.c_str());
        sendLoginForm("Wrong username or password.");
        return;
    }
    Session *s = pickSlot();
    for (int i = 0; i < 16; i++) s->tok[i] = (uint8_t)esp_random();
    s->expSec = nowSec() + SESS_TTL;
    s->used = true;
    char hx[33]; bytesToHex(s->tok, 16, hx);
    String cookie = String("sid=") + hx + "; Path=/; Max-Age=" + String(SESS_TTL);
    srv().sendHeader("Set-Cookie", cookie);
    srv().sendHeader("Location", "/");
    srv().send(302, "text/plain", "ok");
    SerialLog::logf("[CamPortal] login OK user=%s\n", u.c_str());
}

static void handleLogout() {
    Session *s = currentSession();
    if (s) s->used = false;
    srv().sendHeader("Set-Cookie", "sid=; Path=/; Max-Age=0");
    srv().sendHeader("Location", "/login");
    srv().send(302, "text/plain", "bye");
}

// --- home (video + camera control + messaging) -----------------------------
static String messagesFragment() {
    String f;
    size_t n = CamC2::messageCount();
    if (n == 0) return F("<div class=\"t\">No messages yet.</div>");
    for (size_t i = 0; i < n; i++) {
        const CamC2::Message &m = CamC2::message((int)i);
        char hdr[40];
        snprintf(hdr, sizeof(hdr), "0x%08lx", (unsigned long)m.fromId);
        f += F("<div class=\"msg\"><span class=\"t\">");
        f += hdr;
        f += F(" @");
        f += String(m.atSec);
        f += F("s</span><br>");
        f += htmlEsc(String(m.text));
        f += F("</div>");
    }
    return f;
}

static String peerOptions() {
    String o;
    for (size_t i = 0; i < CamC2Config::peerCount(); i++) {
        const CamC2Config::Peer &pr = CamC2Config::peer((int)i);
        if (!pr.inUse || pr.peerId == 0) continue;
        char id[16]; snprintf(id, sizeof(id), "%08lx", (unsigned long)pr.peerId);
        o += F("<option value=\"");
        o += id;
        o += F("\">0x");
        o += id;
        if (pr.flags & CamC2Config::FLAG_PRIMARY_MASTER) o += F(" (primary)");
        o += F("</option>");
    }
    return o;
}

static void handleHome() {
    if (!requireAuth()) return;
    String sid = cookieSid();

    String b = F("<h1>LoRaCam</h1>");
    b += navBar();
    if (CamPortalConfig::usingDefaults())
        b += F("<div class=\"warn\">Portal is using DEFAULT credentials \xe2\x80\x94 set your own "
               "WiFi passphrase + login on the <a href=\"/sec\">Security</a> page.</div>");

    // Live video. onerror auto-reconnects if the stream connection ever ends
    // (bench P3-28: a frozen <img> otherwise needs a manual page refresh).
    b += F("<h2>Live video</h2>");
    if (CameraNode::ready()) {
        b += F("<img id=\"live\" src=\"http://192.168.4.1:81/stream?sid=");
        b += sid;
        b += F("\" alt=\"live\" onerror=\"vidRetry()\">");
    } else {
        b += F("<div class=\"warn\">Camera not detected (no Sense daughterboard?).</div>");
    }

    // Camera control (fetch — avoids reloading the page / killing the stream).
    b += F("<h2>Camera</h2>"
           "<button onclick=\"cmd('snap')\">Snap photo</button>"
           "<button onclick=\"cmd('record')\">Record 30 s</button>"
           "<button onclick=\"cmd('stop')\">Stop</button>"
           "<button onclick=\"cmd('status')\">Status</button>"
           "<pre id=\"out\" class=\"ok\">ready</pre>");

    // Messaging.
    b += F("<h2>Messages</h2><div id=\"msgs\">");
    b += messagesFragment();
    b += F("</div>");
    String opts = peerOptions();
    if (opts.length()) {
        b += F("<form onsubmit=\"return sendMsg()\"><label>To</label><select id=\"to\">");
        b += opts;
        b += F("</select><label>Message</label><input id=\"mt\" maxlength=\"48\">"
               "<button type=\"submit\">Send</button></form>");
    } else {
        b += F("<div class=\"t\">Add a paired master on the <a href=\"/peers\">Pairing</a> "
               "page to send messages.</div>");
    }

    b += F("<script>"
           "function vidReload(){var v=document.getElementById('live');"
           "if(v){v.src=v.src.split('&r=')[0]+'&r='+Date.now();}}"
           "function vidRetry(){setTimeout(vidReload,1500);}"
           // Android/Chrome kills the MJPEG connection when the screen sleeps and
           // the <img> never re-requests it on wake (no error event fires) — bench
           // finding on a Pixel. Reload the stream whenever the page becomes
           // visible again (screen wake / tab return).
           "document.addEventListener('visibilitychange',function(){"
           "if(!document.hidden)vidReload();});"
           "function cmd(a){fetch('/cmd',{method:'POST',headers:{'Content-Type':"
           "'application/x-www-form-urlencoded'},body:'action='+a})"
           ".then(r=>r.text()).then(t=>document.getElementById('out').textContent=t)"
           ".catch(e=>document.getElementById('out').textContent='err '+e);}"
           "function refMsgs(){fetch('/msg').then(r=>r.text()).then(t=>"
           "document.getElementById('msgs').innerHTML=t);}"
           "function sendMsg(){var to=document.getElementById('to').value;"
           "var m=document.getElementById('mt').value;"
           "fetch('/msg',{method:'POST',headers:{'Content-Type':"
           "'application/x-www-form-urlencoded'},body:'to='+to+'&text='+encodeURIComponent(m)})"
           ".then(r=>r.text()).then(_=>{document.getElementById('mt').value='';refMsgs();});"
           "return false;}"
           "setInterval(refMsgs,5000);"
           "</script>");
    sendPage(b);
}

// --- camera command (local trust: gated by the portal login, not the LoRa s_ok
//     latch — the camera works regardless of C2 crypto state; LORACAM-SPEC §Phase3)
static void handleCmd() {
    if (!requireAuth()) return;
    String a = srv().arg("action");
    uint8_t cmd = CamC2::CMD_NONE, args[2]; size_t al = 0;
    if      (a == "snap")   cmd = CamC2::CMD_START_CAPTURE;
    else if (a == "record") { cmd = CamC2::CMD_RECORD; args[0] = 30; args[1] = 0; al = 2; }
    else if (a == "stop")   cmd = CamC2::CMD_STOP;
    else if (a == "status") cmd = CamC2::CMD_GET_STATUS;
    else { srv().send(400, "text/plain", "bad action"); return; }

    uint8_t ack[CamC2::MAX_PAYLOAD]; size_t ackLen = 0;
    uint8_t res = CamC2::executeCommand(cmd, al ? args : nullptr, al, ack, sizeof(ack), ackLen);

    String out = "result=" + String(res) + (res == 0 ? " (ok)" : " (error)");
    if ((cmd == CamC2::CMD_START_CAPTURE || cmd == CamC2::CMD_RECORD ||
         cmd == CamC2::CMD_STOP) && res == 0 && ackLen > 1) {
        out += (cmd == CamC2::CMD_STOP) ? "\nstate=" : "\nfile=";
        for (size_t i = 1; i < ackLen; i++) out += (char)ack[i];
        if (cmd == CamC2::CMD_RECORD)
            out += "\n(recording in the background — see Photos when done)";
    } else if (cmd == CamC2::CMD_GET_STATUS && ackLen >= 8) {
        uint32_t up = (uint32_t)ack[3] | ((uint32_t)ack[4] << 8) |
                      ((uint32_t)ack[5] << 16) | ((uint32_t)ack[6] << 24);
        out += "\nbattery=" + String(ack[1]) + "%  sd=" +
               (ack[2] == 0xFF ? String("none") : String(ack[2]) + "% free") +
               "  uptime=" + String(up) + "s  lastCmd=" + String(ack[7]);
    }
    srv().send(200, "text/plain", out);
}

// --- messaging endpoints ----------------------------------------------------
static void handleMsgGet() {
    if (!requireAuth()) return;
    srv().send(200, "text/html; charset=utf-8", messagesFragment());
}

static void handleMsgPost() {
    if (!requireAuth()) return;
    String toStr = srv().arg("to"); toStr.trim();
    String text  = srv().arg("text");
    uint32_t to = (uint32_t)strtoul(toStr.c_str(), nullptr, 16);
    if (to == 0 || text.length() == 0) { srv().send(400, "text/plain", "bad msg"); return; }
    bool ok = CamC2::sendMessage(C2_RADIO, to, text.c_str());
    srv().send(ok ? 200 : 500, "text/plain", ok ? "sent" : "send-failed");
}

// --- Photos tab (Phase 2b — browse/view/delete the microSD captures) --------
static void handlePhotosGet() {
    if (!requireAuth()) return;
    String sid = cookieSid();     // embed in media links (survives a download handoff)
    String b = F("<h1>Photos</h1>");
    b += navBar();
    if (!CameraNode::sdPresent()) {
        b += F("<div class=\"warn\">No microSD card mounted. Insert a FAT32 card "
               "(32 GB or smaller) and reboot.</div>");
        sendPage(b);
        return;
    }
    static const size_t CAP = 48;
    CameraNode::PhotoInfo list[CAP];
    size_t n = CameraNode::photoList(list, CAP);
    b += F("<p class=\"t\">Card: ");
    b += String((int)CameraNode::sdFreePct());
    b += F("% free \xc2\xb7 newest ");
    b += String((int)n);
    b += F(" shown (cap ");
    b += String((int)CAP);
    b += F(").</p>");
    if (n == 0) {
        b += F("<div class=\"t\">No photos yet \xe2\x80\x94 use Snap on the Home "
               "page or a LoRa capture command.</div>");
    }
    for (size_t i = 0; i < n; i++) {
        bool vid = list[i].vid != 0;
        char nm[20], kb[16];
        snprintf(nm, sizeof(nm), vid ? "VID_%05u.avi" : "IMG_%05u.jpg",
                 (unsigned)list[i].idx);
        if (list[i].bytes >= 1024 * 1024)
            snprintf(kb, sizeof(kb), "%.1f MB", list[i].bytes / (1024.0f * 1024.0f));
        else
            snprintf(kb, sizeof(kb), "%.1f KB", list[i].bytes / 1024.0f);
        b += F("<div class=\"msg\"><a href=\"/photo?i=");
        b += String((unsigned)list[i].idx);
        if (vid) b += F("&v=1");
        b += F("&sid=");
        b += sid;
        b += F("\" target=\"_blank\">");
        b += nm;
        b += F("</a> <span class=\"t\">");
        b += kb;
        b += F("</span> <form style=\"display:inline\" method=\"POST\" action=\"/photodel\" "
               "onsubmit=\"return confirm('Delete ");
        b += nm;
        b += F("?')\"><input type=\"hidden\" name=\"i\" value=\"");
        b += String((unsigned)list[i].idx);
        b += F("\"><input type=\"hidden\" name=\"v\" value=\"");
        b += vid ? F("1") : F("0");
        b += F("\"><button style=\"padding:.1em .6em;background:#a00\">delete</button>"
               "</form></div>");
    }
    sendPage(b);
}

static void handlePhotoGet() {
    if (!requireAuthMedia()) return;
    uint32_t idx = (uint32_t)srv().arg("i").toInt();
    bool vid = srv().arg("v") == "1";
    size_t len = CameraNode::photoOpen(idx, vid);
    if (!len) {
        srv().send(404, "text/plain", "no such file");
        return;
    }
    char nm[20];
    snprintf(nm, sizeof(nm), vid ? "VID_%05u.avi" : "IMG_%05u.jpg", (unsigned)idx);
    // 'attachment' so a .avi the browser can't render inline saves with the right
    // name instead of a confused view. setContentLength => a real Content-Length
    // header (not chunked), so the client gets exactly `len` bytes.
    srv().sendHeader("Content-Disposition", String("attachment; filename=") + nm);
    srv().setContentLength(len);
    srv().send(200, vid ? "video/x-msvideo" : "image/jpeg", "");
    // Serve in chunks: the bus mutex is held only inside each read, so a slow WiFi
    // client never pins the radios' SPI bus for the whole transfer. A 0-length read
    // is EOF or a transient bus-lock miss — retry briefly rather than truncating the
    // file (photoReadChunk already waits up to 2 s for the lock per call).
    uint8_t *buf = (uint8_t *)malloc(8192);
    if (buf) {
        size_t left = len;
        int stall = 0;
        while (left) {
            size_t n = CameraNode::photoReadChunk(buf, 8192);
            if (!n) { if (++stall > 10) break; continue; }
            stall = 0;
            srv().sendContent((const char *)buf, n);
            left -= (n <= left) ? n : left;
        }
        free(buf);
    }
    CameraNode::photoClose();
}

static void handlePhotoDelete() {
    if (!requireAuth()) return;
    uint32_t idx = (uint32_t)srv().arg("i").toInt();
    CameraNode::photoDelete(idx, srv().arg("v") == "1");
    srv().sendHeader("Location", "/photos");
    srv().send(302, "text/plain", "deleted");
}

// --- config (reuse the captive form, behind login) --------------------------
static void handleConfigGet()  { if (!requireAuth()) return; CaptivePortal::serveConfigForm(); }
static void handleConfigPost() { if (!requireAuth()) return; CaptivePortal::serveConfigSave(); }

// --- pairing (peer whitelist) ----------------------------------------------
static void handlePeersGet() {
    if (!requireAuth()) return;
    String b = F("<h1>Pairing \xe2\x80\x94 trusted masters</h1>");
    b += navBar();
    b += F("<p class=\"t\">A master is a commander allowed to control this camera. Each row is "
           "one slot: tick Enabled, set the master's C2 id (its node ID, 8 hex) and the shared "
           "16-byte PSK (32 hex). Leave the key blank to keep the existing one. Mark one Primary "
           "(it receives unsolicited status beacons).</p>");
    b += F("<form method=\"POST\" action=\"/peers\">");
    for (size_t i = 0; i < CamC2Config::peerCount(); i++) {
        const CamC2Config::Peer &pr = CamC2Config::peer((int)i);
        char id[16]; snprintf(id, sizeof(id), "%08lx", (unsigned long)pr.peerId);
        b += F("<h2>Slot "); b += String((int)i); b += F("</h2>");
        b += F("<label><input type=\"checkbox\" name=\"p"); b += String((int)i);
        b += F("en\" value=\"1\""); if (pr.inUse) b += F(" checked"); b += F("> Enabled</label>");
        b += F("<label>Master C2 id (8 hex)</label><input name=\"p"); b += String((int)i);
        b += F("id\" maxlength=\"8\" value=\""); if (pr.inUse && pr.peerId) b += id; b += F("\">");
        b += F("<label>PSK (32 hex, blank = keep)</label><input name=\"p"); b += String((int)i);
        b += F("key\" maxlength=\"32\" value=\"\">");
        b += F("<label><input type=\"checkbox\" name=\"p"); b += String((int)i);
        b += F("pri\" value=\"1\"");
        if (pr.flags & CamC2Config::FLAG_PRIMARY_MASTER) b += F(" checked");
        b += F("> Primary master</label>");
    }
    b += F("<button type=\"submit\">Save pairing</button></form>");
    sendPage(b);
}

static void handlePeersPost() {
    if (!requireAuth()) return;
    const char *err = nullptr;
    for (size_t i = 0; i < CamC2Config::peerCount() && !err; i++) {
        String pre = String("p") + (int)i;
        if (srv().arg(pre + "en") != "1") { CamC2Config::clearPeer((int)i); continue; }

        String idStr = srv().arg(pre + "id"); idStr.trim();
        if (idStr.startsWith("0x") || idStr.startsWith("0X")) idStr = idStr.substring(2);
        if (idStr.length() != 8) { err = "Master C2 id must be 8 hex characters."; break; }
        uint32_t id = (uint32_t)strtoul(idStr.c_str(), nullptr, 16);
        if (id == 0) { err = "Master C2 id cannot be zero."; break; }

        // Start from the existing slot so a same-id edit preserves rxHighWater
        // (replay window) and lets a blank key keep the stored PSK.
        CamC2Config::Peer p = CamC2Config::peer((int)i);
        bool sameId = (p.inUse && p.peerId == id);
        p.inUse  = 1;
        p.peerId = id;
        p.flags  = (srv().arg(pre + "pri") == "1") ? CamC2Config::FLAG_PRIMARY_MASTER : 0;
        if (!sameId) p.rxHighWater = 0;

        String keyStr = srv().arg(pre + "key"); keyStr.trim();
        if (keyStr.length() == 0) {
            if (!sameId) { err = "A new slot needs a 32-hex PSK."; break; }
            // else keep p.psk from the existing slot
        } else if (!hexToBytes(keyStr.c_str(), p.psk, 16)) {
            err = "PSK must be exactly 32 hex characters."; break;
        }
        CamC2Config::setPeer((int)i, p);
    }
    if (err) {
        String b = F("<h1>Pairing</h1>");
        b += navBar();
        b += F("<div class=\"warn\">"); b += err; b += F("</div>");
        b += F("<p><a href=\"/peers\">Back</a></p>");
        sendPage(b);
        return;
    }
    CamC2Config::saveTable();
    String b = F("<h1>Pairing saved</h1>");
    b += navBar();
    b += F("<div class=\"ok\">Peer whitelist updated.</div><p><a href=\"/peers\">Back</a> "
           "\xc2\xb7 <a href=\"/\">Home</a></p>");
    sendPage(b);
}

// --- security (WiFi passphrase + portal login) -----------------------------
static void handleSecGet() {
    if (!requireAuth()) return;
    String b = F("<h1>Security</h1>");
    b += navBar();
    if (CamPortalConfig::usingDefaults())
        b += F("<div class=\"warn\">You are still on the default credentials. Set both below.</div>");
    b += F("<h2>WiFi passphrase</h2>"
           "<form method=\"POST\" action=\"/sec\">"
           "<p class=\"t\">8\xe2\x80\x9363 characters. Changing it reboots the camera to restart the AP.</p>"
           "<label>New WiFi passphrase</label><input name=\"appass\" type=\"password\">"
           "<button type=\"submit\">Update WiFi</button></form>"
           "<h2>Portal login</h2>"
           "<form method=\"POST\" action=\"/sec\">"
           "<label>Username</label><input name=\"user\" value=\"");
    b += htmlEsc(String(CamPortalConfig::user()));
    b += F("\"><label>New password (min 6)</label><input name=\"pass\" type=\"password\">"
           "<button type=\"submit\">Update login</button></form>");
    sendPage(b);
}

static void handleSecPost() {
    if (!requireAuth()) return;
    String appass = srv().arg("appass");
    String user   = srv().arg("user");
    String pass   = srv().arg("pass");
    const char *err = nullptr;
    bool rebootForAp = false;

    if (appass.length() > 0) {
        err = CamPortalConfig::setApPass(appass.c_str());
        if (!err) rebootForAp = true;
    } else if (user.length() > 0 && pass.length() > 0) {
        err = CamPortalConfig::setLogin(user.c_str(), pass.c_str());
    } else {
        err = "Nothing to change.";
    }

    String b;
    if (err) {
        b = F("<h1>Security</h1>"); b += navBar();
        b += F("<div class=\"warn\">"); b += err; b += F("</div><p><a href=\"/sec\">Back</a></p>");
        sendPage(b);
        return;
    }
    if (rebootForAp) {
        b = F("<!doctype html><meta http-equiv=\"refresh\" content=\"4\"><body>"
              "<h2>WiFi passphrase updated \xe2\x80\x94 rebooting. Reconnect with the new "
              "passphrase.</h2></body>");
        srv().send(200, "text/html; charset=utf-8", b);
        delay(600);
        ESP.restart();
        return;
    }
    b = F("<h1>Security</h1>"); b += navBar();
    b += F("<div class=\"ok\">Login updated.</div><p><a href=\"/\">Home</a></p>");
    sendPage(b);
}

static void handleNotFound() {
    srv().sendHeader("Location", "/");
    srv().send(302, "text/plain", "redirect");
}

// Session validation for the :81 stream server (separate TU — see CamPortal.h).
bool sessionValid(const char *hexTok) { return findSession(hexTok) != nullptr; }

// --- lifecycle -------------------------------------------------------------
// Pump the :80 server + DNS from a dedicated task (the bridge's loop() throttles
// itself to 1 Hz for the co-proc self-heal, far too slow for HTTP). The :81 stream
// server runs in esp_http_server's own task already.
static void portalTask(void *) {
    for (;;) { handle(); vTaskDelay(pdMS_TO_TICKS(5)); }
}

void begin() {
    CamPortalConfig::begin();

    char ssid[24];
    snprintf(ssid, sizeof(ssid), "LoRaCam-%02X", (unsigned)(BridgeConfig::mtNodeId() & 0xFFu));

    WiFi.mode(WIFI_AP);
    WiFi.softAPConfig(AP_IP, AP_IP, AP_NETMASK);
    bool apOk = WiFi.softAP(ssid, CamPortalConfig::apPass());
    SerialLog::logf("[CamPortal] SoftAP \"%s\" WPA2 %s @ %s\n",
                    ssid, apOk ? "up" : "FAILED", AP_IP.toString().c_str());

    s_dns.start(53, "*", AP_IP);

    WebServer &h = srv();
    static const char *kHeaders[] = { "Cookie" };
    h.collectHeaders(kHeaders, 1);
    h.on("/",       HTTP_GET,  handleHome);
    h.on("/login",  HTTP_GET,  handleLoginForm);
    h.on("/login",  HTTP_POST, handleLoginPost);
    h.on("/logout", HTTP_GET,  handleLogout);
    h.on("/config", HTTP_GET,  handleConfigGet);
    h.on("/save",   HTTP_POST, handleConfigPost);
    h.on("/cmd",    HTTP_POST, handleCmd);
    h.on("/msg",    HTTP_GET,  handleMsgGet);
    h.on("/msg",    HTTP_POST, handleMsgPost);
    h.on("/peers",  HTTP_GET,  handlePeersGet);
    h.on("/peers",  HTTP_POST, handlePeersPost);
    h.on("/photos",   HTTP_GET,  handlePhotosGet);
    h.on("/photo",    HTTP_GET,  handlePhotoGet);
    h.on("/photodel", HTTP_POST, handlePhotoDelete);
    h.on("/sec",    HTTP_GET,  handleSecGet);
    h.on("/sec",    HTTP_POST, handleSecPost);
    h.onNotFound(handleNotFound);
    h.begin();

    CamStream::start();
    s_up = true;
    xTaskCreatePinnedToCore(portalTask, "CamPortal", 8192, nullptr, 1, nullptr, 1);
    SerialLog::logf("[CamPortal] portal up — http://%s  (login required)\n",
                    AP_IP.toString().c_str());
}

void handle() {
    if (!s_up) return;
    s_dns.processNextRequest();
    srv().handleClient();
}

}  // namespace CamPortal

#endif  // BRIDGE_ROLE_CAMERA
