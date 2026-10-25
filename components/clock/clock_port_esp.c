// ESP-IDF port: RTC-memory time base (survives deep sleep, not power loss),
// esp_timer uptime, system wall clock, bounded SNTP sessions.
#include "clock_port.h"

#include <string.h>
#include <time.h>

#include "esp_attr.h"
#include "esp_timer.h"
#include "esp_netif_sntp.h"

#include "sdkconfig.h"

// RTC slow memory: keeps its contents across deep sleep; a power-on reset
// starts it as garbage/zero — the magic in the base detects that.
static RTC_NOINIT_ATTR struct {
    uint32_t magic;
    uint64_t epoch;
    int64_t uptime_us;
} s_rtc_base;

#define BASE_MAGIC 0x434C4B31u // "CLK1" — must match clock.c

int64_t clock_port_uptime_us(void)
{
    return esp_timer_get_time();
}

clock_port_res_t clock_port_state_load(void *out, size_t cap, size_t *len)
{
    if (cap < sizeof(s_rtc_base)) {
        return CLOCK_PORT_ERROR;
    }
    memcpy(out, &s_rtc_base, sizeof(s_rtc_base));
    *len = sizeof(s_rtc_base);
    return CLOCK_PORT_FOUND;
}

bool clock_port_state_store(const void *buf, size_t len)
{
    if (len != sizeof(s_rtc_base)) {
        return false;
    }
    memcpy(&s_rtc_base, buf, len);
    return true;
}

uint64_t clock_port_now_epoch(void)
{
    time_t now = time(NULL);
    return now < 0 ? 0 : (uint64_t)now;
}

void clock_port_sntp_start(void)
{
    const char *servers[] = {
        CONFIG_CLOCK_SNTP_SERVER,
        CONFIG_CLOCK_SNTP_SERVER_FALLBACK,
    };
    size_t n = servers[1][0] == '\0' ? 1 : 2;
    esp_netif_sntp_config_t cfg =
        ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(n, (const char **)servers);
    cfg.start = false; // started explicitly below: a bounded session, not a daemon
    if (esp_netif_sntp_init(&cfg) == ESP_OK) {
        esp_netif_sntp_start();
    }
}

void clock_port_sntp_stop(void)
{
    esp_netif_sntp_deinit();
}
