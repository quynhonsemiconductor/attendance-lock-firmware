// Host-safe conversion + threshold logic. The ADC read is in power_esp.c.
#include "power/power_calc.h"

// Raw ADC reading (0..4095 at the configured attenuation) → battery mV at
// the pack, through the Kconfig divider (DIV_MUL/DIV_DIV: mv_pin × MUL/DIV).
uint32_t power_battery_from_raw(uint32_t raw_mv, uint32_t div_mul, uint32_t div_div)
{
    if (div_div == 0) {
        return 0;
    }
    return raw_mv / div_div * div_mul + raw_mv % div_div * div_mul / div_div;
}

bool power_low_battery(uint32_t battery_mv, uint32_t low_threshold_mv)
{
    return battery_mv != 0 && battery_mv < low_threshold_mv;
}
