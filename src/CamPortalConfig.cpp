// CamPortalConfig.cpp — see CamPortalConfig.h.

#include "CamPortalConfig.h"
#if defined(BRIDGE_ROLE_CAMERA)

#include <Preferences.h>
#include <mbedtls/sha256.h>
#include <string.h>
#include "SerialLog.h"

// Build-flag defaults (quoted strings in platformio.ini, e.g.
//   '-DBRIDGE_PORTAL_AP_PASS="loracam-bench"'). Absent → hard defaults below,
// and usingDefaults() stays true so the portal nags the operator to change them.
#ifndef BRIDGE_PORTAL_USER
#define BRIDGE_PORTAL_USER    "admin"
#endif
#ifndef BRIDGE_PORTAL_PASS
#define BRIDGE_PORTAL_PASS    "loracam-admin"
#endif
#ifndef BRIDGE_PORTAL_AP_PASS
#define BRIDGE_PORTAL_AP_PASS "loracam-portal"
#endif

namespace CamPortalConfig {

static const char *NVS_NS  = "camportal";
static const uint8_t BLOB_VER = 1;

struct Persist {
    uint8_t  ver;                 // == BLOB_VER
    uint8_t  provisioned;         // 1 once the operator saved non-default creds
    uint8_t  _pad[2];
    char     user[USER_MAX + 1];
    uint8_t  salt[8];
    uint8_t  hash[32];            // SHA-256(salt || password)
    char     apPass[AP_PASS_MAX + 1];
};

static Persist s_cfg;
static bool    s_defaults = true;   // live creds are still the build-flag/hard defaults

// --- helpers ---------------------------------------------------------------

static void sha256Salted(const uint8_t salt[8], const char *pass, uint8_t out[32]) {
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);                 // 0 = SHA-256 (not -224)
    mbedtls_sha256_update(&ctx, salt, 8);
    mbedtls_sha256_update(&ctx, (const uint8_t *)pass, strlen(pass));
    mbedtls_sha256_finish(&ctx, out);
    mbedtls_sha256_free(&ctx);
}

static bool ctEqual(const uint8_t *a, const uint8_t *b, size_t n) {
    uint8_t d = 0;
    for (size_t i = 0; i < n; i++) d |= (uint8_t)(a[i] ^ b[i]);
    return d == 0;
}

static void seedDefaults() {
    memset(&s_cfg, 0, sizeof(s_cfg));
    s_cfg.ver = BLOB_VER;
    s_cfg.provisioned = 0;
    strncpy(s_cfg.user, BRIDGE_PORTAL_USER, USER_MAX);
    strncpy(s_cfg.apPass, BRIDGE_PORTAL_AP_PASS, AP_PASS_MAX);
    for (int i = 0; i < 8; i++) s_cfg.salt[i] = (uint8_t)esp_random();
    sha256Salted(s_cfg.salt, BRIDGE_PORTAL_PASS, s_cfg.hash);
    s_defaults = true;
}

static void persist() {
    Preferences p;
    if (!p.begin(NVS_NS, false)) {
        SerialLog::logf("[CamPortal] NVS open failed — creds NOT persisted\n");
        return;
    }
    p.putBytes("blob", &s_cfg, sizeof(s_cfg));
    p.end();
}

// --- API -------------------------------------------------------------------

void begin() {
    Preferences p;
    bool loaded = false;
    if (p.begin(NVS_NS, true)) {                     // read-only probe
        Persist tmp;
        size_t n = p.getBytes("blob", &tmp, sizeof(tmp));
        p.end();
        if (n == sizeof(tmp) && tmp.ver == BLOB_VER) {
            s_cfg = tmp;
            s_defaults = (s_cfg.provisioned == 0);
            loaded = true;
        }
    }
    if (!loaded) {
        seedDefaults();
        // Don't persist defaults — leave NVS empty so a first real save flips
        // provisioned cleanly. The AP still comes up on the seeded defaults.
    }

    if (s_defaults)
        SerialLog::logf("[CamPortal] WARNING: portal/WiFi using DEFAULT credentials "
                        "(user=%s) — change them in the portal's security section.\n",
                        s_cfg.user);
    else
        SerialLog::logf("[CamPortal] credentials loaded (user=%s, provisioned)\n", s_cfg.user);
}

bool isProvisioned() { return s_cfg.provisioned != 0; }
bool usingDefaults() { return s_defaults; }

const char *apPass() { return s_cfg.apPass; }
const char *user()   { return s_cfg.user; }

bool checkLogin(const char *u, const char *p) {
    if (!u || !p) return false;
    bool userOk = (strncmp(u, s_cfg.user, USER_MAX + 1) == 0);
    uint8_t h[32];
    sha256Salted(s_cfg.salt, p, h);
    bool passOk = ctEqual(h, s_cfg.hash, 32);
    return userOk && passOk;        // both evaluated regardless (no early-out)
}

const char *setApPass(const char *pass) {
    if (!pass) return "AP passphrase missing.";
    size_t len = strlen(pass);
    if (len < AP_PASS_MIN || len > AP_PASS_MAX)
        return "WiFi passphrase must be 8-63 characters.";
    strncpy(s_cfg.apPass, pass, AP_PASS_MAX);
    s_cfg.apPass[AP_PASS_MAX] = '\0';
    s_cfg.provisioned = 1;
    s_defaults = false;
    persist();
    return nullptr;
}

const char *setLogin(const char *u, const char *pass) {
    if (!u || !pass) return "Username/password missing.";
    size_t ulen = strlen(u), plen = strlen(pass);
    if (ulen < 1 || ulen > USER_MAX) return "Username must be 1-23 characters.";
    if (plen < 6)                    return "Portal password must be at least 6 characters.";
    strncpy(s_cfg.user, u, USER_MAX);
    s_cfg.user[USER_MAX] = '\0';
    for (int i = 0; i < 8; i++) s_cfg.salt[i] = (uint8_t)esp_random();
    sha256Salted(s_cfg.salt, pass, s_cfg.hash);
    s_cfg.provisioned = 1;
    s_defaults = false;
    persist();
    return nullptr;
}

}  // namespace CamPortalConfig

#endif  // BRIDGE_ROLE_CAMERA
