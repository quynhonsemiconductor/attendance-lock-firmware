// ESP-IDF port: provisioned creds (NVS namespace "prov", written by the
// provisioning console in PR 5), station Wi-Fi in bounded sessions, HTTPS
// with the bundled CA set, random for jitter.
#include "sync_port.h"

#include <string.h>
#include <strings.h>

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
#include "sdkconfig.h"

#include "power/power.h"
#include "power/power_calc.h"
#include "reader/reader.h"
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
        ESP_LOGE(TAG, "nvs unusable (err=%d) — refusing to wipe provisioning", (int)e);
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
// One-time radio bring-up: the STA netif, the default event loop, the IP
// handler and the Wi-Fi driver are created ONCE and never torn down; a
// session is start/connect → sync → disconnect/stop. Creating the netif
// once but esp_wifi_init/deinit per session left the netif attached to a
// destroyed driver, so session 2+ could come up with no IP (E2E review
// 2026-10-28, firmware item 7): netif and driver now live together.
// Each step keeps its OWN done-flag: a failure partway through init must
// not leak the event group or double-register the IP handler on the retry
// (round-2 review item 5).
static bool s_netif_inited, s_loop_inited, s_netif_created, s_events_created,
    s_handler_registered, s_driver_inited;

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

bool sync_port_wifi_up(sync_fail_t *fail)
{
    if (fail != NULL) {
        fail->stage = SYNC_STAGE_WIFI; // steps below override the step
        fail->code = 0;
    }
    if (!nvs_ready()) {
        if (fail != NULL) {
            fail->stage = SYNC_STAGE_CONFIG;
        }
        return false;
    }
    char ssid[33], pass[64];
    if (!read_wifi_creds(ssid, sizeof(ssid), pass, sizeof(pass))) {
        ESP_LOGW(TAG, "no wifi credentials provisioned");
        if (fail != NULL) {
            fail->stage = SYNC_STAGE_CONFIG;
        }
        return false;
    }
    if (!s_netif_inited) {
        if (esp_netif_init() != ESP_OK) {
            return false;
        }
        s_netif_inited = true;
    }
    if (!s_loop_inited) {
        esp_err_t e = esp_event_loop_create_default();
        if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) {
            return false;
        }
        s_loop_inited = true;
    }
    if (!s_netif_created) { // never twice: a second default-sta netif would
        if (esp_netif_create_default_wifi_sta() == NULL) { // leave the first
            return false; // attached to nothing and the IP events would race
        }
        s_netif_created = true;
    }
    if (!s_events_created) {
        s_wifi_events = xEventGroupCreate();
        if (s_wifi_events == NULL) {
            return false;
        }
        s_events_created = true;
    }
    if (!s_handler_registered) { // exactly once across all sessions
        if (esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_ip_event, NULL) !=
            ESP_OK) {
            return false;
        }
        s_handler_registered = true;
    }
    if (!s_driver_inited) {
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        if (esp_wifi_init(&cfg) != ESP_OK ||
            esp_wifi_set_mode(WIFI_MODE_STA) != ESP_OK ||
            esp_wifi_set_storage(WIFI_STORAGE_RAM) != ESP_OK) {
            return false;
        }
        s_driver_inited = true;
    }
    // Per session: (re)apply credentials — the console may have
    // re-provisioned since the last session — then start/connect.
    xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT);
    wifi_config_t wc = { 0 };
    strlcpy((char *)wc.sta.ssid, ssid, sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password, pass, sizeof(wc.sta.password));
    if (esp_wifi_set_config(WIFI_IF_STA, &wc) != ESP_OK ||
        esp_wifi_start() != ESP_OK || esp_wifi_connect() != ESP_OK) {
        sync_port_wifi_down();
        return false;
    }
    // Bounded wait for a real IP (association alone does not uplink).
    EventBits_t bits = xEventGroupWaitBits(s_wifi_events, WIFI_CONNECTED_BIT, pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(15000));
    if ((bits & WIFI_CONNECTED_BIT) == 0) {
        ESP_LOGW(TAG, "wifi: no IP within 15 s");
        if (fail != NULL) {
            fail->stage = SYNC_STAGE_IP; // associated (or not) but no IP
        }
        sync_port_wifi_down();
        return false;
    }
    return true;
}

