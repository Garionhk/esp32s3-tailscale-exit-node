#pragma once

#include <stdbool.h>
#include <stddef.h>

// Public (WAN) IPv4 address, looked up over HTTPS. Driven from the ntfy task.

// Fetches when due. Blocking (a few seconds at most); ntfy task only.
void wanip_tick(void);

// Ask for a fresh lookup soon, e.g. after Wi-Fi reconnects.
void wanip_refresh_soon(void);

// Copies the last known address; false (and "") if none yet.
bool wanip_get(char *buf, size_t len);
