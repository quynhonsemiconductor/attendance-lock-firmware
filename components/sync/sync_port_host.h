#pragma once
// Test-only hooks for the sync host port.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sync/sync.h" // sync_stage_t (scripted failure stages)

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

// Frozen copy of the last GET (the drain loop's POSTs overwrite the journal
// above — the If-None-Match asserts read this one).
extern sync_port_req_t sync_host_last_get;

// Frozen copy of the FIRST POST (the drain's later envelopes overwrite the
// journal; the dropped counter rides the first one).
extern sync_port_req_t sync_host_first_post;

// POSTs issued since the last reset (the drain loop tests assert on it).
extern int sync_host_post_count;

// Clear the script + journal; provisioned = true, radio down, battery
// 5400 mV / not low.
void sync_host_reset(void);

// Queue one response (consumed in order; an exhausted script fails the
// exchange like a transport error would).
void sync_host_enqueue(long status, const char *body, const char *etag, const char *date);

// Simulate an unprovisioned device.
void sync_host_set_provisioned(bool on);

// Script the battery reading the next session reports in its envelope.
void sync_host_set_battery(uint16_t battery_mv, bool low_battery);

// Script the battery sensor-fault flag (envelope: battery_fault, no
// battery_mv).
void sync_host_set_battery_fault(bool fault);

// Script a radio failure: the next sync_port_wifi_up reports this stage
// (SYNC_STAGE_WIFI or SYNC_STAGE_IP).
void sync_host_fail_wifi(sync_stage_t stage);

// Stage + code a failed exchange reports (default: SYNC_STAGE_DNS, code 0 —
// what an exhausted script models on the bench: resolve/connect failed).
void sync_host_fail_http(sync_stage_t stage, int code);

// Script the reader misreads the next session takes and reports.
void sync_host_set_misreads(uint32_t misreads);
