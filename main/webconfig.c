#include "webconfig.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "lwip/ip4_addr.h"
#include "mbedtls/base64.h"

#include "config.h"
#include "dns_captive.h"
#include "exitnode.h"
#include "ntfy.h"
#include "peerapi.h"
#include "settings.h"
#include "tailscale.h"
#include "wanip.h"
#include "wifi.h"

static const char *TAG = "web";

static httpd_handle_t s_server;
static volatile bool s_ap_mode;
static volatile bool s_auth_needed = true;
static volatile bool s_saved;

// ---- output helpers -----------------------------------------------------------

static void out(httpd_req_t *req, const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0) httpd_resp_send_chunk(req, buf, n < (int)sizeof(buf) ? n : (int)sizeof(buf) - 1);
}

static void out_raw(httpd_req_t *req, const char *s) {
    httpd_resp_send_chunk(req, s, HTTPD_RESP_USE_STRLEN);
}

// HTML-escapes into the response.
static void out_esc(httpd_req_t *req, const char *s) {
    char buf[256];
    size_t o = 0;
    for (; *s; s++) {
        const char *rep = NULL;
        switch (*s) {
        case '&':  rep = "&amp;";  break;
        case '<':  rep = "&lt;";   break;
        case '>':  rep = "&gt;";   break;
        case '"':  rep = "&quot;"; break;
        case '\'': rep = "&#39;";  break;
        }
        size_t need = rep ? strlen(rep) : 1;
        if (o + need >= sizeof(buf)) {
            httpd_resp_send_chunk(req, buf, o);
            o = 0;
        }
        if (rep) { memcpy(buf + o, rep, need); o += need; }
        else buf[o++] = *s;
    }
    if (o) httpd_resp_send_chunk(req, buf, o);
}

static void json_esc(const char *in, char *out, size_t len) {
    size_t o = 0;
    for (; *in && o + 7 < len; in++) {
        unsigned char c = (unsigned char)*in;
        if (c == '"' || c == '\\') { out[o++] = '\\'; out[o++] = c; }
        else if (c < 0x20) o += snprintf(out + o, len - o, "\\u%04x", c);
        else out[o++] = c;
    }
    out[o] = '\0';
}

static const char PAGE_HEAD[] =
    "<!doctype html><meta charset=utf-8>"
    "<meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>Tailscale exit node</title><style>"
    "body{font-family:system-ui,sans-serif;margin:0;padding:1rem;max-width:34rem;"
    "background:#111;color:#eee}"
    "h1{font-size:1.2rem}h2{font-size:.95rem;margin:1.4rem 0 .4rem;color:#8bd}"
    "label{display:block;margin:.6rem 0 .15rem;font-size:.85rem;color:#bbb}"
    "label.cb{display:flex;gap:.5rem;align-items:center;color:#eee;font-size:.95rem}"
    "input[type=text],input[type=password]{width:100%;"
    "box-sizing:border-box;padding:.5rem;border:1px solid #444;border-radius:4px;"
    "background:#1c1c1c;color:#eee;font-size:1rem}"
    "small{color:#888;font-size:.75rem;display:block;margin-top:.2rem}"
    "a{color:#8bd}"
    "button{margin-top:1.2rem;width:100%;padding:.7rem;font-size:1rem;border:0;"
    "border-radius:4px;background:#2a7;color:#022;font-weight:600}"
    "table{width:100%;border-collapse:collapse;font-size:.85rem}"
    "td{padding:.2rem 0;vertical-align:top}td:first-child{color:#999;width:40%}"
    ".err{background:#833;padding:.5rem;border-radius:4px;margin:.8rem 0}"
    ".ok{color:#5d9}.bad{color:#e77}"
    "</style>";

// ---- auth -------------------------------------------------------------------

// Constant-time so the comparison does not leak how much of a guess matched.
static bool secure_equals(const char *a, const char *b) {
    size_t la = strlen(a), lb = strlen(b);
    unsigned char diff = la != lb;
    for (size_t i = 0; i < la; i++) diff |= (unsigned char)a[i] ^ (unsigned char)b[i % (lb ? lb : 1)];
    return diff == 0;
}

