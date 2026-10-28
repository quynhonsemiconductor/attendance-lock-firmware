// Tap acceptance logic (host-safe): per-card debounce + cross-read
// suppression. State is a small fixed table the caller owns.
#include "reader/reader.h"

#include <string.h>

#define READER_MAX_TRACKED 8

typedef struct {
    uint8_t reader;
    uint8_t uid_len;
    uint8_t uid[10];
    uint32_t last_ms;
    uint8_t used;
} reader_seen_t;

static reader_seen_t *seen_table(uint8_t *state)
{
    return (reader_seen_t *)state;
}

size_t reader_state_size(void)
{
    return sizeof(reader_seen_t) * READER_MAX_TRACKED;
}

bool reader_accept(uint8_t *state, size_t state_cap, const reader_card_t *card, uint32_t now_ms,
                   uint32_t debounce_ms, uint32_t cross_read_ms)
{
    if (state == NULL || card == NULL || state_cap < sizeof(reader_seen_t) * READER_MAX_TRACKED) {
        return false;
    }
    reader_seen_t *seen = seen_table(state);
    int slot = -1;
    for (int i = 0; i < READER_MAX_TRACKED; i++) {
        if (!seen[i].used) {
            if (slot < 0) {
                slot = i;
            }
            continue;
        }
        if (seen[i].uid_len == card->uid_len && seen[i].reader == card->reader &&
            memcmp(seen[i].uid, card->uid, card->uid_len) == 0) {
            // Same card, same reader: a held card re-polled.
            uint32_t age = now_ms - seen[i].last_ms;
            seen[i].last_ms = now_ms;
            if (age < debounce_ms) {
                return false;
            }
            return true;
        }
        if (seen[i].uid_len == card->uid_len && seen[i].reader != card->reader &&
            memcmp(seen[i].uid, card->uid, card->uid_len) == 0) {
            // Same card, OTHER reader: one pass past two antennas —
            // suppress the second read inside the cross-read window.
            uint32_t age = now_ms - seen[i].last_ms;
            if (age < cross_read_ms) {
                // Update the original so a held card does not re-fire there.
                seen[i].last_ms = now_ms;
                return false;
            }
        }
    }
    if (slot < 0) {
        // Table full: evict the least recently seen entry, not a fixed
        // slot — overwriting slot 0 would drop a card that may still be
        // held (and re-fire it on the next poll).
        slot = 0;
        for (int i = 1; i < READER_MAX_TRACKED; i++) {
            if ((uint32_t)(now_ms - seen[i].last_ms) > (uint32_t)(now_ms - seen[slot].last_ms)) {
                slot = i;
            }
        }
    }
    seen[slot].used = 1;
    seen[slot].reader = card->reader;
    seen[slot].uid_len = card->uid_len;
    memcpy(seen[slot].uid, card->uid, card->uid_len);
    seen[slot].last_ms = now_ms;
    return true;
}
