#pragma once
// Uplink to opshub: allow-list poll + event batches over the terminal API (ADR §2.4).
// Scaffold stub: the contract below is what firmware v1 implements (work item 7).

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Load provisioned base URL/token and start the 60 s poll loop (no push channel exists - §7).
void sync_init(void);

// One upload attempt for all buffered events; applies acked_through and re-queues rejected events. Returns overall success.
bool sync_uplink_once(void);

