// Host-safe deep-sleep decisions (battery work 2026-11-03).
#include "sleep/sleep.h"

sleep_verdict_t sleep_decide(const sleep_inputs_t *in)
{
    if (!in->door_idle) {
        return SLEEP_STAY_DOOR;
    }
    if (in->sync_busy) {
        return SLEEP_STAY_SYNC;
    }
    if (in->pir_high) {
        return SLEEP_STAY_PIR;
    }
    // Unsigned subtraction: the ms clock wraps every ~49 days.
    if (in->motion_seen && (uint32_t)(in->now_ms - in->last_motion_ms) < in->awake_ms) {
        return SLEEP_STAY_MOTION;
    }
    if (in->now_ms < in->boot_hold_ms) {
        return SLEEP_STAY_CONSOLE; // uptime starts at 0 every boot
    }
    if (in->console_seen && (uint32_t)(in->now_ms - in->console_last_ms) < in->console_hold_ms) {
        return SLEEP_STAY_CONSOLE;
    }
    return SLEEP_GO;
}

const char *sleep_verdict_name(sleep_verdict_t v)
{
    switch (v) {
    case SLEEP_GO:           return "go";
    case SLEEP_STAY_DOOR:    return "door";
    case SLEEP_STAY_SYNC:    return "sync";
    case SLEEP_STAY_PIR:     return "pir";
    case SLEEP_STAY_MOTION:  return "motion";
    case SLEEP_STAY_CONSOLE: return "console";
    }
    return "?";
}

uint64_t sleep_fallback_timer_us(uint64_t timer_us, bool degraded, uint64_t fallback_us)
{
    if (degraded && (timer_us == 0 || timer_us > fallback_us)) {
        return fallback_us;
    }
    return timer_us;
}

uint32_t sleep_boot_hold_ms(bool cold, bool degraded_wake, uint32_t cold_hold_ms,
                            uint32_t poll_window_ms)
{
    if (cold) {
        return cold_hold_ms;
    }
    return degraded_wake ? poll_window_ms : 0;
}

uint64_t sleep_timer_us(uint64_t now_us, uint64_t due_us, uint64_t min_us, uint64_t max_us)
{
    uint64_t d = due_us > now_us ? due_us - now_us : 0;
    if (d < min_us) {
        d = min_us;
    }
    if (d > max_us) {
        d = max_us;
    }
    return d;
}
