// ESP32-S3 Tailscale exit node.
//
// Joins a tailnet through MicroLink and offers itself as an exit node: a
// phone or laptop anywhere on the tailnet can send its internet traffic out
// through this device's Wi-Fi, as if it were at home.
//
// All configuration lives in NVS and is edited from the built-in web portal --
// nothing user-specific is compiled in. See README.md.

#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "commands.h"
#include "config.h"
#include "exitnode.h"
#include "ntfy.h"
#include "peerapi.h"
#include "settings.h"
#include "tailscale.h"
#include "wanip.h"
#include "webconfig.h"
#include "wifi.h"

static const char *TAG = "main";

// What the device is currently doing. Wi-Fi can come and go while the portal
// stays up, so the transitions are explicit.
typedef enum { MODE_SETUP, MODE_AP_FALLBACK, MODE_ONLINE } app_mode_t;
static app_mode_t s_mode = MODE_SETUP;

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

static void init_nvs(void) {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        // Only happens after a partition-layout change; the data is unreadable anyway.
        ESP_LOGW(TAG, "NVS unreadable (%s), erasing", esp_err_to_name(err));
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
}

// Pressing BOOT (GPIO0) just after a reset forces the setup portal on an
// already-provisioned device -- the way back from a forgotten admin password
// or a moved network. It has to be pressed AFTER releasing EN: GPIO0 held low
// through the reset itself puts the ESP32 into its serial bootloader instead.
static bool boot_button_pressed(void) {
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << BOOT_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&io);
    ESP_LOGI(TAG, "press BOOT now to open the setup portal");
    int64_t start = now_ms();
    while (now_ms() - start < BOOT_WINDOW_MS) {
        if (gpio_get_level(BOOT_BUTTON_GPIO) == 0) return true;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return false;
}

