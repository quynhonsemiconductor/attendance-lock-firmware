#pragma once
// Platform port for the store component: the only code that touches the
// flash partition or NVS. store.c is platform-free and runs unchanged in the
// host tests against the RAM simulation (store_port_host.c).

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Flash sector size the ring math assumes (ESP32 flash: 4 KiB).
#define STORE_PORT_SECTOR_SIZE 4096u

// Tri-state result for NVS reads: a missing key is a normal state (fresh
// device), a backend failure is not.
typedef enum {
    STORE_PORT_FOUND = 0,
    STORE_PORT_ABSENT,
    STORE_PORT_ERROR
} store_port_nvs_res_t;

// Size of the event_ring partition in bytes (sector count derives from it).
size_t store_port_flash_size(void);

// All offsets are relative to the start of the event_ring partition.
bool store_port_flash_read(uint32_t offset, void *out, size_t len);
// Programs bytes (1 → 0 only — the caller erases first), like real flash.
bool store_port_flash_write(uint32_t offset, const void *in, size_t len);
// Erases exactly one STORE_PORT_SECTOR_SIZE sector at offset.
bool store_port_flash_erase(uint32_t offset);

// Recursive lock around the whole component (door task push vs sync task
// batch/ack/dropped). Each public store_* call takes it once. The mutex is
// created by store_port_lock_init() in store_init — store_init fails when
// that returns false; the lock/unlock calls then never see NULL.
bool store_port_lock_init(void);
void store_port_lock(void);
void store_port_unlock(void);

// Persistent key/value namespace "store" (allow-list blob, stream UUID,
// seq block end, dropped counter). Each setter commits before returning.
bool store_port_nvs_set_u32(const char *key, uint32_t v);
store_port_nvs_res_t store_port_nvs_get_u32(const char *key, uint32_t *v);
bool store_port_nvs_set_blob(const char *key, const void *v, size_t len);
// len is in/out: in = capacity of out, out = stored length. ABSENT leaves
// out untouched.
store_port_nvs_res_t store_port_nvs_get_blob(const char *key, void *out, size_t *len);
bool store_port_nvs_erase_key(const char *key);

// Cryptographically random bytes (stream UUID generation). The ESP port
// wraps esp_fill_random with bootloader_random_enable/disable so the RNG has
// entropy before Wi-Fi is up.
void store_port_random_bytes(void *out, size_t len);
