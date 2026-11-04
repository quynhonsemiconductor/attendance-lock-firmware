// The poll task: wake → REQA/anticoll → post to the door queue → hard
// power-down. It owns NOTHING the door task touches — taps and faults both
// travel the queue. Between polls it just delays: tickless idle (sdkconfig
// PM) light-sleeps the chip, and the door task's pm lock keeps it awake
// while the lock is released. Tap acceptance (debounce + cross-read) runs
// through the host-tested logic. A reader that fails its health check is
// probed every ~5 s (prototype's recovery cadence) instead of every poll.
//
// With PIRs (CONFIG_READER_PIR_GPIO >= 0, battery work 2026-11-01; a second
// one on CONFIG_READER_PIR2_GPIO since 2026-11-03) the delay between polls
// follows reader_scan_plan: fast (READER_POLL_MS) while there is motion on
// either PIR, a slow fallback otherwise. A high PIR level is a GPIO light-
// sleep wakeup and a level interrupt that notifies this task, so the first
// poll after motion runs at once. The interrupt and the wakeup are disarmed
// while the level stays high (a level source would fire / wake continuously)
// and re-armed when it drops. A PIR stuck high for SLEEP_PIR_STUCK_S stops
// counting as motion (it would keep the chip awake forever).
//
// Deep sleep (SLEEP_DEEP_ENABLE): the door task reads reader_presence() to
// decide when to sleep and calls reader_sleep_prepare() to stop this task
// between two polls. A PIR (ext1) wake counts as motion at boot.
#include "reader/rc522.h"
#include "reader/reader.h"

#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
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

// Held by the poll task for one pass over both readers; reader_sleep_prepare
// takes it and never gives it back (deep sleep restarts the firmware).
static SemaphoreHandle_t s_poll_lock;

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

// Motion state shared with the door task (reader_presence): written by the
// poll task, read under s_pres_mux.
static portMUX_TYPE s_pres_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_motion_seen;
static uint32_t s_last_motion_ms;

static void note_motion(uint32_t t)
{
    portENTER_CRITICAL(&s_pres_mux);
    s_motion_seen = true;
    s_last_motion_ms = t;
    portEXIT_CRITICAL(&s_pres_mux);
}

#if CONFIG_READER_PIR_GPIO >= 0
#if defined(CONFIG_READER_PIR2_GPIO) && CONFIG_READER_PIR2_GPIO >= 0
#define PIR_COUNT 2
static const int k_pir_gpio[PIR_COUNT] = { CONFIG_READER_PIR_GPIO, CONFIG_READER_PIR2_GPIO };
#else
#define PIR_COUNT 1
static const int k_pir_gpio[PIR_COUNT] = { CONFIG_READER_PIR_GPIO };
#endif

#if defined(CONFIG_SLEEP_PIR_STUCK_S)
#define PIR_STUCK_MS ((uint32_t)CONFIG_SLEEP_PIR_STUCK_S * 1000u)
#else
#define PIR_STUCK_MS 0u // never stuck: the old single-PIR behaviour
#endif

typedef struct {
    gpio_num_t gpio;
    bool ok;               // configured (ISR attached)
    volatile bool fired;   // ISR saw a high level (and disabled the interrupt)
    bool armed;            // interrupt + light-sleep wakeup enabled
    bool high;             // level at the last pir_update
    bool stuck;            // high for PIR_STUCK_MS without a break
    uint32_t high_since_ms;
} pir_t;

static pir_t s_pir[PIR_COUNT];
static TaskHandle_t s_task;
static volatile uint32_t s_pir_wakes;
static bool s_pir_any_ok;

// Level interrupt: disarm first (it would re-fire while the level stays
// high), then wake the poll task. Not IRAM: the ISR service is installed
// without ESP_INTR_FLAG_IRAM, so it is simply deferred during flash writes.
static void pir_isr(void *arg)
{
    pir_t *p = (pir_t *)arg;
    gpio_intr_disable(p->gpio);
    p->fired = true;
    s_pir_wakes++;
    BaseType_t woken = pdFALSE;
    if (s_task != NULL) {
        vTaskNotifyGiveFromISR(s_task, &woken);
    }
    portYIELD_FROM_ISR(woken);
}

