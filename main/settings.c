#include "settings.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_mac.h"
#include "nvs.h"

#include "config.h"

static const char *TAG = "settings";

settings_t g_settings;

// All well inside the 15-character NVS key limit.
#define K_SSID    "ssid"
#define K_PASS    "pass"
#define K_AUTHKEY "authkey"
#define K_HOST    "host"
#define K_EXIT    "exit"
#define K_NOLAN   "nolan"
#define K_DERP    "derp"
#define K_NHOST   "ntfy_host"
#define K_NPORT   "ntfy_port"
#define K_NTLS    "ntfy_tls"
#define K_NTOPIC  "ntfy_topic"
#define K_NUSER   "ntfy_user"
#define K_NPASS   "ntfy_pass"
#define K_NTOKEN  "ntfy_token"
#define K_CMD     "cmd_pass"
#define K_ADMIN   "admpass"
#define K_OK      "ok"
#define K_RESET   "ts_reset"
#define K_REG     "ts_reg"

static void set_defaults(void) {
    memset(&g_settings, 0, sizeof(g_settings));
    g_settings.exit_enabled = true;
    g_settings.block_lan    = true;
    strcpy(g_settings.ntfy_host, "ntfy.sh");
    g_settings.ntfy_port    = 443;
    g_settings.ntfy_tls     = true;
}

static void get_str(nvs_handle_t h, const char *key, char *out, size_t len) {
    size_t n = len;
    if (nvs_get_str(h, key, out, &n) != ESP_OK) out[0] = '\0';
}

static bool get_bool(nvs_handle_t h, const char *key, bool dflt) {
    uint8_t v;
    return nvs_get_u8(h, key, &v) == ESP_OK ? v != 0 : dflt;
}

void settings_begin(void) {
    set_defaults();

    nvs_handle_t h;
    // A never-written namespace cannot be opened read-only, so this opens
    // read-write even though it only reads.
    if (nvs_open(NVS_NAMESPACE_CFG, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "could not open NVS, using defaults");
        return;
    }

    g_settings.provisioned = get_bool(h, K_OK, false);
    if (!g_settings.provisioned) {
        ESP_LOGI(TAG, "no stored config -- first boot");
        nvs_close(h);
        return;
    }

    get_str(h, K_SSID,    g_settings.wifi_ssid,   sizeof(g_settings.wifi_ssid));
    get_str(h, K_PASS,    g_settings.wifi_pass,   sizeof(g_settings.wifi_pass));
    get_str(h, K_AUTHKEY, g_settings.ts_authkey,  sizeof(g_settings.ts_authkey));
    get_str(h, K_HOST,    g_settings.ts_hostname, sizeof(g_settings.ts_hostname));
    get_str(h, K_ADMIN,   g_settings.admin_pass,  sizeof(g_settings.admin_pass));
    g_settings.exit_enabled = get_bool(h, K_EXIT, true);
    g_settings.block_lan    = get_bool(h, K_NOLAN, true);
    if (nvs_get_u16(h, K_DERP, &g_settings.derp_region) != ESP_OK) g_settings.derp_region = 0;
    get_str(h, K_NHOST,  g_settings.ntfy_host,  sizeof(g_settings.ntfy_host));
    if (!g_settings.ntfy_host[0]) strcpy(g_settings.ntfy_host, "ntfy.sh");
    if (nvs_get_u16(h, K_NPORT, &g_settings.ntfy_port) != ESP_OK) g_settings.ntfy_port = 443;
    g_settings.ntfy_tls = get_bool(h, K_NTLS, true);
    get_str(h, K_NTOPIC, g_settings.ntfy_topic, sizeof(g_settings.ntfy_topic));
    get_str(h, K_NUSER,  g_settings.ntfy_user,  sizeof(g_settings.ntfy_user));
    get_str(h, K_NPASS,  g_settings.ntfy_pass,  sizeof(g_settings.ntfy_pass));
    get_str(h, K_NTOKEN, g_settings.ntfy_token, sizeof(g_settings.ntfy_token));
    get_str(h, K_CMD,    g_settings.cmd_pass,   sizeof(g_settings.cmd_pass));
    nvs_close(h);

    // A record flagged complete but missing essentials is worse than no
    // record: it would skip the setup portal and then fail forever.
    if (!g_settings.wifi_ssid[0] || !g_settings.ts_authkey[0]) {
        ESP_LOGW(TAG, "stored config is incomplete -- forcing setup");
        g_settings.provisioned = false;
        return;
    }

    // Never log the auth key or passwords, only whether they are set.
    ESP_LOGI(TAG, "loaded: ssid=%s host=%s exit=%d block_lan=%d authkey set=%d",
             g_settings.wifi_ssid, settings_effective_hostname(),
             g_settings.exit_enabled, g_settings.block_lan,
             g_settings.ts_authkey[0] != '\0');
}

