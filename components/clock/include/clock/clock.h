#pragma once
// Time source (NTP + server Date fallback; same-boot rewrite, §2.8).
// Scaffold stub: the contract below is what firmware v1 implements (work item 7).

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Start SNTP; the server's Date header is the fallback when NTP is unreachable.
void clock_init(void);

// True when the current time is trusted (affects time_synced on events, §3).
bool clock_synced(void);

