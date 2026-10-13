#include "peerapi.h"

#include <string.h>

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "lwip/sockets.h"
#include "mbedtls/base64.h"

#include "config.h"
#include "wifi.h"

static const char *TAG = "peerapi";

#define DNS_MAX           4096
#define UPSTREAM_TIMEOUT  1500    // ms per server, UDP
#define TCP_TIMEOUT       3000    // ms, only for truncated answers
#define FALLBACK_DNS      0x01010101u   // 1.1.1.1 (same in either byte order)

static httpd_handle_t s_server;
static peerapi_stats_t s_stats;

// The server task handles one request at a time, so static buffers are safe.
static uint8_t s_query[DNS_MAX];
static uint8_t s_reply[DNS_MAX];

// ---- source check -------------------------------------------------------------

// esp_http_server listens on a dual-stack IPv6 socket, so IPv4 clients show up
// as ::ffff:a.b.c.d.
static bool from_tailnet(httpd_req_t *req) {
    struct sockaddr_storage ss;
    socklen_t len = sizeof(ss);
    if (getpeername(httpd_req_to_sockfd(req), (struct sockaddr *)&ss, &len) != 0) return false;

    uint32_t a = 0;   // network byte order
    if (ss.ss_family == AF_INET) {
        a = ((struct sockaddr_in *)&ss)->sin_addr.s_addr;
    } else if (ss.ss_family == AF_INET6) {
        const uint8_t *b = ((struct sockaddr_in6 *)&ss)->sin6_addr.s6_addr;
        static const uint8_t mapped[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};
        if (memcmp(b, mapped, 12) != 0) return false;
        memcpy(&a, b + 12, 4);
    } else {
        return false;
    }
    return (ntohl(a) & 0xFFC00000u) == 0x64400000u;   // 100.64.0.0/10
}

// ---- upstream -----------------------------------------------------------------

static void set_timeout(int s, int ms) {
    struct timeval tv = { .tv_sec = ms / 1000, .tv_usec = (ms % 1000) * 1000 };
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

static int query_udp(uint32_t server, const uint8_t *q, size_t qlen, uint8_t *out, size_t cap) {
    int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s < 0) return -1;
    set_timeout(s, UPSTREAM_TIMEOUT);
    struct sockaddr_in to = { .sin_family = AF_INET, .sin_port = htons(53), .sin_addr.s_addr = server };
    int n = -1;
    if (sendto(s, q, qlen, 0, (struct sockaddr *)&to, sizeof(to)) == (int)qlen) {
        int64_t deadline = esp_timer_get_time() + UPSTREAM_TIMEOUT * 1000LL;
        while (esp_timer_get_time() < deadline) {
            struct sockaddr_in from;
            socklen_t fl = sizeof(from);
            int r = recvfrom(s, out, cap, 0, (struct sockaddr *)&from, &fl);
            if (r < 0) break;   // timeout
            // Same server, same transaction ID -- ignore anything else.
            if (r >= 12 && from.sin_addr.s_addr == server && out[0] == q[0] && out[1] == q[1]) {
                n = r;
                break;
            }
        }
    }
    close(s);
    return n;
}

static bool io_all(int s, void *buf, size_t len, bool sending) {
    uint8_t *p = buf;
    while (len) {
        int r = sending ? send(s, p, len, 0) : recv(s, p, len, 0);
        if (r <= 0) return false;
        p += r;
        len -= r;
    }
    return true;
}

// DNS over TCP (RFC 1035 4.2.2): two-byte length prefix each way.
static int query_tcp(uint32_t server, const uint8_t *q, size_t qlen, uint8_t *out, size_t cap) {
    int s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s < 0) return -1;
    set_timeout(s, TCP_TIMEOUT);
    struct sockaddr_in to = { .sin_family = AF_INET, .sin_port = htons(53), .sin_addr.s_addr = server };
    int n = -1;
    uint8_t hdr[2] = { qlen >> 8, qlen & 0xFF };
    if (connect(s, (struct sockaddr *)&to, sizeof(to)) == 0 &&
        io_all(s, hdr, 2, true) && io_all(s, (void *)q, qlen, true) &&
        io_all(s, hdr, 2, false)) {
        size_t rlen = (hdr[0] << 8) | hdr[1];
        if (rlen >= 12 && rlen <= cap && io_all(s, out, rlen, false)) n = rlen;
    }
    close(s);
    return n;
}

