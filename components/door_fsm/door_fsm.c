#include "door_fsm/door_fsm.h"

#include <string.h>

#define DENY_RED_MS 2000u
#define BEEP_ON_MS 120u
#define BEEP_OFF_MS 120u

door_decision_t door_decide(const store_card_t *cards, size_t n_cards, bool clock_synced,
                            uint64_t now_epoch, const uint8_t *uid, uint8_t uid_len)
{
    const store_card_t *match = NULL;
    for (size_t i = 0; i < n_cards; i++) {
        if (cards[i].uid_len == uid_len && memcmp(cards[i].uid, uid, uid_len) == 0) {
            match = &cards[i];
            break;
        }
    }
    if (match == NULL) {
        return DOOR_DENY_UNKNOWN;
    }
    if (match->valid_from == 0 && match->valid_until == 0) {
        return DOOR_ALLOW; // permanent card: the no-clock rule keeps it working
    }
    if (!clock_synced) {
        return DOOR_DENY_NOCLOCK; // temporary cards fail closed offline (§2.4)
    }
    if (match->valid_from != 0 && now_epoch < (uint64_t)match->valid_from) {
        return DOOR_DENY_WINDOW; // not valid yet
    }
    if (match->valid_until != 0 && now_epoch > (uint64_t)match->valid_until) {
        return DOOR_DENY_WINDOW; // expired
    }
    return DOOR_ALLOW;
}

void door_fsm_init(door_fsm_t *fsm, uint32_t hold_ms)
{
    memset(fsm, 0, sizeof(*fsm));
    fsm->hold_ms = hold_ms;
}

static void start_beeps(door_fsm_t *fsm, uint32_t now_ms, uint8_t count)
{
    fsm->beeps_left = count;
    fsm->beep_on = true;
    fsm->beep_change_ms = now_ms + BEEP_ON_MS;
}

door_out_t door_fsm_apply(door_fsm_t *fsm, uint32_t now_ms, door_decision_t decision)
{
    door_out_t out = { 0 };
    if (decision == DOOR_ALLOW) {
        fsm->released = true;
        fsm->released_at_ms = now_ms; // a re-tap extends the window
        fsm->red_until_ms = 0;
        start_beeps(fsm, now_ms, 1);
        out.release = true;
        out.green = true;
        out.buzzer = true;
        return out;
    }
    // Every deny keeps the door where it is: locked stays locked, an open
    // window keeps running to its end (a deny never slams the door).
    // out.engage reflects the CURRENT state — it is false while released,
    // so the caller does not re-drive the lock onto a moving servo.
    fsm->red_until_ms = now_ms + DENY_RED_MS;
    start_beeps(fsm, now_ms, 2);
    out.engage = !fsm->released;
    out.release = fsm->released;
    out.green = fsm->released;
    out.red = true;
    out.buzzer = true;
    return out;
}

door_out_t door_fsm_fault(door_fsm_t *fsm, uint32_t now_ms)
{
    fsm->red_until_ms = now_ms + DENY_RED_MS;
    door_out_t out = {
        .engage = !fsm->released,
        .release = fsm->released,
        .green = fsm->released,
        .red = true,
    };
    return out;
}

door_out_t door_fsm_tick(door_fsm_t *fsm, uint32_t now_ms)
{
    if (fsm->released && now_ms - fsm->released_at_ms >= fsm->hold_ms) {
        fsm->released = false;
    }
    // Buzzer pattern: flips on the schedule until the beeps run out.
    if (fsm->beeps_left > 0 && fsm->beep_change_ms != 0 &&
        (int32_t)(now_ms - fsm->beep_change_ms) >= 0) {
        if (fsm->beep_on) {
            fsm->beep_on = false;
            fsm->beeps_left--;
            fsm->beep_change_ms = fsm->beeps_left > 0 ? now_ms + BEEP_OFF_MS : 0;
        } else {
            fsm->beep_on = true;
            fsm->beep_change_ms = now_ms + BEEP_ON_MS;
        }
    }
    door_out_t out = {
        .engage = !fsm->released,
        .release = fsm->released,
        .green = fsm->released,
        .red = fsm->red_until_ms != 0 && (int32_t)(fsm->red_until_ms - now_ms) > 0,
        .buzzer = fsm->beep_on,
    };
    if (!out.red) {
        fsm->red_until_ms = 0;
    }
    return out;
}

bool door_fsm_released(const door_fsm_t *fsm)
{
    return fsm->released;
}

const char *door_decision_name(door_decision_t d)
{
    switch (d) {
    case DOOR_ALLOW:        return "allow";
    case DOOR_DENY_UNKNOWN: return "unknown-card";
    case DOOR_DENY_WINDOW:  return "outside-window";
    case DOOR_DENY_NOCLOCK: return "no-clock";
    case DOOR_DENY_BATTERY: return "low-battery";
    case DOOR_DENY_FAULT:   return "reader-fault";
    }
    return "?";
}