bool settings_save(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE_CFG, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGE(TAG, "could not open NVS for write");
        return false;
    }

    // Cleared first and set last, so a power cut mid-save reads back as
    // unprovisioned rather than as a mix of old and new values.
    esp_err_t err = nvs_set_u8(h, K_OK, 0);
    if (err == ESP_OK) err = nvs_commit(h);
    if (err == ESP_OK) err = nvs_set_str(h, K_SSID,    g_settings.wifi_ssid);
    if (err == ESP_OK) err = nvs_set_str(h, K_PASS,    g_settings.wifi_pass);
    if (err == ESP_OK) err = nvs_set_str(h, K_AUTHKEY, g_settings.ts_authkey);
    if (err == ESP_OK) err = nvs_set_str(h, K_HOST,    g_settings.ts_hostname);
    if (err == ESP_OK) err = nvs_set_str(h, K_ADMIN,   g_settings.admin_pass);
    if (err == ESP_OK) err = nvs_set_u8(h,  K_EXIT,    g_settings.exit_enabled);
    if (err == ESP_OK) err = nvs_set_u8(h,  K_NOLAN,   g_settings.block_lan);
    if (err == ESP_OK) err = nvs_set_u16(h, K_DERP,    g_settings.derp_region);
    if (err == ESP_OK) err = nvs_set_str(h, K_NHOST,   g_settings.ntfy_host);
    if (err == ESP_OK) err = nvs_set_u16(h, K_NPORT,   g_settings.ntfy_port);
    if (err == ESP_OK) err = nvs_set_u8(h,  K_NTLS,    g_settings.ntfy_tls);
    if (err == ESP_OK) err = nvs_set_str(h, K_NTOPIC,  g_settings.ntfy_topic);
    if (err == ESP_OK) err = nvs_set_str(h, K_NUSER,   g_settings.ntfy_user);
    if (err == ESP_OK) err = nvs_set_str(h, K_NPASS,   g_settings.ntfy_pass);
    if (err == ESP_OK) err = nvs_set_str(h, K_NTOKEN,  g_settings.ntfy_token);
    if (err == ESP_OK) err = nvs_set_str(h, K_CMD,     g_settings.cmd_pass);
    if (err == ESP_OK) err = nvs_commit(h);
    if (err == ESP_OK) err = nvs_set_u8(h,  K_OK, 1);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "write failed: %s", esp_err_to_name(err));
        return false;
    }
    g_settings.provisioned = true;
    ESP_LOGI(TAG, "saved");
    return true;
}

void settings_clear(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE_CFG, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_erase_all(h);
    nvs_commit(h);
    nvs_close(h);
    set_defaults();
    ESP_LOGI(TAG, "cleared");
}

const char *settings_effective_hostname(void) {
    static char dflt[40];
    if (g_settings.ts_hostname[0]) return g_settings.ts_hostname;
    if (!dflt[0]) {
        uint8_t mac[6];
        esp_read_mac(mac, ESP_MAC_WIFI_STA);
        snprintf(dflt, sizeof(dflt), TS_DEFAULT_HOST_PREFIX "%02x%02x", mac[4], mac[5]);
    }
    return dflt;
}

// ---- Tailscale bookkeeping --------------------------------------------------

static void put_flag(const char *key, bool v) {
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE_CFG, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, key, v);
    nvs_commit(h);
    nvs_close(h);
}

static bool get_flag(const char *key) {
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE_CFG, NVS_READWRITE, &h) != ESP_OK) return false;
    bool v = get_bool(h, key, false);
    nvs_close(h);
    return v;
}

void settings_request_identity_reset(void) {
    put_flag(K_RESET, true);
    put_flag(K_REG, false);
}

bool settings_take_identity_reset(void) {
    if (!get_flag(K_RESET)) return false;
    put_flag(K_RESET, false);
    return true;
}

bool settings_ts_registered(void) { return get_flag(K_REG); }

void settings_set_ts_registered(bool registered) { put_flag(K_REG, registered); }
