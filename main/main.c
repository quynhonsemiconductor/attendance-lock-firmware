// app_main — the door loop (work item 7, PR 4): boots locked, serves taps
// from the local cache, syncs on the interval. Feedback: green+beep =
// allow, red+double-beep = deny (battery/wake decisions 2026-10-25). The
// low-battery flag only REPORTS — it never unlocks.
#include <string.h>

#include "clock/clock.h"
#include "door/door.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lock/lock.h"
#include "power/power.h"
#include "provision/provision.h"
#include "reader/reader.h"
#include "store/store.h"
#include "sync/sync.h"

#include "sdkconfig.h"

static const char *TAG = "door";

static store_card_t s_cards[STORE_CARDS_MAX];
static size_t s_n_cards;
static bool s_cache_loaded;
static bool s_unlocked;
static uint32_t s_unlocked_at_ms;
static bool s_exit_prev;
static uint32_t s_exit_at_ms;

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void feedback(bool allow)
{
    gpio_set_level(CONFIG_DOOR_FEEDBACK_GPIO_GREEN, allow ? 1 : 0);
    gpio_set_level(CONFIG_DOOR_FEEDBACK_GPIO_RED, allow ? 0 : 1);
    if (allow) {
        gpio_set_level(CONFIG_DOOR_FEEDBACK_GPIO_BUZZER, 1);
        // The beep/double-beep pattern self-clears below on the next loop
        // pass (the loop ticks every poll interval, ~400 ms).
    }
}

static void feedback_clear(void)
{
    gpio_set_level(CONFIG_DOOR_FEEDBACK_GPIO_GREEN, 0);
    gpio_set_level(CONFIG_DOOR_FEEDBACK_GPIO_RED, 0);
    gpio_set_level(CONFIG_DOOR_FEEDBACK_GPIO_BUZZER, 0);
}

static void on_card(const reader_card_t *card)
{
    uint64_t now = 0;
    bool synced = clock_synced();
    if (synced) {
        clock_stamp(card->boot_id, (int64_t)card->uptime_us, &now, &synced);
    }
    door_decision_t d = door_decide(s_cards, s_n_cards, synced, now, card->uid, card->uid_len);
    bool allow = (d == DOOR_ALLOW);
    ESP_LOGI(TAG, "tap r=%u result=%s reason=%d lowbatt=%u", card->reader,
             allow ? "allow" : "deny", d, (unsigned)power_battery_mv());

    feedback(allow);
    if (allow) {
        lock_release();
        s_unlocked = true;
        s_unlocked_at_ms = now_ms();
    }

    sync_event_t ev = {
        .reader = card->reader,
        .uid_len = card->uid_len,
        .result = allow ? 0 : 1,
        .boot_id = clock_boot_id(),
        .uptime_us = card->uptime_us,
    };
    memcpy(ev.uid, card->uid, card->uid_len);
    if (!store_event_push((const uint8_t *)&ev, sizeof(ev), NULL)) {
        ESP_LOGW(TAG, "event ring full — tap not buffered (counted)");
    }
}

static void load_cache(void)
{
    // Refresh the RAM mirror from the store (the sync task replaces the
    // cache in NVS; the door reads its own copy to keep the tap path short).
    char etag[STORE_ETAG_MAX];
    size_t n = 0, etag_len = 0;
    if (store_allow_list_read(s_cards, STORE_CARDS_MAX, &n, etag, sizeof(etag), &etag_len) ==
        STORE_OK) {
        s_n_cards = n;
        s_cache_loaded = true;
    }
}

static void poll_exit_button(void)
{
    // Convenience unlock (inside); free egress is mechanical and outside
    // firmware scope (ADR §2.9). Not an attendance event (§2.4).
    int level = gpio_get_level(CONFIG_DOOR_EXIT_BTN_GPIO);
    uint32_t now = now_ms();
    if (level == 0 && s_exit_prev == 0 && !s_unlocked &&
        (int32_t)(now - s_exit_at_ms) > CONFIG_DOOR_EXIT_DEBOUNCE_MS) {
        lock_release();
        s_unlocked = true;
        s_unlocked_at_ms = now;
        s_exit_at_ms = now;
        feedback(true);
    }
    s_exit_prev = level;
}

static void poll_unlock_hold(void)
{
    if (s_unlocked && (uint32_t)(now_ms() - s_unlocked_at_ms) > CONFIG_DOOR_UNLOCK_HOLD_MS) {
        lock_engage();
        s_unlocked = false;
        feedback_clear();
    }
}

void app_main(void)
{
    // Fail-secure order: the lock comes up FIRST, before anything that could
    // block or crash. A hung boot or a watchdog reset lands here again —
    // locked (§2.8: boots locked).
    lock_init();

    provision_init();
    bool store_ok = store_init();
    clock_init();
    power_init();
    sync_init();

    gpio_config_t out = {
        .pin_bit_mask = (1ULL << CONFIG_DOOR_FEEDBACK_GPIO_GREEN) |
                        (1ULL << CONFIG_DOOR_FEEDBACK_GPIO_RED) |
                        (1ULL << CONFIG_DOOR_FEEDBACK_GPIO_BUZZER),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&out);
    gpio_config_t in = {
        .pin_bit_mask = 1ULL << CONFIG_DOOR_EXIT_BTN_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&in);

    load_cache();
    reader_start(on_card);

    esp_task_wdt_config_t wdt = {
        .timeout_ms = 30000,
        .idle_core_mask = 0,
        .trigger_panic = true,
    };
    esp_task_wdt_init(&wdt);
    esp_task_wdt_add(NULL);

    if (!store_ok) {
        ESP_LOGE(TAG, "store unavailable — taps still decide from an empty cache");
    }
    if (power_low_battery()) {
        // Report only — it never unlocks (battery decisions 2026-10-25).
        ESP_LOGW(TAG, "battery low at boot: %u mV", (unsigned)power_battery_mv());
    }

    uint32_t last_cache_refresh = 0;
    for (;;) {
        poll_exit_button();
        poll_unlock_hold();
        if ((uint32_t)(now_ms() - last_cache_refresh) > 60000) {
            load_cache(); // picks up sync's replaced list within a minute
            last_cache_refresh = now_ms();
        }
        esp_task_wdt_reset();
        // No deep sleep here: the poll task light-sleeps between polls and
        // this loop yields; the deep-sleep cycle lands with PR 5/6.
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}
