#pragma once
// Host-testable conversion + threshold + brownout logic (power.h is the
// device API).
#include <stdbool.h>
#include <stdint.h>

// Pin mV (calibrated ADC reading) → pack mV through the divider
// (mv_pin × MUL/DIV).
uint32_t power_battery_from_raw(uint32_t raw_mv, uint32_t div_mul, uint32_t div_div);

// Report-only alert (contract: low_battery at ~5.0 V). 0 mV = unknown —
// never alerts.
bool power_is_low(uint32_t battery_mv, uint32_t low_threshold_mv);

// Release gate: deny when a reading exists and it sits below the effective
// cutoff plus a safety margin (the servo surge would brown the rail out).
bool power_should_deny(uint32_t battery_mv, uint32_t cutoff_mv, uint32_t margin_mv);

// Brownout backoff (host-safe state machine; the port feeds the reset
// reason): each consecutive brownout reset doubles the post-boot window in
// which a release is refused; a clean power-on clears it.
typedef struct {
    uint32_t count;    // consecutive brownout resets
    uint32_t backoff_ms; // release refused this long after boot
} power_brownout_t;

power_brownout_t power_brownout_on_reset(const power_brownout_t *prev, bool was_brownout);

bool power_brownout_in_window(uint32_t boot_ms, uint32_t backoff_ms);
