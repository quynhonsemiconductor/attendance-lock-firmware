// ESP-IDF port: provisioned creds (NVS namespace "prov", written by the
// provisioning console in PR 5), station Wi-Fi in bounded sessions, HTTPS
// with the bundled CA set, random for jitter.
#include "sync_port.h"

#include <string.h>

#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_random.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "sync/sync.h"

#define NS "prov"
#define UA_BASE "qnsc-door-terminal/"

static bool nvs_ready(void)
{
    static bool done;
    if (done) {
        return true;
    }
    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        e = nvs_flash_init();
    }
    done = (e == ESP_OK);
    return done;
}

bool sync_port_creds(char *url, size_t url_cap, char *token, size_t token_cap)
{
    if (!nvs_ready()) {
        return false;
    }
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    size_t ul = url_cap, tl = token_cap;
    esp_err_t e1 = nvs_get_str(h, "url", url, &ul);
    esp_err_t e2 = nvs_get_str(h, "token", token, &tl);
    nvs_close(h);
    return e1 == ESP_OK && e2 == ESP_OK && url[0] != '\0' && token[0] != '\0';
}

bool sync_port_wifi_up(void)
{
    // Station-only, auto-connect off: the session controls the connection
    // explicitly (connect → sync → off, deep-sleep design).
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    if (esp_wifi_init(&cfg) != ESP_OK) {
        return false;
    }
    if (esp_wifi_set_mode(WIFI_MODE_STA) != ESP_OK ||
        esp_wifi_set_storage(WIFI_STORAGE_RAM) != ESP_OK) {
        esp_wifi_deinit();
        return false;
    }
    // The STA credential set comes from provisioning (namespace "prov").
    if (esp_wifi_start() != ESP_OK || esp_wifi_connect() != ESP_OK) {
        esp_wifi_stop();
        esp_wifi_deinit();
        return false;
    }
    // Bounded wait for the IP (the session must not hang; the watchdog is
    // armed on this task in PR 4).
    for (int i = 0; i < 150; i++) { // 15 s max
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(500)); // let DHCP finish
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    sync_port_wifi_down();
    return false;
}

void sync_port_wifi_down(void)
{
    esp_wifi_disconnect();
    esp_wifi_stop();
    esp_wifi_deinit();
}

bool sync_port_http(const char *method, const char *url, const char *path, const char *token,
                    const char *if_none_match, const char *body, size_t body_len,
                    long *status, char *resp, size_t cap, size_t *resp_len, char *etag,
                    size_t etag_cap, char *date, size_t date_cap)
{
    char full[192];
    if (snprintf(full, sizeof(full), "%s%s", url, path) >= (int)sizeof(full)) {
        return false;
    }
    esp_http_client_config_t cfg = {
        .url = full,
        .method = strcmp(method, "POST") == 0 ? HTTP_METHOD_POST : HTTP_METHOD_GET,
        .crt_bundle_attach = esp_crt_bundle_attach, // the CA set, not one pin
        .timeout_ms = 10000,
        .buffer_size = 2048,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (c == NULL) {
        return false;
    }
    char ua[40];
    snprintf(ua, sizeof(ua), UA_BASE "%s", esp_app_get_description()->version);
    esp_http_client_set_header(c, "User-Agent", ua);
    char auth[160];
    if (snprintf(auth, sizeof(auth), "Bearer %s", token) < (int)sizeof(auth)) {
        esp_http_client_set_header(c, "Authorization", auth);
    }
    if (if_none_match[0] != '\0') {
        esp_http_client_set_header(c, "If-None-Match", if_none_match);
    }
    if (body != NULL) {
        esp_http_client_set_header(c, "Content-Type", "application/json");
        esp_http_client_set_post_field(c, body, (int)body_len);
    }
    bool ok = false;
    esp_err_t e = esp_http_client_perform(c);
    if (e == ESP_OK) {
        *status = (long)esp_http_client_get_status_code(c);
        *resp_len = 0;
        if (resp != NULL) {
            int read_total = 0;
            int n;
            while ((n = esp_http_client_read(c, resp + read_total, (int)(cap - read_total))) > 0) {
                read_total += n;
                if ((size_t)read_total >= cap) {
                    break;
                }
            }
            resp[read_total] = '\0';
            *resp_len = (size_t)read_total;
        }
        char *hdr = NULL; // v6 signature: char **value_out
        if (etag != NULL && esp_http_client_get_header(c, "ETag", &hdr) == ESP_OK && hdr) {
            snprintf(etag, etag_cap, "%s", hdr);
        }
        hdr = NULL;
        if (date != NULL && esp_http_client_get_header(c, "Date", &hdr) == ESP_OK && hdr) {
            snprintf(date, date_cap, "%s", hdr);
        }
        ok = true;
    }
    esp_http_client_cleanup(c);
    return ok;
}

uint32_t sync_port_random_u32(void)
{
    return esp_random();
}

void sync_port_delay_ms(uint32_t ms)
{
    vTaskDelay(pdMS_TO_TICKS(ms));
}

static void sync_task(void *arg)
{
    (void)arg;
    uint32_t fails = 0;
    for (;;) {
        bool ok = sync_session_run();
        fails = ok ? 0 : fails + 1;
        vTaskDelay(pdMS_TO_TICKS(sync_next_delay_ms(ok, fails)));
    }
}

void sync_port_task_start(void)
{
    xTaskCreate(sync_task, "sync", 6144, NULL, tskIDLE_PRIORITY + 2, NULL);
}
