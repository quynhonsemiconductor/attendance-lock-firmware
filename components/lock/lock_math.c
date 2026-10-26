// Pure pulse math (host-tested): µs pulse → LEDC duty ticks.
#include "lock/lock_math.h"

#include <stdint.h>

uint32_t lock_servo_duty_ticks(uint32_t pulse_us, uint32_t freq_hz, uint32_t resolution_bits)
{
    uint32_t max_ticks = resolution_bits >= 32 ? UINT32_MAX : ((uint32_t)1 << resolution_bits) - 1;
    uint32_t period_us = 1000000u / freq_hz;
    // Round to nearest tick; clamp — an out-of-range pulse would drive the
    // servo past its mechanical stop.
    uint64_t ticks = ((uint64_t)pulse_us * max_ticks + period_us / 2) / period_us;
    return ticks > max_ticks ? max_ticks : (uint32_t)ticks;
}
