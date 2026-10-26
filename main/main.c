// app_main — the door loop (work item 7, PR 4b): boots locked, one door
// task owns the FSM + the allow-list snapshot; the reader task only posts
// taps to a queue. Feedback: green+single-beep = allow, red 2 s+double-beep
// = deny. The low-battery flag only REPORTS — it never unlocks; a reading
// under cutoff+margin DENIES the release (battery decisions 2026-10-25).
#include <string.h>

#include "clock/clock.h"
#include "door_fsm/door_fsm.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_pm.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lock/lock.h"
#include "power/power.h"
#include "power/power_calc.h"
#include "provision/provision.h"
#include "reader/reader.h"
#include "store/store.h"
#include "sync/sync.h"

#include "sdkconfig.h"

static const char *TAG = "door";

// What the reader task posts; a fault marker needs no card.
typedef struct {
    bool fault;
    uint8_t reader;
    uint8_t uid_len;
    uint8_t uid[10];
    uint64_t uptime_us;
} door_msg_t;

static QueueHandle_t s_taps;
static door_fsm_t s_fsm;
static esp_pm_lock_handle_t s_nosleep;

// Two allow-list buffers: the reload fills the inactive one, then the door
// task flips the pointer — a tap never walks a list being overwritten.
static store_card_t s_cards[2][STORE_CARDS_MAX];
static size_t s_n_cards[2];
static uint8_t s_active; // 0/1 buffer index the decisions read

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

// Lock commands only on a state CHANGE: re-driving lock_engage every 50 ms
// tick would restart the servo pulse forever (hum + battery drain) and the
// release timer would never fire. First state after lock_init: engaged.
static bool s_lock_engaged = true;
static uint32_t s_servo_until_ms; // a move is running until this time

static void drive_lock(bool engage)
{
    if (engage == s_lock_engaged) {
        return;
    }
    if (engage) {
        lock_engage();
    } else {
        lock_release();
    }
    s_lock_engaged = engage;
    s_servo_until_ms = now_ms() + CONFIG_LOCK_SERVO_MOVE_MS;
}

static void apply_out(const door_out_t *out)
{
    if (out->engage) {
        drive_lock(true);
    } else if (out->release) {
        drive_lock(false);
    }
    gpio_set_level(CONFIG_DOOR_FEEDBACK_GPIO_GREEN, out->green ? 1 : 0);
    gpio_set_level(CONFIG_DOOR_FEEDBACK_GPIO_RED, out->red ? 1 : 0);
    gpio_set_level(CONFIG_DOOR_FEEDBACK_GPIO_BUZZER, out->buzzer ? 1 : 0);
}

static void feedback_off(void)
{
    gpio_set_level(CONFIG_DOOR_FEEDBACK_GPIO_GREEN, 0);
    gpio_set_level(CONFIG_DOOR_FEEDBACK_GPIO_RED, 0);
    gpio_set_level(CONFIG_DOOR_FEEDBACK_GPIO_BUZZER, 0);
}

static bool battery_deny(uint32_t battery_mv)
{
    return power_should_deny(battery_mv, CONFIG_POWER_BATTERY_CUTOFF_MV,
                             CONFIG_POWER_BATTERY_DENY_MARGIN_MV) ||
           power_brownout_blocked(now_ms());
}

// Latest battery reading, refreshed between servo moves — a tap during a
// move reuses it instead of sampling the sagging rail (false deny).
static uint32_t s_last_battery_mv;
static bool s_last_low_battery;
static bool s_battery_valid;

static void handle_tap(const door_msg_t *msg)
{
    if (now_ms() >= s_servo_until_ms) {
        // Sample only with the servo idle — the move surge skews the rail.
        s_last_battery_mv = power_battery_mv();
        s_last_low_battery = power_is_low(s_last_battery_mv, CONFIG_POWER_BATTERY_LOW_MV);
        s_battery_valid = s_last_battery_mv != 0;
    }
    uint32_t battery_mv = s_battery_valid ? s_last_battery_mv : 0;

    uint64_t stamp = 0;
    bool synced = clock_synced();
    if (synced) {
        clock_stamp(clock_boot_id(), (int64_t)msg->uptime_us, &stamp, &synced);
    }
    door_decision_t d = door_decide(s_cards[s_active], s_n_cards[s_active], synced, stamp,
                                    msg->uid, msg->uid_len);
    if (d == DOOR_ALLOW && battery_deny(battery_mv)) {
        d = DOOR_DENY_BATTERY; // the surge would brown the rail out — stay locked
    }
    door_out_t out = door_fsm_apply(&s_fsm, now_ms(), d);
    apply_out(&out);
    ESP_LOGI(TAG, "tap r=%u %s (%s) battery=%umV%s", msg->reader,
             d == DOOR_ALLOW ? "allow" : "deny", door_decision_name(d), battery_mv,
             s_last_low_battery && s_battery_valid ? " low" : "");

    sync_event_t ev = {
        .reader = msg->reader,
        .uid_len = msg->uid_len,
        .result = d == DOOR_ALLOW ? 0 : 1,
        .battery_mv = (uint16_t)(battery_mv > 0xFFFF ? 0xFFFF : battery_mv),
        .low_battery = s_last_low_battery && s_battery_valid,
        .boot_id = clock_boot_id(),
        .uptime_us = msg->uptime_us,
    };
    memcpy(ev.uid, msg->uid, msg->uid_len);
    if (!store_event_push((const uint8_t *)&ev, sizeof(ev), NULL)) {
        ESP_LOGW(TAG, "event ring full — tap not buffered (counted)");
    }
}

