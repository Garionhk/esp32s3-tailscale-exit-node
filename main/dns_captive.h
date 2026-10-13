#pragma once

#include <stdint.h>

// Tiny DNS responder for the setup AP: answers every A query with the AP's
// own address, which is what makes phones pop the setup page by themselves.
// AP mode only -- in STA mode it would hijack name resolution on the LAN.

void dns_captive_start(uint32_t ap_ip_nbo);
void dns_captive_stop(void);
