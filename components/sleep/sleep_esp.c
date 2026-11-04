// Device side of deep sleep (battery work 2026-11-03): wake cause, the
// bolt-engaged flag and the held pins in RTC memory, deep-sleep entry.
#include "sleep/sleep.h"

#include <stdio.h>

#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "sleep";

#define SLEEP_MAGIC 0x534C5031u // "SLP1"

// RTC slow memory: survives deep sleep; a power-on starts it as garbage,
// which the magic rejects.
static RTC_NOINIT_ATTR struct {
    uint32_t magic;
    uint32_t locked;   // 1 = went to sleep with the bolt engaged + settled
    uint64_t held_low; // pins held low / high through the sleep
    uint64_t held_high;
    uint32_t sleeps;   // deep sleeps since power-on (log only)
} s_rtc;

static bool s_booted;
static sleep_wake_t s_wake;
static bool s_resumed_locked;
static uint64_t s_wake_pins;

static void release_holds(uint64_t mask, int level)
{
    for (int pin = 0; pin < 64; pin++) {
        if ((mask & (1ULL << pin)) == 0 || !GPIO_IS_VALID_OUTPUT_GPIO(pin)) {
            continue;
        }
        // Drive the held level first: gpio_hold_dis on a pin still in its
        // reset (input) mode would let it float for a moment.
        gpio_set_level((gpio_num_t)pin, level);
        gpio_set_direction((gpio_num_t)pin, GPIO_MODE_OUTPUT);
        gpio_hold_dis((gpio_num_t)pin);
    }
}

sleep_wake_t sleep_boot(void)
{
    if (s_booted) {
        return s_wake;
    }
    s_booted = true;
    bool deep = esp_reset_reason() == ESP_RST_DEEPSLEEP;
    bool rtc_ok = s_rtc.magic == SLEEP_MAGIC;
    if (rtc_ok) {
        // Also after a reset that hit DURING sleep (brownout, watchdog): an
        // RTC pad hold outlives a system reset, and a held RST would leave
        // the readers dead.
        release_holds(s_rtc.held_low, 0);
        release_holds(s_rtc.held_high, 1);
    }
    gpio_deep_sleep_hold_dis();

    uint32_t causes = esp_sleep_get_wakeup_causes();
    if (!deep) {
        s_wake = SLEEP_WAKE_COLD;
    } else if (causes & (1u << ESP_SLEEP_WAKEUP_EXT0)) {
        s_wake = SLEEP_WAKE_EXIT; // checked first: a press must not be lost
    } else if (causes & (1u << ESP_SLEEP_WAKEUP_EXT1)) {
        s_wake = SLEEP_WAKE_PIR;
        s_wake_pins = esp_sleep_get_ext1_wakeup_status();
    } else if (causes & (1u << ESP_SLEEP_WAKEUP_TIMER)) {
        s_wake = SLEEP_WAKE_TIMER;
    } else {
        s_wake = SLEEP_WAKE_OTHER;
    }
    // Only a deep-sleep wake with an intact flag skips the boot engage; a
    // crash, watchdog or power-on always engages (fail-secure, §2.8).
    s_resumed_locked = deep && rtc_ok && s_rtc.locked == 1;
    if (!rtc_ok || !deep) {
        s_rtc.magic = SLEEP_MAGIC;
        s_rtc.sleeps = 0;
    }
    s_rtc.locked = 0; // consumed: a later reset must not inherit it
    s_rtc.held_low = 0;
    s_rtc.held_high = 0;

    switch (s_wake) {
    case SLEEP_WAKE_PIR:
        ESP_LOGI(TAG, "wake #%u: pir (mask 0x%llx)", (unsigned)s_rtc.sleeps,
                 (unsigned long long)s_wake_pins);
        break;
    case SLEEP_WAKE_TIMER:
        ESP_LOGI(TAG, "wake #%u: timer (sync due)", (unsigned)s_rtc.sleeps);
        break;
    case SLEEP_WAKE_EXIT:
        ESP_LOGI(TAG, "wake #%u: exit button", (unsigned)s_rtc.sleeps);
        break;
    case SLEEP_WAKE_OTHER:
        ESP_LOGW(TAG, "wake #%u: unexpected cause 0x%x", (unsigned)s_rtc.sleeps,
                 (unsigned)causes);
        break;
    default:
        break;
    }
    if (deep && !s_resumed_locked) {
        ESP_LOGW(TAG, "deep-sleep wake without the locked flag: engaging the bolt");
    }
    return s_wake;
}

bool sleep_resumed_locked(void)
{
    return s_resumed_locked;
}

static uint64_t hold_pins(const int *pins, size_t n, int level)
{
    uint64_t mask = 0;
    for (size_t i = 0; i < n; i++) {
        int pin = pins[i];
        if (pin < 0 || !GPIO_IS_VALID_OUTPUT_GPIO(pin)) {
            continue;
        }
        gpio_set_level((gpio_num_t)pin, level);
        gpio_config_t io = {
            .pin_bit_mask = 1ULL << pin,
            .mode = GPIO_MODE_OUTPUT, // also detaches a peripheral (LEDC)
        };
        (void)gpio_config(&io);
        gpio_set_level((gpio_num_t)pin, level);
        (void)gpio_hold_en((gpio_num_t)pin);
        mask |= 1ULL << pin;
    }
    return mask;
}

void sleep_enter(bool bolt_engaged, uint64_t wake_mask, int exit_gpio, uint64_t timer_us,
                 const int *hold_low, size_t n_low, const int *hold_high, size_t n_high)
{
    s_rtc.magic = SLEEP_MAGIC;
    s_rtc.held_low = hold_pins(hold_low, n_low, 0);
    s_rtc.held_high = hold_pins(hold_high, n_high, 1);
    gpio_deep_sleep_hold_en();
    s_rtc.locked = bolt_engaged ? 1 : 0; // only 1 skips the next boot engage
    s_rtc.sleeps++;

    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    if (wake_mask != 0 &&
        esp_sleep_enable_ext1_wakeup_io(wake_mask, ESP_EXT1_WAKEUP_ANY_HIGH) != ESP_OK) {
        ESP_LOGE(TAG, "ext1 wakeup on 0x%llx refused", (unsigned long long)wake_mask);
    }
    // ext1 is ANY_HIGH for the PIRs; the active-low button needs its own
    // level, which only ext0 gives (it keeps the RTC peripherals powered: a
    // few µA). The board's 10k pull-up holds the pin high while asleep.
    if (exit_gpio >= 0 && esp_sleep_enable_ext0_wakeup((gpio_num_t)exit_gpio, 0) != ESP_OK) {
        ESP_LOGE(TAG, "ext0 wakeup on GPIO%d refused", exit_gpio);
    }
    if (timer_us > 0) {
        (void)esp_sleep_enable_timer_wakeup(timer_us);
    }
    ESP_LOGI(TAG, "deep sleep #%u: pir mask 0x%llx, exit gpio %d, timer %llu s",
             (unsigned)s_rtc.sleeps, (unsigned long long)wake_mask, exit_gpio,
             (unsigned long long)(timer_us / 1000000u));
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(20)); // let the UART drain the last line
    esp_deep_sleep_start();
}
