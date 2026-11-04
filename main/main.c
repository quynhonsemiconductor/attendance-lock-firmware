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
#include "esp_rtc_time.h"
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
#include "sleep/sleep.h"
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
    // An implausible reading (divider fault) denies like a low pack:
    // fail-secure. Round-3 bench finding: without this arm "34 V" from an
    // unwired divider let every release through.
    return power_release_blocked(battery_mv, CONFIG_POWER_BATTERY_CUTOFF_MV,
                                 CONFIG_POWER_BATTERY_DENY_MARGIN_MV,
                                 CONFIG_POWER_BATTERY_MIN_MV, CONFIG_POWER_BATTERY_MAX_MV) ||
           power_brownout_blocked(now_ms());
}

// Latest battery reading, refreshed between servo moves — a tap during a
// move reuses it instead of sampling the sagging rail (false deny).
static uint32_t s_last_battery_mv;
static bool s_last_low_battery;
static bool s_battery_valid;
static bool s_battery_fault; // reading exists but cannot be a pack voltage

// Refresh the cached reading. Servo must be IDLE here: the move surge sags
// the rail and would read a false low (false deny). The raw reading is
// cached even when it is implausible, so the release gate sees the fault.
static void sample_battery(void)
{
    uint32_t mv = power_battery_mv();
    bool plausible = power_reading_plausible(mv, CONFIG_POWER_BATTERY_MIN_MV,
                                             CONFIG_POWER_BATTERY_MAX_MV);
    if (!plausible && !s_battery_fault) {
        ESP_LOGW(TAG, "battery reading implausible (%u mV): divider wiring?", (unsigned)mv);
    } else if (plausible && s_battery_fault) {
        ESP_LOGI(TAG, "battery reading plausible again (%u mV)", (unsigned)mv);
    }
    s_battery_fault = !plausible;
    s_last_battery_mv = mv;
    s_last_low_battery = power_is_low(mv, CONFIG_POWER_BATTERY_LOW_MV) && plausible;
    s_battery_valid = mv != 0;
}

static void handle_tap(const door_msg_t *msg)
{
    if (now_ms() >= s_servo_until_ms) {
        sample_battery();
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
    ESP_LOGI(TAG, "tap r=%u %s (%s) battery=%umV%s%s", msg->reader,
             d == DOOR_ALLOW ? "allow" : "deny", door_decision_name(d), battery_mv,
             s_last_low_battery && s_battery_valid ? " low" : "",
             s_battery_fault ? " fault" : "");

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

#if CONFIG_DOOR_EXIT_GPIO >= 0
// Inside exit button (KEY1, GPIO6 active low; 2026-11-03). A press opens
// like an allowed tap: same hold window and feedback, same battery gate (a
// release on a sagging pack browns the rail out; the inside handle still
// opens mechanically). Logged on the console only: the event wire format
// (sync_event_t) has no "no card" form yet, so nothing is uploaded.
static door_exit_t s_exit;
static bool s_exit_stuck_logged;

static void exit_init(bool woke_by_exit)
{
    gpio_config_t in = {
        .pin_bit_mask = 1ULL << CONFIG_DOOR_EXIT_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE, // backs up the board's 10k
    };
    (void)gpio_config(&in);
    door_exit_init(&s_exit);
    if (woke_by_exit) {
        // The wake IS the press (handled by the door task's first tick);
        // the button still being down must not fire it a second time.
        s_exit.raw = true;
        s_exit.down = true;
        s_exit.raw_since_ms = now_ms();
    }
}

static void handle_exit(void)
{
    if (now_ms() >= s_servo_until_ms) {
        sample_battery();
    }
    uint32_t battery_mv = s_battery_valid ? s_last_battery_mv : 0;
    door_decision_t d = battery_deny(battery_mv) ? DOOR_DENY_BATTERY : DOOR_ALLOW;
    door_out_t out = door_fsm_apply(&s_fsm, now_ms(), d);
    apply_out(&out);
    ESP_LOGI(TAG, "exit_button %s (%s) battery=%umV", d == DOOR_ALLOW ? "allow" : "deny",
             door_decision_name(d), battery_mv);
}

// One sample per door-task tick (50 ms): debounce, press edge, stuck log.
static void poll_exit(bool *wake_press)
{
    bool pressed = gpio_get_level(CONFIG_DOOR_EXIT_GPIO) == 0;
    uint32_t t = now_ms();
    bool fire = door_exit_update(&s_exit, pressed, t, CONFIG_DOOR_EXIT_DEBOUNCE_MS);
    if (*wake_press) {
        *wake_press = false;
        fire = true;
    }
    if (fire) {
        handle_exit();
    }
    bool stuck = door_exit_stuck(&s_exit, t, CONFIG_DOOR_EXIT_STUCK_S * 1000u);
    if (stuck && !s_exit_stuck_logged) {
        ESP_LOGW(TAG, "exit button held %d s: stuck? (ignored for sleep)",
                 CONFIG_DOOR_EXIT_STUCK_S);
    } else if (!stuck && s_exit_stuck_logged) {
        ESP_LOGI(TAG, "exit button released");
    }
    s_exit_stuck_logged = stuck;
}
#endif

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
        // The clock's uptime, not esp_timer: under deep sleep it is the RTC
        // counter, the only one continuous across a wake (clock_stamp).
        .uptime_us = (uint64_t)clock_uptime_us(),
    };
    memcpy(msg.uid, card->uid, card->uid_len);
    xQueueSend(s_taps, &msg, 0); // full queue = a tap we could not keep up with
}

