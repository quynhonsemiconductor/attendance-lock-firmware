// ESP-IDF port: the encrypted event_ring partition + the default NVS
// partition ("store" namespace). Flash encryption is transparent here —
// esp_partition_* see plaintext (§2.8: NVS encryption uses the HMAC eFuse
// scheme; the event_ring partition is marked `encrypted` in partitions.csv).
// Note: with flash encryption a BLANK sector reads back as garbage — store.c
// treats blank and torn identically (seal and move on).
#include "store_port.h"

#include <string.h>

#include "bootloader_random.h"
#include "esp_partition.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "nvs_flash.h"

#define NS "store"

static const esp_partition_t *ring_part(void)
{
    static const esp_partition_t *part;
    if (part == NULL) {
        part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, 0x40, "event_ring");
    }
    return part;
}

static SemaphoreHandle_t store_mtx;

void store_port_lock(void)
{
    if (store_mtx == NULL) {
        store_mtx = xSemaphoreCreateRecursiveMutex();
    }
    xSemaphoreTakeRecursive(store_mtx, portMAX_DELAY);
}

void store_port_unlock(void)
{
    xSemaphoreGiveRecursive(store_mtx);
}

static bool nvs_ready(void)
{
    static bool done;
    if (done) {
        return true;
    }
    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        // Standard IDF recovery. With the prod profile the NVS is encrypted,
        // so this erase only happens on a version/layout change — the
        // allow-list cache and stream UUID are re-synced/regenerated.
        nvs_flash_erase();
        e = nvs_flash_init();
    }
    done = (e == ESP_OK);
    return done;
}

size_t store_port_flash_size(void)
{
    const esp_partition_t *p = ring_part();
    return p == NULL ? 0 : p->size;
}

bool store_port_flash_read(uint32_t offset, void *out, size_t len)
{
    const esp_partition_t *p = ring_part();
    if (p == NULL || (size_t)offset + len > p->size) {
        return false;
    }
    return esp_partition_read(p, offset, out, len) == ESP_OK;
}

bool store_port_flash_write(uint32_t offset, const void *in, size_t len)
{
    const esp_partition_t *p = ring_part();
    if (p == NULL || (size_t)offset + len > p->size) {
        return false;
    }
    return esp_partition_write(p, offset, in, len) == ESP_OK;
}

bool store_port_flash_erase(uint32_t offset)
{
    const esp_partition_t *p = ring_part();
    if (p == NULL || (size_t)offset + STORE_PORT_SECTOR_SIZE > p->size) {
        return false;
    }
    return esp_partition_erase_range(p, offset, STORE_PORT_SECTOR_SIZE) == ESP_OK;
}

bool store_port_nvs_set_u32(const char *key, uint32_t v)
{
    if (!nvs_ready()) {
        return false;
    }
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) {
        return false;
    }
    bool ok = nvs_set_u32(h, key, v) == ESP_OK && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    return ok;
}

store_port_nvs_res_t store_port_nvs_get_u32(const char *key, uint32_t *v)
{
    if (!nvs_ready()) {
        return STORE_PORT_ERROR;
    }
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) {
        return STORE_PORT_ERROR;
    }
    esp_err_t e = nvs_get_u32(h, key, v);
    nvs_close(h);
    if (e == ESP_ERR_NVS_NOT_FOUND) {
        return STORE_PORT_ABSENT;
    }
    return e == ESP_OK ? STORE_PORT_FOUND : STORE_PORT_ERROR;
}

bool store_port_nvs_set_blob(const char *key, const void *v, size_t len)
{
    if (!nvs_ready()) {
        return false;
    }
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) {
        return false;
    }
    // nvs_set_blob on an existing key replaces it atomically on commit:
    // the new entry is appended first, the old one is only freed once the
    // commit lands — a power cut leaves the old value readable.
    bool ok = nvs_set_blob(h, key, v, len) == ESP_OK && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    return ok;
}

store_port_nvs_res_t store_port_nvs_get_blob(const char *key, void *out, size_t *len)
{
    if (!nvs_ready()) {
        return STORE_PORT_ERROR;
    }
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) {
        return STORE_PORT_ERROR;
    }
    esp_err_t e = nvs_get_blob(h, key, out, len);
    nvs_close(h);
    if (e == ESP_ERR_NVS_NOT_FOUND) {
        return STORE_PORT_ABSENT;
    }
    if (e == ESP_ERR_NVS_INVALID_LENGTH) {
        return STORE_PORT_ERROR; // caller buffer too small
    }
    return e == ESP_OK ? STORE_PORT_FOUND : STORE_PORT_ERROR;
}

void store_port_random_bytes(void *out, size_t len)
{
    // Before Wi-Fi/BT is up the SAR ADC entropy source must be enabled
    // explicitly (bootloader_random); the stream UUID must not repeat.
    bootloader_random_enable();
    esp_fill_random(out, len);
    bootloader_random_disable();
}
