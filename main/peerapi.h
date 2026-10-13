#pragma once

#include <stdint.h>

// Minimal Tailscale "peer API": just the DNS-over-HTTP endpoint that clients
// use when this device is their exit node.
//
// A client with an exit node selected sends every DNS query as
//   POST http://<exit-node-100.x>:<peerapi4 port>/dns-query
//   Content-Type: application/dns-message
// and expects a 200 with the raw DNS reply. MicroLink advertises the port in
// Hostinfo.Services (peerapi4 + peerapi-dns-proxy); this module answers it by
// forwarding the query to the LAN's DNS server, so whatever the home network
// resolves with (router, Pi-hole, ...) applies to remote clients too.
//
// Only tailnet peers (100.64.0.0/10) are served.

typedef struct {
    uint32_t queries;
    uint32_t failures;     // no upstream answered
    uint32_t refused;      // request not from the tailnet
    uint32_t total_ms;     // sum of upstream round trips, for an average
} peerapi_stats_t;

void peerapi_start(void);
void peerapi_get_stats(peerapi_stats_t *out);
