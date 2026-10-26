// ADC read: GPIO1 behind a divider on the pack (battery decisions
// 2026-10-25). One-shot reads, averaged over four samples.
#include "power/power.h"

#include <string.h>

#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"

#include "power/power_calc.h"
#include "sdkconfig.h"

static const char *TAG = "power";
static adc_oneshot_unit_handle_t s_adc;
static bool s_ok;

bool power_init(void)
{
#if !CONFIG_POWER_BATTERY_MONITOR
    return false;
#else
    adc_oneshot_unit_init_cfg_t init = {
        .unit_id = ADC_UNIT_1,
    };
    if (adc_oneshot_new_unit(&init, &s_adc) != ESP_OK) {
        ESP_LOGE(TAG, "adc init failed");
        return false;
    }
    adc_oneshot_chan_cfg_t ch = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    if (adc_oneshot_config_channel(s_adc, CONFIG_POWER_BATTERY_ADC_CHANNEL, &ch) != ESP_OK) {
        ESP_LOGE(TAG, "adc channel config failed");
        return false;
    }
    s_ok = true;
    return true;
#endif
}

uint32_t power_battery_mv(void)
{
    if (!s_ok) {
        return 0;
    }
    uint32_t sum = 0;
    for (int i = 0; i < 4; i++) {
        int raw = 0;
        if (adc_oneshot_read(s_adc, CONFIG_POWER_BATTERY_ADC_CHANNEL, &raw) != ESP_OK) {
            return 0;
        }
        sum += (uint32_t)raw;
    }
    uint32_t raw = sum / 4;
    // 12 dB attenuation ≈ 1 mV/LSB at the pin (calibrated at bench bring-up);
    // the divider scales pin → pack.
    return power_battery_from_raw(raw, CONFIG_POWER_BATTERY_DIV_MUL,
                                  CONFIG_POWER_BATTERY_DIV_DIV);
}

bool power_low_battery(void)
{
    return power_low_battery(power_battery_mv(), CONFIG_POWER_BATTERY_LOW_MV);
}