void sync_port_wifi_down(void)
{
    // stop only: the driver + netif stay up for the next session (the
    // per-step init flags above).
    esp_wifi_disconnect();
    esp_wifi_stop();
}

void sync_port_battery(uint16_t *battery_mv, bool *low_battery, bool *fault)
{
    uint32_t mv = power_battery_mv(); // averaged, servo-idle reading
    // Round-3 bench finding: an unwired divider floats the tap near full
    // scale (33,9xx mV). Never report that as a pack voltage — the server
    // rejected every batch with 422.
    if (!power_reading_plausible(mv, CONFIG_POWER_BATTERY_MIN_MV,
                                 CONFIG_POWER_BATTERY_MAX_MV)) {
        ESP_LOGW(TAG, "battery reading implausible (%u mV): divider wiring?",
                 (unsigned)mv);
        *battery_mv = 0;
        *low_battery = false;
        *fault = true;
        return;
    }
    if (mv > 0xFFFF) {
        mv = 0xFFFF;
    }
    *battery_mv = (uint16_t)mv;
    *low_battery = power_is_low(mv, CONFIG_POWER_BATTERY_LOW_MV);
    *fault = false;
}

// What the HTTP event handler captures from one exchange. The body goes
// through sync_body_acc_add (the host-tested bounded accumulator; round-2
// review item 4) fed from HTTP_EVENT_ON_DATA — esp_http_client_perform
// CONSUMES the body, so reading it back with esp_http_client_read after
// perform returns 0 bytes and every 200 looked malformed (E2E review
// 2026-10-28, firmware item 1). ETag and Date come from
// HTTP_EVENT_ON_HEADER (esp_http_client_get_header reads the REQUEST
// headers — it can never see a response ETag; item 2).
// SAVE_RESPONSE_HEADERS is deliberately NOT used: its header store caps at
// 10 fields and helmet adds more, so the capture would be a lottery.
typedef struct {
    sync_body_acc_t body;
    char *etag;
    size_t etag_cap;
    char *date;
    size_t date_cap;
} http_capture_t;

static esp_err_t http_event_cb(esp_http_client_event_t *evt)
{
    http_capture_t *cap = evt->user_data;
    if (cap == NULL) {
        return ESP_OK;
    }
    switch (evt->event_id) {
    case HTTP_EVENT_ON_HEADER:
        // Header names are case-insensitive on the wire; Fastify sends
        // lowercase ("etag", "date"), the RFC examples uppercase.
        if (cap->etag != NULL && strcasecmp(evt->header_key, "ETag") == 0) {
            snprintf(cap->etag, cap->etag_cap, "%s", evt->header_value);
        } else if (cap->date != NULL && strcasecmp(evt->header_key, "Date") == 0) {
            snprintf(cap->date, cap->date_cap, "%s", evt->header_value);
        }
        break;
    case HTTP_EVENT_ON_DATA:
        // return value ignored on purpose: keep draining, the overflow latch
        // fails the exchange after perform returns
        (void)sync_body_acc_add(&cap->body, evt->data, (size_t)evt->data_len);
        break;
    default:
        break;
    }
    return ESP_OK;
}

