// ESP-IDF port: the "prov" NVS namespace (exactly the keys sync_port_esp.c
// reads) and the UART0 line reader with local echo (idf.py monitor does not
// echo). The token and the Wi-Fi pass are never read back, never logged.
#include "provision_port.h"

#include <stdio.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "provision/provision.h"
#include "sdkconfig.h"

static const char *TAG = "prov";

#define NS "prov"
#define PROV_LINE_MAX 256
#define REPLY_MAX 512

static bool nvs_ready(void)
{
    static bool done;
    static bool logged;
    if (done) {
        return true;
    }
    esp_err_t e = nvs_flash_init();
    if (e == ESP_OK) {
        done = true;
        return true;
    }
    // NEVER wipe here: provisioning (base URL, token, Wi-Fi) lives in this
    // partition — an erase would silently de-provision the door. Only an
    // explicit factory-reset action may erase NVS.
    if (!logged) { // called on every access: complain once, not per access
        ESP_LOGE(TAG, "nvs unusable (%s) — refusing to wipe provisioning", esp_err_to_name(e));
        logged = true;
    }
    return false;
}

bool provision_port_set_str(const char *key, const char *value)
{
    if (!nvs_ready()) {
        return false;
    }
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) { // first write creates "prov"
        return false;
    }
    bool ok = nvs_set_str(h, key, value) == ESP_OK && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    return ok;
}

bool provision_port_get_str(const char *key, char *out, size_t cap)
{
    out[0] = '\0';
    if (!nvs_ready()) {
        return false;
    }
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) {
        return false; // namespace absent = not provisioned yet
    }
    size_t len = cap;
    esp_err_t e = nvs_get_str(h, key, out, &len);
    nvs_close(h);
    return e == ESP_OK;
}

bool provision_port_has_str(const char *key)
{
    if (!nvs_ready()) {
        return false;
    }
    // Entry iteration reads metadata only: the token and the Wi-Fi pass are
    // never read back (§2.8). IDF v6 API: the iterator comes back by
    // out-param and nvs_entry_next() clears it at the end of the iteration.
    bool found = false;
    nvs_iterator_t it = NULL;
    esp_err_t e = nvs_entry_find("nvs", NS, NVS_TYPE_STR, &it);
    while (e == ESP_OK) {
        nvs_entry_info_t info;
        nvs_entry_info(it, &info);
        if (strcmp(info.key, key) == 0) {
            found = true;
            break;
        }
        e = nvs_entry_next(&it);
    }
    if (it != NULL) {
        nvs_release_iterator(it);
    }
    return found;
}

bool provision_port_allow_http(void)
{
#ifdef CONFIG_PROVISION_ALLOW_HTTP
    return true; // dev bench only (Kconfig; asserted unset in prod by CI)
#else
    return false;
#endif
}

// --- UART0 console task -------------------------------------------------

// CR LF terminals send both line-end characters: remember the CR and eat
// the LF that starts the next line.
static bool skip_lf;

static size_t read_line(char *buf, size_t cap)
{
    size_t n = 0;
    for (;;) {
        int c = fgetc(stdin);
        if (c == EOF) {
            vTaskDelay(pdMS_TO_TICKS(20)); // between characters: yield
            continue;
        }
        if (c == '\r' || c == '\n') {
            if (c == '\n' && n == 0 && skip_lf) {
                skip_lf = false; // the LF of a CR LF pair
                continue;
            }
            skip_lf = (c == '\r');
            putchar('\n');
            break;
        }
        if ((c == 0x7F || c == '\b') && n > 0) { // backspace edits the tail
            n--;
            fputs("\b \b", stdout);
            continue;
        }
        if (c >= 0x20 && c < 0x7F && n + 1 < cap) {
            buf[n++] = (char)c;
            putchar(c); // local echo: monitor does not echo
        }
    }
    buf[n] = '\0';
    fflush(stdout);
    return n;
}

static void console_task(void *arg)
{
    (void)arg;
    static char line[PROV_LINE_MAX]; // file-scope-sized buffers, off the stack
    static char reply[REPLY_MAX]; // (matches the other components)
    for (;;) {
        fputs("\rdoor> ", stdout);
        fflush(stdout);
        read_line(line, sizeof(line));
        provision_handle_line(line, reply, sizeof(reply));
        fputs(reply, stdout);
    }
}

void provision_init(void)
{
    // Low priority: a human at a bench; the door task always preempts it.
    xTaskCreate(console_task, "console", 4096, NULL, tskIDLE_PRIORITY + 1, NULL);
}
