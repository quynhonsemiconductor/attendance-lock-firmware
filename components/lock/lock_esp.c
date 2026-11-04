// Device wiring: relay GPIO or servo on LEDC. Build only on ESP-IDF.
#include "lock/lock_esp.h"

#include <stdbool.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "lock/lock_math.h"

#include "sdkconfig.h"

static const char *TAG = "lock";
static esp_timer_handle_t s_release_timer;

#if CONFIG_LOCK_BACKEND_SERVO
#if CONFIG_LOCK_SERVO_PWR_GPIO >= 0
// Servo power switch (2026-11-03): EN -> P-MOSFET high side on the 6 V.
static volatile bool s_pwr_on;

static void servo_power(bool on)
{
    gpio_set_level(CONFIG_LOCK_SERVO_PWR_GPIO,
                   on ? CONFIG_LOCK_SERVO_PWR_ACTIVE : !CONFIG_LOCK_SERVO_PWR_ACTIVE);
    s_pwr_on = on;
}
#endif

static void release_cb(void *arg)
{
    (void)arg;
    // No pulse = no holding current (and no hum). The bolt position is
    // mechanical; a bolt that can be pushed back sets LOCK_SERVO_RELEASE=n.
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
#if CONFIG_LOCK_SERVO_PWR_GPIO >= 0
    servo_power(false); // pulse first, then the rail: no half-powered twitch
#endif
}
#endif

void lock_esp_init(void)
{
#if CONFIG_LOCK_BACKEND_SERVO
#if CONFIG_LOCK_SERVO_PWR_GPIO >= 0
    // Off before the pin turns into an output (the board's pull keeps the
    // MOSFET off until then): the boot engage powers it on for its move.
    gpio_set_level(CONFIG_LOCK_SERVO_PWR_GPIO, !CONFIG_LOCK_SERVO_PWR_ACTIVE);
    gpio_config_t pwr = {
        .pin_bit_mask = 1ULL << CONFIG_LOCK_SERVO_PWR_GPIO,
        .mode = GPIO_MODE_OUTPUT,
    };
    (void)gpio_config(&pwr);
    servo_power(false);
#endif
    ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_14_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = CONFIG_LOCK_SERVO_FREQ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    if (ledc_timer_config(&timer) != ESP_OK) {
        ESP_LOGE(TAG, "ledc timer config failed");
        return;
    }
    ledc_channel_config_t ch = {
        .gpio_num = CONFIG_LOCK_SERVO_GPIO,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0,
        .hpoint = 0,
    };
    if (ledc_channel_config(&ch) != ESP_OK) {
        ESP_LOGE(TAG, "ledc channel config failed");
    }
    const esp_timer_create_args_t targs = {
        .callback = release_cb,
        .name = "lock-release",
    };
    if (esp_timer_create(&targs, &s_release_timer) != ESP_OK) {
        ESP_LOGE(TAG, "release timer create failed");
        s_release_timer = NULL;
    }
#else
    // The output register is set to the ENGAGED level BEFORE the pin turns
    // into an output: an active-low relay would otherwise see the default
    // low level as a command glitch at boot. Boot = engaged is §2.8 anyway.
    gpio_set_level(CONFIG_LOCK_RELAY_GPIO, CONFIG_LOCK_RELAY_ACTIVE ? 1 : 0);
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << CONFIG_LOCK_RELAY_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
#endif
}

#if CONFIG_LOCK_BACKEND_RELAY
void lock_esp_relay_write(int level)
{
    gpio_set_level(CONFIG_LOCK_RELAY_GPIO,
                   level ? (CONFIG_LOCK_RELAY_ACTIVE ? 1 : 0)
                         : (CONFIG_LOCK_RELAY_ACTIVE ? 0 : 1));
}
#endif

#if CONFIG_LOCK_BACKEND_SERVO
void lock_esp_servo_write(uint32_t pulse_us)
{
    // The host-tested math (lock_math.c) sizes the duty; clamping lives
    // there, not twice.
    uint32_t duty =
        lock_servo_duty_ticks(pulse_us, CONFIG_LOCK_SERVO_FREQ, 14);
#if CONFIG_LOCK_SERVO_PWR_GPIO >= 0
    // A pending release of the previous move must not cut the power of this
    // one: stop it first (lock_esp_servo_release_after re-arms it).
    if (s_release_timer != NULL) {
        esp_timer_stop(s_release_timer);
    }
    if (!s_pwr_on) {
        servo_power(true);
#if CONFIG_LOCK_SERVO_PWR_LEAD_MS > 0
        vTaskDelay(pdMS_TO_TICKS(CONFIG_LOCK_SERVO_PWR_LEAD_MS) ? pdMS_TO_TICKS(CONFIG_LOCK_SERVO_PWR_LEAD_MS) : 1);
#endif
    }
#endif
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

void lock_esp_servo_release_after(uint32_t move_ms)
{
    if (s_release_timer == NULL) {
        return;
    }
    esp_timer_stop(s_release_timer); // restartable: engage→release→engage
    esp_timer_start_once(s_release_timer, (uint64_t)move_ms * 1000);
}
#endif
