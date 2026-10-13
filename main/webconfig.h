#pragma once

#include <stdbool.h>

// Configuration portal. One esp_http_server instance on port 80, bound to
// every interface, so it answers on the setup AP, the LAN address and the
// Tailscale 100.x address alike. What changes between modes:
//
//   setup    -- open AP + captive portal, no login (unprovisioned, or BOOT
//               was pressed -- physical access already implies control)
//   fallback -- open AP + captive portal, admin login required
//   online   -- LAN / tailnet only, admin login required
//
// The settings page never sends a stored secret back to the browser; blank
// secret fields mean "keep what is stored".

void webconfig_start(void);

// ap_mode turns on the captive DNS + redirects; require_auth the login.
void webconfig_set_mode(bool ap_mode, bool require_auth);

// True once a save has been accepted. The main loop restarts the device.
bool webconfig_saved(void);
