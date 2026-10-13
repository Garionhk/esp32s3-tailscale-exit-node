#include "wifi.h"

#include <stdio.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#include "config.h"
#include "exitnode.h"

static const char *TAG = "wifi";

#define BIT_CONNECTED BIT0

static EventGroupHandle_t s_events;
static esp_netif_t *s_sta;
static esp_netif_t *s_ap;
static bool s_ap_on;
static bool s_sta_configured;     // false in setup mode: nothing to join
static volatile uint32_t s_sta_ip;
static volatile uint8_t s_last_reason;
static volatile bool s_hold_reconnect;   // set while wifi_diagnose() scans

const char *wifi_reason_text(uint8_t r) {
    switch (r) {
    case 0:                                  return "none";
    case WIFI_REASON_NO_AP_FOUND:            return "network not seen (5 GHz only, hidden or misspelled?)";
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:      return "authentication failed (usually a wrong password)";
    case WIFI_REASON_ASSOC_LEAVE:            return "left the network";
    case WIFI_REASON_BEACON_TIMEOUT:         return "signal lost";
    default:                                 return "disconnected";
    }
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
    if (base == WIFI_EVENT) {
        switch (id) {
        case WIFI_EVENT_STA_START:
            // esp_netif re-adds the lwIP netif on every start, so the
            // forwarding guard has to be reapplied each time.
            exitnode_guard_netif(s_sta);
            if (s_sta_configured) esp_wifi_connect();
            break;
        case WIFI_EVENT_AP_START:
            exitnode_guard_netif(s_ap);
            break;
        case WIFI_EVENT_STA_DISCONNECTED: {
            wifi_event_sta_disconnected_t *d = data;
            s_last_reason = d->reason;
            s_sta_ip = 0;
            xEventGroupClearBits(s_events, BIT_CONNECTED);
            exitnode_set_sta_ip(0, 0);
            ESP_LOGW(TAG, "disconnected: %s (reason %d)", wifi_reason_text(d->reason), d->reason);
            // Auto-reconnect while running only as a station. In AP+STA
            // mode the main loop paces retries instead, because a station
            // that is constantly connecting makes the AP channel hop and
            // breaks scans on the setup page.
            if (s_sta_configured && !s_ap_on && !s_hold_reconnect) esp_wifi_connect();
            break;
        }
        default:
            break;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        s_sta_ip = e->ip_info.ip.addr;
        s_last_reason = 0;
        exitnode_set_sta_ip(e->ip_info.ip.addr, e->ip_info.netmask.addr);
        ESP_LOGI(TAG, "ip " IPSTR "  mask " IPSTR "  gw " IPSTR,
                 IP2STR(&e->ip_info.ip), IP2STR(&e->ip_info.netmask), IP2STR(&e->ip_info.gw));
        xEventGroupSetBits(s_events, BIT_CONNECTED);
    }
}

void wifi_init(void) {
    s_events = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_sta = esp_netif_create_default_wifi_sta();
    s_ap  = esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));   // credentials live in our NVS namespace

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL));
}

static void apply_sta_config(const char *ssid, const char *pass) {
    wifi_config_t wc = {0};
    strlcpy((char *)wc.sta.ssid, ssid, sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password, pass, sizeof(wc.sta.password));
    // Accept open and WPA networks alike; the password decides.
    wc.sta.threshold.authmode = pass[0] ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;
    wc.sta.pmf_cfg.capable = true;
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
}

void wifi_start_sta(const char *ssid, const char *pass) {
    s_sta_configured = true;
    ESP_LOGI(TAG, "joining %s", ssid);
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    apply_sta_config(ssid, pass);
    ESP_ERROR_CHECK(esp_wifi_start());
    // Power save adds up to a DTIM interval of latency per packet, which
    // every forwarded packet would pay twice.
    esp_wifi_set_ps(WIFI_PS_NONE);
}

bool wifi_wait_connected(uint32_t timeout_ms) {
    EventBits_t b = xEventGroupWaitBits(s_events, BIT_CONNECTED, pdFALSE, pdTRUE,
                                        pdMS_TO_TICKS(timeout_ms));
    return (b & BIT_CONNECTED) != 0;
}

const char *wifi_ap_ssid(void) {
    static char ssid[32];
    if (!ssid[0]) {
        uint8_t mac[6];
        esp_read_mac(mac, ESP_MAC_WIFI_STA);
        snprintf(ssid, sizeof(ssid), AP_SSID_PREFIX "%02X%02X", mac[4], mac[5]);
    }
    return ssid;
}

void wifi_start_ap(void) {
    wifi_config_t ap = {0};
    strlcpy((char *)ap.ap.ssid, wifi_ap_ssid(), sizeof(ap.ap.ssid));
    ap.ap.ssid_len = strlen(wifi_ap_ssid());
    ap.ap.channel = AP_CHANNEL;
    ap.ap.max_connection = AP_MAX_CONN;
    ap.ap.authmode = WIFI_AUTH_OPEN;

    s_ap_on = true;
    // AP+STA rather than AP: in fallback mode the station keeps retrying, so
    // a router coming back brings the device online without a reboot. In
    // setup mode the idle station is what makes scanning possible.
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    esp_err_t err = esp_wifi_start();   // no-op if already running
    if (err != ESP_OK) ESP_LOGE(TAG, "esp_wifi_start: %s", esp_err_to_name(err));
    esp_wifi_set_ps(WIFI_PS_NONE);
    ESP_LOGI(TAG, "AP \"%s\" up", wifi_ap_ssid());
}

