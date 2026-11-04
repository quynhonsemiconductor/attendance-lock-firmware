// ESP-IDF port: RTC-memory time base (survives deep sleep, not power loss),
// esp_timer uptime, system wall clock, bounded SNTP sessions.
#include "clock_port.h"

#include <string.h>
#include <time.h>

#include "esp_attr.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_rtc_time.h"
#include "esp_random.h"
#include "esp_log.h"
#include "esp_sntp.h"

static const char *TAG = "clock";

#include "bootloader_random.h"

#include "sdkconfig.h"

// RTC slow memory: keeps its contents across deep sleep AND soft restarts;
// a power-on reset starts it as garbage/zero — the magic detects that.
static RTC_NOINIT_ATTR clock_state_t s_rtc_state;

int64_t clock_port_uptime_us(void)
{
#if CONFIG_SLEEP_DEEP_ENABLE
    // esp_timer restarts at 0 on every deep-sleep wake (the wake is a
    // reboot), which would break the "uptime continuous across deep sleep"
    // contract in clock.h: an event from before a sleep would be stamped
    // against a base taken after it. The RTC counter keeps running through
    // deep sleep. It runs off the internal RC slow clock (calibrated, but
    // drifts more than the crystal); the base is re-taken every sync
    // session, which bounds the stamp error to that drift over one interval.
    return (int64_t)esp_rtc_get_time_us();
#else
    return esp_timer_get_time();
#endif
}

void clock_port_log(const char *msg)
{
    ESP_LOGE(TAG, "%s", msg);
}

bool clock_port_deep_sleep_wake(void)
{
    return esp_reset_reason() == ESP_RST_DEEPSLEEP;
}

clock_port_res_t clock_port_state_load(void *out, size_t cap, size_t *len)
{
    if (cap < sizeof(s_rtc_state)) {
        return CLOCK_PORT_ERROR;
    }
    memcpy(out, &s_rtc_state, sizeof(s_rtc_state));
    *len = sizeof(s_rtc_state);
    return CLOCK_PORT_FOUND;
}

bool clock_port_state_store(const void *buf, size_t len)
{
    if (len != sizeof(s_rtc_state)) {
        return false;
    }
    memcpy(&s_rtc_state, buf, len);
    return true;
}

void clock_port_random_bytes(void *out, size_t len)
{
    bootloader_random_enable();
    esp_fill_random(out, len);
    bootloader_random_disable();
}

uint64_t clock_port_now_epoch(void)
{
    time_t now = time(NULL);
    return now < 0 ? 0 : (uint64_t)now;
}

void clock_port_sntp_start(void)
{
    // Plain lwip SNTP (the esp_netif_sntp wrapper was removed in IDF v6):
    // poll mode, started explicitly — a bounded session, not a daemon.
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, CONFIG_CLOCK_SNTP_SERVER);
    if (CONFIG_CLOCK_SNTP_SERVER_FALLBACK[0] != '\0') {
        esp_sntp_setservername(1, CONFIG_CLOCK_SNTP_SERVER_FALLBACK);
    }
    esp_sntp_init();
}

void clock_port_sntp_stop(void)
{
    esp_sntp_stop();
}
