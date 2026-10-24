#pragma once
// Allow-list cache (encrypted NVS) and the event ring (encrypted data
// partition, §2.8). Firmware v1: the real implementation.
//
// Concurrency: not thread-safe. The door task pushes events, the sync task
// batches/acks — callers serialise through one queue or a mutex above this.
//
// Failure mode: every function returns/keeps the previous state on failure;
// a broken store never blocks the door (§2.4) — the caller drops and counts.

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
// first seq is handed out; a reboot continues at the next block boundary.
#define STORE_SEQ_BLOCK 1000

// Upper bound for a cached allow-list body (NVS blob). A 200 response larger
// than this is refused and the old list stays active.
#define STORE_ALLOW_LIST_MAX (16 * 1024)

// One buffered event as returned by store_event_batch(): an assigned seq
// plus the opaque payload that was pushed.
typedef struct {
    uint32_t seq;
    uint8_t bytes[STORE_EVENT_MAX];
} store_event_t;

// Mounts the ring partition and the NVS namespace, recovers the write/read
// cursors, the seq block and the stream UUID (created when NVS has none).
// Returns false when the partition/NVS is unusable; retrying after a wipe
// (factory reset) is valid.
bool store_init(void);

// Number of events the ring holds once mounted (constant after boot).
size_t store_ring_capacity(void);

// --- Allow-list cache -------------------------------------------------

// Atomically replaces the cached allow-list from a 200 response body. The new
// list becomes readable only after both NVS writes committed; on any failure
// the old list stays readable and active (§2.8).
bool store_allow_list_replace(const char *body, size_t len);

// Reads the active cached list into out (cap bytes). Returns false when no
// valid copy exists; *len_out receives the body length.
bool store_allow_list_read(char *out, size_t cap, size_t *len_out);

// --- Stream + seq -----------------------------------------------------

// Copies the 16-byte stream UUID into out, creating it in NVS first when
// absent (new board, reflash, NVS wipe → new stream, §2.4).
void store_stream_uuid(uint8_t out[16]);

// Returns the next seq and reserves it. Block ends are persisted ahead of
// use: after a reboot seq continues at the next STORE_SEQ_BLOCK boundary.
uint32_t store_seq_next(void);

// --- Event ring -------------------------------------------------------

// Buffers one event, assigning it the next seq. Returns false when the ring
// is full — the caller has already been counted (§2.4: the door is never
// blocked on the buffer); the event is not stored.
bool store_event_push(const uint8_t *event, size_t len, uint32_t *seq_out);

// Copies up to max_events (≤ STORE_BATCH_MAX) oldest unacked events into out,
// oldest first. Returns the count.
size_t store_event_batch(store_event_t *out, size_t max_events);

// Deletes every buffered event with seq ≤ acked_through — accepted AND
// rejected ones (§2.4: one bad event never blocks the queue). Events beyond
// acked_through are kept. Resends after a lost response are free (the server
// upserts ON CONFLICT DO NOTHING), so an unacknowledged deletion lost in a
// reboot only costs a resend.
void store_ack(uint32_t acked_through);

// Reads and clears the count of events dropped because the ring was full
// since the last take. The count is persisted, so it survives reboots until
// it has been sent.
uint32_t store_dropped_take(void);

#ifdef __cplusplus
}
#endif
