#pragma once
// Platform port for the sync component: credentials, radio, HTTP. The
// session logic (sync.c) is platform-free and host-tested against a
// scripted transport.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sync/sync.h" // sync_fail_t / sync_stage_t (the session summary)

// Provisioned credentials (base URL without trailing slash, bearer token).
// False when the device is not yet provisioned — the session skips.
bool sync_port_creds(char *url, size_t url_cap, char *token, size_t token_cap);

// Radio: bring the station up (blocks until it has an IP or fails) and take
// it back down. False = no network this session (normal: the door works
// offline, §2.1). On failure fail->stage names the step: SYNC_STAGE_WIFI
// (no connection / no credentials) or SYNC_STAGE_IP (associated, no IP).
bool sync_port_wifi_up(sync_fail_t *fail);
void sync_port_wifi_down(void);

// Live battery reading for the session envelope (sent EVERY session, empty
// ring included — the server's terminal TTL and battery badge key on it).
// *battery_mv = 0 when no reading is available (unknown is reportable too).
// *fault = true when the reading exists but is outside the plausibility
// window (divider wiring): the envelope then omits battery_mv and sends
// battery_fault instead.
void sync_port_battery(uint16_t *battery_mv, bool *low_battery, bool *fault);

// One HTTPS exchange against <url><path>. Headers sent: Authorization
// (Bearer token), User-Agent (qnsc-door-terminal/<version>),
// If-None-Match when if_none_match[0] != '\0', Content-Type for a body.
// Response: status, body (up to cap), ETag and Date response headers.
// TLS: esp_crt_bundle on the device; false = transport failure (no HTTP
// status), never a 4xx/5xx. On failure fail->stage names the step
// (SYNC_STAGE_DNS for resolve/connect, SYNC_STAGE_TLS for the handshake or
// certificate, SYNC_STAGE_CONFIG for a local setup error) and fail->code
// carries the platform error.
bool sync_port_http(const char *method, const char *url, const char *path, const char *token,
                    const char *if_none_match, const char *body, size_t body_len,
                    long *status, char *resp, size_t cap, size_t *resp_len, char *etag,
                    size_t etag_cap, char *date, size_t date_cap, sync_fail_t *fail);

// 32 random bits (backoff jitter).
uint32_t sync_port_random_u32(void);

// Sleeps ms milliseconds (between sessions — never called with the radio up
// and the door task waiting on us).
void sync_port_delay_ms(uint32_t ms);

// Spawn the session task (device build; host tests call sync_session_run
// directly). The task runs one bounded session per Kconfig interval.
void sync_port_task_start(void);
