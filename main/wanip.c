#include "wanip.h"

#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lwip/inet.h"

#include "config.h"

static const char *TAG = "wanip";

static char s_ip[16];
static volatile int64_t s_next_us;   // 0 = due now
static SemaphoreHandle_t s_lock;

static int64_t now_us(void) { return esp_timer_get_time(); }

void wanip_refresh_soon(void) { s_next_us = 0; }

bool wanip_get(char *buf, size_t len) {
    if (!s_lock) { if (len) buf[0] = '\0'; return false; }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(buf, s_ip, len);
    xSemaphoreGive(s_lock);
    return buf[0] != '\0';
}

void wanip_tick(void) {
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    if (now_us() < s_next_us) return;

    esp_http_client_config_t cfg = {
        .url = WANIP_URL,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 8000,
        .user_agent = "esp32-ts-exit",
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    char body[24] = "";
    bool ok = false;
    if (c && esp_http_client_open(c, 0) == ESP_OK &&
        esp_http_client_fetch_headers(c) >= 0 &&
        esp_http_client_get_status_code(c) == 200) {
        int n = esp_http_client_read(c, body, sizeof(body) - 1);
        if (n > 0) {
            body[n] = '\0';
            // Trim trailing whitespace and insist on a dotted IPv4 address,
            // so an error page never ends up in a notification.
            for (int i = n - 1; i >= 0 && (body[i] == '\n' || body[i] == '\r' || body[i] == ' '); i--) {
                body[i] = '\0';
            }
            struct in_addr a;
            ok = inet_pton(AF_INET, body, &a) == 1;
        }
    }
    if (c) esp_http_client_cleanup(c);

    if (ok) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        bool changed = strcmp(s_ip, body) != 0;
        strlcpy(s_ip, body, sizeof(s_ip));
        xSemaphoreGive(s_lock);
        if (changed) ESP_LOGI(TAG, "public address %s", body);
        s_next_us = now_us() + (int64_t)WANIP_INTERVAL_MS * 1000;
    } else {
        ESP_LOGW(TAG, "lookup failed, retrying in %d s", WANIP_RETRY_MS / 1000);
        s_next_us = now_us() + (int64_t)WANIP_RETRY_MS * 1000;
    }
}
