// The poll task: wake → REQA/anticoll → callback → hard power-down, with a
// light sleep between polls (battery/wake decisions 2026-10-25). Tap
// acceptance (debounce + cross-read) runs through the host-tested logic.
// A reader that fails its health check is probed every ~5 s (prototype's
// recovery cadence) instead of every poll.
#include "reader/rc522.h"
#include "reader/reader.h"

#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "sdkconfig.h"

#define OFFLINE_PROBE_MS 5000

static const char *TAG = "reader";

static void (*s_on_card)(const reader_card_t *card);
static rc522_conf_t s_conf[2];
static bool s_online[2];
static uint32_t s_last_probe_ms[2];
static uint8_t *s_tap_state;

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void poll_loop(void *arg)
{
    (void)arg;
    memset(s_tap_state, 0, reader_state_size());
    for (;;) {
        uint32_t now = now_ms();
        for (int i = 0; i < 2; i++) {
            bool polled = true;
            if (!s_online[i]) {
                if ((uint32_t)(now - s_last_probe_ms[i]) < OFFLINE_PROBE_MS) {
                    polled = false; // dead reader: probe on the 5 s cadence
                } else {
                    s_last_probe_ms[i] = now;
                }
            }
            if (polled) {
                uint8_t uid[4];
                uint8_t uid_len = sizeof(uid);
                if (rc522_wake_poll(&s_conf[i], uid, &uid_len) && s_on_card != NULL) {
                    reader_card_t card = { .reader = (uint8_t)i, .uid_len = uid_len };
                    memcpy(card.uid, uid, uid_len);
                    if (reader_accept(s_tap_state, reader_state_size(), &card, now,
                                      CONFIG_READER_DEBOUNCE_MS,
                                      CONFIG_READER_CROSS_READ_MS)) {
                        s_on_card(&card);
                    }
                }
            }
            rc522_power_down(&s_conf[i]);
        }
        // Light sleep between polls: the poll timer wakes us. Uptime
        // continues across light sleep, so event stamps stay valid.
        esp_sleep_enable_timer_wakeup((uint64_t)CONFIG_READER_POLL_MS * 1000);
        esp_light_sleep_start();
    }
}

void reader_start(void (*on_card)(const reader_card_t *card))
{
    s_on_card = on_card;
    s_tap_state = calloc(1, reader_state_size());
    if (s_tap_state == NULL) {
        ESP_LOGE(TAG, "tap state alloc failed — no card reads");
        return;
    }
    if (!rc522_bus_init(CONFIG_READER_SPI_HOST, CONFIG_READER_SPI_SCK, CONFIG_READER_SPI_MISO,
                        CONFIG_READER_SPI_MOSI)) {
        ESP_LOGE(TAG, "spi bus init failed — no card reads");
        return;
    }
    s_conf[0] = (rc522_conf_t){ CONFIG_READER_SPI_HOST, CONFIG_READER_CS_OUT,
                                CONFIG_READER_RST_OUT };
    s_conf[1] = (rc522_conf_t){ CONFIG_READER_SPI_HOST, CONFIG_READER_CS_IN,
                                CONFIG_READER_RST_IN };
    for (int i = 0; i < 2; i++) {
        s_online[i] = rc522_init(&s_conf[i]);
        if (!s_online[i]) {
            ESP_LOGW(TAG, "reader %d offline at boot — probing every %d ms", i,
                     OFFLINE_PROBE_MS);
        }
        rc522_power_down(&s_conf[i]);
    }
    if (xTaskCreate(poll_loop, "reader", 4096, NULL, tskIDLE_PRIORITY + 3, NULL) != pdPASS) {
        ESP_LOGE(TAG, "reader task create failed");
    }
}
