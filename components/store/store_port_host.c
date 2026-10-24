// Host port: RAM flash with real NOR semantics (program = bitwise AND,
// erase = 0xFF) and a small in-memory NVS. Optional XOR keystream simulates
// flash encryption (a blank sector reads back as garbage), and fault
// injection hooks cut writes/erases/NVS commits mid-operation so the tests
// can reproduce power cuts. Test helpers live in store_port_host.h.
#include "store_port_host.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "store_port.h"

#define HOST_FLASH_SIZE (4u * 1024u * 1024u)
#define HOST_MAX_ENTRIES 16
#define HOST_KEY_MAX 16

static uint8_t *fl;
static bool keystream; // simulate transparent decryption of encrypted flash
static uint32_t write_budget; // 0 = unlimited; else fail after N more bytes
static int fail_erase_pending;
static int fail_nvs_set_pending;

static uint32_t rng_state = 0x12345678u; // deterministic until store_host_seed()

typedef struct {
    char key[HOST_KEY_MAX];
    uint8_t *data;
    size_t len;
} ent_t;

static ent_t ents[HOST_MAX_ENTRIES];
static int nents;

size_t store_port_flash_size(void)
{
    return HOST_FLASH_SIZE;
}

void store_port_lock(void) {}
void store_port_unlock(void) {}

static inline uint8_t ks_byte(uint32_t off)
{
    return (uint8_t)(0xA5u ^ (off * 0x9Eu));
}

static bool bounds(uint32_t offset, size_t len)
{
    return (size_t)offset + len <= HOST_FLASH_SIZE && fl != NULL;
}

bool store_port_flash_read(uint32_t offset, void *out, size_t len)
{
    if (!bounds(offset, len)) {
        return false;
    }
    uint8_t *dst = out;
    for (size_t i = 0; i < len; i++) {
        dst[i] = fl[offset + i];
        if (keystream) {
            dst[i] ^= ks_byte(offset + (uint32_t)i);
        }
    }
    return true;
}

bool store_port_flash_write(uint32_t offset, const void *in, size_t len)
{
    if (!bounds(offset, len)) {
        return false;
    }
    const uint8_t *src = in;
    size_t n = len;
    if (write_budget > 0 && write_budget < len) {
        // Power cut: program the prefix, then die.
        n = write_budget;
        write_budget = 0;
        for (size_t i = 0; i < n; i++) {
            uint8_t b = src[i];
            if (keystream) {
                b ^= ks_byte(offset + (uint32_t)i);
            }
            fl[offset + i] &= b; // NOR program: 1 → 0 only
        }
        return false;
    }
    if (write_budget > 0) {
        write_budget -= (uint32_t)len;
    }
    for (size_t i = 0; i < n; i++) {
        uint8_t b = src[i];
        if (keystream) {
            b ^= ks_byte(offset + (uint32_t)i);
        }
        fl[offset + i] &= b;
    }
    return true;
}

bool store_port_flash_erase(uint32_t offset)
{
    if (fail_erase_pending > 0) {
        fail_erase_pending--;
        return false;
    }
    if (!bounds(offset, STORE_PORT_SECTOR_SIZE) || offset % STORE_PORT_SECTOR_SIZE != 0) {
        return false;
    }
    memset(fl + offset, 0xFF, STORE_PORT_SECTOR_SIZE);
    return true;
}

static ent_t *find(const char *key)
{
    for (int i = 0; i < nents; i++) {
        if (strcmp(ents[i].key, key) == 0) {
            return &ents[i];
        }
    }
    return NULL;
}

bool store_port_nvs_set_u32(const char *key, uint32_t v)
{
    return store_port_nvs_set_blob(key, (uint8_t[4]){(uint8_t)v, (uint8_t)(v >> 8),
                                                     (uint8_t)(v >> 16), (uint8_t)(v >> 24)},
                                   4);
}

store_port_nvs_res_t store_port_nvs_get_u32(const char *key, uint32_t *v)
{
    uint8_t b[4];
    size_t len = 4;
    store_port_nvs_res_t r = store_port_nvs_get_blob(key, b, &len);
    if (r != STORE_PORT_FOUND) {
        return r;
    }
    if (len != 4) {
        return STORE_PORT_ERROR;
    }
    *v = (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
    return STORE_PORT_FOUND;
}

bool store_port_nvs_set_blob(const char *key, const void *v, size_t len)
{
    if (fail_nvs_set_pending > 0) {
        fail_nvs_set_pending--;
        return false;
    }
    if (strlen(key) >= HOST_KEY_MAX) {
        return false;
    }
    ent_t *e = find(key);
    if (e == NULL) {
        if (nents >= HOST_MAX_ENTRIES) {
            return false;
        }
        e = &ents[nents++];
        snprintf(e->key, HOST_KEY_MAX, "%s", key);
    }
    uint8_t *copy = malloc(len);
    if (copy == NULL) {
        return false;
    }
    memcpy(copy, v, len);
    free(e->data);
    e->data = copy;
    e->len = len;
    return true;
}

store_port_nvs_res_t store_port_nvs_get_blob(const char *key, void *out, size_t *len)
{
    ent_t *e = find(key);
    if (e == NULL) {
        return STORE_PORT_ABSENT;
    }
    if (e->len > *len) {
        return STORE_PORT_ERROR; // caller buffer too small
    }
    memcpy(out, e->data, e->len);
    *len = e->len;
    return STORE_PORT_FOUND;
}

bool store_port_nvs_erase_key(const char *key)
{
    ent_t *e = find(key);
    if (e == NULL) {
        return false;
    }
    free(e->data);
    *e = ents[--nents];
    ents[nents] = (ent_t){0};
    return true;
}

void store_port_random_bytes(void *out, size_t len)
{
    uint8_t *p = out;
    for (size_t i = 0; i < len; i++) {
        rng_state ^= rng_state << 13;
        rng_state ^= rng_state >> 17;
        rng_state ^= rng_state << 5;
        p[i] = (uint8_t)rng_state;
    }
}

// --- Test helpers (store_port_host.h) ---

void store_host_reset(void)
{
    free(fl);
    fl = malloc(HOST_FLASH_SIZE);
    memset(fl, 0xFF, HOST_FLASH_SIZE);
    store_host_nvs_wipe();
    keystream = false;
    write_budget = 0;
    fail_erase_pending = 0;
    fail_nvs_set_pending = 0;
}

void store_host_nvs_wipe(void)
{
    for (int i = 0; i < nents; i++) {
        free(ents[i].data);
        ents[i] = (ent_t){0};
    }
    nents = 0;
}

uint8_t *store_host_flash(void)
{
    return fl;
}

void store_host_seed(uint32_t seed)
{
    rng_state = seed | 1u;
}

void store_host_set_keystream(bool on)
{
    keystream = on;
}

void store_host_fail_write_after(uint32_t bytes)
{
    write_budget = bytes;
}

void store_host_fail_erase_next(void)
{
    fail_erase_pending = 1;
}

void store_host_fail_nvs_set(uint32_t count)
{
    fail_nvs_set_pending = (int)count;
}