static void pir_arm(pir_t *p, bool arm)
{
    if (arm == p->armed) {
        return;
    }
    if (arm) {
        // gpio_wakeup_enable also keeps the pin configured through sleep
        // (it clears the PM_SLP_DISABLE_GPIO switch for this pin).
        gpio_wakeup_enable(p->gpio, GPIO_INTR_HIGH_LEVEL);
        gpio_intr_enable(p->gpio);
    } else {
        gpio_intr_disable(p->gpio);
        gpio_wakeup_disable(p->gpio);
    }
    p->armed = arm;
}

static bool pir_init_one(pir_t *p, int gpio)
{
    p->gpio = (gpio_num_t)gpio;
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << gpio,
        .mode = GPIO_MODE_INPUT,
        .pull_down_en = GPIO_PULLDOWN_ENABLE, // unplugged PIR reads "no motion"
        .intr_type = GPIO_INTR_HIGH_LEVEL,
    };
    if (gpio_config(&io) != ESP_OK) {
        return false;
    }
    gpio_intr_disable(p->gpio);
    esp_err_t err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) { // already installed is fine
        return false;
    }
    if (gpio_isr_handler_add(p->gpio, pir_isr, p) != ESP_OK) {
        return false;
    }
    p->armed = false;
    pir_arm(p, true);
    return true;
}

static bool s_fast;

// Sample the PIRs, keep each arming in step with its level, log mode
// changes and return the cadence. A pulse that ended before this task ran
// still counts as motion (the ISR flag), so a short wave is never lost.
static reader_scan_plan_t pir_update(void)
{
    uint32_t t = now_ms();
    bool motion = false;
    bool any_high = false;
    for (int i = 0; i < PIR_COUNT; i++) {
        pir_t *p = &s_pir[i];
        if (!p->ok) {
            continue;
        }
        bool fired = p->fired;
        if (fired) {
            p->fired = false;
            p->armed = true; // the ISR only disabled the interrupt —
            pir_arm(p, false); // finish the disarm (light-sleep wakeup too)
        }
        bool high = gpio_get_level(p->gpio) == 1;
        if (high && !p->high) {
            p->high_since_ms = t;
        }
#if CONFIG_READER_PIR_DEBUG
        if (high != p->high || fired) {
            ESP_LOGI(TAG, "pir: GPIO%d level %d%s (isr %u)", (int)p->gpio, high ? 1 : 0,
                     fired ? " after isr" : "", (unsigned)s_pir_wakes);
        }
#endif
        p->high = high;
        bool stuck = reader_pir_stuck(high, p->high_since_ms, t, PIR_STUCK_MS);
        if (stuck != p->stuck) {
            p->stuck = stuck;
            if (stuck) {
                ESP_LOGW(TAG, "pir: GPIO%d high for %u s — ignored until it drops",
                         (int)p->gpio, (unsigned)(PIR_STUCK_MS / 1000u));
            } else {
                ESP_LOGI(TAG, "pir: GPIO%d dropped — in use again", (int)p->gpio);
            }
        }
        if (!stuck && (fired || high)) {
            motion = true;
        }
        any_high = any_high || (high && !stuck);
        pir_arm(p, !high); // re-armed only once the level has dropped
    }
    if (motion) {
        note_motion(t);
    }
    portENTER_CRITICAL(&s_pres_mux);
    bool seen = s_motion_seen;
    uint32_t last = s_last_motion_ms;
    portEXIT_CRITICAL(&s_pres_mux);
    reader_scan_plan_t plan =
        reader_scan_plan(t, any_high, seen, last, CONFIG_READER_PIR_ACTIVE_MS,
                         CONFIG_READER_POLL_MS, CONFIG_READER_IDLE_POLL_MS);
    if (plan.fast != s_fast) {
        s_fast = plan.fast;
        if (plan.fast) {
            ESP_LOGI(TAG, "pir: motion -> fast scan every %d ms (until %d ms after last motion)",
                     CONFIG_READER_POLL_MS, CONFIG_READER_PIR_ACTIVE_MS);
        } else {
            ESP_LOGI(TAG, "pir: idle -> fallback poll every %d ms (pir wakes since boot: %u)",
                     CONFIG_READER_IDLE_POLL_MS, (unsigned)s_pir_wakes);
        }
    }
    return plan;
}
#endif

uint32_t reader_pir_wake_count(void)
{
#if CONFIG_READER_PIR_GPIO >= 0
    return s_pir_wakes;
#else
    return 0;
#endif
}

