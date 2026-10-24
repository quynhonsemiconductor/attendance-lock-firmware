#pragma once
// Card readers (§2.9: OSDP/SC preferred, Wiegand+tamper acceptable).
// Scaffold stub: the contract below is what firmware v1 implements (work item 7).

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum { READER_LANE_IN, READER_LANE_OUT } reader_lane_t;

// Bring up both readers (in/out). Non-fatal on absence: a missing reader logs and disables its lane.
void reader_init(void);

// Poll both readers. Returns true when a card was read; fills uid_out (hex, NUL-terminated) and lane_out. Non-blocking.
bool reader_poll(char *uid_out, size_t uid_len, reader_lane_t *lane_out);

