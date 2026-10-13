#pragma once

// Tunables. Credentials live in NVS (settings.c), not here.

// ---- Storage --------------------------------------------------------------
#define NVS_NAMESPACE_CFG     "tscfg"

// ---- Networking -----------------------------------------------------------
#define WIFI_CONNECT_TIMEOUT_MS   30000
#define WIFI_RETRY_INTERVAL_MS    10000

// A connected device that loses Wi-Fi gets this long to reconnect on its own
// before the fallback AP comes up. Exit-node users would rather ride out a
// router hiccup than have an open AP appear every time the link blips.
#define WIFI_LOST_GRACE_MS        30000

// Setup / fallback access point. Open, like WOL-NTFY: the settings page is
// what needs protecting, and in fallback mode it asks for the admin password.
#define AP_SSID_PREFIX            "TS-Exit-Setup-"
#define AP_CHANNEL                1
#define AP_MAX_CONN               4

// How long after a reset the BOOT button is watched for, to force the setup
// portal. It has to be pressed AFTER releasing EN: GPIO0 held low through the
// reset itself puts the ESP32 into its serial bootloader instead.
#define BOOT_BUTTON_GPIO          0
#define BOOT_WINDOW_MS            2000

// ---- Tailscale --------------------------------------------------------------
#define TS_DEFAULT_HOST_PREFIX    "esp32-exit-"
#define TS_MAX_PEERS              16

// Peer API port (DNS-over-HTTP for exit-node clients), advertised to the
// tailnet. Outside the NAPT port range (20000-39999, see CMakeLists.txt) and
// lwIP's ephemeral range (49152+), so it never collides with either.
#define PEERAPI_PORT              41112

// ---- ntfy ---------------------------------------------------------------------
// Server, topic, login and command passphrase live in NVS. Only behaviour
// tuning belongs here.

// Stamped on everything this device publishes. Commands and replies share
// one topic, so the receive path skips messages carrying it.
#define NTFY_SELF_TAG             "tsexit"

// ntfy sends a keepalive about every 45 s; silence past this means a socket
// that looks open but will never deliver again.
#define NTFY_STREAM_TIMEOUT_MS    90000
#define NTFY_RECONNECT_MIN_MS     1000
#define NTFY_RECONNECT_MAX_MS     30000
#define NTFY_MAX_LINE             2048    // longer stream lines are dropped
#define NTFY_DEDUP_SLOTS          8       // recently seen message IDs
#define NTFY_PUBLISH_QUEUE        6

// Public (WAN) address lookup: plain-text IPv4 reply.
#define WANIP_URL                 "https://api.ipify.org"
#define WANIP_INTERVAL_MS         (15 * 60 * 1000)
#define WANIP_RETRY_MS            60000

// Address change reporting
#define ADDR_POLL_MS              5000
#define ADDR_DEBOUNCE_MS          10000   // a change must hold this long
#define ONLINE_DEADLINE_MS        60000   // send "online" even if Tailscale isn't up

// ---- Web portal -------------------------------------------------------------
#define ADMIN_USER                "admin"
#define WEB_MAX_BODY              2048   // largest form POST accepted

// ---- Debug ------------------------------------------------------------------
#define HEAP_LOG_INTERVAL_MS      60000

// Internal RAM below this is logged as a warning. WiFi, lwIP and TLS need
// internal (DMA-capable) memory; PSRAM cannot stand in for it.
#define HEAP_LOW_WATER_BYTES      32768
