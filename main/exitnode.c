#include "exitnode.h"

#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_netif_net_stack.h"
#include "lwip/ip4_addr.h"
#include "lwip/lwip_napt.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "lwip/tcpip.h"
#include "wireguardif.h"

static const char *TAG = "exitnode";

static exitnode_stats_t s_stats;

static struct netif *s_wg;
static volatile bool s_enabled;
static volatile bool s_block_lan;
static volatile uint32_t s_vpn_ip;      // network byte order
static volatile uint32_t s_sta_ip;      // network byte order
static volatile uint32_t s_sta_mask;    // network byte order

static netif_input_fn s_orig_input;     // tcpip_input, saved from the first netif

// ---- load shedding ----------------------------------------------------------
//
// The internet side can deliver far more than the board can encrypt and relay
// (a speed test pushes 100+ Mbit/s at a ~1 Mbit/s exit). Left alone, that
// flood fills the Wi-Fi RX buffers and lwIP's single input queue, and packets
// are dropped indiscriminately -- including the DERP connection's own
// segments, until the relay server gives up on it and resets it.
//
// So forwarded replies (NAPT ports, see CMakeLists.txt) are dropped at the
// door while the stack is congested: just after lwIP refused a packet, or
// while the relay queue is backed up. Everything addressed to the device's
// own sockets (DERP, DNS, the portal) still gets through, and the dropped
// flows see loss and slow down, which is how TCP finds the right rate.
#define NAPT_PORT_FIRST          20000
#define NAPT_PORT_LAST           39999
#define EXITNODE_SHED_BACKLOG    16      // relay queue depth that triggers shedding
#define EXITNODE_CONGESTED_US    200000  // shed for this long after lwIP refused input

static volatile int64_t s_congested_until_us;
static uint32_t (*s_backlog_fn)(void);

void exitnode_set_backlog_source(uint32_t (*fn)(void)) { s_backlog_fn = fn; }

// ---- helpers ----------------------------------------------------------------

static inline uint32_t rd32(const uint8_t *p) {
    uint32_t v;
    memcpy(&v, p, 4);
    return v;   // stays in network byte order
}

static bool is_multicast_or_bcast(uint32_t a_nbo) {
    uint32_t a = lwip_ntohl(a_nbo);
    return (a >> 28) == 0xE || a == 0xFFFFFFFFu;
}

// Destinations a "block LAN access" exit node must not reach: RFC 1918,
// link-local, loopback, "this network", and the device's own LAN subnet in
// case that uses a public range.
static bool is_private(uint32_t a_nbo) {
    uint32_t a = lwip_ntohl(a_nbo);
    if ((a & 0xFF000000u) == 0x0A000000u) return true;   // 10/8
    if ((a & 0xFFF00000u) == 0xAC100000u) return true;   // 172.16/12
    if ((a & 0xFFFF0000u) == 0xC0A80000u) return true;   // 192.168/16
    if ((a & 0xFFFF0000u) == 0xA9FE0000u) return true;   // 169.254/16
    if ((a & 0xFF000000u) == 0x7F000000u) return true;   // 127/8
    if ((a & 0xFF000000u) == 0x00000000u) return true;   // 0/8
    uint32_t ip = s_sta_ip, mask = s_sta_mask;
    if (ip && mask && (a_nbo & mask) == (ip & mask)) return true;
    return false;
}

// ---- Wi-Fi input guard --------------------------------------------------------

