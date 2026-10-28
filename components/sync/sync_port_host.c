// Host port: scripted transport + fake credentials for the session tests.
// Test helpers in sync_port_host.h.
#include "sync_port_host.h"

#include "sync_port.h"

#include <stdio.h>
#include <string.h>

static bool provisioned = true;
static char s_url[128] = "https://opshub.example";
static char s_token[128] = "test-token";
static char s_ssid[64] = "bench", s_pass[64] = "bench";

static bool wifi_up;

// Request journal (last request wins; the tests assert on it) plus a frozen
// copy of the last GET (the drain loop's POSTs overwrite the journal).
sync_port_req_t sync_host_last_request;
sync_port_req_t sync_host_last_get;
sync_port_req_t sync_host_first_post;
int sync_host_post_count;

// Scripted battery reading for the envelope (sent every session).
static uint16_t s_battery_mv = 5400;
static bool s_battery_low;

// Scripted responses, consumed in order; an empty script = transport failure.
// A drained session (GET + up to N batches) needs one slot per exchange.
#define SCRIPT_MAX 8
static sync_port_resp_t script[SCRIPT_MAX];
static int script_len;
static int script_pos;

bool sync_port_creds(char *url, size_t url_cap, char *token, size_t token_cap)
{
    if (!provisioned) {
        return false;
    }
    snprintf(url, url_cap, "%s", s_url);
    snprintf(token, token_cap, "%s", s_token);
    return true;
}

bool sync_port_wifi_up(void)
{
    if (wifi_up) {
        return false; // double-up = failure, like a real radio stack
    }
    return (wifi_up = true);
}

void sync_port_wifi_down(void)
{
    wifi_up = false;
}

void sync_port_battery(uint16_t *battery_mv, bool *low_battery)
{
    *battery_mv = s_battery_mv;
    *low_battery = s_battery_low;
}

bool sync_port_http(const char *method, const char *url, const char *path, const char *token,
                    const char *if_none_match, const char *body, size_t body_len,
                    long *status, char *resp, size_t cap, size_t *resp_len, char *etag,
                    size_t etag_cap, char *date, size_t date_cap)
{
    memset(&sync_host_last_request, 0, sizeof(sync_host_last_request));
    if (strcmp(method, "POST") == 0) {
        sync_host_post_count++;
    }
    snprintf(sync_host_last_request.method, sizeof(sync_host_last_request.method), "%s", method);
    snprintf(sync_host_last_request.url, sizeof(sync_host_last_request.url), "%s", url);
    snprintf(sync_host_last_request.path, sizeof(sync_host_last_request.path), "%s", path);
    snprintf(sync_host_last_request.token, sizeof(sync_host_last_request.token), "%s", token);
    snprintf(sync_host_last_request.if_none_match, sizeof(sync_host_last_request.if_none_match),
             "%s", if_none_match);
    if (body != NULL && body_len < sizeof(sync_host_last_request.body)) {
        memcpy(sync_host_last_request.body, body, body_len);
    }
    if (strcmp(method, "GET") == 0) {
        sync_host_last_get = sync_host_last_request; // frozen: POSTs overwrite
    }
    if (strcmp(method, "POST") == 0 && sync_host_post_count == 1) {
        sync_host_first_post = sync_host_last_request; // post_count was just
    }                                       // incremented to 1 by the first POST
    if (script_pos >= script_len) {
        return false; // scripted transport exhausted = transport failure
    }
    sync_port_resp_t r = script[script_pos++];
    *status = r.status;
    *resp_len = 0;
    if (resp != NULL && r.body != NULL) {
        size_t n = strlen(r.body);
        if (n >= cap) {
            return false; // same semantics as the device port: an oversized
                          // body is a transport failure, never a truncated 200
        }
        memcpy(resp, r.body, n);
        *resp_len = n;
    }
    if (etag != NULL) {
        snprintf(etag, etag_cap, "%s", r.etag ? r.etag : "");
    }
    if (date != NULL) {
        snprintf(date, date_cap, "%s", r.date ? r.date : "");
    }
    return true;
}

uint32_t sync_port_random_u32(void)
{
    return 42; // deterministic jitter in tests
}

void sync_port_delay_ms(uint32_t ms)
{
    (void)ms;
}

void sync_port_task_start(void)
{
}

// --- Test helpers ---

void sync_host_reset(void)
{
    script_len = 0;
    script_pos = 0;
    memset(&sync_host_last_request, 0, sizeof(sync_host_last_request));
    memset(&sync_host_last_get, 0, sizeof(sync_host_last_get));
    memset(&sync_host_first_post, 0, sizeof(sync_host_first_post));
    sync_host_post_count = 0;
    s_battery_mv = 5400;
    s_battery_low = false;
    provisioned = true;
    wifi_up = false;
}

void sync_host_enqueue(long status, const char *body, const char *etag, const char *date)
{
    if (script_len < SCRIPT_MAX) {
        script[script_len++] = (sync_port_resp_t){ status, body, etag, date };
    }
}

void sync_host_set_provisioned(bool on)
{
    provisioned = on;
}

void sync_host_set_battery(uint16_t battery_mv, bool low_battery)
{
    s_battery_mv = battery_mv;
    s_battery_low = low_battery;
}
