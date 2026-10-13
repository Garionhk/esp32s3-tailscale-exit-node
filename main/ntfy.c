#include "ntfy.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"

#include "config.h"
#include "settings.h"
#include "wanip.h"
#include "wifi.h"

static const char *TAG = "ntfy";

typedef struct {
    char *title;
    char *body;
} pub_item_t;

static QueueHandle_t s_queue;
static volatile bool s_sending;   // a dequeued message is being posted
static ntfy_message_cb_t s_on_message;
static const char *volatile s_state = "off";

static char s_auth[192];          // "Bearer ..." / "Basic ..." / ""
static char s_base[160];          // "https://ntfy.sh:443/<topic>"

// Subscription state
static esp_http_client_handle_t s_sub;
static int64_t s_last_data_us;
static int64_t s_next_attempt_us;
static uint32_t s_backoff_ms = NTFY_RECONNECT_MIN_MS;
static char s_line[NTFY_MAX_LINE + 1];
static size_t s_line_len;
static bool s_line_oversized;

// Last message ID seen, used as ?since= on reconnect so the gap while we were
// away is replayed. Deliberately empty on cold boot: replaying the topic's
// history would re-run every command ever sent.
static char s_last_id[40];

// Ring of recently handled IDs. A reconnect can legitimately redeliver a
// message already acted on; without this a reboot command could run twice.
static char s_seen[NTFY_DEDUP_SLOTS][40];
static int s_seen_next;

static int64_t now_us(void) { return esp_timer_get_time(); }

static char *psram_strdup(const char *s) {
    size_t n = strlen(s) + 1;
    char *d = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!d) d = malloc(n);
    if (d) memcpy(d, s, n);
    return d;
}

static void build_auth(void) {
    s_auth[0] = '\0';
    if (g_settings.ntfy_token[0]) {
        snprintf(s_auth, sizeof(s_auth), "Bearer %s", g_settings.ntfy_token);
    } else if (g_settings.ntfy_user[0]) {
        char raw[140];
        snprintf(raw, sizeof(raw), "%s:%s", g_settings.ntfy_user, g_settings.ntfy_pass);
        size_t olen = 0;
        strcpy(s_auth, "Basic ");
        mbedtls_base64_encode((unsigned char *)s_auth + 6, sizeof(s_auth) - 6, &olen,
                              (const unsigned char *)raw, strlen(raw));
        s_auth[6 + olen] = '\0';
    }
}

static esp_http_client_handle_t make_client(const char *url, int timeout_ms) {
    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = timeout_ms,
        .user_agent = "esp32-ts-exit",
        .buffer_size = 1024,
        .buffer_size_tx = 768,
    };
    if (g_settings.ntfy_tls) cfg.crt_bundle_attach = esp_crt_bundle_attach;
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (c && s_auth[0]) esp_http_client_set_header(c, "Authorization", s_auth);
    return c;
}

static void login_hint(int status) {
    if (status == 401 || status == 403) {
        ESP_LOGW(TAG, "the server rejected the login -- check the ntfy username, password "
                      "or token on the settings page, and this user's access to the topic");
        s_state = "login refused";
    }
}

// ---- publish ------------------------------------------------------------------