// Returns false and answers the request if it should be rejected.
static bool authorized(httpd_req_t *req) {
    if (!s_auth_needed) return true;

    // No admin password stored. Leaving the page open would let anyone on the
    // LAN (or anyone in range of the fallback AP) repoint the device, so it
    // stays shut until one is set -- which needs the BOOT button.
    if (!g_settings.admin_pass[0]) {
        httpd_resp_set_status(req, "403 Forbidden");
        httpd_resp_set_type(req, "text/html");
        out_raw(req, PAGE_HEAD);
        out(req, "<h1>Settings locked</h1><p>No admin password is set yet. Press "
                 "<b>EN</b> (reset) on the board, then immediately press and hold "
                 "<b>BOOT</b> for two seconds. It opens the <b>%s</b> network, where "
                 "you can set one.</p>", wifi_ap_ssid());
        httpd_resp_send_chunk(req, NULL, 0);
        return false;
    }

    char hdr[192];
    if (httpd_req_get_hdr_value_str(req, "Authorization", hdr, sizeof(hdr)) == ESP_OK &&
        strncmp(hdr, "Basic ", 6) == 0) {
        unsigned char dec[144];
        size_t olen = 0;
        if (mbedtls_base64_decode(dec, sizeof(dec) - 1, &olen,
                                  (const unsigned char *)hdr + 6, strlen(hdr + 6)) == 0) {
            dec[olen] = '\0';
            char expect[sizeof(ADMIN_USER) + sizeof(g_settings.admin_pass) + 1];
            snprintf(expect, sizeof(expect), "%s:%s", ADMIN_USER, g_settings.admin_pass);
            if (secure_equals((const char *)dec, expect)) return true;
        }
    }

    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"Tailscale exit node\"");
    httpd_resp_sendstr(req, "Login required");
    return false;
}

// ---- status ---------------------------------------------------------------------

static const char *mode_name(void) {
    if (!s_ap_mode) return "online";
    return s_auth_needed ? "Wi-Fi fallback (setup AP open)" : "setup";
}

static void fmt_ip(uint32_t nbo, char *buf, size_t len) {
    if (!nbo) { snprintf(buf, len, "-"); return; }
    ip4_addr_t a = { .addr = nbo };
    snprintf(buf, len, IPSTR, IP2STR(&a));
}

