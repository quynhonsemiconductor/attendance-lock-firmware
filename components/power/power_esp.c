// Device side: calibrated one-shot ADC + brownout counter in RTC memory.
// The reading is calibrated on-chip (curve fitting) so battery_mv is real
// millivolts at the pin, not raw counts — the divider math stays host-safe.
#include "power/power.h"
#include "power/power_calc.h"

#include "esp_attr.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_rom_sys.h"

#include "sdkconfig.h"

static const char *TAG = "power";

static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t s_cali;
static bool s_ok;

// Survives brownout resets (RTC slow memory); a clean power-on zeroes it.
static RTC_NOINIT_ATTR struct {
    uint32_t magic;
    power_brownout_t st;
} s_brownout;
#define BROWNOUT_MAGIC 0x42523155u // "BR1U"

bool power_init(void)
{
#if !CONFIG_POWER_BATTERY_MONITOR
    return false;
#else
    bool was_brownout = esp_reset_reason() == ESP_RST_BROWNOUT;
    if (s_brownout.magic == BROWNOUT_MAGIC) {
        s_brownout.st = power_brownout_on_reset(&s_brownout.st, was_brownout);
    } else {
        s_brownout.st = power_brownout_on_reset(
            &(power_brownout_t){ 0, 0 }, was_brownout); // first boot counts too
    }
    s_brownout.magic = BROWNOUT_MAGIC;
    if (s_brownout.st.count > 0) {
        ESP_LOGW(TAG, "brownout resets: %u, release blocked %u ms after boot",
                 (unsigned)s_brownout.st.count, (unsigned)s_brownout.st.backoff_ms);
    }

    adc_oneshot_unit_init_cfg_t init = {
        .unit_id = ADC_UNIT_1,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
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
    adc_cali_curve_fitting_config_t cali = {
        .unit_id = ADC_UNIT_1,
        .chan = (adc_channel_t)CONFIG_POWER_BATTERY_ADC_CHANNEL,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    if (adc_cali_create_scheme_curve_fitting(&cali, &s_cali) != ESP_OK) {
        ESP_LOGE(TAG, "adc calibration failed — reporting raw-scale mV");
        s_cali = NULL; // reads still work, accuracy falls back to 1 mV/LSB
    }
    s_ok = true;
    return true;
#endif
}

uint32_t power_battery_mv(void)
{
#if !CONFIG_POWER_BATTERY_MONITOR
    return 0;
#else
    if (!s_ok) {
        return 0;
    }
    // 8 samples, 1 ms apart: the servo must be idle here (the caller gates
    // that), the spread averages out the divider noise.
    uint32_t sum = 0;
    for (int i = 0; i < 8; i++) {
        int raw = 0;
        if (adc_oneshot_read(s_adc, CONFIG_POWER_BATTERY_ADC_CHANNEL, &raw) != ESP_OK) {
            return 0;
        }
        int mv = raw;
        if (s_cali != NULL && adc_cali_raw_to_voltage(s_cali, raw, &mv) != ESP_OK) {
            return 0;
        }
        sum += (uint32_t)mv;
        esp_rom_delay_us(1000);
    }
    return power_battery_from_raw(sum / 8, CONFIG_POWER_BATTERY_DIV_MUL,
                                  CONFIG_POWER_BATTERY_DIV_DIV);
#endif
}

bool power_low_battery(void)
{
    return power_is_low(power_battery_mv(), CONFIG_POWER_BATTERY_LOW_MV);
}

bool power_release_denied(void)
{
    return power_should_deny(power_battery_mv(), CONFIG_POWER_BATTERY_CUTOFF_MV,
                             CONFIG_POWER_BATTERY_DENY_MARGIN_MV);
}

uint32_t power_brownout_count(void)
{
    return s_brownout.magic == BROWNOUT_MAGIC ? s_brownout.st.count : 0;
}

bool power_brownout_blocked(uint32_t boot_ms)
{
    return s_brownout.magic == BROWNOUT_MAGIC &&
           power_brownout_in_window(boot_ms, s_brownout.st.backoff_ms);
}
