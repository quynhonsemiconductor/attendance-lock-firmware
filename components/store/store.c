// store: scaffold stub - compiles, constants are the ADR sizing (§2.8).
// Firmware v1 replaces the bodies with the encrypted-NVS implementation.
#include "store/store.h"

// ≥ 90 days of one door (~10-20k events, §2.8): the partition is sized for
// this; the stub reports the DESIGN capacity, not a runtime-detected one.
#define STORE_RING_CAPACITY 20480

size_t store_ring_capacity(void) {
    return STORE_RING_CAPACITY;
}

bool store_allow_list_replace(const char *body, size_t len) {
    (void)body; (void)len;
    return false; // scaffold stub
}

bool store_event_push(const uint8_t *event, size_t len) {
    (void)event; (void)len;
    return false; // scaffold stub
}

size_t store_event_batch(uint8_t *out, size_t max_events) {
    (void)out; (void)max_events;
    return 0; // scaffold stub
}
