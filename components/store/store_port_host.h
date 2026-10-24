#pragma once
// Test-only hooks for the host port (store_port_host.h). Never included by
// store.c or the firmware build.

#include <stdbool.h>
#include <stdint.h>

// Fresh blank flash + empty NVS + all fault injection off.
void store_host_reset(void);

// Empty the NVS only (flash untouched) — NVS-wipe tests.
void store_host_nvs_wipe(void);

// Direct flash poke (raw, NOT keystream-decoded): tests corrupt bytes to
// simulate power cuts mid-write.
uint8_t *store_host_flash(void);

// Deterministic random bytes (stream UUID tests).
void store_host_seed(uint32_t seed);

// Simulate flash encryption: reads XOR a keystream, so a blank (0xFF) sector
// reads back as garbage — "blank" and "torn" become indistinguishable at
// boot, exactly as on the device.
void store_host_set_keystream(bool on);

// Fault injection: the next flash writes fail after `bytes` programmed bytes
// (0 = off); the next erase fails; the next `count` NVS set calls fail
// without mutating anything.
void store_host_fail_write_after(uint32_t bytes);
void store_host_fail_erase_next(void);
void store_host_fail_nvs_set(uint32_t count);
