#pragma once
// Test-only hooks for the sync host port.

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    long status;
    const char *body;
    const char *etag;
    const char *date;
} sync_port_resp_t;

typedef struct {
    char method[8];
    char url[128];
    char path[64];
    char token[128];
    char if_none_match[80];
    char body[16384]; // the POST envelope is up to ~12 KB
} sync_port_req_t;

extern sync_port_req_t sync_host_last_request;

// Clear the script + journal; provisioned = true, radio down.
void sync_host_reset(void);

// Queue one response (consumed in order; an exhausted script fails the
// exchange like a transport error would).
void sync_host_enqueue(long status, const char *body, const char *etag, const char *date);

// Simulate an unprovisioned device.
void sync_host_set_provisioned(bool on);
