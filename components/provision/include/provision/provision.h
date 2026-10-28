#pragma once
// Serial provisioning (§2.8): set base URL + token, print status. NEVER
// reads the token back; no unlock, card or test-output commands.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Register the provisioning console on the serial line (inside the enclosure).
void provision_init(void);

// True when a base URL and token are stored (encrypted NVS, prod profile).
bool provisioned(void);

// Handle one console line ("set url …", "set token …", "set wifi ssid …",
// "set wifi pass …", "status") and write the reply to out: nul-terminated,
// at most out_cap bytes, one line per message. Platform-free — the UART
// task and the host tests both go through here. The token and the Wi-Fi
// pass never appear in a reply (or in any log).
void provision_handle_line(const char *line, char *out, size_t out_cap);
