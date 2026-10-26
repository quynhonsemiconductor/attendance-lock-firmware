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

// Streams a response body to completion via read_fn (returns bytes read,
// 0 = EOF, < 0 = error). Fills resp (NUL-terminated, ≤ cap-1 bytes).
// Returns false when the body does not fit cap-1 (more data followed) or on
// a read error — an oversized body must never arrive as a truncated 200.
bool sync_read_body(int (*read_fn)(char *buf, int len, void *ctx), void *ctx, char *resp,
                    size_t cap, size_t *len_out);

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
// carry the NEWEST event's snapshot — a live reading for the terminal list.
// Omitted when the batch is empty (no snapshot to speak of). Returns bytes
// written, or 0 when out is too small (nothing is half-written).
size_t sync_encode_envelope(const uint8_t stream[16], uint32_t dropped,
                            const store_event_t *events, size_t n_events, char *out, size_t cap);

#ifdef __cplusplus
}
#endif
