#include "door/door.h"

#include <string.h>

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
