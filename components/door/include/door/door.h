#pragma once
// The door decision (ADR §2.4): local, from the cached allow-list — the door
// works without opshub. Host-safe; the main loop owns GPIO/lock/store.
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
} door_decision_t;

// Decision for one tap:
// - uid compared byte-wise against the cached cards
// - no window (both 0): permanent card — allowed in every clock state
// - any window set while the clock never synced: DENY (temporary cards fail
//   closed offline, §2.4)
// - windowed card with a synced clock: allowed only inside the window
//   (valid_from == 0 = open from forever; valid_until == 0 = never expires)
door_decision_t door_decide(const store_card_t *cards, size_t n_cards, bool clock_synced,
                            uint64_t now_epoch, const uint8_t *uid, uint8_t uid_len);

#ifdef __cplusplus
}
#endif
