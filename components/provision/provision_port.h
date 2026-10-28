#pragma once
// Platform port for the provision component: the "prov" NVS namespace and
// the profile flag. provision.c is platform-free and runs unchanged in the
// host tests against a RAM double (provision_port_host.c).

#include <stdbool.h>
#include <stddef.h>

// The keys are exactly what sync_port_esp.c reads: url, token, wifi_ssid,
// wifi_pass.

// Write one string entry (the first write creates the namespace). Commits
// before returning; false = not stored.
bool provision_port_set_str(const char *key, const char *value);

// Read a printable value (url, wifi_ssid). False when unset: out[0] = '\0'.
bool provision_port_get_str(const char *key, char *out, size_t cap);

// Existence WITHOUT reading the value — the token and the Wi-Fi pass are
// write-only: never read back, `status` reports them as set/unset.
bool provision_port_has_str(const char *key);

// Plain-http base URLs are a dev-bench convenience (no secure boot, no
// flash encryption): the prod build rejects them (Kconfig
// PROVISION_ALLOW_HTTP, asserted absent in CI).
bool provision_port_allow_http(void);