static void send_status_table(httpd_req_t *req) {
    char ip[20], vpn[20];
    fmt_ip(wifi_sta_ip(), ip, sizeof(ip));
    tailscale_vpn_ip_str(vpn, sizeof(vpn));
    exitnode_stats_t st;
    exitnode_get_stats(&st);

    out_raw(req, "<h2>Status</h2><table>");
    out(req, "<tr><td>Mode</td><td>%s</td></tr>", mode_name());

    out_raw(req, "<tr><td>Wi-Fi</td><td>");
    if (wifi_is_connected()) {
        out_esc(req, g_settings.wifi_ssid);
        out(req, " <span class=ok>connected</span>, %s, %d dBm", ip, wifi_rssi());
    } else if (g_settings.provisioned) {
        out(req, "<span class=bad>not connected</span> (%s)",
            wifi_reason_text(wifi_last_disconnect_reason()));
    } else {
        out_raw(req, "not configured");
    }
    out_raw(req, "</td></tr>");

    out(req, "<tr><td>Tailscale</td><td><span class=%s>%s</span>",
        tailscale_connected() ? "ok" : (tailscale_failed() ? "bad" : "x"),
        tailscale_state_name());
    if (vpn[0]) out(req, ", %s", vpn);
    out_raw(req, "</td></tr>");
    if (tailscale_error()[0]) {
        out_raw(req, "<tr><td>Tailscale error</td><td class=bad>");
        out_esc(req, tailscale_error());
        out_raw(req, "</td></tr>");
    }
    out_raw(req, "<tr><td>Device name</td><td>");
    out_esc(req, settings_effective_hostname());
    out_raw(req, "</td></tr>");
    int direct, relayed;
    tailscale_peer_paths(&direct, &relayed);
    out(req, "<tr><td>Peers</td><td>%d (%d direct, %d via DERP relay)</td></tr>",
        tailscale_peer_count(), direct, relayed);
    char dcode[8];
    uint16_t dreg = tailscale_derp_region(dcode, sizeof(dcode));
    if (dreg) {
        out(req, "<tr><td>DERP relay</td><td>region %u (%s), %s</td></tr>", dreg, dcode,
            g_settings.derp_region ? "fixed in settings" : "lowest latency");
    }

    out(req, "<tr><td>Exit node</td><td>%s%s</td></tr>",
        g_settings.exit_enabled ? "advertised" : "off",
        g_settings.exit_enabled && g_settings.block_lan ? ", LAN blocked" : "");
    out(req, "<tr><td>Forwarded</td><td>%lu packets, %llu KB</td></tr>",
        (unsigned long)st.fwd_packets, (unsigned long long)(st.fwd_bytes / 1024));
    out(req, "<tr><td>Dropped</td><td>LAN %lu, IPv6 %lu, exit off %lu, guard %lu, "
             "shed under load %lu, stack full %lu</td></tr>",
        (unsigned long)st.drop_lan, (unsigned long)st.drop_ipv6,
        (unsigned long)st.drop_disabled, (unsigned long)st.drop_guard,
        (unsigned long)st.shed, (unsigned long)st.stack_full);
    peerapi_stats_t ds;
    peerapi_get_stats(&ds);
    out(req, "<tr><td>DNS for clients</td><td>%lu queries, %lu failed, avg %lu ms</td></tr>",
        (unsigned long)ds.queries, (unsigned long)ds.failures,
        (unsigned long)(ds.queries ? ds.total_ms / ds.queries : 0));

    char wan[16];
    if (!wanip_get(wan, sizeof(wan))) strcpy(wan, "-");
    out(req, "<tr><td>Public IP</td><td>%s</td></tr>", wan);
    out(req, "<tr><td>ntfy</td><td>%s</td></tr>", ntfy_state());
    out(req, "<tr><td>Memory free</td><td>heap %u KB (lowest %u KB), PSRAM %u KB</td></tr>",
        (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
        (unsigned)(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024),
        (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    out(req, "<tr><td>Uptime</td><td>%llu min</td></tr>",
        (unsigned long long)(esp_timer_get_time() / 60000000ULL));
    out(req, "<tr><td>Firmware</td><td>%s</td></tr>", esp_app_get_description()->version);
    out_raw(req, "</table>");
}

// ---- pages ----------------------------------------------------------------------

static void send_form(httpd_req_t *req, const char *error) {
    const settings_t *s = &g_settings;
    bool fresh = !s->provisioned;

    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    out_raw(req, PAGE_HEAD);
    out_raw(req, "<h1>Tailscale exit node</h1>");
    if (error && error[0]) {
        out_raw(req, "<div class=err>");
        out_esc(req, error);
        out_raw(req, "</div>");
    }
    if (s->provisioned) send_status_table(req);

    out_raw(req, "<form method=POST action=/save>");

    out_raw(req, "<h2>Wi-Fi</h2><label>Network (2.4 GHz only)</label>"
                 "<input type=text name=ssid list=nets maxlength=32 required value='");
    out_esc(req, s->wifi_ssid);
    out_raw(req, "'><datalist id=nets></datalist>"
                 "<label>Password</label><input type=password name=wpass maxlength=64>");
    if (!fresh) out_raw(req, "<small>Leave blank to keep the stored password.</small>");

    out_raw(req, "<h2>Tailscale</h2><label>Auth key</label>"
                 "<input type=password name=authkey maxlength=127 autocomplete=off "
                 "placeholder='tskey-auth-...'>");
    out_raw(req, "<small>Create one at <a href='https://login.tailscale.com/admin/settings/keys' "
                 "target=_blank rel=noopener>login.tailscale.com/admin/settings/keys</a>. "
                 "A single-use key is enough for one device.");
    if (!fresh) out_raw(req, " Leave blank to keep the stored key. A <b>new</b> key "
                             "registers this device as a new machine.");
    out_raw(req, "</small><label>Device name</label><input type=text name=host maxlength=63 "
                 "pattern='[a-z0-9]([a-z0-9-]*[a-z0-9])?' value='");
    out_esc(req, s->ts_hostname);
    out_raw(req, "' placeholder='");
    out_esc(req, settings_effective_hostname());
    out_raw(req, "'><small>Lowercase letters, digits and hyphens. Blank uses the default "
                 "shown.</small>");
    if (!fresh) {
        out_raw(req, "<label class=cb><input type=checkbox name=forget> Forget Tailscale "
                     "identity (register as a new machine)</label>");
    }

    out_raw(req, "<h2>Exit node</h2>");
    out(req, "<label class=cb><input type=checkbox name=exit%s> Offer this device as an "
             "exit node</label>", s->exit_enabled ? " checked" : "");
    out(req, "<label class=cb><input type=checkbox name=nolan%s> Block access to private "
             "networks (this LAN) through the exit node</label>",
        s->block_lan ? " checked" : "");
    out_raw(req, "<small>After the device appears in the admin console, approve the exit "
                 "node under <i>Edit route settings</i>, and consider disabling key "
                 "expiry for it.</small>");
    char derp_val[8] = "";
    if (s->derp_region) snprintf(derp_val, sizeof(derp_val), "%u", s->derp_region);
    out(req, "<label>DERP relay region</label><input type=text name=derp inputmode=numeric "
             "maxlength=5 pattern='[0-9]*' placeholder='automatic' value='%s'>", derp_val);
    out_raw(req, "<small>Blank picks the fastest relay at start-up (shown in Status). "
                 "Enter a region number, e.g. 10 for Seattle, to fix it.</small>");

    // ntfy: pushes the addresses on boot / change, takes commands. Same
    // fields and rules as WOL-NTFY.
    out_raw(req, "<h2>ntfy <small>(optional: blank topic = off)</small></h2>"
                 "<label>Server</label><input type=text name=nhost maxlength=63 value='");
    out_esc(req, s->ntfy_host);
    out(req, "'><label>Port</label><input type=text name=nport inputmode=numeric maxlength=5 "
             "pattern='[0-9]*' value='%u'>", (unsigned)s->ntfy_port);
    out(req, "<label class=cb><input type=checkbox name=ntls%s> TLS (https)</label>",
        s->ntfy_tls ? " checked" : "");
    out_raw(req, "<label>Topic</label><input type=text name=ntopic maxlength=64 "
                 "pattern='[-_A-Za-z0-9]*' value='");
    out_esc(req, s->ntfy_topic);
    out_raw(req, "'><small>Messages and commands share this topic. Make it long and "
                 "unguessable.</small>"
                 "<label>Username</label><input type=text name=nuser maxlength=64 value='");
    out_esc(req, s->ntfy_user);
    out_raw(req, "'><label>Password</label><input type=password name=npass maxlength=64 "
                 "autocomplete=off>"
                 "<label>or Token</label><input type=password name=ntoken maxlength=79 "
                 "autocomplete=off>"
                 "<small>A token wins over username and password. Blank keeps stored values; "
                 "tick below to remove them.</small>"
                 "<label class=cb><input type=checkbox name=nclear> Clear stored ntfy login</label>"
                 "<label>Command passphrase</label><input type=password name=cmdpass "
                 "maxlength=64 autocomplete=off>"
                 "<small>Prefix for every command (<i>passphrase status</i>). Required when a "
                 "topic is set; must differ from the admin password. Blank keeps the stored "
                 "one.</small>");

    out_raw(req, "<h2>Admin password (this page)</h2>"
                 "<label>Password</label><input type=password name=admpass maxlength=64>");
    out(req, "<small>Login is <b>%s</b>.%s</small>", ADMIN_USER,
        fresh ? "" : " Leave blank to keep the stored one.");

    out_raw(req, "<button type=submit>Save &amp; restart</button></form>");

    // Populates the network picker. Typing a network name by hand is how
    // the SSID typo happened once already.
    out_raw(req, "<script>fetch('/scan').then(r=>r.json()).then(a=>{"
                 "document.getElementById('nets').innerHTML="
                 "a.map(s=>{const o=document.createElement('option');o.value=s;"
                 "return o.outerHTML}).join('')}).catch(()=>{});</script>");

    httpd_resp_send_chunk(req, NULL, 0);
}

static esp_err_t h_root(httpd_req_t *req) {
    if (!authorized(req)) return ESP_OK;
    send_form(req, NULL);
    return ESP_OK;
}

static esp_err_t h_scan(httpd_req_t *req) {
    if (!authorized(req)) return ESP_OK;
    char *buf = malloc(2048);
    if (!buf) return httpd_resp_send_500(req);
    wifi_scan_json(buf, 2048);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, buf);
    free(buf);
    return ESP_OK;
}

static esp_err_t h_status(httpd_req_t *req) {
    if (!authorized(req)) return ESP_OK;
    char ip[20], vpn[20], err[200], ssid[80];
    fmt_ip(wifi_sta_ip(), ip, sizeof(ip));
    tailscale_vpn_ip_str(vpn, sizeof(vpn));
    json_esc(tailscale_error(), err, sizeof(err));
    json_esc(g_settings.wifi_ssid, ssid, sizeof(ssid));
    exitnode_stats_t st;
    exitnode_get_stats(&st);
    peerapi_stats_t ds;
    peerapi_get_stats(&ds);

    char wan[16];
    wanip_get(wan, sizeof(wan));

    char *buf = malloc(1400);
    if (!buf) return httpd_resp_send_500(req);
    snprintf(buf, 1400,
             "{\"mode\":\"%s\",\"wifi\":{\"ssid\":\"%s\",\"connected\":%s,\"ip\":\"%s\",\"rssi\":%d},"
             "\"tailscale\":{\"state\":\"%s\",\"ip\":\"%s\",\"error\":\"%s\",\"peers\":%d},"
             "\"exit_node\":{\"enabled\":%s,\"block_lan\":%s,\"fwd_packets\":%lu,\"fwd_bytes\":%llu,"
             "\"drop_lan\":%lu,\"drop_ipv6\":%lu,\"drop_disabled\":%lu,\"drop_guard\":%lu},"
             "\"dns\":{\"queries\":%lu,\"failures\":%lu,\"refused\":%lu,\"total_ms\":%lu},"
             "\"public_ip\":\"%s\",\"ntfy\":\"%s\","
             "\"heap_free\":%u,\"heap_min\":%u,\"psram_free\":%u,\"uptime_s\":%llu,\"version\":\"%s\"}",
             mode_name(), ssid, wifi_is_connected() ? "true" : "false", ip, wifi_rssi(),
             tailscale_state_name(), vpn, err, tailscale_peer_count(),
             g_settings.exit_enabled ? "true" : "false", g_settings.block_lan ? "true" : "false",
             (unsigned long)st.fwd_packets, (unsigned long long)st.fwd_bytes,
             (unsigned long)st.drop_lan, (unsigned long)st.drop_ipv6,
             (unsigned long)st.drop_disabled, (unsigned long)st.drop_guard,
             (unsigned long)ds.queries, (unsigned long)ds.failures,
             (unsigned long)ds.refused, (unsigned long)ds.total_ms,
             wan, ntfy_state(),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned long long)(esp_timer_get_time() / 1000000ULL),
             esp_app_get_description()->version);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_sendstr(req, buf);
    free(buf);
    return ESP_OK;
}

// ---- form parsing -----------------------------------------------------------------

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    c = (char)tolower((unsigned char)c);
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

// Finds `key` in an application/x-www-form-urlencoded body and URL-decodes its
// value into out. Returns false if the key is absent. A value too long for
// `out` is reported as absent-with-overflow via *too_long.
static bool form_get(const char *body, const char *key, char *out, size_t len, bool *too_long) {
    size_t klen = strlen(key);
    const char *p = body;
    if (too_long) *too_long = false;
    while (p && *p) {
        const char *amp = strchr(p, '&');
        const char *end = amp ? amp : p + strlen(p);
        if ((size_t)(end - p) >= klen + 1 && strncmp(p, key, klen) == 0 && p[klen] == '=') {
            size_t o = 0;
            for (const char *v = p + klen + 1; v < end; v++) {
                char c = *v;
                if (c == '+') c = ' ';
                else if (c == '%' && v + 2 < end && hexval(v[1]) >= 0 && hexval(v[2]) >= 0) {
                    c = (char)(hexval(v[1]) * 16 + hexval(v[2]));
                    v += 2;
                }
                if (o + 1 >= len) { if (too_long) *too_long = true; out[0] = '\0'; return false; }
                out[o++] = c;
            }
            out[o] = '\0';
            return true;
        }
        p = amp ? amp + 1 : NULL;
    }
    if (len) out[0] = '\0';
    return false;
}

static void trim(char *s) {
    char *b = s;
    while (*b && isspace((unsigned char)*b)) b++;
    if (b != s) memmove(s, b, strlen(b) + 1);
    size_t n = strlen(s);
    while (n && isspace((unsigned char)s[n - 1])) s[--n] = '\0';
}

static bool valid_hostname(const char *h) {
    size_t n = strlen(h);
    if (n == 0 || n > 63 || h[0] == '-' || h[n - 1] == '-') return false;
    for (size_t i = 0; i < n; i++) {
        if (!(islower((unsigned char)h[i]) || isdigit((unsigned char)h[i]) || h[i] == '-')) return false;
    }
    return true;
}

// ntfy topics are [-_A-Za-z0-9]{1,64}; anything else would also break the URL.
static bool valid_topic(const char *t) {
    size_t n = strlen(t);
    if (n == 0 || n > 64) return false;
    for (; *t; t++) {
        if (!isalnum((unsigned char)*t) && *t != '-' && *t != '_') return false;
    }
    return true;
}

static bool valid_authkey(const char *k) {
    if (strncmp(k, "tskey-", 6) != 0 || strlen(k) < 20) return false;
    for (; *k; k++) {
        if (!isalnum((unsigned char)*k) && *k != '-' && *k != '_') return false;
    }
    return true;
}

// Strictly copies at most dst_len-1 chars; returns false if src did not fit.
static bool copy_field(char *dst, size_t dst_len, const char *src) {
    if (strlen(src) >= dst_len) return false;
    strcpy(dst, src);
    return true;
}

static esp_err_t h_save(httpd_req_t *req) {
    if (!authorized(req)) return ESP_OK;

    if (req->content_len == 0 || req->content_len > WEB_MAX_BODY) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Form too large");
        return ESP_OK;
    }
    char *body = calloc(1, req->content_len + 1);
    if (!body) return httpd_resp_send_500(req);
    size_t got = 0;
    while (got < req->content_len) {
        int r = httpd_req_recv(req, body + got, req->content_len - got);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (r <= 0) { free(body); return ESP_FAIL; }
        got += r;
    }

    // Field buffers sized one larger than the settings fields, so an
    // over-long value is caught by copy_field() rather than truncated.
    char ssid[34], wpass[66], authkey[129], host[65], admpass[66];
    bool tl_ssid, tl_wpass, tl_key, tl_host, tl_adm;
    form_get(body, "ssid", ssid, sizeof(ssid), &tl_ssid);
    form_get(body, "wpass", wpass, sizeof(wpass), &tl_wpass);
    form_get(body, "authkey", authkey, sizeof(authkey), &tl_key);
    form_get(body, "host", host, sizeof(host), &tl_host);
    form_get(body, "admpass", admpass, sizeof(admpass), &tl_adm);
    char tmp[8], derp[8];
    bool tl_derp;
    form_get(body, "derp", derp, sizeof(derp), &tl_derp);
    char nhost[65], nport[8], ntopic[66], nuser[66], npass[66], ntoken[81], cmdpass[66];
    bool tl_nh, tl_np, tl_nt, tl_nu, tl_npw, tl_ntk, tl_cmd;
    form_get(body, "nhost", nhost, sizeof(nhost), &tl_nh);
    form_get(body, "nport", nport, sizeof(nport), &tl_np);
    form_get(body, "ntopic", ntopic, sizeof(ntopic), &tl_nt);
    form_get(body, "nuser", nuser, sizeof(nuser), &tl_nu);
    form_get(body, "npass", npass, sizeof(npass), &tl_npw);
    form_get(body, "ntoken", ntoken, sizeof(ntoken), &tl_ntk);
    form_get(body, "cmdpass", cmdpass, sizeof(cmdpass), &tl_cmd);
    bool ntls   = form_get(body, "ntls", tmp, sizeof(tmp), NULL);
    bool nclear = form_get(body, "nclear", tmp, sizeof(tmp), NULL);
    bool exit_on = form_get(body, "exit", tmp, sizeof(tmp), NULL);
    bool nolan   = form_get(body, "nolan", tmp, sizeof(tmp), NULL);
    bool forget  = form_get(body, "forget", tmp, sizeof(tmp), NULL);
    free(body);

    trim(ssid);
    trim(authkey);
    trim(host);
    for (char *c = host; *c; c++) *c = (char)tolower((unsigned char)*c);

    const char *err = NULL;
    settings_t s = g_settings;   // a rejected form leaves the running config untouched
    bool fresh = !g_settings.provisioned;

    if (tl_ssid || tl_wpass || tl_key || tl_host || tl_adm) err = "A field is too long.";
    else if (!ssid[0])                              err = "Wi-Fi network is required.";
    else if (fresh && !wpass[0])                    err = "Wi-Fi password is required.";
    else if (wpass[0] && strlen(wpass) < 8)         err = "Wi-Fi passwords are at least 8 characters.";
    else if (fresh && !authkey[0])                  err = "Tailscale auth key is required.";
    else if (authkey[0] && !valid_authkey(authkey)) err = "That does not look like a Tailscale auth key (tskey-...).";
    else if (host[0] && !valid_hostname(host))      err = "Device name: lowercase letters, digits and hyphens, not starting or ending with a hyphen.";
    else if (!admpass[0] && !s.admin_pass[0])       err = "Admin password is required.";
    trim(nhost); trim(nport); trim(ntopic); trim(nuser); trim(cmdpass);
    if (!err && (tl_nh || tl_np || tl_nt || tl_nu || tl_npw || tl_ntk || tl_cmd)) err = "A field is too long.";
    long nport_v = 443;
    if (!err && ntopic[0]) {
        char *end = NULL;
        nport_v = nport[0] ? strtol(nport, &end, 10) : 443;
        const char *eff_cmd = cmdpass[0] ? cmdpass : s.cmd_pass;
        const char *eff_adm = admpass[0] ? admpass : s.admin_pass;
        if (!valid_topic(ntopic))                 err = "ntfy topic must be 1-64 letters, digits, - or _.";
        else if (!nhost[0])                       err = "ntfy server is required when a topic is set.";
        else if ((end && *end) || nport_v < 1 || nport_v > 65535) err = "ntfy port must be 1-65535.";
        else if (!eff_cmd[0])                     err = "A command passphrase is required when a topic is set.";
        else if (!strcasecmp(eff_cmd, eff_adm))   err = "Use a command passphrase different from the admin password.";
    }
    trim(derp);
    long derp_id = 0;
    if (!err && derp[0]) {
        char *end = NULL;
        derp_id = strtol(derp, &end, 10);
        if (tl_derp || *end || derp_id < 1 || derp_id > 65535) err = "DERP region must be a number, or blank for automatic.";
    }

    if (err) {
        send_form(req, err);
        return ESP_OK;
    }

    bool new_key = authkey[0] && strcmp(authkey, s.ts_authkey) != 0;

    copy_field(s.wifi_ssid, sizeof(s.wifi_ssid), ssid);
    if (wpass[0])   copy_field(s.wifi_pass, sizeof(s.wifi_pass), wpass);
    if (authkey[0]) copy_field(s.ts_authkey, sizeof(s.ts_authkey), authkey);
    copy_field(s.ts_hostname, sizeof(s.ts_hostname), host);
    if (admpass[0]) copy_field(s.admin_pass, sizeof(s.admin_pass), admpass);
    s.exit_enabled = exit_on;
    s.block_lan = nolan;
    s.derp_region = (uint16_t)derp_id;
    if (nhost[0]) copy_field(s.ntfy_host, sizeof(s.ntfy_host), nhost);
    s.ntfy_port = (uint16_t)nport_v;
    s.ntfy_tls = ntls;
    copy_field(s.ntfy_topic, sizeof(s.ntfy_topic), ntopic);
    copy_field(s.ntfy_user, sizeof(s.ntfy_user), nuser);
    if (nclear) {
        s.ntfy_user[0] = s.ntfy_pass[0] = s.ntfy_token[0] = '\0';
    }
    if (npass[0])   copy_field(s.ntfy_pass, sizeof(s.ntfy_pass), npass);
    if (ntoken[0])  copy_field(s.ntfy_token, sizeof(s.ntfy_token), ntoken);
    if (cmdpass[0]) copy_field(s.cmd_pass, sizeof(s.cmd_pass), cmdpass);

    // settings_save() persists g_settings, so swap the copy in and roll back
    // if the write fails.
    settings_t previous = g_settings;
    g_settings = s;
    if (!settings_save()) {
        g_settings = previous;
        send_form(req, "Could not write to flash.");
        return ESP_OK;
    }
    // A different key means a different node. Starting from fresh keys
    // avoids re-authorising the old node under a new key by accident.
    if (new_key || forget || fresh) settings_request_identity_reset();

    ESP_LOGI(TAG, "settings saved (new auth key=%d, forget identity=%d)", new_key, forget);

    // Post/Redirect/Get: answer the POST with a redirect, so reloading the
    // confirmation page (or a browser restoring the tab after the restart)
    // fetches /saved instead of submitting the form -- and restarting the
    // device -- again.
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/saved");
    httpd_resp_send(req, NULL, 0);

    s_saved = true;   // main loop restarts after the redirect has been followed
    return ESP_OK;
}