static void on_fault_post(uint8_t reader)
{
    door_msg_t msg = { .fault = true, .reader = reader };
    xQueueSend(s_taps, &msg, 0);
}

#if CONFIG_SLEEP_DEEP_ENABLE
static sleep_wake_t s_wake;
static sleep_verdict_t s_last_verdict = SLEEP_GO;

// Deep-sleep gate (battery work 2026-11-03), run every door-task tick. The
// chip sleeps only with the bolt engaged and settled, no feedback running,
// no tap queued, no sync session due, no PIR high and no motion for
// READER_PIR_ACTIVE_MS. It wakes on a PIR (ext1) or when the next session
// is due (timer). Returns only when it decided to stay awake.
static void maybe_sleep(void)
{
    uint32_t t = now_ms();
    reader_presence_t pres;
    reader_presence(&pres);
    uint32_t console_ms = 0;
    bool console_seen = provision_console_last_input(&console_ms);
    // The exit button: a press in progress keeps the chip up (the wake
    // would fire at once anyway); a stuck one is neither waited for nor
    // armed as a wake source.
    bool exit_busy = false;
    int exit_gpio = -1;
#if CONFIG_DOOR_EXIT_GPIO >= 0
    bool exit_stuck = door_exit_stuck(&s_exit, t, CONFIG_DOOR_EXIT_STUCK_S * 1000u);
    exit_busy = s_exit.raw && !exit_stuck;
    exit_gpio = exit_stuck ? -1 : CONFIG_DOOR_EXIT_GPIO;
#endif
    sleep_inputs_t in = {
        .now_ms = t,
        .door_idle = door_fsm_idle(&s_fsm, t) && (int32_t)(t - s_servo_until_ms) >= 0 &&
                     s_lock_engaged && uxQueueMessagesWaiting(s_taps) == 0 && !exit_busy,
        .sync_busy = sync_busy(),
        .pir_high = pres.any_high,
        .motion_seen = pres.motion_seen,
        .last_motion_ms = pres.last_motion_ms,
        .awake_ms = CONFIG_READER_PIR_ACTIVE_MS,
        .boot_hold_ms = s_wake == SLEEP_WAKE_COLD ? CONFIG_SLEEP_BOOT_AWAKE_S * 1000u : 0,
        .console_seen = console_seen,
        .console_last_ms = console_ms,
        .console_hold_ms = CONFIG_SLEEP_CONSOLE_IDLE_S * 1000u,
    };
    sleep_verdict_t v = sleep_decide(&in);
    if (v != SLEEP_GO) {
        s_last_verdict = v;
        return;
    }
    // Stop the other tasks between two steps: sync first (non-blocking),
    // then the reader (waits for a running poll). Past the queue re-check
    // below there is no way back, so nothing after it may fail.
    if (!sync_sleep_prepare()) {
        return; // a session started meanwhile
    }
    if (!reader_sleep_prepare(500)) {
        sync_sleep_cancel();
        return;
    }
    // The reader is parked and can no longer post. Anything queued now came
    // from the poll pass the park waited for (or a fault post): a person at
    // the door. Call the sleep off; the next tick handles it with full
    // feedback.
    if (uxQueueMessagesWaiting(s_taps) != 0) {
        reader_sleep_cancel();
        sync_sleep_cancel();
        return;
    }
    feedback_off();
    reader_presence(&pres); // fresh mask: a PIR may have gone stuck meanwhile
    // Timer floor 1 s: a session that is already due wakes the chip almost
    // at once (the gate saw it as not due a moment ago; it is not worth a
    // way back from here). Cap 24 h: a sanity bound — a schedule further
    // out than that (e.g. sync failed to start) still gets a daily wake.
    const uint64_t k_timer_min_us = 1000000ull;
    const uint64_t k_timer_max_us = 24ull * 3600u * 1000000u;
    uint64_t now_rtc = esp_rtc_get_time_us();
    uint64_t timer_us =
        sleep_timer_us(now_rtc, sync_next_due_rtc_us(), k_timer_min_us, k_timer_max_us);
    ESP_LOGI(TAG, "idle (last stay: %s, uptime %u ms) -> deep sleep",
             sleep_verdict_name(s_last_verdict), (unsigned)t);
    // RST low = RC522 hard power-down; CS high = deselected; the servo
    // signal and feedback low (a floating line can twitch the servo or
    // half-drive the buzzer transistor).
    const int hold_low[] = {
        CONFIG_READER_RST_OUT,
        CONFIG_READER_RST_IN,
#if CONFIG_LOCK_BACKEND_SERVO
        CONFIG_LOCK_SERVO_GPIO,
#if CONFIG_LOCK_SERVO_PWR_GPIO >= 0 && CONFIG_LOCK_SERVO_PWR_ACTIVE
        CONFIG_LOCK_SERVO_PWR_GPIO, // servo rail off
#endif
#elif CONFIG_LOCK_RELAY_ACTIVE
        CONFIG_LOCK_RELAY_GPIO, // active-high relay: engaged = low
#endif
        CONFIG_DOOR_FEEDBACK_GPIO_GREEN,
        CONFIG_DOOR_FEEDBACK_GPIO_RED,
        CONFIG_DOOR_FEEDBACK_GPIO_BUZZER,
    };
    const int hold_high[] = {
        CONFIG_READER_CS_OUT,
        CONFIG_READER_CS_IN,
#if CONFIG_LOCK_BACKEND_RELAY && !CONFIG_LOCK_RELAY_ACTIVE
        CONFIG_LOCK_RELAY_GPIO, // active-low relay: engaged = high
#endif
#if CONFIG_LOCK_BACKEND_SERVO && CONFIG_LOCK_SERVO_PWR_GPIO >= 0 && !CONFIG_LOCK_SERVO_PWR_ACTIVE
        CONFIG_LOCK_SERVO_PWR_GPIO, // active-low enable: servo rail off
#endif
        -1, // keeps the array non-empty in every configuration
    };
    // A PIR that went high since the gate is still in the mask: ext1 then
    // wakes the chip at once, which costs one boot, never a missed person.
    // The bolt state is passed, not assumed: only a sleep with the bolt
    // engaged and settled lets the next wake skip the boot engage.
    bool bolt_engaged = s_lock_engaged && (int32_t)(now_ms() - s_servo_until_ms) >= 0;
    sleep_enter(bolt_engaged, pres.wake_mask, exit_gpio, timer_us, hold_low,
                sizeof(hold_low) / sizeof(hold_low[0]), hold_high,
                sizeof(hold_high) / sizeof(hold_high[0]));
}
#endif

