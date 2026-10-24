// Host port: RAM flash with real NOR semantics (program = bitwise AND,
// erase = 0xFF) and a small in-memory NVS. Test helpers live in
// store_port_host.h.
#include "store_port_host.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "store_port.h"

#define HOST_FLASH_SIZE (4u * 1024u * 1024u)
#define HOST_MAX_ENTRIES 16
#define HOST_KEY_MAX 16

static uint8_t *fl;

typedef struct {
    char key[HOST_KEY_MAX];
    uint8_t *data;
    size_t len;
} ent_t;

static ent_t ents[HOST_MAX_ENTRIES];
static int nents;

static uint32_t rng_state = 0x12345678u; // deterministic until store_host_seed()

size_t store_port_flash_size(void)
{
    return HOST_FLASH_SIZE;
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
    memcpy(out, fl + offset, len);
    return true;
}

bool store_port_flash_write(uint32_t offset, const void *in, size_t len)
{
    if (!bounds(offset, len)) {
        return false;
    }
    const uint8_t *src = in;
    for (size_t i = 0; i < len; i++) {
        fl[offset + i] &= src[i]; // NOR program: 1 → 0 only
    }
    return true;
}

bool store_port_flash_erase(uint32_t offset)
{
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

bool store_port_nvs_get_u32(const char *key, uint32_t *v)
{
    uint8_t b[4];
    size_t len = 4;
    if (!store_port_nvs_get_blob(key, b, &len)) {
        *v = 0; // missing key = fresh device = 0 (port contract)
        return true;
    }
    if (len != 4) {
        return false;
    }
    *v = (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
    return true;
}

bool store_port_nvs_set_blob(const char *key, const void *v, size_t len)
{
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

bool store_port_nvs_get_blob(const char *key, void *out, size_t *len)
{
    ent_t *e = find(key);
    if (e == NULL || e->len > *len) {
        return false;
    }
    memcpy(out, e->data, e->len);
    *len = e->len;
    return true;
}

bool store_port_nvs_erase_key(const char *key)
{
    ent_t *e = find(key);
    if (e == NULL) {
        return false;
    }
    free(e->data);
    *e = ents[--nents];
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
