#pragma once
// Serial provisioning (§2.8): set base URL + token, print status. NEVER reads the token back; no unlock, card or test-output commands.
// Scaffold stub: the contract below is what firmware v1 implements (work item 7).

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Register the provisioning console on the serial line (inside the enclosure).
void provision_init(void);

// True when a base URL and token are stored (encrypted NVS, prod profile).
bool provisioned(void);

