// The poll task: wake → REQA/anticoll → post to the door queue → hard
// power-down. It owns NOTHING the door task touches — taps and faults both
// travel the queue. Between polls it just delays: tickless idle (sdkconfig
// PM) light-sleeps the chip, and the door task's pm lock keeps it awake
// while the lock is released. Tap acceptance (debounce + cross-read) runs
// through the host-tested logic. A reader that fails its health check is
// probed every ~5 s (prototype's recovery cadence) instead of every poll.
#include "reader/rc522.h"
#include "reader/reader.h"

#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "sdkconfig.h"

#define OFFLINE_PROBE_MS 5000

static const char *TAG = "reader";

static void (*s_on_card)(const reader_card_t *card);
static void (*s_on_fault)(uint8_t reader);
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
    esp_task_wdt_add(NULL);
    memset(s_tap_state, 0, reader_state_size());
    for (;;) {
        uint32_t now = now_ms();
        for (int i = 0; i < 2; i++) {
            bool was_probe = !s_online[i];
            bool poll = s_online[i];
            if (!poll && (uint32_t)(now - s_last_probe_ms[i]) >= OFFLINE_PROBE_MS) {
                s_last_probe_ms[i] = now;
                poll = true; // dead reader: probe on the 5 s cadence
            }
            if (poll) {
                uint8_t uid[4];
                uint8_t uid_len = sizeof(uid);
                switch (rc522_wake_poll(&s_conf[i], uid, &uid_len)) {
                case RC522_CARD:
                    if (!s_online[i]) {
                        s_online[i] = true;
                    }
                    if (s_on_card != NULL) {
                        reader_card_t card = { .reader = (uint8_t)i, .uid_len = uid_len };
                        memcpy(card.uid, uid, uid_len);
                        if (reader_accept(s_tap_state, reader_state_size(), &card, now,
                                          CONFIG_READER_DEBOUNCE_MS,
                                          CONFIG_READER_CROSS_READ_MS)) {
                            s_on_card(&card);
                        }
                    }
                    break;
                case RC522_NONE:
                    // Empty field is a normal poll, never a fault. A probe
                    // that gets this far passed the health check: recovered.
                    if (was_probe) {
                        s_online[i] = true;
                        ESP_LOGI(TAG, "reader %d back online", i);
                    }
                    break;
                case RC522_FAULT:
                    if (s_online[i]) {
                        s_online[i] = false;
                        ESP_LOGW(TAG, "reader %d fault (health check) — probing", i);
                        if (s_on_fault != NULL) {
                            s_on_fault((uint8_t)i);
                        }
                    }
                    break;
                }
            }
            rc522_power_down(&s_conf[i]);
        }
        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(CONFIG_READER_POLL_MS));
    }
}

uint32_t reader_misread_count(void)
{
    return rc522_misread_count();
}

void reader_start(void (*on_card)(const reader_card_t *card),
                  void (*on_fault)(uint8_t reader))
{
    s_on_card = on_card;
    s_on_fault = on_fault;
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
        rc522_pins_init(&s_conf[i]); // both CS high before either reader talks
    }
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
