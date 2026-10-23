#pragma once
// Lock output (§2.8/§2.9: fail-secure electric strike; free egress is mechanical).
// Scaffold stub: the contract below is what firmware v1 implements (work item 7).

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Configure the strike GPIO. The boot state is LOCKED and every error path re-locks (fail-secure, boots locked).
void lock_init(void);

// Engage the strike (locked). Blocking for the pulse duration the hardware needs; nothing else.
void lock_engage(void);

// Release the strike for at most ms milliseconds, then re-engage. There is NO permanent-open state in production firmware.
void lock_release_for(uint32_t ms);

