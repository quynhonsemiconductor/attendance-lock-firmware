// ESP-IDF port: provisioned creds (NVS namespace "prov", written by the
// provisioning console in PR 5), station Wi-Fi in bounded sessions, HTTPS
// with the bundled CA set, random for jitter.
#include "sync_port.h"

#include <string.h>

#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "sync/sync.h"

static const char *TAG = "sync";

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
        // NEVER wipe here: provisioning (base URL, token, Wi-Fi) lives in
        // this partition — an erase would silently de-provision the door.
        // Only the explicit factory-reset console command erases NVS.
        ESP_LOGE(TAG, "nvs needs erase (err=%d) — run factory-reset", (int)e);
        return false;
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

#define WIFI_CONNECTED_BIT BIT0
static EventGroupHandle_t s_wifi_events;
static bool s_netif_up;

static void wifi_ip_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    if (id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
    }
}

static bool read_wifi_creds(char *ssid, size_t ssid_cap, char *pass, size_t pass_cap)
{
    if (!nvs_ready()) {
        return false;
    }
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    size_t sl = ssid_cap, pl = pass_cap;
    esp_err_t e1 = nvs_get_str(h, "wifi_ssid", ssid, &sl);
    esp_err_t e2 = nvs_get_str(h, "wifi_pass", pass, &pl);
    nvs_close(h);
    return e1 == ESP_OK && e2 == ESP_OK && ssid[0] != '\0';
}

bool sync_port_wifi_up(void)
{
    if (!nvs_ready()) {
        return false;
    }
    char ssid[33], pass[64];
    if (!read_wifi_creds(ssid, sizeof(ssid), pass, sizeof(pass))) {
        ESP_LOGW(TAG, "no wifi credentials provisioned");
        return false;
    }
    if (esp_netif_init() != ESP_OK) {
        return false;
    }
    if (esp_event_loop_create_default() != ESP_OK &&
        esp_event_loop_create_default() != ESP_ERR_INVALID_STATE) {
        return false;
    }
    if (!s_netif_up) { // created once; sessions reuse it
        esp_netif_create_default_wifi_sta();
        s_netif_up = true;
    }
    if (s_wifi_events == NULL) {
        s_wifi_events = xEventGroupCreate();
    }
    xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT);

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    if (esp_wifi_init(&cfg) != ESP_OK) {
        return false;
    }
    if (esp_wifi_set_mode(WIFI_MODE_STA) != ESP_OK ||
        esp_wifi_set_storage(WIFI_STORAGE_RAM) != ESP_OK) {
        esp_wifi_deinit();
        return false;
    }
    wifi_config_t wc = { 0 };
    strlcpy((char *)wc.sta.ssid, ssid, sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password, pass, sizeof(wc.sta.password));
    if (esp_wifi_set_config(WIFI_IF_STA, &wc) != ESP_OK) {
        esp_wifi_deinit();
        return false;
    }
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_ip_event, NULL);
    if (esp_wifi_start() != ESP_OK || esp_wifi_connect() != ESP_OK) {
        esp_wifi_stop();
        esp_wifi_deinit();
        return false;
    }
    // Bounded wait for a real IP (association alone does not uplink).
    EventBits_t bits = xEventGroupWaitBits(s_wifi_events, WIFI_CONNECTED_BIT, pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(15000));
    if ((bits & WIFI_CONNECTED_BIT) == 0) {
        ESP_LOGW(TAG, "wifi: no IP within 15 s");
        sync_port_wifi_down();
        return false;
    }
    return true;
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
    char ua[64];
    snprintf(ua, sizeof(ua), UA_BASE "%s", esp_app_get_description()->version);
    esp_http_client_set_header(c, "User-Agent", ua);
    char auth[160];
    int auth_len = snprintf(auth, sizeof(auth), "Bearer %s", token);
    if (auth_len < 0 || auth_len >= (int)sizeof(auth)) {
        ESP_LOGW(TAG, "token too long for the Authorization header");
        esp_http_client_cleanup(c);
        return false;
    }
    esp_http_client_set_header(c, "Authorization", auth);
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
    // ≥ 10 KiB: the TLS handshake peaks deep in mbedtls; verified against
    // uxTaskGetStackHighWaterMark on the target during bench bring-up.
    xTaskCreate(sync_task, "sync", 10240, NULL, tskIDLE_PRIORITY + 2, NULL);
}
