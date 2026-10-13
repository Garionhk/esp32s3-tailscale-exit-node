#pragma once

#include <stdbool.h>
#include <stddef.h>

// ntfy command handling. Every command is prefixed with the command
// passphrase ("<passphrase> status"); anything else is ignored silently, so
// the topic name alone gives nobody control of the device.

// ntfy message callback (runs on the ntfy task).
void commands_handle(const char *message);

// The address summary used by "status" and by the push messages. With
// detailed, adds peers, relay, traffic and memory.
void commands_status_text(char *buf, size_t len, bool detailed);

// Set by reboot / exit / forget / factoryreset once the reply is queued.
// The main loop restarts after the reply has been sent.
bool commands_restart_pending(void);
