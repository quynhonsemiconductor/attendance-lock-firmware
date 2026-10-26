#pragma once
// Fail-secure lock output (ADR §2.8): boots locked, releases only on a
// decided allow, re-engages on its own. Two backends behind one interface,
// selected by Kconfig:
//   LOCK_BACKEND_RELAY — GPIO to a relay/strike driver (active level, hold)
//   LOCK_BACKEND_SERVO — MG996-class servo on LEDC 50 Hz (rack-and-pinion
//   bolt): lock/unlock pulse widths in µs, move time, hold-vs-release after
//   the move (the release option saves battery and stops the hum; a bolt
//   that can be pushed back needs the held pulse).

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Pulse (µs) → LEDC duty ticks at freq_hz/resolution bits. Rounds to
// nearest; clamped to [0, max_ticks]. Host-testable pulse math (the
// prototype's `us * ((1 << RES) - 1) / (1000000 / FREQ)`).
uint32_t lock_servo_duty_ticks(uint32_t pulse_us, uint32_t freq_hz, uint32_t resolution_bits);

// Brings the output up and ENGAGES (fail-secure boot state, §2.8).
void lock_init(void);

// Drives to the locked position/level.
void lock_engage(void);

// Drives to the released position/level. The caller (door task) re-engages
// via lock_engage() after DOOR_UNLOCK_HOLD_MS — no blocking here.
void lock_release(void);

#ifdef __cplusplus
}
#endif