static void door_task(void *arg)
{
    (void)arg;
    esp_task_wdt_add(NULL);
    uint32_t last_reload = 0;
    bool was_released = false;
    bool pm_held = true; // held since app_main's boot engage (below)
#if CONFIG_DOOR_EXIT_GPIO >= 0
    bool exit_wake_press = sleep_boot() == SLEEP_WAKE_EXIT; // idempotent
#endif
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
#if CONFIG_DOOR_EXIT_GPIO >= 0
        poll_exit(&exit_wake_press);
#endif
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
#if CONFIG_SLEEP_DEEP_ENABLE
        maybe_sleep();
#endif
    }
}

void app_main(void)
{
    // First: release the pins the previous deep sleep held (RC522 RST/CS,
    // servo, feedback) before any driver touches them, and learn why we run.
    sleep_wake_t wake = sleep_boot();
#if CONFIG_SLEEP_DEEP_ENABLE
    s_wake = wake;
#else
    (void)wake;
#endif

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

    // Prime the battery cache while the rail is still quiet: the boot engage
    // move below sags it, and a tap during that window reuses the cache —
    // without a sample here it is invalid (0 mV) and the low-battery deny
    // would be skipped. power_init is ADC setup only (fast, non-blocking).
    power_init();
    sample_battery();

    // Fail-secure order: the lock comes up FIRST, before anything that could
    // block or crash. A hung boot or a watchdog reset lands here again —
    // locked (§2.8: boots locked). The pm lock covers the boot move too;
    // door_task releases it once the move has settled.
    if (esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "door", &s_nosleep) != ESP_OK) {
        ESP_LOGE(TAG, "pm lock create failed — no light sleep at all");
    } else {
        esp_pm_lock_acquire(s_nosleep);
    }
    if (sleep_resumed_locked()) {
        // Deep-sleep wake: the chip only sleeps with the bolt engaged and
        // settled — no boot engage (a servo surge on every PIR wake). Any
        // other boot (power-on, reset, crash) engages: §2.8 fail-secure.
        lock_resume();
        s_servo_until_ms = now_ms();
    } else {
        lock_init();
        s_servo_until_ms = now_ms() + CONFIG_LOCK_SERVO_MOVE_MS;
    }

    provision_init();
    bool store_ok = store_init();
    clock_init();
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
    if (s_battery_fault) {
        // Same red fault pattern a reader fault shows (2 s): the pack
        // reading is unusable, which is a fault, not a battery level.
        door_out_t fault = door_fsm_fault(&s_fsm, now_ms());
        gpio_set_level(CONFIG_DOOR_FEEDBACK_GPIO_RED, fault.red ? 1 : 0);
    }
#if CONFIG_DOOR_EXIT_GPIO >= 0
    exit_init(wake == SLEEP_WAKE_EXIT);
#endif
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
    if (s_last_low_battery && s_battery_valid) {
        // Report only — it never unlocks (battery decisions 2026-10-25).
        // The cached sample is used on purpose: a second ADC read here would
        // race the boot servo move and can read the sagging rail.
        ESP_LOGW(TAG, "battery low at boot: %u mV", (unsigned)s_last_battery_mv);
    }
    if (power_brownout_count() > 0) {
        ESP_LOGW(TAG, "%u brownout resets since the last clean boot — releases "
                      "denied during the backoff window",
                 (unsigned)power_brownout_count());
    }
}
