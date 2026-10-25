#pragma once
// Battery monitor (battery decisions 2026-10-25): ADC behind a divider →
// battery_mv; a low-battery flag ~1–2 weeks before cutoff (threshold in
// Kconfig). It NEVER unlocks — the prototype's "low battery → stay open"
// behaviour is dropped (ADR §2.8).
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ADC/attenuation setup. Returns false when the monitor is disabled in
// Kconfig or the ADC fails — the door works regardless.
bool power_init(void);

// Divider-corrected battery voltage in millivolts (0 when unavailable).
uint32_t power_battery_mv(void);

// True when battery_mv has been below the Kconfig threshold.
bool power_low_battery(void);

#ifdef __cplusplus
}
#endif
