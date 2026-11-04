#pragma once
// Deep sleep between PIR wakes and sync sessions (battery work 2026-11-03).
//
// Logic (sleep_plan.c, host-tested): may the chip sleep now, and for how
// long. Device side (sleep_esp.c): what woke us, the "slept with the bolt
// engaged" flag in RTC memory, pin holds and the deep-sleep entry.
//
// Pins held through deep sleep: RC522 RST low (hard power-down), RC522 CS
// high, servo signal / feedback low (a floating line can twitch the servo
// or half-drive the buzzer). They are released at the next boot before any
// driver configures them.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// --- logic -------------------------------------------------------------

typedef enum {
    SLEEP_GO = 0,       // nothing keeps the chip awake
    SLEEP_STAY_DOOR,    // released, servo moving, feedback or a queued tap
    SLEEP_STAY_SYNC,    // a session runs or is due
    SLEEP_STAY_PIR,     // a PIR is high right now
    SLEEP_STAY_MOTION,  // motion within awake_ms (person finding the card)
    SLEEP_STAY_CONSOLE, // power-on window or recent console input
} sleep_verdict_t;

typedef struct {
    uint32_t now_ms;          // uptime ms since this boot/wake
    bool door_idle;           // door_fsm_idle + servo settled + no queued tap
    bool sync_busy;
    bool pir_high;
    bool motion_seen;
    uint32_t last_motion_ms;
    uint32_t awake_ms;        // stay this long after the last motion
    uint32_t boot_hold_ms;    // 0 after a deep-sleep wake
    bool console_seen;        // any console input since boot
    uint32_t console_last_ms;
    uint32_t console_hold_ms;
} sleep_inputs_t;

// First reason to stay awake, in the order above, or SLEEP_GO.
sleep_verdict_t sleep_decide(const sleep_inputs_t *in);

const char *sleep_verdict_name(sleep_verdict_t v);

// Timer wake duration: until due_us (RTC µs), clamped to [min_us, max_us].
// A due time already past gives min_us (wake again almost at once). With
// min_us > max_us, max_us wins (the cap is applied last).
uint64_t sleep_timer_us(uint64_t now_us, uint64_t due_us, uint64_t min_us, uint64_t max_us);

// --- device ------------------------------------------------------------

typedef enum {
    SLEEP_WAKE_COLD = 0, // power-on, reset button, crash/watchdog restart
    SLEEP_WAKE_PIR,      // ext1: a PIR went high
    SLEEP_WAKE_TIMER,    // RTC timer: sync due
    SLEEP_WAKE_EXIT,     // ext0: the exit button pulled its pin low
    SLEEP_WAKE_OTHER,    // deep sleep, unexpected cause
} sleep_wake_t;

// Call FIRST in app_main: reads the wake cause, releases the pin holds of
// the previous sleep (each pin first driven to its held level, so nothing
// glitches) and logs the wake. Idempotent.
sleep_wake_t sleep_boot(void);

// True when this boot is a deep-sleep wake AND the chip went to sleep with
// the bolt engaged and settled: the lock must not be driven again.
bool sleep_resumed_locked(void);

// Enters deep sleep, never returns. Pins in hold_low / hold_high are driven
// and held through sleep (negative entries are skipped). wake_mask: RTC
// GPIOs that wake on a high level (ext1, 0 = none); exit_gpio: one RTC GPIO
// that wakes on a LOW level (ext0, the active-low exit button; -1 = none);
// timer_us: 0 = no timer.
// bolt_engaged: the bolt is engaged and settled. Only then does the next
// wake skip the boot engage; false makes that wake engage (fail-secure).
void sleep_enter(bool bolt_engaged, uint64_t wake_mask, int exit_gpio, uint64_t timer_us,
                 const int *hold_low,
                 size_t n_low,
                 const int *hold_high, size_t n_high) __attribute__((__noreturn__));

#ifdef __cplusplus
}
#endif
