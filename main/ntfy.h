#pragma once

#include <stdbool.h>
#include <stddef.h>

// ntfy transport, ported from WOL-NTFY to esp_http_client. One task owns all
// ntfy I/O: a long-lived streaming subscription to the topic, short-lived
// POSTs for outgoing messages, and the public-IP lookup (wanip.c).
// Everything else talks to it through ntfy_publish(), so no caller ever blocks
// on TLS.

// Called on the ntfy task for each inbound message that survives the dedup
// and self-message filters. The argument is the message body.
typedef void (*ntfy_message_cb_t)(const char *message);

// Starts the task if a topic is configured. Call once Wi-Fi is up.
void ntfy_start(ntfy_message_cb_t on_message);

bool ntfy_enabled(void);

// Queues a message (copied). Returns false if ntfy is off or the queue is full.
bool ntfy_publish(const char *title, const char *body);

// Messages queued or being sent. Lets a restart wait for the reply to go out.
int ntfy_pending(void);

// "off", "connecting", "subscribed", "reconnecting", "login refused"
const char *ntfy_state(void);