static void do_publish(const pub_item_t *it) {
    esp_http_client_handle_t c = make_client(s_base, 10000);
    if (!c) return;
    esp_http_client_set_method(c, HTTP_METHOD_POST);
    esp_http_client_set_header(c, "Content-Type", "text/plain; charset=utf-8");
    esp_http_client_set_header(c, "X-Tags", NTFY_SELF_TAG);   // lets the stream skip our echo
    if (it->title && it->title[0]) esp_http_client_set_header(c, "X-Title", it->title);
    esp_http_client_set_post_field(c, it->body, strlen(it->body));
    esp_err_t err = esp_http_client_perform(c);
    int status = esp_http_client_get_status_code(c);
    esp_http_client_cleanup(c);
    if (err != ESP_OK || status != 200) {
        ESP_LOGW(TAG, "publish failed (%s, HTTP %d, heap %u)", esp_err_to_name(err), status,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        login_hint(status);
    }
}

// ---- subscribe ----------------------------------------------------------------

static bool already_seen(const char *id) {
    if (!id[0]) return false;
    for (int i = 0; i < NTFY_DEDUP_SLOTS; i++) {
        if (strcmp(s_seen[i], id) == 0) return true;
    }
    strlcpy(s_seen[s_seen_next], id, sizeof(s_seen[0]));
    s_seen_next = (s_seen_next + 1) % NTFY_DEDUP_SLOTS;
    return false;
}

// One newline-delimited JSON object from the stream.
static void handle_line(const char *line) {
    if (strlen(line) < 2) return;   // blank keepalive padding
    cJSON *doc = cJSON_Parse(line);
    if (!doc) {
        ESP_LOGW(TAG, "could not parse line");
        return;
    }
    const cJSON *event = cJSON_GetObjectItem(doc, "event");
    if (!cJSON_IsString(event) || strcmp(event->valuestring, "message") != 0) {
        // "open", "keepalive", "poll_request": nothing to do; they already
        // proved the connection is alive.
        cJSON_Delete(doc);
        return;
    }
    // Only real messages advance the cursor. Keepalives carry IDs too, and
    // feeding one back as ?since= asks for a message the server never stored.
    const cJSON *id = cJSON_GetObjectItem(doc, "id");
    const char *ids = cJSON_IsString(id) ? id->valuestring : "";
    if (ids[0]) strlcpy(s_last_id, ids, sizeof(s_last_id));

    // Commands and replies share one topic, so skip anything this device sent.
    const cJSON *tags = cJSON_GetObjectItem(doc, "tags");
    const cJSON *tag;
    cJSON_ArrayForEach(tag, tags) {
        if (cJSON_IsString(tag) && strcmp(tag->valuestring, NTFY_SELF_TAG) == 0) {
            cJSON_Delete(doc);
            return;
        }
    }
    if (already_seen(ids)) {
        ESP_LOGI(TAG, "ignoring duplicate %s", ids);
        cJSON_Delete(doc);
        return;
    }
    const cJSON *msg = cJSON_GetObjectItem(doc, "message");
    if (cJSON_IsString(msg) && msg->valuestring[0] && s_on_message) {
        s_on_message(msg->valuestring);   // never logged: it carries the passphrase
    }
    cJSON_Delete(doc);
}

static void sub_close(const char *why) {
    if (s_sub) {
        ESP_LOGW(TAG, "disconnecting: %s", why);
        esp_http_client_close(s_sub);
        esp_http_client_cleanup(s_sub);
        s_sub = NULL;
    }
    if (strcmp(s_state, "login refused") != 0) s_state = "reconnecting";
    s_line_len = 0;
    s_line_oversized = false;
    s_next_attempt_us = now_us() + (int64_t)s_backoff_ms * 1000;
    s_backoff_ms = s_backoff_ms * 2 > NTFY_RECONNECT_MAX_MS ? NTFY_RECONNECT_MAX_MS : s_backoff_ms * 2;
}

static void sub_connect(void) {
    char url[220];
    snprintf(url, sizeof(url), "%s/json%s%s", s_base, s_last_id[0] ? "?since=" : "", s_last_id);
    ESP_LOGI(TAG, "connecting to %s:%u (%s, heap %u)", g_settings.ntfy_host,
             (unsigned)g_settings.ntfy_port, g_settings.ntfy_tls ? "tls" : "plain",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    if (strcmp(s_state, "off") == 0) s_state = "connecting";

    // Short read timeout: the task polls the stream between other jobs.
    s_sub = make_client(url, 500);
    if (!s_sub) { sub_close("client init failed"); return; }
    esp_http_client_set_header(s_sub, "Accept", "application/x-ndjson");
    if (esp_http_client_open(s_sub, 0) != ESP_OK) { sub_close("connect failed"); return; }
    if (esp_http_client_fetch_headers(s_sub) < 0) { sub_close("no response headers"); return; }
    int status = esp_http_client_get_status_code(s_sub);
    if (status != 200) {
        ESP_LOGW(TAG, "unexpected response: HTTP %d", status);
        login_hint(status);
        sub_close("bad status");
        return;
    }
    ESP_LOGI(TAG, "subscribed");
    s_state = "subscribed";
    s_last_data_us = now_us();
    s_backoff_ms = NTFY_RECONNECT_MIN_MS;
    s_line_len = 0;
    s_line_oversized = false;
}

static void sub_poll(void) {
    char buf[512];
    int n = esp_http_client_read(s_sub, buf, sizeof(buf));
    if (n == -ESP_ERR_HTTP_EAGAIN) {
        // nothing yet
    } else if (n <= 0) {
        sub_close(n == 0 ? "stream closed by server" : "read error");
        return;
    } else {
        s_last_data_us = now_us();
        for (int i = 0; i < n; i++) {
            char c = buf[i];
            if (c == '\n') {
                if (!s_line_oversized) {
                    s_line[s_line_len] = '\0';
                    handle_line(s_line);
                }
                s_line_len = 0;
                s_line_oversized = false;
            } else if (c != '\r') {
                // Drop absurdly long lines rather than growing without bound.
                if (s_line_len < NTFY_MAX_LINE) s_line[s_line_len++] = c;
                else if (!s_line_oversized) {
                    ESP_LOGW(TAG, "line too long, discarding");
                    s_line_oversized = true;
                }
            }
        }
    }
    if (now_us() - s_last_data_us > (int64_t)NTFY_STREAM_TIMEOUT_MS * 1000) {
        sub_close("no data past keepalive timeout");
    }
}

// ---- task -----------------------------------------------------------------------

static void ntfy_task(void *arg) {
    for (;;) {
        if (!wifi_is_connected()) {
            if (s_sub) sub_close("Wi-Fi down");
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        wanip_tick();

        pub_item_t it;
        while (xQueueReceive(s_queue, &it, 0) == pdTRUE) {
            s_sending = true;
            do_publish(&it);
            free(it.title);
            free(it.body);
            s_sending = false;
        }

        if (s_sub) {
            sub_poll();   // blocks up to the 500 ms read timeout
        } else if (now_us() >= s_next_attempt_us) {
            sub_connect();
        } else {
            // Waiting out the backoff, but stay responsive to publishes.
            if (xQueueReceive(s_queue, &it, pdMS_TO_TICKS(250)) == pdTRUE) {
                s_sending = true;
                do_publish(&it);
                free(it.title);
                free(it.body);
                s_sending = false;
            }
        }
    }
}

void ntfy_start(ntfy_message_cb_t on_message) {
    if (s_queue || !g_settings.ntfy_topic[0]) return;
    s_on_message = on_message;
    build_auth();
    snprintf(s_base, sizeof(s_base), "%s://%s:%u/%s", g_settings.ntfy_tls ? "https" : "http",
             g_settings.ntfy_host, (unsigned)g_settings.ntfy_port, g_settings.ntfy_topic);
    s_queue = xQueueCreate(NTFY_PUBLISH_QUEUE, sizeof(pub_item_t));
    s_state = "connecting";
    xTaskCreate(ntfy_task, "ntfy", 8192, NULL, 3, NULL);
}

bool ntfy_enabled(void) { return s_queue != NULL; }

bool ntfy_publish(const char *title, const char *body) {
    if (!s_queue) return false;
    pub_item_t it = { .title = psram_strdup(title ? title : ""), .body = psram_strdup(body) };
    if (!it.title || !it.body) { free(it.title); free(it.body); return false; }
    if (xQueueSend(s_queue, &it, 0) != pdTRUE) {
        free(it.title);
        free(it.body);
        ESP_LOGW(TAG, "publish queue full, message dropped");
        return false;
    }
    return true;
}

int ntfy_pending(void) {
    return s_queue ? (int)uxQueueMessagesWaiting(s_queue) + (s_sending ? 1 : 0) : 0;
}

const char *ntfy_state(void) { return s_state; }
