#include "commands.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lwip/ip4_addr.h"

#include "exitnode.h"
#include "ntfy.h"
#include "peerapi.h"
#include "settings.h"
#include "tailscale.h"
#include "wanip.h"
#include "wifi.h"

static const char *TAG = "cmd";

static volatile bool s_restart;

bool commands_restart_pending(void) { return s_restart; }

// ---- status text ------------------------------------------------------------------

static size_t appendf(char *buf, size_t len, size_t o, const char *fmt, ...) {
    if (o >= len) return o;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf + o, len - o, fmt, ap);
    va_end(ap);
    return n > 0 ? o + (size_t)n : o;
}

// Plain-words rating, so a weak link is obvious at a glance. Below about
// -75 dBm the link starts losing packets and the relay connection suffers.
static const char *signal_quality(int rssi) {
    if (rssi >= -55) return "excellent";
    if (rssi >= -67) return "good";
    if (rssi >= -75) return "fair";
    return "weak - move closer to the router";
}

void commands_status_text(char *buf, size_t len, bool detailed) {
    char lan[16] = "-", vpn[16], wan[16];
    if (wifi_sta_ip()) {
        ip4_addr_t a = { .addr = wifi_sta_ip() };
        snprintf(lan, sizeof(lan), IPSTR, IP2STR(&a));
    }
    tailscale_vpn_ip_str(vpn, sizeof(vpn));
    if (!wanip_get(wan, sizeof(wan))) strcpy(wan, "unknown yet");

    size_t o = 0;
    if (wifi_is_connected()) {
        int rssi = wifi_rssi();
        o = appendf(buf, len, o, "Wi-Fi     %s, %d dBm (%s)\n", g_settings.wifi_ssid, rssi,
                    signal_quality(rssi));
        o = appendf(buf, len, o, "LAN       %s\n", lan);
    } else {
        o = appendf(buf, len, o, "Wi-Fi     not connected (%s)\n",
                    wifi_reason_text(wifi_last_disconnect_reason()));
    }
    o = appendf(buf, len, o, "Tailscale %s  %s, %s, exit node %s\n", vpn[0] ? vpn : "-",
                settings_effective_hostname(), tailscale_state_name(),
                g_settings.exit_enabled ? "on" : "off");
    if (tailscale_error()[0]) o = appendf(buf, len, o, "  error: %s\n", tailscale_error());
    o = appendf(buf, len, o, "Public    %s\n", wan);
    o = appendf(buf, len, o, "Settings  http://%s%s%s\n", lan, vpn[0] ? "  http://" : "", vpn);

    if (detailed) {
        int direct, relayed;
        tailscale_peer_paths(&direct, &relayed);
        char code[8];
        uint16_t reg = tailscale_derp_region(code, sizeof(code));
        o = appendf(buf, len, o, "Peers     %d (%d direct, %d relay), DERP region %u (%s)\n",
                    tailscale_peer_count(), direct, relayed, reg, code);
        exitnode_stats_t st;
        exitnode_get_stats(&st);
        o = appendf(buf, len, o, "Exit      fwd %lu pkts / %llu KB, shed %lu, LAN blocked %lu\n",
                    (unsigned long)st.fwd_packets, (unsigned long long)(st.fwd_bytes / 1024),
                    (unsigned long)st.shed, (unsigned long)st.drop_lan);
        peerapi_stats_t ds;
        peerapi_get_stats(&ds);
        o = appendf(buf, len, o, "DNS       %lu queries, %lu failed\n",
                    (unsigned long)ds.queries, (unsigned long)ds.failures);
        o = appendf(buf, len, o, "Memory    heap %u KB (lowest %u KB)\n",
                    (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                    (unsigned)(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024));
    }

    uint64_t up_min = esp_timer_get_time() / 60000000ULL;
    appendf(buf, len, o, "fw %s, up %lluh%02llum", esp_app_get_description()->version,
            (unsigned long long)(up_min / 60), (unsigned long long)(up_min % 60));
}

// ---- commands ---------------------------------------------------------------------

static void reply(const char *title, const char *body) {
    ntfy_publish(title, body);
}

static const char HELP[] =
    "Prefix every command with the passphrase.\n"
    "status (or ip)       addresses, peers, traffic\n"
    "reboot               restart the device\n"
    "exit on | exit off   offer / stop offering the exit node (restarts)\n"
    "forget confirm       new Tailscale identity on next boot (restarts)\n"
    "factoryreset confirm clear settings, back to setup portal (restarts)\n"
    "help                 this list";

// Splits off the next whitespace-separated word, lowercased.
static const char *next_word(const char *p, char *out, size_t len) {
    while (*p && isspace((unsigned char)*p)) p++;
    size_t o = 0;
    while (*p && !isspace((unsigned char)*p)) {
        if (o + 1 < len) out[o++] = (char)tolower((unsigned char)*p);
        p++;
    }
    out[o] = '\0';
    return p;
}

void commands_handle(const char *message) {
    if (!g_settings.cmd_pass[0]) return;

    // Passphrase first, compared case-insensitively. Copy the first word as
    // typed (not lowercased) and compare with strcasecmp.
    const char *p = message;
    while (*p && isspace((unsigned char)*p)) p++;
    char pass[66];
    size_t o = 0;
    while (*p && !isspace((unsigned char)*p) && o + 1 < sizeof(pass)) pass[o++] = *p++;
    pass[o] = '\0';
    if (strcasecmp(pass, g_settings.cmd_pass) != 0) {
        ESP_LOGI(TAG, "message without the passphrase, ignored");
        return;
    }

    char verb[24], arg[24];
    p = next_word(p, verb, sizeof(verb));
    next_word(p, arg, sizeof(arg));
    ESP_LOGI(TAG, "command: %s %s", verb, arg);   // never log the passphrase

    const char *name = settings_effective_hostname();
    char title[80];
    static char body[1024];   // ntfy task only

    if (!verb[0] || !strcmp(verb, "help") || !strcmp(verb, "?")) {
        snprintf(title, sizeof(title), "%s commands", name);
        reply(title, HELP);
    } else if (!strcmp(verb, "status") || !strcmp(verb, "ip")) {
        commands_status_text(body, sizeof(body), true);
        snprintf(title, sizeof(title), "%s status", name);
        reply(title, body);
    } else if (!strcmp(verb, "reboot") || !strcmp(verb, "restart")) {
        snprintf(title, sizeof(title), "%s rebooting", name);
        reply(title, "Restarting now. An \"online\" message follows when it is back.");
        s_restart = true;
    } else if (!strcmp(verb, "exit") && (!strcmp(arg, "on") || !strcmp(arg, "off"))) {
        bool on = !strcmp(arg, "on");
        snprintf(title, sizeof(title), "%s exit node %s", name, on ? "on" : "off");
        if (g_settings.exit_enabled == on) {
            reply(title, "Already set; nothing to do.");
            return;
        }
        g_settings.exit_enabled = on;
        if (!settings_save()) {
            g_settings.exit_enabled = !on;
            reply(title, "Could not write to flash; unchanged.");
            return;
        }
        reply(title, on ? "Saved. Restarting; approve the exit node in the admin console if asked."
                        : "Saved. Restarting without the exit node.");
        s_restart = true;
    } else if (!strcmp(verb, "forget")) {
        snprintf(title, sizeof(title), "%s forget identity", name);
        if (strcmp(arg, "confirm") != 0) {
            reply(title, "This registers the device as a NEW machine using the stored auth key. "
                         "A single-use key that was already used will be refused. "
                         "Send \"forget confirm\" to go ahead.");
            return;
        }
        settings_request_identity_reset();
        reply(title, "Restarting with a fresh identity. Delete the old machine in the admin console.");
        s_restart = true;
    } else if (!strcmp(verb, "factoryreset")) {
        snprintf(title, sizeof(title), "%s factory reset", name);
        if (strcmp(arg, "confirm") != 0) {
            reply(title, "This clears all settings (Wi-Fi, ntfy, passwords) and restarts into "
                         "the setup portal. Send \"factoryreset confirm\" to go ahead.");
            return;
        }
        reply(title, "Settings cleared. Restarting into the setup portal.");
        settings_clear();
        s_restart = true;
    } else {
        snprintf(title, sizeof(title), "%s", name);
        snprintf(body, sizeof(body), "Unknown command \"%s\". Send \"help\" for the list.", verb);
        reply(title, body);
    }
}
