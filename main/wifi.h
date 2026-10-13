#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_netif.h"

// Wi-Fi ownership for the whole app. MicroLink's own Wi-Fi helpers
// (ml_net_switch) are not used.

// One-time driver/netif setup. Leaves the radio off.
void wifi_init(void);

// Station only, joining the configured network. Reconnects on its own.
void wifi_start_sta(const char *ssid, const char *pass);

// Blocks until an IP is obtained or the timeout passes.
bool wifi_wait_connected(uint32_t timeout_ms);

// Adds the open setup AP. With no SSID configured (setup mode) the station
// side stays idle so scans work; otherwise it keeps retrying underneath.
void wifi_start_ap(void);

// Back to station only.
void wifi_stop_ap(void);

// Kicks the station side to try again (fallback mode retry).
void wifi_retry(void);

bool     wifi_is_connected(void);
bool     wifi_ap_active(void);
uint32_t wifi_sta_ip(void);          // network byte order, 0 if none
uint32_t wifi_ap_ip(void);           // network byte order
int      wifi_rssi(void);            // dBm, 0 if not connected
uint8_t  wifi_last_disconnect_reason(void);
const char *wifi_reason_text(uint8_t reason);

esp_netif_t *wifi_sta_netif(void);
esp_netif_t *wifi_ap_netif(void);

// "TS-Exit-Setup-A1B2", last 2 bytes of the MAC
const char *wifi_ap_ssid(void);

// Runs a scan and writes a JSON array of unique SSIDs into buf.
// Returns the number of networks, or -1 if the radio is busy.
int wifi_scan_json(char *buf, size_t len);

// Logs every 2.4 GHz network in range and whether the configured SSID is
// among them -- separates a typo / 5 GHz-only network from a bad password.
void wifi_diagnose(const char *ssid);
