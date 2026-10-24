#pragma once
// Test-only hooks for the host port (store_port_host.c). Never included by
// store.c or the firmware build.

#include <stdint.h>

// Fresh blank flash + empty NVS.
void store_host_reset(void);

// Empty the NVS only (flash untouched) — NVS-wipe tests.
void store_host_nvs_wipe(void);

// Direct flash poke: tests corrupt bytes to simulate power cuts mid-write.
uint8_t *store_host_flash(void);

// Deterministic random bytes (stream UUID tests).
void store_host_seed(uint32_t seed);
