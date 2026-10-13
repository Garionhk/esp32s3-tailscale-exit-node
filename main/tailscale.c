#include "tailscale.h"

#include <stdio.h>

#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/def.h"
#include "microlink.h"

#include "config.h"
#include "exitnode.h"
#include "settings.h"

static const char *TAG = "tailscale";

static microlink_t *s_ml;
static microlink_state_t s_last = ML_STATE_IDLE;

// True when this boot registered without the auth key, relying on the node
// key from an earlier successful registration. See tailscale_start().
static bool s_keyless;

static const char *const STATE_NAMES[] = {
    [ML_STATE_IDLE]         = "idle",
    [ML_STATE_WIFI_WAIT]    = "waiting for Wi-Fi",
    [ML_STATE_CONNECTING]   = "connecting",
    [ML_STATE_REGISTERING]  = "registering",
    [ML_STATE_CONNECTED]    = "connected",
    [ML_STATE_RECONNECTING] = "reconnecting",
    [ML_STATE_ERROR]        = "failed",
};

static uint32_t relay_backlog(void) {
    return microlink_relay_backlog(s_ml);
}

void tailscale_boot(void) {
    if (!settings_take_identity_reset()) return;
    ESP_LOGW(TAG, "forgetting Tailscale identity; this device will register as a new node");
    esp_err_t err = microlink_factory_reset();
    if (err != ESP_OK) ESP_LOGE(TAG, "factory reset: %s", esp_err_to_name(err));
    settings_set_ts_registered(false);
}

void tailscale_start(void) {
    if (s_ml) return;

    // MicroLink sends the auth key on every registration. Once this node key
    // has registered, the control plane recognises it without one -- and a
    // single-use key would be refused the second time. So the key is only
    // sent until the first success. If keyless registration is ever refused
    // (node deleted, key expired), tailscale_tick() clears the flag and
    // reboots, and the next attempt sends the key again.
    s_keyless = settings_ts_registered();

    static microlink_config_t cfg;   // MicroLink keeps the pointers
    cfg = (microlink_config_t){
        .auth_key            = s_keyless ? "" : g_settings.ts_authkey,
        .device_name         = settings_effective_hostname(),
        .enable_derp         = true,
        .enable_stun         = true,
        .enable_disco        = true,
        .max_peers           = TS_MAX_PEERS,
        .advertise_exit_node = g_settings.exit_enabled,
        .peerapi_port        = g_settings.exit_enabled ? PEERAPI_PORT : 0,
        .derp_region         = g_settings.derp_region,
    };

    ESP_LOGI(TAG, "starting as \"%s\" (%s, exit node %s)", cfg.device_name,
             s_keyless ? "known node" : "registering with auth key",
             cfg.advertise_exit_node ? "advertised" : "off");

    s_ml = microlink_init(&cfg);
    if (!s_ml) {
        ESP_LOGE(TAG, "microlink_init failed");
        return;
    }
    exitnode_set_backlog_source(relay_backlog);
    if (microlink_start(s_ml) != ESP_OK) {
        ESP_LOGE(TAG, "microlink_start failed");
    }
}

void tailscale_tick(void) {
    if (!s_ml) return;

    void *wg = microlink_get_netif(s_ml);
    if (wg) exitnode_attach(wg, g_settings.exit_enabled, g_settings.block_lan);
    exitnode_set_vpn_ip(lwip_htonl(microlink_get_vpn_ip(s_ml)));

    microlink_state_t st = microlink_get_state(s_ml);
    if (st == s_last) return;
    ESP_LOGI(TAG, "state: %s", tailscale_state_name());

    if (st == ML_STATE_CONNECTED) {
        char ip[16];
        tailscale_vpn_ip_str(ip, sizeof(ip));
        ESP_LOGI(TAG, "on the tailnet as %s", ip);
        if (!s_keyless && !settings_ts_registered()) settings_set_ts_registered(true);
        if (g_settings.exit_enabled) {
            ESP_LOGI(TAG, "exit node advertised -- approve it in the admin console "
                          "(Machines > %s > Edit route settings)", settings_effective_hostname());
        }
    } else if (st == ML_STATE_ERROR) {
        ESP_LOGE(TAG, "control plane refused registration: %s", tailscale_error());
        if (s_keyless) {
            // The stored node identity is no longer accepted. Try once more
            // with the auth key; if that fails too, the error stays on screen.
            ESP_LOGW(TAG, "retrying with the stored auth key after reboot");
            settings_set_ts_registered(false);
            vTaskDelay(pdMS_TO_TICKS(1000));
            esp_restart();
        }
    }
    s_last = st;
}

void tailscale_wifi_reconnected(void) {
    if (!s_ml) return;
    microlink_state_t st = microlink_get_state(s_ml);
    if (st == ML_STATE_ERROR || st == ML_STATE_IDLE) return;
    ESP_LOGI(TAG, "Wi-Fi back, rebinding");
    microlink_rebind(s_ml);
}

bool tailscale_started(void)   { return s_ml != NULL; }
bool tailscale_connected(void) { return s_ml && microlink_is_connected(s_ml); }
bool tailscale_failed(void)    { return s_ml && microlink_get_state(s_ml) == ML_STATE_ERROR; }

const char *tailscale_state_name(void) {
    if (!s_ml) return "not started";
    microlink_state_t st = microlink_get_state(s_ml);
    return st < sizeof(STATE_NAMES) / sizeof(STATE_NAMES[0]) ? STATE_NAMES[st] : "unknown";
}

const char *tailscale_error(void) {
    return s_ml ? microlink_get_last_error(s_ml) : "";
}

void tailscale_vpn_ip_str(char *buf, size_t len) {
    uint32_t ip = s_ml ? microlink_get_vpn_ip(s_ml) : 0;
    if (!ip || len < 16) { if (len) buf[0] = '\0'; return; }
    microlink_ip_to_str(ip, buf);
}

int tailscale_peer_count(void) {
    return s_ml ? microlink_get_peer_count(s_ml) : 0;
}

uint16_t tailscale_derp_region(char *code, size_t len) {
    if (!s_ml) { if (len) code[0] = '\0'; return 0; }
    return microlink_get_derp_region(s_ml, code, len);
}

void tailscale_log_diag(void) {
    if (!s_ml) return;
    microlink_diag_t d;
    microlink_get_diag(s_ml, &d);
    ESP_LOGI(TAG, "diag: relay resets %lu | drops wg_rx %lu disco_rx %lu relay_tx %lu stale %lu | "
                  "relay write max %lu ms (waits %lu), read gap max %lu ms | queue max tx %lu wg_rx %lu",
             (unsigned long)d.derp_resets, (unsigned long)d.wg_rx_drops,
             (unsigned long)d.disco_rx_drops, (unsigned long)d.derp_tx_drops,
             (unsigned long)d.derp_tx_stale, (unsigned long)d.derp_write_max_ms,
             (unsigned long)d.derp_write_retries, (unsigned long)d.derp_read_gap_max_ms,
             (unsigned long)d.derp_tx_queue_max, (unsigned long)d.wg_rx_queue_max);
}

void tailscale_peer_paths(int *direct, int *relayed) {
    *direct = *relayed = 0;
    int n = tailscale_peer_count();
    for (int i = 0; i < n; i++) {
        microlink_peer_info_t info;
        if (microlink_get_peer_info(s_ml, i, &info) != ESP_OK) continue;
        if (info.direct_path) (*direct)++;
        else (*relayed)++;
    }
}
