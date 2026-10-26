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

const char *door_decision_name(door_decision_t d);

#ifdef __cplusplus
}
#endif
