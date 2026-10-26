// Host-safe battery math. The ADC read + calibration is in power_esp.c.
#include "power/power_calc.h"

uint32_t power_battery_from_raw(uint32_t raw_mv, uint32_t div_mul, uint32_t div_div)
{
    if (div_div == 0) {
        return 0;
    }
    return raw_mv / div_div * div_mul + raw_mv % div_div * div_mul / div_div;
}

bool power_is_low(uint32_t battery_mv, uint32_t low_threshold_mv)
{
    return battery_mv != 0 && battery_mv < low_threshold_mv;
}

bool power_should_deny(uint32_t battery_mv, uint32_t cutoff_mv, uint32_t margin_mv)
{
    return battery_mv != 0 && battery_mv < cutoff_mv + margin_mv;
}

power_brownout_t power_brownout_on_reset(const power_brownout_t *prev, bool was_brownout)
{
    if (!was_brownout) {
        return (power_brownout_t){ 0, 0 }; // clean boot: the rail held
    }
    power_brownout_t next = {
        .count = prev->count + 1,
        // 30 s doubling to a 16-minute cap: repeated brownouts mean the
        // battery (or the wiring) cannot take the release surge — give it
        // an ever-longer quiet window instead of a reset loop.
        .backoff_ms = prev->backoff_ms == 0 ? 30000u : prev->backoff_ms * 2,
    };
    if (next.backoff_ms > 16u * 60u * 1000u) {
        next.backoff_ms = 16u * 60u * 1000u;
    }
    return next;
}

bool power_brownout_in_window(uint32_t boot_ms, uint32_t backoff_ms)
{
    return boot_ms < backoff_ms;
}