void wifi_stop_ap(void) {
    if (!s_ap_on) return;
    s_ap_on = false;
    esp_wifi_set_mode(WIFI_MODE_STA);
    ESP_LOGI(TAG, "AP down");
}

void wifi_retry(void) {
    if (!s_sta_configured || wifi_is_connected()) return;
    esp_wifi_disconnect();
    esp_wifi_connect();
}

bool wifi_is_connected(void) {
    return (xEventGroupGetBits(s_events) & BIT_CONNECTED) != 0;
}

bool wifi_ap_active(void) { return s_ap_on; }

uint32_t wifi_sta_ip(void) { return s_sta_ip; }

uint32_t wifi_ap_ip(void) {
    esp_netif_ip_info_t info;
    if (esp_netif_get_ip_info(s_ap, &info) != ESP_OK) return 0;
    return info.ip.addr;
}

int wifi_rssi(void) {
    wifi_ap_record_t ap;
    if (!wifi_is_connected() || esp_wifi_sta_get_ap_info(&ap) != ESP_OK) return 0;
    return ap.rssi;
}

uint8_t wifi_last_disconnect_reason(void) { return s_last_reason; }

esp_netif_t *wifi_sta_netif(void) { return s_sta; }
esp_netif_t *wifi_ap_netif(void) { return s_ap; }

// Returns a heap array the caller frees, or NULL.
static wifi_ap_record_t *scan(uint16_t *count) {
    *count = 0;
    wifi_scan_config_t sc = { .show_hidden = false };
    if (esp_wifi_scan_start(&sc, true) != ESP_OK) return NULL;
    uint16_t n = 0;
    esp_wifi_scan_get_ap_num(&n);
    if (n > 30) n = 30;
    wifi_ap_record_t *recs = calloc(n ? n : 1, sizeof(*recs));
    if (!recs) { esp_wifi_clear_ap_list(); return NULL; }
    esp_wifi_scan_get_ap_records(&n, recs);
    *count = n;
    return recs;
}

static void json_escape(const char *in, char *out, size_t len) {
    size_t o = 0;
    for (; *in && o + 7 < len; in++) {
        unsigned char c = (unsigned char)*in;
        if (c == '"' || c == '\\') { out[o++] = '\\'; out[o++] = c; }
        else if (c < 0x20) o += snprintf(out + o, len - o, "\\u%04x", c);
        else out[o++] = c;
    }
    out[o] = '\0';
}

int wifi_scan_json(char *buf, size_t len) {
    uint16_t n;
    wifi_ap_record_t *recs = scan(&n);
    if (!recs) { snprintf(buf, len, "[]"); return -1; }

    size_t o = snprintf(buf, len, "[");
    int shown = 0;
    for (int i = 0; i < n; i++) {
        const char *ssid = (const char *)recs[i].ssid;
        if (!ssid[0]) continue;
        bool dup = false;
        for (int j = 0; j < i; j++) {
            if (strcmp(ssid, (const char *)recs[j].ssid) == 0) { dup = true; break; }
        }
        if (dup) continue;
        char esc[100];
        json_escape(ssid, esc, sizeof(esc));
        int w = snprintf(buf + o, len - o, "%s\"%s\"", shown ? "," : "", esc);
        if (w < 0 || o + w + 2 >= len) break;
        o += w;
        shown++;
    }
    snprintf(buf + o, len - o, "]");
    free(recs);
    return shown;
}

void wifi_diagnose(const char *ssid) {
    ESP_LOGI(TAG, "scanning to see what is actually in range...");
    // The radio must be idle to scan. Scanning while the station is still
    // trying to associate returns nothing and looks like a dead antenna.
    s_hold_reconnect = true;
    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(200));

    uint16_t n;
    wifi_ap_record_t *recs = scan(&n);
    s_hold_reconnect = false;
    if (!recs || n == 0) {
        ESP_LOGW(TAG, "scan found no networks. If this persists, suspect the antenna "
                      "or an underpowered USB port rather than the credentials.");
        free(recs);
        return;
    }

    bool found = false;
    ESP_LOGI(TAG, "%d network(s) visible on 2.4 GHz:", n);
    for (int i = 0; i < n; i++) {
        bool ours = strcmp((const char *)recs[i].ssid, ssid) == 0;
        found |= ours;
        ESP_LOGI(TAG, "  %-32s ch%-3d %4d dBm  %s%s", (const char *)recs[i].ssid,
                 recs[i].primary, recs[i].rssi,
                 recs[i].authmode == WIFI_AUTH_OPEN ? "open" : "secured",
                 ours ? "   <== configured SSID" : "");
    }
    free(recs);

    if (found) {
        ESP_LOGW(TAG, "SSID is in range, so the password is the likely problem");
    } else {
        ESP_LOGW(TAG, "\"%s\" was NOT found. It is probably 5 GHz-only (the ESP32 "
                      "cannot see 5 GHz), hidden, or misspelled.", ssid);
    }
}
