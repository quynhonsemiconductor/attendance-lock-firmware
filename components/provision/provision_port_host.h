#pragma once
// Test-only hooks for the provision host port. Never included by
// provision.c or the firmware build.

#include <stdbool.h>

// Empty map, allow_http off (prod-like).
void provision_host_reset(void);

// Flip the profile flag: plain-http base URLs are dev-bench only.
void provision_host_set_allow_http(bool on);

// The stored value for a key, or NULL when unset (asserts the exact NVS
// key names sync_port_esp.c reads).
const char *provision_host_get(const char *key);
