#pragma once
// Pure lock math, host-tested (no Kconfig, no device headers).
#include <stdint.h>

uint32_t lock_servo_duty_ticks(uint32_t pulse_us, uint32_t freq_hz, uint32_t resolution_bits);
