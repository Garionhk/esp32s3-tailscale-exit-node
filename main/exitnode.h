#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_netif.h"

// Exit-node forwarding policy.
//
// Traffic from tailnet peers arrives decrypted on MicroLink's WireGuard netif.
// lwIP forwards it (CONFIG_LWIP_IP_FORWARD) out of the Wi-Fi station, with
// NAPT on the WireGuard netif rewriting the source to this device's LAN
// address. Replies are de-NAT'd on the way in and routed back into the tunnel
// by WireGuard's cryptokey routing.
//
// Turning on IP forwarding is global in lwIP, so this module also makes sure
// the WireGuard netif is the ONLY way in: the Wi-Fi interfaces drop anything
// not addressed to the device itself.

typedef struct {
    uint32_t fwd_packets;      // forwarded tailnet -> internet
    uint64_t fwd_bytes;
    uint32_t drop_lan;         // exit traffic to private addresses (block_lan)
    uint32_t drop_disabled;    // exit traffic while the exit node is off
    uint32_t drop_ipv6;        // IPv6 is advertised but not forwarded
    uint32_t drop_guard;       // Wi-Fi frames trying to use us as a router
    uint32_t shed;             // forwarded replies dropped at the door under load
    uint32_t stack_full;       // lwIP input queue was full (packet lost)
} exitnode_stats_t;

// Called on every Wi-Fi interface start. Wraps the lwIP input function so
// frames not addressed to this interface are dropped before lwIP can forward
// them (LAN host -> tailnet, or setup-AP client -> LAN).
void exitnode_guard_netif(esp_netif_t *netif);

// Station address and netmask in network byte order (0 when disconnected).
void exitnode_set_sta_ip(uint32_t ip, uint32_t mask);

// Hooks the WireGuard netif: installs the inbound filter and enables NAPT.
// Safe to call repeatedly; only acts when the netif changes.
void exitnode_attach(void *wg_netif, bool enabled, bool block_lan);

// The VPN address can arrive after the netif exists; keep the filter's idea
// of "addressed to us" current. Network byte order.
void exitnode_set_vpn_ip(uint32_t ip);

// Source of "packets waiting to go out via the relay". Forwarded replies are
// shed while it is at or above EXITNODE_SHED_BACKLOG.
void exitnode_set_backlog_source(uint32_t (*fn)(void));

bool exitnode_attached(void);
void exitnode_get_stats(exitnode_stats_t *out);