static esp_err_t h_saved(httpd_req_t *req) {
    if (!authorized(req)) return ESP_OK;
    const settings_t *s = &g_settings;
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    out_raw(req, PAGE_HEAD);
    out_raw(req, "<h1>Saved</h1><p>Restarting. The device will join <b>");
    out_esc(req, s->wifi_ssid);
    out_raw(req, "</b> and appear in the Tailscale admin console as <b>");
    out_esc(req, settings_effective_hostname());
    out_raw(req, "</b>.</p>");
    if (s->exit_enabled) {
        out_raw(req, "<p>Then approve it as an exit node: <i>Machines &rarr; the device "
                     "&rarr; Edit route settings &rarr; Use as exit node</i>.</p>");
    }
    out_raw(req, "<p>Afterwards this page is at the device's LAN address or its Tailscale "
                 "100.x address, login <b>" ADMIN_USER "</b>.</p>"
                 "<p><a href='/'>Back to settings</a></p>");
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

// Captive-portal probes (/generate_204, /hotspot-detect.html, /ncsi.txt ...)
// all land here. Answering them with a redirect is what makes phones pop the
// setup page on their own.
static esp_err_t h_not_found(httpd_req_t *req, httpd_err_code_t code) {
    (void)code;
    char loc[40];
    if (s_ap_mode) {
        char ip[20];
        fmt_ip(wifi_ap_ip(), ip, sizeof(ip));
        snprintf(loc, sizeof(loc), "http://%s/", ip);
    } else {
        snprintf(loc, sizeof(loc), "/");
    }
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", loc);
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

// ---- lifecycle ------------------------------------------------------------------

void webconfig_start(void) {
    if (s_server) return;
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 8192;
    cfg.max_uri_handlers = 8;
    cfg.lru_purge_enable = true;   // phones open many probe sockets on the AP

    if (httpd_start(&s_server, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed");
        s_server = NULL;
        return;
    }
    static const httpd_uri_t routes[] = {
        { .uri = "/",       .method = HTTP_GET,  .handler = h_root },
        { .uri = "/scan",   .method = HTTP_GET,  .handler = h_scan },
        { .uri = "/status", .method = HTTP_GET,  .handler = h_status },
        { .uri = "/save",   .method = HTTP_POST, .handler = h_save },
        { .uri = "/saved",  .method = HTTP_GET,  .handler = h_saved },
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        httpd_register_uri_handler(s_server, &routes[i]);
    }
    httpd_register_err_handler(s_server, HTTPD_404_NOT_FOUND, h_not_found);
    ESP_LOGI(TAG, "portal on :80");
}

void webconfig_set_mode(bool ap_mode, bool require_auth) {
    s_ap_mode = ap_mode;
    s_auth_needed = require_auth;
    if (ap_mode) dns_captive_start(wifi_ap_ip());
    else dns_captive_stop();
    ESP_LOGI(TAG, "mode: %s, login %s", ap_mode ? "AP" : "LAN/tailnet",
             require_auth ? "required" : "off");
}

bool webconfig_saved(void) { return s_saved; }
