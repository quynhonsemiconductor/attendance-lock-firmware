#pragma once
// The door FSM (ADR §2.4, work item 7): boots locked, taps decide locally,
// an allow releases for the hold window then re-engages, a re-tap during
// the window extends it. Host-safe — the door task (main.c) owns the
// queue, GPIO, lock and power glue; this file knows only state and time.
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "store/store.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DOOR_ALLOW = 0,
    DOOR_DENY_UNKNOWN,  // uid not on the list
    DOOR_DENY_WINDOW,   // window exists and is not currently open
    DOOR_DENY_NOCLOCK,  // windowed card while the clock never synced (§2.4)
    DOOR_DENY_BATTERY,  // below cutoff+margin, or the brownout backoff window
    DOOR_DENY_FAULT,    // a reader fault — no decision could be made
} door_decision_t;

// What the caller must drive after an input or a tick, right now.
typedef struct {
    bool engage;               // drive the lock to engaged
    bool release;              // drive the lock to released
    bool green, red, buzzer;   // feedback levels
} door_out_t;

typedef struct {
    bool released;
    uint32_t released_at_ms;   // re-tap during the window moves this forward
    uint32_t hold_ms;
    uint32_t red_until_ms;     // red outlives the deny by 2 s
    uint8_t beeps_left;        // 1 = allow (single), 2 = deny (double)
    bool beep_on;
    uint32_t beep_change_ms;   // when the buzzer level flips next
} door_fsm_t;

// Decision for one tap:
// - uid compared byte-wise against the cached cards
// - no window (both 0): permanent card — allowed in every clock state
// - any window set while the clock never synced: DENY (temporary cards fail
//   closed offline, §2.4)
// - windowed card with a synced clock: allowed only inside the window
// (valid_from == 0 = open from forever; valid_until == 0 = never expires)
door_decision_t door_decide(const store_card_t *cards, size_t n_cards, bool clock_synced,
                            uint64_t now_epoch, const uint8_t *uid, uint8_t uid_len);

void door_fsm_init(door_fsm_t *fsm, uint32_t hold_ms);

// Applies a decision at now_ms. DOOR_ALLOW releases (or extends an open
// window); every deny keeps the door locked and shows its feedback.
door_out_t door_fsm_apply(door_fsm_t *fsm, uint32_t now_ms, door_decision_t decision);

// A reader went unhealthy: the door stays exactly as it is (a fault during
// an open window does not slam the door) and the red LED marks it.
door_out_t door_fsm_fault(door_fsm_t *fsm, uint32_t now_ms);

// Time passes: relock after the hold window, retire feedback. Call at the
// door task's cadence.
door_out_t door_fsm_tick(door_fsm_t *fsm, uint32_t now_ms);

bool door_fsm_released(const door_fsm_t *fsm);

// Nothing left to drive at now_ms: locked, no beep running or pending, no
// red feedback. The deep-sleep gate (main.c) only sleeps when this holds
// (battery work 2026-11-03): sleeping mid-pattern would freeze the buzzer
// or cut the deny feedback short.
bool door_fsm_idle(const door_fsm_t *fsm, uint32_t now_ms);

const char *door_decision_name(door_decision_t d);

// --- exit button (KEY1 on GPIO6, active low; 2026-11-03) ---------------
// Debounce for the inside exit button, sampled at the door task's 50 ms
// cadence. A press opens like an allowed tap (door_fsm_apply(DOOR_ALLOW)),
// once per press: holding the button does not keep the door open.
typedef struct {
    bool raw;              // last sampled level (true = pressed)
    uint32_t raw_since_ms; // when raw last changed
    bool down;             // debounced level
} door_exit_t;

void door_exit_init(door_exit_t *b);

// Feeds one sample; true exactly once per debounced press (the edge).
bool door_exit_update(door_exit_t *b, bool pressed, uint32_t now_ms, uint32_t debounce_ms);

// Pressed (raw) for at least stuck_ms: a jammed or shorted button. The
// deep-sleep gate ignores a stuck button and does not arm its wake —
// otherwise it would keep the chip awake (or re-wake it) until the pack dies.
bool door_exit_stuck(const door_exit_t *b, uint32_t now_ms, uint32_t stuck_ms);

#ifdef __cplusplus
}
#endif