bool sync_port_http(const char *method, const char *url, const char *path, const char *token,
                    const char *if_none_match, const char *body, size_t body_len,
                    long *status, char *resp, size_t cap, size_t *resp_len, char *etag,
                    size_t etag_cap, char *date, size_t date_cap, sync_fail_t *fail)
{
    if (fail != NULL) {
        fail->stage = SYNC_STAGE_CONFIG; // until the exchange starts
        fail->code = 0;
    }
    char full[192];
    if (snprintf(full, sizeof(full), "%s%s", url, path) >= (int)sizeof(full)) {
        ESP_LOGW(TAG, "url too long for the request buffer");
        return false;
    }
    http_capture_t capture = {
        .etag = etag,
        .etag_cap = etag_cap,
        .date = date,
        .date_cap = date_cap,
    };
    sync_body_acc_init(&capture.body, resp, cap);
    esp_http_client_config_t cfg = {
        .url = full,
        .method = strcmp(method, "POST") == 0 ? HTTP_METHOD_POST : HTTP_METHOD_GET,
        .crt_bundle_attach = esp_crt_bundle_attach, // the CA set, not one pin
        .timeout_ms = 10000,
        .buffer_size = 2048,
        // Never follow a redirect: the Authorization header would go to the
        // redirect target. opshub never redirects the terminal API.
        .disable_auto_redirect = true,
        .event_handler = http_event_cb,
        .user_data = &capture,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (c == NULL) {
        ESP_LOGW(TAG, "http client init failed");
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
        long code = (long)esp_http_client_get_status_code(c);
        if (capture.body.overflow) {
            // Same rule as the host port: an oversized body is a transport
            // failure, never a silently truncated 200 (a truncated
            // allow-list would freeze revocations).
            ESP_LOGW(TAG, "response body over %u bytes: failing, not truncating",
                     (unsigned)(cap - 1));
            if (fail != NULL) {
                fail->stage = SYNC_STAGE_HTTP;
                fail->code = 0;
            }
        } else {
            *status = code;
            if (resp != NULL) {
                resp[capture.body.len] = '\0';
                *resp_len = capture.body.len;
            } else {
                *resp_len = 0;
            }
            if (fail != NULL) {
                fail->stage = SYNC_STAGE_NONE;
                fail->code = 0;
            }
            ok = true;
        }
    } else {
        // The summary names the stage: esp-tls keeps its last error on the
        // transport's error handle, so a handshake/certificate failure is
        // distinguishable from resolve/connect (both of which arrive as
        // ESP_ERR_HTTP_CONNECT; round-3 bench: the URL typo showed only
        // "sock < 0>"). tls_code != 0 means a real TLS-level error; a TCP
        // connect timeout sets the esp-tls last_error but no mbedtls code.
        int tls_code = 0, tls_flags = 0;
        esp_err_t tls_err = ESP_OK;
        if (strncasecmp(url, "https://", 8) == 0) {
            tls_err = esp_http_client_get_and_clear_last_tls_error(c, &tls_code, &tls_flags);
        }
        int socket_errno = esp_http_client_get_errno(c);
        ESP_LOGW(TAG, "%s %s failed: %s (0x%x), tls 0x%x, errno %d", method, path,
                 esp_err_to_name(e), (unsigned)e, (unsigned)tls_code, socket_errno);
        if (fail != NULL) {
            if (tls_err != ESP_OK && tls_code != 0) {
                fail->stage = SYNC_STAGE_TLS;
                fail->code = (int)tls_err;
            } else {
                fail->stage = SYNC_STAGE_DNS;
                fail->code = socket_errno != 0 ? socket_errno : (int)e;
            }
        }
    }
    esp_http_client_cleanup(c);
    return ok;
}

uint32_t sync_port_take_misreads(void)
{
    // Reader counter is monotonic since boot; the session wants the delta
    // since the previous session (the envelope field is per session).
    static uint32_t last;
    uint32_t total = reader_misread_count();
    uint32_t delta = total - last;
    last = total;
    return delta;
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
        uint32_t delay_ms = sync_next_delay_ms(ok, fails);
        // One summary line per session at INFO, success included: the bench
        // operator reads WHY a session failed and WHEN the next one runs
        // without the server log (round-3 bench finding). The `sync: `
        // prefix in the monitor is this log tag.
        sync_session_result_t res;
        sync_session_result(&res);
        char line[320];
        if (sync_format_summary(&res, fails, delay_ms, line, sizeof(line)) > 0) {
            ESP_LOGI(TAG, "%s", line);
        }
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
}

void sync_port_task_start(void)
{
    // ≥ 10 KiB: the TLS handshake peaks deep in mbedtls; verified against
    // uxTaskGetStackHighWaterMark on the target during bench bring-up.
    xTaskCreate(sync_task, "sync", 10240, NULL, tskIDLE_PRIORITY + 2, NULL);
}