static void reload_allow_list(void)
{
    char etag[STORE_ETAG_MAX];
    size_t n = 0, etag_len = 0;
    uint8_t inactive = (uint8_t)(1u - s_active);
    if (store_allow_list_read(s_cards[inactive], STORE_CARDS_MAX, &n, etag, sizeof(etag),
                              &etag_len) == STORE_OK) {
        s_n_cards[inactive] = n;
        s_active = inactive; // one store — handle_tap (same task) sees the new list whole
    }
}

// Reader-task context: convert + post, nothing else.
static void on_card_post(const reader_card_t *card)
{
    door_msg_t msg = {
        .reader = card->reader,
        .uid_len = card->uid_len,
        .uptime_us = (uint64_t)esp_timer_get_time(),
    };
    memcpy(msg.uid, card->uid, card->uid_len);
    xQueueSend(s_taps, &msg, 0); // full queue = a tap we could not keep up with
}

static void on_fault_post(uint8_t reader)
{
    door_msg_t msg = { .fault = true, .reader = reader };
    xQueueSend(s_taps, &msg, 0);
}

static void door_task(void *arg)
{
    (void)arg;
    esp_task_wdt_add(NULL);
    uint32_t last_reload = 0;
    bool was_released = false;
    bool pm_held = true; // held since app_main's boot engage (below)
    for (;;) {
        door_msg_t msg;
        if (xQueueReceive(s_taps, &msg, pdMS_TO_TICKS(50))) {
            if (msg.fault) {
                door_out_t out = door_fsm_fault(&s_fsm, now_ms());
                gpio_set_level(CONFIG_DOOR_FEEDBACK_GPIO_RED, out.red ? 1 : 0);
                ESP_LOGW(TAG, "reader %u fault — door stays %s", msg.reader,
                         door_fsm_released(&s_fsm) ? "released" : "locked");
            } else {
                handle_tap(&msg);
            }
        }
        door_out_t out = door_fsm_tick(&s_fsm, now_ms());
        apply_out(&out);

        // pm lock edges: acquire on locked->released, hold through the
        // relock's servo move (drive_lock stamped s_servo_until_ms), never
        // re-acquire on a re-tap that only extends the window.
        bool released = door_fsm_released(&s_fsm);
        if (!was_released && released && !pm_held) {
            esp_pm_lock_acquire(s_nosleep);
            pm_held = true;
        }
        if (!released && pm_held && now_ms() >= s_servo_until_ms) {
            esp_pm_lock_release(s_nosleep);
            pm_held = false;
        }
        was_released = released;

        if ((uint32_t)(now_ms() - last_reload) > 60000) {
            reload_allow_list(); // picks up sync's replaced list within a minute
            last_reload = now_ms();
        }
        esp_task_wdt_reset();
    }
}

void app_main(void)
{
    // Power management on: DFS + light sleep between the reader polls
    // (the pm lock keeps the chip awake while the lock moves or is released).
    esp_pm_config_t pm = {
        .max_freq_mhz = 240,
        .min_freq_mhz = 10,
        .light_sleep_enable = true,
    };
    if (esp_pm_configure(&pm) != ESP_OK) {
        ESP_LOGE(TAG, "esp_pm_configure failed — running flat out");
    }

    // Fail-secure order: the lock comes up FIRST, before anything that could
    // block or crash. A hung boot or a watchdog reset lands here again —
    // locked (§2.8: boots locked). The pm lock covers the boot move too;
    // door_task releases it once the move has settled.
    if (esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "door", &s_nosleep) != ESP_OK) {
        ESP_LOGE(TAG, "pm lock create failed — no light sleep at all");
    } else {
        esp_pm_lock_acquire(s_nosleep);
    }
    lock_init();
    s_servo_until_ms = now_ms() + CONFIG_LOCK_SERVO_MOVE_MS;

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
    (void)gpio_config(&out);
    feedback_off();

    // The TWDT is initialised by sdkconfig (10 s, panic) — nothing to init
    // here; the reader and door tasks subscribe themselves.

    door_fsm_init(&s_fsm, CONFIG_DOOR_UNLOCK_HOLD_MS);
    s_taps = xQueueCreate(8, sizeof(door_msg_t));
    reload_allow_list(); // primes buffer 0 (and flips to it)

    // The reader task does nothing but post (queue only — no shared state
    // with the door task).
    reader_start(on_card_post, on_fault_post);
    if (xTaskCreate(door_task, "door", 4096, NULL, tskIDLE_PRIORITY + 4, NULL) != pdPASS) {
        ESP_LOGE(TAG, "door task create failed");
    }

    if (!store_ok) {
        ESP_LOGE(TAG, "store unavailable — taps still decide from an empty cache");
    }
    if (power_low_battery()) {
        // Report only — it never unlocks (battery decisions 2026-10-25).
        ESP_LOGW(TAG, "battery low at boot: %u mV", (unsigned)power_battery_mv());
    }
    if (power_brownout_count() > 0) {
        ESP_LOGW(TAG, "%u brownout resets since the last clean boot — releases "
                      "denied during the backoff window",
                 (unsigned)power_brownout_count());
    }
}
