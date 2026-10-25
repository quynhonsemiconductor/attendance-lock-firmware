// Device wiring: relay GPIO or servo on LEDC. Build only on ESP-IDF.
#include "lock_esp.h"

#include <stdbool.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "driver/ledc.h"

#include "sdkconfig.h"

static const char *TAG = "lock";
static esp_timer_handle_t s_release_timer;

#if CONFIG_LOCK_BACKEND == LOCK_BACKEND_SERVO
static void release_cb(void *arg)
{
    (void)arg;
    // No pulse = no holding current (and no hum). The bolt position is
    // mechanical; a bolt that can be pushed back sets LOCK_SERVO_RELEASE=n.
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}
#endif

void lock_esp_init(void)
{
#if CONFIG_LOCK_BACKEND == LOCK_BACKEND_SERVO
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

void lock_esp_relay_write(int level)
{
    gpio_set_level(CONFIG_LOCK_RELAY_GPIO,
                   level ? (CONFIG_LOCK_RELAY_ACTIVE ? 1 : 0)
                         : (CONFIG_LOCK_RELAY_ACTIVE ? 0 : 1));
}

void lock_esp_servo_write(uint32_t pulse_us)
{
    // 50 Hz servo on a 14-bit LEDC: duty = pulse/period * max_ticks.
    const uint32_t resolution = 14;
    uint32_t max_ticks = (1u << resolution) - 1;
    uint64_t duty =
        ((uint64_t)pulse_us * max_ticks + (1000000u / CONFIG_LOCK_SERVO_FREQ) / 2) /
        (1000000u / CONFIG_LOCK_SERVO_FREQ);
    if (duty > max_ticks) {
        duty = max_ticks;
    }
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, (uint32_t)duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
    if (s_release_timer != NULL) {
        esp_timer_stop(s_release_timer); // restartable: engage→release→engage
        esp_timer_start_once(s_release_timer, (uint64_t)move_ms * 1000);
    }
}
