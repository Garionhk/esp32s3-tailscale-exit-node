#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Thin wrapper around MicroLink: start-up order, identity bookkeeping and the
// bits of state the portal shows.

// Before Wi-Fi comes up: honours a pending "forget identity" request by
// wiping MicroLink's stored keys. MicroLink requires this before init.
void tailscale_boot(void);

// After the first IP. Safe to call more than once; only the first call acts.
void tailscale_start(void);

// Main-loop housekeeping: hooks the exit node onto the WireGuard netif,
// tracks state changes, records first successful registration.
void tailscale_tick(void);

// Wi-Fi came back after a drop: move MicroLink's sockets to the new link.
void tailscale_wifi_reconnected(void);

bool        tailscale_started(void);
bool        tailscale_connected(void);
bool        tailscale_failed(void);       // control plane refused us
const char *tailscale_state_name(void);
const char *tailscale_error(void);        // "" when none
void        tailscale_vpn_ip_str(char *buf, size_t len);   // "" until assigned
int         tailscale_peer_count(void);
uint16_t    tailscale_derp_region(char *code, size_t len);   // 0 until known
void        tailscale_peer_paths(int *direct, int *relayed);   // relayed = via DERP

// Logs one line of data-path diagnostics (queue drops, relay write stalls,
// read gaps, resets). Maxima reset after each call.
void        tailscale_log_diag(void);
