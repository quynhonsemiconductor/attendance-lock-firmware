#pragma once
// Encrypted-NVS allow-list cache and the event ring (§2.8).
// Scaffold stub: the contract below is what firmware v1 implements (work item 7).

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Maximum number of events the ring partition holds. Constant after boot.
size_t store_ring_capacity(void);

// Atomically replace the cached allow-list from a 200 response body. Old list stays readable on failure.
bool store_allow_list_replace(const char *body, size_t len);

// Append one event to the ring. Returns false when FULL - the caller drops and counts (§2.4); the door is never blocked on the buffer.
bool store_event_push(const uint8_t *event, size_t len);

// Pop up to max_events buffered events for upload (oldest first). Persisted removal happens on sync ack.
size_t store_event_batch(uint8_t *out, size_t max_events);