// Runs in the Wi-Fi driver's context for every received frame, so it only
// peeks at fixed offsets. Wi-Fi RX buffers are single contiguous pbufs.
static err_t guarded_input(struct pbuf *p, struct netif *inp) {
    if (p->len >= 14 + 20) {
        const uint8_t *f = (const uint8_t *)p->payload;
        if (f[12] == 0x08 && f[13] == 0x00 && (f[14] >> 4) == 4) {
            const uint8_t *iph = f + 14;
            uint32_t dst  = rd32(iph + 16);
            uint32_t self = ip4_addr_get_u32(netif_ip4_addr(inp));
            uint32_t mask = ip4_addr_get_u32(netif_ip4_netmask(inp));
            bool ok;
            if (self == 0) {
                // No address yet: only DHCP replies (UDP to port 68) matter.
                size_t ihl = (iph[0] & 0x0F) * 4;
                ok = iph[9] == 17 && p->len >= 14 + ihl + 4 &&
                     iph[ihl + 2] == 0 && iph[ihl + 3] == 68;
            } else {
                ok = dst == self || is_multicast_or_bcast(dst) ||
                     dst == (self | ~mask);
            }
            if (!ok) {
                s_stats.drop_guard++;
                pbuf_free(p);
                return ERR_OK;
            }

            // Reply to a forwarded flow? (TCP/UDP to a NAPT port)
            size_t ihl = (iph[0] & 0x0F) * 4;
            if (self && dst == self && (iph[9] == 6 || iph[9] == 17) &&
                p->len >= 14 + ihl + 4) {
                uint16_t dport = (uint16_t)((iph[ihl + 2] << 8) | iph[ihl + 3]);
                if (dport >= NAPT_PORT_FIRST && dport <= NAPT_PORT_LAST &&
                    (esp_timer_get_time() < s_congested_until_us ||
                     (s_backlog_fn && s_backlog_fn() >= EXITNODE_SHED_BACKLOG))) {
                    s_stats.shed++;
                    pbuf_free(p);
                    return ERR_OK;
                }
            }
        }
    }
    err_t err = s_orig_input(p, inp);
    if (err != ERR_OK) {
        // lwIP's input queue is full. The caller frees the pbuf.
        s_stats.stack_full++;
        s_congested_until_us = esp_timer_get_time() + EXITNODE_CONGESTED_US;
    }
    return err;
}

void exitnode_guard_netif(esp_netif_t *esp_netif) {
    struct netif *n = esp_netif ? esp_netif_get_netif_impl(esp_netif) : NULL;
    if (!n || n->input == guarded_input) return;
    if (!s_orig_input) s_orig_input = n->input;
    n->input = guarded_input;
    ESP_LOGI(TAG, "forwarding guard on %c%c%d", n->name[0], n->name[1], n->num);
}

void exitnode_set_sta_ip(uint32_t ip, uint32_t mask) {
    s_sta_mask = mask;
    s_sta_ip = ip;
}

// ---- WireGuard inbound filter -------------------------------------------------

// Runs on MicroLink's WireGuard task for every decrypted packet, before lwIP.
static bool wg_rx_filter(struct pbuf *p, void *ctx) {
    (void)ctx;
    if (p->len < 20) return true;   // let lwIP reject malformed packets
    const uint8_t *iph = (const uint8_t *)p->payload;

    if ((iph[0] >> 4) == 6) {
        s_stats.drop_ipv6++;
        return false;
    }

    uint32_t dst = rd32(iph + 16);
    uint32_t sta = s_sta_ip;

    // Addressed to the device itself (portal over 100.x, or its LAN IP).
    if (dst == s_vpn_ip || (sta && dst == sta) || is_multicast_or_bcast(dst)) {
        return true;
    }
    // Peer-to-peer through us: lwIP refuses to forward out of the netif it
    // arrived on, so leave that decision to it.
    if ((lwip_ntohl(dst) & 0xFFC00000u) == 0x64400000u) return true;   // 100.64/10

    if (!s_enabled) {
        s_stats.drop_disabled++;
        return false;
    }
    if (s_block_lan && is_private(dst)) {
        s_stats.drop_lan++;
        return false;
    }
    s_stats.fwd_packets++;
    s_stats.fwd_bytes += p->tot_len;
    return true;
}

static void napt_on(void *arg) {
    // NAPT on the INSIDE interface: packets arriving on it get their source
    // rewritten to the outgoing interface's address. Runs on the TCPIP thread.
    if (!ip_napt_enable_netif((struct netif *)arg, 1)) {
        ESP_LOGE(TAG, "ip_napt_enable_netif failed");
    }
}

void exitnode_attach(void *wg_netif, bool enabled, bool block_lan) {
    struct netif *n = wg_netif;
    s_enabled = enabled;
    s_block_lan = block_lan;
    if (!n || n == s_wg) return;

    s_wg = n;
    // The filter goes on regardless of `enabled`: IP forwarding is compiled
    // in, so with the exit node off it is the filter that stops peers from
    // routing through this device.
    wireguardif_set_rx_filter(n, wg_rx_filter, NULL);
    if (enabled) tcpip_callback(napt_on, n);

    ESP_LOGI(TAG, "attached to WireGuard netif (exit node %s, LAN access %s)",
             enabled ? "ON" : "off", block_lan ? "blocked" : "allowed");
}

void exitnode_set_vpn_ip(uint32_t ip) { s_vpn_ip = ip; }

bool exitnode_attached(void) { return s_wg != NULL; }

void exitnode_get_stats(exitnode_stats_t *out) { *out = s_stats; }
