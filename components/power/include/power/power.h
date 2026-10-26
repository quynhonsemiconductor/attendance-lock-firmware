#pragma once
// Battery monitor (battery decisions 2026-10-25): calibrated ADC behind a
// divider → battery_mv; a low-battery flag at ~5.0 V (Kconfig) against the
// ~4.8 V effective cutoff, a release gate just above the cutoff (the servo
// surge browns out an already-sagging pack), and a brownout-reset counter
// with a doubling backoff. None of it ever unlocks (ADR §2.8).
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ADC/calibration setup + brownout bookkeeping. Returns false when the
// monitor is disabled in Kconfig or the ADC fails — the door works
// regardless (reads come back 0 mV = unknown).
bool power_init(void);

// Divider-corrected battery voltage in millivolts (0 when unavailable).
// Averages 8 samples taken with the servo idle.
uint32_t power_battery_mv(void);

// True when battery_mv is below the low-battery threshold (report only).
bool power_low_battery(void);

// Consecutive brownout resets seen since the last clean power-on.
uint32_t power_brownout_count(void);

// True during the post-brownout backoff window (boot_ms = ms since boot).
bool power_brownout_blocked(uint32_t boot_ms);

#ifdef __cplusplus
}
#endif
