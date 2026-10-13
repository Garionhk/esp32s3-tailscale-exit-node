#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Runtime configuration, held in NVS and edited from the web portal.
// Nothing user-specific is compiled into the binary, so a build can be shared
// without leaking a Wi-Fi password or a Tailscale auth key.

typedef struct {
    // Wi-Fi
    char wifi_ssid[33];
    char wifi_pass[65];

    // Tailscale
    char ts_authkey[128];     // tskey-auth-...; never sent back to the browser
    char ts_hostname[64];     // blank = TS_DEFAULT_HOST_PREFIX + MAC suffix

    // Exit node
    bool exit_enabled;        // advertise 0.0.0.0/0 + ::/0 and forward traffic
    bool block_lan;           // drop exit traffic bound for private addresses
    uint16_t derp_region;     // Tailscale relay region, 0 = lowest latency

    // ntfy (optional; blank topic = off). Pushes the device's addresses and
    // takes passphrase-prefixed commands on the same topic.
    char ntfy_host[64];       // default "ntfy.sh"
    uint16_t ntfy_port;       // default 443
    bool ntfy_tls;            // default on
    char ntfy_topic[65];
    char ntfy_user[65];       // login: token wins over user/password
    char ntfy_pass[65];
    char ntfy_token[80];
    char cmd_pass[65];        // prefixes every command sent over ntfy

    // Login for the settings page (user ADMIN_USER)
    char admin_pass[65];

    bool provisioned;         // the "ok" sentinel
} settings_t;

extern settings_t g_settings;

// Loads from NVS into g_settings. Leaves defaults in place if the device has
// never been provisioned.
void settings_begin(void);

// Persists g_settings. Returns false if the write failed.
bool settings_save(void);

// Wipes the config namespace (Tailscale node identity is separate; see
// settings_request_identity_reset()).
void settings_clear(void);

// Hostname actually used: the stored one, or the MAC-derived default.
const char *settings_effective_hostname(void);

// ---- Tailscale bookkeeping (same namespace, not shown in the portal) ------

// Ask the next boot to wipe MicroLink's stored node keys before it starts,
// so the device registers as a brand-new node. Used when the auth key
// changes or the user ticks "forget identity".
void settings_request_identity_reset(void);
bool settings_take_identity_reset(void);   // returns and clears the request

// True once this identity has registered successfully. Later boots then
// re-register without the auth key, so a single-use key keeps working.
bool settings_ts_registered(void);
void settings_set_ts_registered(bool registered);