// Portal with no Wi-Fi behind it. Deliberately has no timeout: an
// unprovisioned device has no useful fallback state, so a page telling you
// how to fix that is the most useful thing it can be doing.
//
// Deliberately open, too. This is only reached when the device is
// unprovisioned (nothing to protect yet) or when someone pressed BOOT, which
// already implies physical access -- anyone holding the board can reflash it
// regardless. Requiring the admin password here would turn a forgotten
// password into a lockout with no way back except erasing flash over USB.
static void run_setup_portal(void) {
    s_mode = MODE_SETUP;
    wifi_start_ap();
    webconfig_start();
    webconfig_set_mode(/*ap_mode=*/true, /*require_auth=*/false);

    ESP_LOGI(TAG, "=== SETUP MODE ===");
    int64_t last_nag = 0;
    while (!webconfig_saved()) {
        if (now_ms() - last_nag > 5000) {
            last_nag = now_ms();
            ESP_LOGI(TAG, "join Wi-Fi \"%s\" then open http://192.168.4.1", wifi_ap_ssid());
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    vTaskDelay(pdMS_TO_TICKS(2500));   // let the browser follow the redirect to /saved
    esp_restart();
}

static void go_online(void) {
    s_mode = MODE_ONLINE;
    webconfig_set_mode(/*ap_mode=*/false, /*require_auth=*/true);
    // Only reachable from the tailnet (source-checked); answers DNS for
    // clients using this device as their exit node.
    if (g_settings.exit_enabled) peerapi_start();
    if (!tailscale_started()) tailscale_start();
    else tailscale_wifi_reconnected();
    ntfy_start(commands_handle);   // no-op without a topic, or if already running
    wanip_refresh_soon();          // the public address may have changed
    ESP_LOGI(TAG, "online, heap %u bytes free", (unsigned)esp_get_free_heap_size());
}

// ---- address reporting over ntfy -----------------------------------------------------
//
// "online" goes out once Tailscale is connected (or after ONLINE_DEADLINE_MS
// regardless, so a refused registration still gets reported). After that, a
// change to the LAN, Tailscale or public address that holds for
// ADDR_DEBOUNCE_MS is reported once.

typedef struct {
    uint32_t lan;
    char vpn[16];
    char wan[16];
} addrs_t;

static void read_addrs(addrs_t *a) {
    a->lan = wifi_sta_ip();
    tailscale_vpn_ip_str(a->vpn, sizeof(a->vpn));
    wanip_get(a->wan, sizeof(a->wan));
}

static bool addrs_equal(const addrs_t *x, const addrs_t *y) {
    return x->lan == y->lan && !strcmp(x->vpn, y->vpn) && !strcmp(x->wan, y->wan);
}

static void report_addresses(const char *what) {
    static char body[768];
    char title[96];
    commands_status_text(body, sizeof(body), false);
    snprintf(title, sizeof(title), "%s %s", settings_effective_hostname(), what);
    ntfy_publish(title, body);
}

static void address_watch(void) {
    static int64_t online_at, next_poll, candidate_since;
    static bool sent_online, reported_failure;
    static addrs_t published, candidate;

    if (!ntfy_enabled() || s_mode != MODE_ONLINE) return;
    int64_t now = now_ms();
    if (!online_at) online_at = now;
    if (now < next_poll) return;
    next_poll = now + ADDR_POLL_MS;

    addrs_t cur;
    read_addrs(&cur);

    if (!sent_online) {
        // Wait for the 100.x address and the public address, within reason.
        bool ready = tailscale_connected() && cur.vpn[0] && cur.wan[0];
        if (ready || now - online_at > ONLINE_DEADLINE_MS) {
            report_addresses("online");
            published = cur;
            sent_online = true;
        }
        return;
    }

    if (tailscale_failed() && !reported_failure) {
        reported_failure = true;
        report_addresses("Tailscale refused");
    }

    if (addrs_equal(&cur, &published)) {
        candidate_since = 0;
        return;
    }
    if (!candidate_since || !addrs_equal(&cur, &candidate)) {
        candidate = cur;
        candidate_since = now;
        return;
    }
    if (now - candidate_since >= ADDR_DEBOUNCE_MS) {
        // A lost address (Wi-Fi down) is not news; its return is.
        if (cur.lan && (cur.vpn[0] || !published.vpn[0])) {
            report_addresses("address changed");
        }
        published = cur;
        candidate_since = 0;
    }
}

// Wi-Fi is configured but unreachable. Bring the portal up so the settings can
// be fixed, while the station side keeps retrying in the background -- a
// router coming back should not need a reboot.
static void start_ap_fallback(void) {
    s_mode = MODE_AP_FALLBACK;
    wifi_start_ap();
    webconfig_set_mode(/*ap_mode=*/true, /*require_auth=*/true);
    ESP_LOGW(TAG, "Wi-Fi unreachable; portal on \"%s\" while retrying in background",
             wifi_ap_ssid());
}

static void log_memory(void) {
    size_t free_int = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t min_int  = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    ESP_LOGI(TAG, "heap free %u (min %u, largest block %u), PSRAM free %u, tailscale %s",
             (unsigned)free_int, (unsigned)min_int,
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             tailscale_state_name());
    exitnode_stats_t st;
    exitnode_get_stats(&st);
    peerapi_stats_t ds;
    peerapi_get_stats(&ds);
    ESP_LOGI(TAG, "exit: fwd %lu pkts / %llu KB, shed %lu, stack full %lu, dropped lan %lu ipv6 %lu; "
                  "dns %lu queries (%lu failed)",
             (unsigned long)st.fwd_packets, (unsigned long long)(st.fwd_bytes / 1024),
             (unsigned long)st.shed, (unsigned long)st.stack_full,
             (unsigned long)st.drop_lan, (unsigned long)st.drop_ipv6,
             (unsigned long)ds.queries, (unsigned long)ds.failures);
    tailscale_log_diag();
    static size_t warned_min = SIZE_MAX;
    if (min_int < HEAP_LOW_WATER_BYTES && min_int < warned_min) {
        warned_min = min_int;
        ESP_LOGW(TAG, "internal RAM low-water mark %u bytes -- below %u", (unsigned)min_int,
                 (unsigned)HEAP_LOW_WATER_BYTES);
    }
}

void app_main(void) {
    ESP_LOGI(TAG, "=== ESP32-S3 Tailscale exit node %s ===", esp_app_get_description()->version);

    init_nvs();
    settings_begin();
    tailscale_boot();   // pending identity wipe has to happen before MicroLink starts
    wifi_init();

    // Only worth waiting for on a provisioned board; otherwise setup is next anyway.
    bool forced = g_settings.provisioned && boot_button_pressed();
    if (forced) ESP_LOGW(TAG, "BOOT pressed -- forcing setup portal");

    if (!g_settings.provisioned || forced) {
        run_setup_portal();   // restarts on save; never returns
    }

    wifi_start_sta(g_settings.wifi_ssid, g_settings.wifi_pass);
    webconfig_start();
    if (wifi_wait_connected(WIFI_CONNECT_TIMEOUT_MS)) {
        go_online();
    } else {
        ESP_LOGW(TAG, "could not join: %s", wifi_reason_text(wifi_last_disconnect_reason()));
        wifi_diagnose(g_settings.wifi_ssid);
        start_ap_fallback();
    }

    int64_t last_retry = now_ms();
    int64_t lost_since = 0;
    int64_t last_heap_log = 0;

    for (;;) {
        if (webconfig_saved()) {
            vTaskDelay(pdMS_TO_TICKS(2500));   // let the browser follow the redirect to /saved
            esp_restart();
        }
        if (commands_restart_pending()) {
            // Let the ntfy reply go out first (bounded, in case the server is down).
            for (int i = 0; i < 100 && ntfy_pending() > 0; i++) vTaskDelay(pdMS_TO_TICKS(100));
            vTaskDelay(pdMS_TO_TICKS(1000));
            esp_restart();
        }

        switch (s_mode) {
        case MODE_SETUP:
            break;   // run_setup_portal() never returns

        case MODE_AP_FALLBACK:
            if (wifi_is_connected()) {
                ESP_LOGI(TAG, "Wi-Fi back, leaving AP mode");
                wifi_stop_ap();
                go_online();
            } else if (now_ms() - last_retry > WIFI_RETRY_INTERVAL_MS) {
                last_retry = now_ms();
                ESP_LOGI(TAG, "retrying Wi-Fi (%s)", wifi_reason_text(wifi_last_disconnect_reason()));
                wifi_retry();
            }
            break;

        case MODE_ONLINE:
            if (wifi_is_connected()) {
                if (lost_since) {
                    ESP_LOGI(TAG, "Wi-Fi back after %lld s", (now_ms() - lost_since) / 1000);
                    lost_since = 0;
                    tailscale_wifi_reconnected();
                    wanip_refresh_soon();
                }
            } else if (!lost_since) {
                lost_since = now_ms();
                ESP_LOGW(TAG, "Wi-Fi link lost, reconnecting");
            } else if (now_ms() - lost_since > WIFI_LOST_GRACE_MS) {
                lost_since = 0;
                last_retry = now_ms();
                start_ap_fallback();
            }
            break;
        }

        tailscale_tick();
        address_watch();

        if (now_ms() - last_heap_log > HEAP_LOG_INTERVAL_MS) {
            last_heap_log = now_ms();
            log_memory();
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