static int resolve(const uint8_t *q, size_t qlen, uint8_t *out, size_t cap) {
    // The LAN's resolvers first (DHCP-provided), then a public fallback.
    uint32_t servers[3];
    int count = 0;
    esp_netif_dns_info_t info;
    esp_netif_t *sta = wifi_sta_netif();
    if (esp_netif_get_dns_info(sta, ESP_NETIF_DNS_MAIN, &info) == ESP_OK && info.ip.u_addr.ip4.addr) {
        servers[count++] = info.ip.u_addr.ip4.addr;
    }
    if (esp_netif_get_dns_info(sta, ESP_NETIF_DNS_BACKUP, &info) == ESP_OK && info.ip.u_addr.ip4.addr &&
        (count == 0 || info.ip.u_addr.ip4.addr != servers[0])) {
        servers[count++] = info.ip.u_addr.ip4.addr;
    }
    servers[count++] = FALLBACK_DNS;

    for (int i = 0; i < count; i++) {
        int n = query_udp(servers[i], q, qlen, out, cap);
        if (n <= 0) continue;
        if (out[2] & 0x02) {   // TC: answer did not fit in UDP
            int t = query_tcp(servers[i], q, qlen, out, cap);
            if (t > 0) return t;
            // Fall through with the truncated answer; the client may retry.
            n = query_udp(servers[i], q, qlen, out, cap);
        }
        if (n > 0) return n;
    }
    return -1;
}

// ---- HTTP -----------------------------------------------------------------------

static esp_err_t answer(httpd_req_t *req, size_t qlen) {
    if (qlen < 12) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "not a DNS message");
    }
    s_stats.queries++;
    int64_t t0 = esp_timer_get_time();
    int n = resolve(s_query, qlen, s_reply, sizeof(s_reply));
    s_stats.total_ms += (uint32_t)((esp_timer_get_time() - t0) / 1000);
    if (n <= 0) {
        s_stats.failures++;
        httpd_resp_set_status(req, "502 Bad Gateway");
        return httpd_resp_sendstr(req, "upstream DNS did not answer");
    }
    // The Tailscale client checks this Content-Type exactly.
    httpd_resp_set_type(req, "application/dns-message");
    return httpd_resp_send(req, (const char *)s_reply, n);
}

static bool check_source(httpd_req_t *req) {
    if (from_tailnet(req)) return true;
    s_stats.refused++;
    httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "tailnet peers only");
    return false;
}

static esp_err_t h_post(httpd_req_t *req) {
    if (!check_source(req)) return ESP_OK;
    if (req->content_len == 0 || req->content_len > sizeof(s_query)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad length");
    }
    size_t got = 0;
    while (got < req->content_len) {
        int r = httpd_req_recv(req, (char *)s_query + got, req->content_len - got);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (r <= 0) return ESP_FAIL;
        got += r;
    }
    return answer(req, got);
}

// RFC 8484 GET: ?dns=<base64url, no padding>
static esp_err_t h_get(httpd_req_t *req) {
    if (!check_source(req)) return ESP_OK;
    static char query[1400];
    static char b64[1400];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "dns", b64, sizeof(b64) - 4) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing dns parameter");
    }
    size_t len = strlen(b64);
    for (size_t i = 0; i < len; i++) {
        if (b64[i] == '-') b64[i] = '+';
        else if (b64[i] == '_') b64[i] = '/';
    }
    while (len % 4) b64[len++] = '=';
    b64[len] = '\0';
    size_t qlen = 0;
    if (mbedtls_base64_decode(s_query, sizeof(s_query), &qlen, (const unsigned char *)b64, len) != 0) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad base64");
    }
    return answer(req, qlen);
}

void peerapi_start(void) {
    if (s_server) return;
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = PEERAPI_PORT;
    cfg.ctrl_port = ESP_HTTPD_DEF_CTRL_PORT + 1;   // the portal has the default
    cfg.max_open_sockets = 4;
    cfg.max_uri_handlers = 2;
    cfg.lru_purge_enable = true;
    cfg.stack_size = 6144;

    if (httpd_start(&s_server, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start on :%d failed", PEERAPI_PORT);
        s_server = NULL;
        return;
    }
    static const httpd_uri_t post = { .uri = "/dns-query", .method = HTTP_POST, .handler = h_post };
    static const httpd_uri_t get  = { .uri = "/dns-query", .method = HTTP_GET,  .handler = h_get };
    httpd_register_uri_handler(s_server, &post);
    httpd_register_uri_handler(s_server, &get);
    ESP_LOGI(TAG, "DoH on :%d/dns-query", PEERAPI_PORT);
}

void peerapi_get_stats(peerapi_stats_t *out) { *out = s_stats; }
