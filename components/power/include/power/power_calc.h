#pragma once
// Host-testable conversion (power.h is the device API).
#include <stdbool.h>
#include <stdint.h>

uint32_t power_battery_from_raw(uint32_t raw_mv, uint32_t div_mul, uint32_t div_div);
bool power_low_battery(uint32_t battery_mv, uint32_t low_threshold_mv);