void reader_presence(reader_presence_t *out)
{
    memset(out, 0, sizeof(*out));
    portENTER_CRITICAL(&s_pres_mux);
    out->motion_seen = s_motion_seen;
    out->last_motion_ms = s_last_motion_ms;
    portEXIT_CRITICAL(&s_pres_mux);
#if CONFIG_READER_PIR_GPIO >= 0
    for (int i = 0; i < PIR_COUNT; i++) {
        const pir_t *p = &s_pir[i];
        if (!p->ok || p->stuck) {
            continue; // a stuck PIR neither holds the chip awake nor wakes it
        }
        out->wake_mask |= 1ULL << p->gpio;
        // fired: the ISR saw a high level the poll task has not sampled yet
        if (p->fired || gpio_get_level(p->gpio) == 1) {
            out->any_high = true;
        }
    }
#endif
}

bool reader_sleep_prepare(uint32_t timeout_ms)
{
    if (s_poll_lock == NULL) {
        return true; // no poll task: nothing to stop
    }
    if (xSemaphoreTake(s_poll_lock, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        return false;
    }
    // Between polls both readers are already in hard power-down (RST low);
    // the deep-sleep entry holds RST low. Kept: s_poll_lock (never resumes).
#if CONFIG_READER_PIR_GPIO >= 0
    for (int i = 0; i < PIR_COUNT; i++) {
        if (s_pir[i].ok) {
            gpio_intr_disable(s_pir[i].gpio);
        }
    }
#endif
    return true;
}

static void poll_loop(void *arg)
{
    (void)arg;
    esp_task_wdt_add(NULL);
    memset(s_tap_state, 0, reader_state_size());
    for (;;) {
        xSemaphoreTake(s_poll_lock, portMAX_DELAY);
#if CONFIG_READER_PIR_GPIO >= 0
        if (s_pir_any_ok) {
            (void)pir_update(); // log "motion" before this poll's taps
        }
#endif
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
                    // A card in the field is a person at the door even when
                    // the PIR missed them (hand over the lens): keep the fast
                    // cadence (and the chip awake) for the next tap (bench
                    // 2026-11-02).
                    note_motion(now);
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
#if CONFIG_READER_PIR_GPIO >= 0
        if (s_pir_any_ok) {
            // A PIR edge notifies this task: the next poll starts at once,
            // so a tap right after motion is read well inside 1 s.
            reader_scan_plan_t plan = pir_update();
            xSemaphoreGive(s_poll_lock);
            (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(plan.wait_ms));
            continue;
        }
#endif
        xSemaphoreGive(s_poll_lock);
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
    s_poll_lock = xSemaphoreCreateMutex();
    if (s_poll_lock == NULL) {
        ESP_LOGE(TAG, "poll lock alloc failed — no card reads");
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
#if CONFIG_READER_PIR_GPIO >= 0
    // A PIR (ext1) deep-sleep wake is motion even when the pulse has ended
    // by now: the person who woke the door gets the fast window.
    if (esp_sleep_get_wakeup_causes() & (1u << ESP_SLEEP_WAKEUP_EXT1)) {
        note_motion(now_ms());
    }
    // Before the task exists: an early edge only sets the fired flag, which
    // the first pir_update picks up. On failure the readers fall back to the
    // plain READER_POLL_MS cadence (no PIR is never worse than before).
    if (esp_sleep_enable_gpio_wakeup() != ESP_OK) {
        ESP_LOGE(TAG, "gpio light-sleep wakeup refused — PIRs only cut the poll rate");
    }
    for (int i = 0; i < PIR_COUNT; i++) {
        s_pir[i].ok = pir_init_one(&s_pir[i], k_pir_gpio[i]);
        if (s_pir[i].ok) {
            s_pir_any_ok = true;
            ESP_LOGI(TAG, "pir %d on GPIO%d: fast %d ms for %d ms after motion, idle %d ms", i,
                     k_pir_gpio[i], CONFIG_READER_POLL_MS, CONFIG_READER_PIR_ACTIVE_MS,
                     CONFIG_READER_IDLE_POLL_MS);
        } else {
            ESP_LOGE(TAG, "pir %d init failed on GPIO%d", i, k_pir_gpio[i]);
        }
    }
    if (!s_pir_any_ok) {
        ESP_LOGE(TAG, "no PIR usable — polling every %d ms", CONFIG_READER_POLL_MS);
    }
    TaskHandle_t *task = &s_task;
#else
    TaskHandle_t *task = NULL;
#endif
    if (xTaskCreate(poll_loop, "reader", 4096, NULL, tskIDLE_PRIORITY + 3, task) != pdPASS) {
        ESP_LOGE(TAG, "reader task create failed");
    }
}
