#pragma once
// Allow-list cache (encrypted NVS) and the event ring (encrypted data
// partition, §2.8). Firmware v1: the real implementation.
//
// Concurrency: every public call takes the port's recursive lock, so the
// door task may push while the sync task batches/acks.
//
// Failure mode: every function keeps the previous state on failure and
// reports it through its return value; a broken store never blocks the door
// (§2.4) — the caller drops and counts. Sync surfaces persist failures as a
// status (revocations must not silently stop).

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Fixed payload size of one buffered event (the sync component's wire
// fields: seq/reader/uid/result/device_ts/time_synced fit well under this).
#define STORE_EVENT_MAX 52

// Server contract: batches of up to 100 events (§2.4).
#define STORE_BATCH_MAX 100

// Seq block reservation (§2.8): the block end is persisted before the block's
// first seq is handed out; a reboot continues at the next STORE_SEQ_BLOCK
// boundary. uint32 seq: 4.29e9 events ≈ 13 years at 10 events/s — no wrap
// path is implemented, by design.
#define STORE_SEQ_BLOCK 1000

// Card UID: uppercase hex without separators on the wire, 4/7/10 raw bytes
// here (8/14/20 hex chars, §2.4).
#define STORE_UID_MAX 10
// ETag of the last accepted allow-list (opaque server string, §2.4). The
// server sends a quoted 64-hex digest — 66 chars + NUL; 80 leaves room for
// a longer future format. The stored bytes must round-trip byte-exact into
// If-None-Match or every poll is a 200.
#define STORE_ETAG_MAX 80

// Card cap, shared by the NVS cache AND the JSON decoder (one number: the
// decoder refuses a body with more cards than the cache can hold, and the
// sync response buffer is sized for this many cards' JSON).
// More are refused loudly (STORE_ERR_TOO_MANY) — the old list staying
// active must never be silent, or revocations would stop flowing.
#define STORE_CARDS_MAX 256

// One buffered event as returned by store_event_batch(): an assigned seq,
// the payload that was pushed, and its length.
typedef struct {
    uint32_t seq;
    size_t len;
    uint8_t bytes[STORE_EVENT_MAX];
} store_event_t;

// One allow-list card in the compact binary cache form.
typedef struct {
    uint8_t uid_len; // 4, 7 or 10
    uint8_t uid[STORE_UID_MAX];
    uint32_t valid_from;  // epoch seconds, 0 = absent (permanent from now)
    uint32_t valid_until; // epoch seconds, 0 = absent (never expires)
} store_card_t;

// Status of store operations. STORE_ABSENT = nothing cached yet (fresh
// device); STORE_ERR_TOO_SMALL = caller's buffer too small (retry with the
// sizes returned through the out params); STORE_ERR_* are loud failures the
// caller must log/flag.
typedef enum {
    STORE_OK = 0,
    STORE_ABSENT,
    STORE_ERR_NVS,
    STORE_ERR_TOO_SMALL,
    STORE_ERR_TOO_MANY,
    STORE_ERR_ARG
} store_status_t;

// Mounts the ring partition and the NVS namespace, recovers the write/read
// cursors and the max ring seq, and loads/creates the stream UUID (created
// when NVS has none — a wipe therefore starts a new stream whose seq block
// continues past every seq still in the ring, §2.4). Returns false only on
// backend failure; retrying after a wipe (factory reset) is valid.
bool store_init(void);

// Number of events the ring holds once mounted (constant after boot).
size_t store_ring_capacity(void);

// --- Allow-list cache -------------------------------------------------

// Atomically replaces the cached list (cards + ETag in one NVS blob). The
// new list becomes readable only after the commit; on any failure the old
// list stays readable and the error is returned (loud, never silent).
// Oversize (> STORE_CARDS_MAX) is STORE_ERR_TOO_MANY: the caller must log
// and flag it — keeping the stale list silently would freeze revocations.
store_status_t store_allow_list_replace(const store_card_t *cards, size_t n,
                                        const char *etag, size_t etag_len);

// Reads the active cached list. out/etag may be NULL when their caps are 0;
// *n_out and *etag_len_out always receive the stored sizes, so
// STORE_ERR_TOO_SMALL is retryable with correctly sized buffers.
store_status_t store_allow_list_read(store_card_t *out, size_t max_cards, size_t *n_out,
                                     char *etag, size_t etag_cap, size_t *etag_len_out);

// --- Stream + seq -----------------------------------------------------

// Copies the 16-byte stream UUID (created in store_init when NVS had none).
void store_stream_uuid(uint8_t out[16]);

// Returns the next seq, or 0 when the block-end persist failed (the caller
// must not store an event with a possibly-unreserved seq). Block ends are
// persisted ahead of use: after a reboot seq continues at the next
// STORE_SEQ_BLOCK boundary, or past the highest seq in the ring.
uint32_t store_seq_next(void);

// --- Event ring -------------------------------------------------------

// Buffers one event, assigning it the next seq. Returns false when the ring
// is full (the caller's drop is already counted — §2.4: the door is never
// blocked on the buffer) or on flash/NVS failure (not counted; hardware
// trouble, not capacity). The event is not stored either way.
bool store_event_push(const uint8_t *event, size_t len, uint32_t *seq_out);

// Copies up to max_events (≤ STORE_BATCH_MAX) oldest unacked events into out,
// oldest first. Peeks only: a batch that never got uploaded is served again.
size_t store_event_batch(store_event_t *out, size_t max_events);

// Deletes every buffered event with seq ≤ acked_through — accepted AND
// rejected ones (§2.4: one bad event never blocks the queue). Sectors the
// cursor fully passes are erased right away (sync task; same total wear, and
// a reboot then replays at most one partial sector).
void store_ack(uint32_t acked_through);

// Dropped counter: peek with store_dropped_count(), then call
// store_dropped_ack(n) after the upload was ACKED by the server — never
// before (a cleared-before-200 crash would under-report). The count is
// persisted, so it survives reboots until it has been sent.
uint32_t store_dropped_count(void);
void store_dropped_ack(uint32_t n);

#ifdef __cplusplus
}
#endif
