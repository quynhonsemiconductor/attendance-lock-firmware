#pragma once
// Pure lock math, host-tested (no Kconfig, no device headers).
#include <stdbool.h>
#include <stdint.h>

uint32_t lock_servo_duty_ticks(uint32_t pulse_us, uint32_t freq_hz, uint32_t resolution_bits);

// No release pending (a move cleared it, or the release already ran).
#define LOCK_NO_RELEASE INT64_MAX

// Servo release callback guard (lock_esp.c): act only once the deadline the
// LATEST move recorded has passed. A stale callback — dispatched for an
// older move before esp_timer_stop could recall it — finds no deadline
// (cleared by the new move) or a future one (re-armed), and does nothing.
bool lock_release_due(int64_t now_us, int64_t release_at_us);
