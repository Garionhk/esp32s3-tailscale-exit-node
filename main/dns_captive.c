#include "dns_captive.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

static const char *TAG = "dns";

static TaskHandle_t s_task;
static volatile bool s_run;
static uint32_t s_ip;

#define DNS_PORT 53
#define DNS_MAX  512

// Builds the reply in place. Returns its length, or 0 to stay silent.
static int answer(uint8_t *buf, int len) {
    if (len < 12 || (buf[2] & 0x80)) return 0;          // short, or not a query
    uint16_t qd = (buf[4] << 8) | buf[5];
    if (qd == 0) return 0;

    // Walk the first question's name.
    int p = 12;
    while (p < len && buf[p] != 0) {
        if (buf[p] & 0xC0) return 0;                     // no compression in queries
        p += buf[p] + 1;
    }
    p += 1;                                              // the zero label
    if (p + 4 > len) return 0;
    uint16_t qtype = (buf[p] << 8) | buf[p + 1];
    p += 4;                                              // qtype + qclass

    // Reply: same ID, QR=1 AA=1 RD copied, RA=1, one question kept.
    buf[2] = 0x84 | (buf[2] & 0x01);
    buf[3] = 0x80;
    buf[4] = 0; buf[5] = 1;                              // qdcount 1
    buf[6] = 0; buf[7] = 0;                              // ancount
    buf[8] = buf[9] = buf[10] = buf[11] = 0;             // nscount, arcount

    if (qtype != 1 /* A */ || p + 16 > DNS_MAX) return p;   // NODATA for anything else

    buf[7] = 1;
    uint8_t *a = buf + p;
    a[0] = 0xC0; a[1] = 0x0C;                            // pointer to the question name
    a[2] = 0; a[3] = 1;                                  // type A
    a[4] = 0; a[5] = 1;                                  // class IN
    a[6] = 0; a[7] = 0; a[8] = 0; a[9] = 60;             // TTL 60 s
    a[10] = 0; a[11] = 4;                                // rdlength
    memcpy(a + 12, &s_ip, 4);                            // already network order
    return p + 16;
}

static void dns_task(void *arg) {
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "socket failed");
        goto out;
    }
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(DNS_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "bind :53 failed");
        close(sock);
        goto out;
    }
    struct timeval tv = { .tv_sec = 1 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    static uint8_t buf[DNS_MAX];
    while (s_run) {
        struct sockaddr_in from;
        socklen_t flen = sizeof(from);
        int n = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr *)&from, &flen);
        if (n <= 0) continue;
        int r = answer(buf, n);
        if (r > 0) sendto(sock, buf, r, 0, (struct sockaddr *)&from, flen);
    }
    close(sock);
out:
    s_task = NULL;
    vTaskDelete(NULL);
}

void dns_captive_start(uint32_t ap_ip_nbo) {
    s_ip = ap_ip_nbo;
    s_run = true;    // also revives a task that was told to stop but has not yet
    if (s_task) return;
    xTaskCreate(dns_task, "dns_captive", 3072, NULL, 4, &s_task);
    ESP_LOGI(TAG, "captive DNS on :53");
}

void dns_captive_stop(void) {
    s_run = false;   // the task notices within its 1 s receive timeout
}
