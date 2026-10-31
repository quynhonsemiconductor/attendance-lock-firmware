#pragma once
// Uplink to opshub: bounded sync sessions over the terminal API (§2.4).
// Deep-sleep design: a session connects, syncs, disconnects — no long-lived
// connections; everything that must survive sleep lives in RTC memory
// (clock base) or NVS (allow-list cache, ring, creds).
//
// The door task never calls into this: the sync task runs sessions on the
// configured interval and owns store batch/ack/dropped + allow-list replace.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "store/store.h"

#ifdef __cplusplus
extern "C" {
#endif

// Response buffer for the allow-list GET: worst case 256 cards of JSON
// (~21 KiB with 10-byte UIDs and both windows set). A body that does not
// fit is a transport failure, never a truncated 200 — the host tests pin
// the 256-card worst case against this cap.
#define SYNC_RESP_CAP (24 * 1024)

// The events envelope's dropped field is server-capped (0..100000): a larger
// local count is CLAMPED for reporting (the store keeps its true count).
#define SYNC_DROPPED_REPORT_MAX 100000u

// Per-session drain budget: the ring is uploaded in batches of
// STORE_BATCH_MAX until empty, but a session never spends longer than this
// on the drain (Wi-Fi session stays bounded; the next session continues).
#define SYNC_DRAIN_BUDGET_MS 60000u

// Event payload the reader task pushes into the ring (≤ STORE_EVENT_MAX);
// the sync component owns this wire-side layout.
typedef struct __attribute__((packed)) {
    uint8_t reader;       // 0 = in, 1 = out
    uint8_t uid_len;      // 4, 7 or 10 (only these encode)
    uint8_t uid[10];
    uint8_t result;       // 0 = allow, 1 = deny
    uint32_t boot_id;     // power segment the event belongs to (LE)
    uint64_t uptime_us;   // capture time (LE) — device_ts derived at upload
    uint16_t battery_mv;  // pack voltage at enqueue (0 = unknown; LE)
    uint8_t low_battery;  // 1 when battery_mv was under the alert threshold
} sync_event_t;

// Starts the sync task (device build): one bounded session per interval.
void sync_init(void);

// One bounded session: Wi-Fi up → SNTP window → allow-list poll (ETag/304)
// → event batch upload (ack/reject) → SNTP stop → Wi-Fi down. Returns true
// when both exchanges ended well (304 counts as good). Host tests drive this
// directly against a scripted transport.
bool sync_session_run(void);

// --- session outcome (the bench summary line) --------------------------

// Where a session stopped, in the order the session meets the stages
// (BENCH-TEST.md "Reading the sync log"). ESP-IDF reports DNS and TCP
// connect failures alike (ESP_ERR_HTTP_CONNECT), so SYNC_STAGE_DNS covers
// both: a typo'd `set url` is the bench case this exists for.
typedef enum {
    SYNC_STAGE_NONE = 0, // no failure
    SYNC_STAGE_CONFIG,   // unprovisioned or a local request-setup error
    SYNC_STAGE_WIFI,     // the station did not connect
    SYNC_STAGE_IP,       // associated but no IP (DHCP)
    SYNC_STAGE_SNTP,     // no time source at all (SNTP and Date failed)
    SYNC_STAGE_DNS,      // cannot resolve/connect
    SYNC_STAGE_TLS,      // TLS handshake or certificate
    SYNC_STAGE_HTTP,     // an HTTP status the session could not use
} sync_stage_t;

// A platform transport failure: the stage it stopped at + the platform code
// (esp_err_t / esp-tls), 0 when the platform has no better number.
typedef struct {
    sync_stage_t stage;
    int code;
} sync_fail_t;

// What happened in the last session. Rendered once per session at INFO so a
// bench operator can read the cadence and the first failure without
// cross-referencing the server log.
typedef struct {
    bool ok;
    sync_stage_t fail_stage; // SYNC_STAGE_NONE when ok
    long fail_http_status;   // SYNC_STAGE_HTTP only
    int fail_code;           // platform code behind fail_stage (0 = none)
    char fail_detail[96];    // short human explanation; "" when ok
    long allow_list_status;  // HTTP status of the GET; 0 = never got one
    uint32_t events_sent;    // distinct events put in an envelope
    uint32_t events_acked;   // events the server acked this session
    uint32_t misreads;       // UID reads the reader rejected since the last
                             // session (BCC / confirm-read): a bad reader
                             // is visible in the log and on the server
    bool time_unsynced;      // neither SNTP nor the Date header gave a time
} sync_session_result_t;

// Fills out with the result of the last sync_session_run() (the sync task
// owns the session; host tests call this right after driving one).
void sync_session_result(sync_session_result_t *out);

// Renders the one-line summary message (BENCH-TEST.md "Reading the sync
// log"), e.g. "session failed (http 401: terminal token refused: check
// `set token`) allow-list=401 events=0/0 misreads=0 next in 30 s (backoff
// 1/5)".
// The leading `sync: ` in the monitor comes from the ESP log tag, so this
// text is exactly the operator-facing line. consecutive_fails counts this
// session (0 on success); the display caps at the 5 backoff steps. Returns
// the message length, or 0 when out is too small.
size_t sync_format_summary(const sync_session_result_t *r, uint32_t consecutive_fails,
                           uint32_t next_delay_ms, char *out, size_t cap);

// Delay until the next session in ms: the configured interval (Kconfig,
// default 30 min) when clean, otherwise exponential backoff + jitter.
// Deep sleep replaces this delay once the main loop arms it (PR 4).
uint32_t sync_next_delay_ms(bool last_ok, uint32_t consecutive_fails);

// Encode one buffered event as the request's event object (testable slice
// of the envelope encoder). Emits compact JSON for
// {"seq":N,"reader":"in|out","uid":"HEX","result":"allow|deny",
//  "device_ts":"...Z","time_synced":bool}. Returns bytes written or 0.
size_t sync_encode_event(const sync_event_t *ev, uint32_t seq, char *out, size_t cap);

// --- JSON codec (exposed for the host tests; used by the session) ------

// Parses the allow-list body (fixture: allow-list.json). Unknown fields are
// skipped (§2.4: fields may be added). Returns the number of cards written
// to out (≤ max_cards), or SIZE_MAX when the body is malformed.
size_t sync_decode_allow_list(const char *body, size_t len, store_card_t *out,
                              size_t max_cards, bool *too_many);

// Parses the events response (fixture: events-response.json):
// {"acked_through":N,"rejected":[seq,…]}. rejected seqs land in out_rejected
// (≤ max_rejected); *rejected_len receives the full count even when
// truncated. acked_through is always set when the body parses.
bool sync_decode_events_response(const char *body, size_t len, uint32_t *acked_through,
                                 uint32_t *out_rejected, size_t max_rejected,
                                 size_t *rejected_len);

// Renders the POST /v1/door/terminal/events envelope
// (fixture: events-request.json) for the given stream + batch.
// battery_mv/low_battery are envelope-level (door-terminal-v1 contract) and
// emitted whenever a reading exists (battery_mv != 0): an empty batch is
// still a heartbeat that refreshes the server's terminal TTL and battery
// badge. With NO reading (battery_mv == 0) BOTH fields are omitted — the
// server must not read an absent pack as 0 mV (round-2 review item 1).
// battery_fault (round-3 bench finding: the divider floated at "34 V") OMITS
// both battery fields and sends `"battery_fault":true` instead: the server
// may ignore the flag, but it must never see an implausible voltage.
// misreads (reader UID integrity, bench 2026-10-31) is an OPTIONAL field:
// emitted only when non-zero, so a healthy session stays byte-identical to
// the contract fixture; a server may ignore it (unknown fields are skipped).
// dropped is clamped to SYNC_DROPPED_REPORT_MAX. Returns bytes written, or
// 0 when out is too small (nothing is half-written).
size_t sync_encode_envelope(const uint8_t stream[16], uint32_t dropped, uint32_t misreads,
                            uint16_t battery_mv, bool low_battery, bool battery_fault,
                            const store_event_t *events, size_t n_events, char *out, size_t cap);

// Bounded accumulator for a streamed response body (round-2 review item 4):
// the ESP port feeds it from HTTP_EVENT_ON_DATA. cap-1 usable bytes; data
// past the cap latches overflow — an oversized body must never arrive as a
// silently truncated 200. Host-tested.
typedef struct {
    char *buf;
    size_t cap;
    size_t len;
    bool overflow;
} sync_body_acc_t;

void sync_body_acc_init(sync_body_acc_t *acc, char *buf, size_t cap);
// Adds one chunk. Returns false once overflow is latched (keep draining,
// then fail the exchange); the chunk is dropped, never truncated in.
bool sync_body_acc_add(sync_body_acc_t *acc, const char *data, size_t n);

#ifdef __cplusplus
}
#endif
