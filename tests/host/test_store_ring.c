// Event ring: wraparound, torn records, ack deletion + durable ack state,
// full-ring drop counting, seq block reservation, stream regeneration,
// fault injection (power cuts, NVS failures) — §2.8, work item 7.
#include "unity.h"

#include <string.h>

#include "store/store.h"
#include "store_port_host.h"

// Host flash is 4 MiB → 1024 sectors × 63 records.
#define N_SECTORS 1024u
#define RECS_PER_SECTOR 63u
#define CAPACITY ((size_t)(N_SECTORS * RECS_PER_SECTOR))

void setUp(void)
{
    store_host_reset();
    store_host_seed(0xC0FFEEu);
    TEST_ASSERT_TRUE(store_init());
}

void tearDown(void) {}

static void fill(uint8_t *buf, size_t len, uint8_t seed)
{
    for (size_t i = 0; i < len; i++) {
        buf[i] = (uint8_t)(seed + i);
    }
}

void test_capacity_matches_partition(void)
{
    TEST_ASSERT_EQUAL_size_t(CAPACITY, store_ring_capacity());
}

void test_push_batch_roundtrip_and_peek(void)
{
    uint8_t in[8];
    for (uint32_t i = 0; i < 5; i++) {
        fill(in, sizeof(in), (uint8_t)i);
        uint32_t seq = 0;
        TEST_ASSERT_TRUE(store_event_push(in, sizeof(in), &seq));
        TEST_ASSERT_EQUAL_UINT32(i + 1, seq);
    }
    store_event_t out[STORE_BATCH_MAX];
    size_t n = store_event_batch(out, 10);
    TEST_ASSERT_EQUAL_size_t(5, n);
    for (size_t i = 0; i < n; i++) {
        TEST_ASSERT_EQUAL_UINT32(i + 1, out[i].seq);
        TEST_ASSERT_EQUAL_size_t(8, out[i].len);
        for (size_t b = 0; b < sizeof(in); b++) {
            TEST_ASSERT_EQUAL_UINT8((uint8_t)(i + b), out[i].bytes[b]);
        }
    }
    // Peek semantics: a batch that was never uploaded is served again.
    TEST_ASSERT_EQUAL_size_t(5, store_event_batch(out, 10));
}

void test_ack_deletes_through_acked(void)
{
    uint8_t in[4];
    for (uint32_t i = 0; i < 10; i++) {
        fill(in, sizeof(in), (uint8_t)i);
        TEST_ASSERT_TRUE(store_event_push(in, sizeof(in), NULL));
    }
    store_event_t out[STORE_BATCH_MAX];
    store_ack(5); // accepted AND rejected seqs die together (§2.4)
    size_t n = store_event_batch(out, 10);
    TEST_ASSERT_EQUAL_size_t(5, n);
    TEST_ASSERT_EQUAL_UINT32(6, out[0].seq);
    TEST_ASSERT_EQUAL_UINT32(10, out[4].seq);
    store_ack(20);
    TEST_ASSERT_EQUAL_size_t(0, store_event_batch(out, 10));
}

void test_ack_is_durable_across_reboot(void)
{
    // Blocker 2: the durable ack state is the set of erased sectors. Wrap
    // the ring with every sector acked (and therefore erased), reboot: the
    // ring must come back EMPTY, not replay the whole history.
    uint8_t in[4];
    for (size_t i = 0; i < CAPACITY; i++) {
        fill(in, sizeof(in), (uint8_t)(i % 256));
        TEST_ASSERT_TRUE(store_event_push(in, sizeof(in), NULL));
        if ((i + 1) % RECS_PER_SECTOR == 0) {
            store_ack((uint32_t)(i + 1));
        }
    }
    store_ack(UINT32_MAX);
    TEST_ASSERT_TRUE(store_init()); // reboot
    store_event_t out[STORE_BATCH_MAX];
    TEST_ASSERT_EQUAL_size_t(0, store_event_batch(out, STORE_BATCH_MAX));
    // At most one partial sector replays: a new event continues the stream.
    uint32_t seq = 0;
    fill(in, sizeof(in), 0x99);
    TEST_ASSERT_TRUE(store_event_push(in, sizeof(in), &seq));
    TEST_ASSERT_EQUAL_UINT32(((CAPACITY + 999) / 1000) * 1000 + 1, seq); // block reservation
}

void test_ack_erases_sector_zero_on_first_lap(void)
{
    // N1: boundary b erases the sector [b-63, b); with from == 0 the loop
    // used to start at 0 and `b > 0` killed it, so sector 0 survived its
    // own ack and a reboot replayed it.
    uint8_t in[4];
    for (uint32_t i = 0; i < 150; i++) {
        fill(in, sizeof(in), (uint8_t)i);
        TEST_ASSERT_TRUE(store_event_push(in, sizeof(in), NULL));
    }
    store_ack(100); // cursor passes sector 0's end (63)
    TEST_ASSERT_TRUE(store_init()); // reboot
    store_event_t out[STORE_BATCH_MAX];
    size_t n = store_event_batch(out, STORE_BATCH_MAX);
    // Sector 0 (seqs 1..63) was erased — its ack state went with it, so
    // seqs 64..100 replay too (free resend); seqs 101..150 were never sent.
    TEST_ASSERT_EQUAL_size_t(87, n);
    TEST_ASSERT_EQUAL_UINT32(64, out[0].seq);
    TEST_ASSERT_EQUAL_UINT32(150, out[86].seq);
}

void test_generation_check_rejects_mismatched_sector(void)
{
    // Defense-in-depth: a record whose sector generation does not match the
    // absolute index must never be served (a mutant removing the check
    // survives the rest of the suite). Forge a valid header bumping sector
    // 1 from gen 2 (abs 63..125) to gen 5 (abs 252..314): its old records
    // must not be served as events 64..126.
    uint8_t in[4];
    for (uint32_t i = 0; i < 126; i++) {
        fill(in, sizeof(in), (uint8_t)i);
        TEST_ASSERT_TRUE(store_event_push(in, sizeof(in), NULL));
    }
    uint8_t *fl = store_host_flash();
    fl[4096 + 4] = 5; // sector 1 header: gen = 5 (bytes are CRC-covered)
    // fix the header CRC (poly 0xEDB88320 over the first 28 bytes)
    uint32_t crc = 0;
    crc = ~crc;
    for (uint32_t i = 0; i < 28; i++) {
        crc ^= fl[4096 + i];
        for (int b = 0; b < 8; b++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    crc = ~crc;
    memcpy(fl + 4096 + 28, &crc, 4);
    TEST_ASSERT_TRUE(store_init());
    store_event_t out[STORE_BATCH_MAX];
    size_t n = store_event_batch(out, STORE_BATCH_MAX);
    // Only sector 0's records (seqs 1..63) are servable; sector 1's records
    // belong to absolute indices 252..314 now, not 63..125.
    TEST_ASSERT_EQUAL_size_t(63, n);
    TEST_ASSERT_EQUAL_UINT32(1, out[0].seq);
    TEST_ASSERT_EQUAL_UINT32(63, out[62].seq);
}

void test_reboot_recovers_ring_and_seq(void)
{
    uint8_t in[6];
    for (uint32_t i = 0; i < 10; i++) {
        fill(in, sizeof(in), (uint8_t)i);
        TEST_ASSERT_TRUE(store_event_push(in, sizeof(in), NULL));
    }
    TEST_ASSERT_TRUE(store_init()); // power cycle: same flash, same NVS
    store_event_t out[STORE_BATCH_MAX];
    size_t n = store_event_batch(out, 10);
    TEST_ASSERT_EQUAL_size_t(10, n);
    TEST_ASSERT_EQUAL_UINT32(1, out[0].seq);
    TEST_ASSERT_EQUAL_UINT32(10, out[9].seq);
    // Seq survives via the reserved block end, not the ring contents.
    TEST_ASSERT_EQUAL_UINT32(1001, store_seq_next());
}

void test_full_ring_survives_reboot_still_full(void)
{
    // Blocker 1: a full ring must stay full across a reboot — the sectors'
    // record counts are recovered for ALL sectors, not just the write
    // sector, and rec_at rejects records whose sector generation does not
    // match the absolute index.
    uint8_t in[4];
    fill(in, sizeof(in), 1);
    for (size_t i = 0; i < CAPACITY; i++) {
        TEST_ASSERT_TRUE(store_event_push(in, sizeof(in), NULL));
    }
    TEST_ASSERT_TRUE(store_init()); // reboot with a full, unacked ring
    TEST_ASSERT_FALSE(store_event_push(in, sizeof(in), NULL));
    TEST_ASSERT_EQUAL_UINT32(1, store_dropped_count());
    // Ack everything: the sectors are erased by ack, space comes back.
    store_ack(UINT32_MAX);
    TEST_ASSERT_TRUE(store_event_push(in, sizeof(in), NULL));
}

void test_torn_record_skipped_and_sector_sealed(void)
{
    uint8_t in[4];
    for (uint32_t i = 0; i < 5; i++) {
        fill(in, sizeof(in), (uint8_t)i);
        TEST_ASSERT_TRUE(store_event_push(in, sizeof(in), NULL));
    }
    // Power cut mid-write: the last record (sector 0, slot 4) is garbage.
    // Corrupt its CRC field only (record offset 32 + 4*64, CRC at +8) so the
    // magic still decodes — the CRC is what must reject the record.
    uint8_t *fl = store_host_flash();
    memset(fl + 32 + 4 * 64 + 8, 0x00, 4);
    TEST_ASSERT_TRUE(store_init());
    store_event_t out[STORE_BATCH_MAX];
    // The torn event is gone; the four intact ones are served.
    size_t n = store_event_batch(out, 10);
    TEST_ASSERT_EQUAL_size_t(4, n);
    // The sealed sector cannot take the next event — it lands in sector 1.
    // The seq block was already reserved, so the reboot continues at 1001.
    uint32_t seq = 0;
    fill(in, sizeof(in), 0x77);
    TEST_ASSERT_TRUE(store_event_push(in, sizeof(in), &seq));
    TEST_ASSERT_EQUAL_UINT32(1001, seq);
    // The queue head sits before the torn gap; acking the four served events
    // moves the head past it and the new event is served next.
    store_ack(4);
    n = store_event_batch(out, 10);
    TEST_ASSERT_EQUAL_size_t(1, n);
    TEST_ASSERT_EQUAL_UINT32(1001, out[0].seq);
    TEST_ASSERT_EQUAL_UINT8(0x77, out[0].bytes[0]);
    // Acking the four served events erases their whole sector (durable ack):
    // the torn slot is gone entirely, so it can never be resurrected.
    TEST_ASSERT_EQUAL_UINT8(0xFF, fl[32 + 4 * 64 + 8]);
}

void test_encrypted_blank_boots_like_real_flash(void)
{
    // With flash encryption a blank sector decrypts to garbage: the boot
    // must still work (garbage = UNKNOWN sector, erased before first use),
    // and a torn sector is indistinguishable from blank — both sealed.
    store_host_set_keystream(true);
    store_host_reset();
    TEST_ASSERT_TRUE(store_init());
    uint8_t in[4];
    for (uint32_t i = 0; i < 5; i++) {
        fill(in, sizeof(in), (uint8_t)i);
        TEST_ASSERT_TRUE(store_event_push(in, sizeof(in), NULL));
    }
    TEST_ASSERT_TRUE(store_init()); // reboot: write sector has < 63 records → sealed
    store_event_t out[STORE_BATCH_MAX];
    TEST_ASSERT_EQUAL_size_t(5, store_event_batch(out, 10));
    uint32_t seq = 0;
    TEST_ASSERT_TRUE(store_event_push(in, sizeof(in), &seq));
    TEST_ASSERT_EQUAL_UINT32(1001, seq); // continued in a fresh sector
}

void test_fault_header_write_cut(void)
{
    uint8_t in[4];
    fill(in, sizeof(in), 1);
    // Fill sector 0 so the next push advances (erases + writes a header).
    for (uint32_t i = 0; i < RECS_PER_SECTOR; i++) {
        TEST_ASSERT_TRUE(store_event_push(in, sizeof(in), NULL));
    }
    // Cut the sector header write at 10 bytes (advance to sector 1).
    store_host_fail_write_after(10);
    TEST_ASSERT_FALSE(store_event_push(in, sizeof(in), NULL));
    TEST_ASSERT_EQUAL_UINT32(0, store_dropped_count()); // hw failure: not a drop
    TEST_ASSERT_TRUE(store_init()); // torn header → UNKNOWN → erased on demand
    store_event_t out[STORE_BATCH_MAX];
    TEST_ASSERT_EQUAL_size_t(RECS_PER_SECTOR, store_event_batch(out, 100));
    uint32_t seq = 0;
    TEST_ASSERT_TRUE(store_event_push(in, sizeof(in), &seq));
    TEST_ASSERT_EQUAL_UINT32(1001, seq); // sealed tail skipped, no reuse
}

void test_fault_erase_failure_not_counted(void)
{
    uint8_t in[4];
    fill(in, sizeof(in), 1);
    for (uint32_t i = 0; i < RECS_PER_SECTOR; i++) {
        TEST_ASSERT_TRUE(store_event_push(in, sizeof(in), NULL));
    }
    // First leave sector 1 with a torn header (UNKNOWN), so the next advance
    // must ERASE it before reuse — and make that erase fail once.
    store_host_fail_write_after(10);
    TEST_ASSERT_FALSE(store_event_push(in, sizeof(in), NULL));
    store_host_fail_erase_next();
    TEST_ASSERT_FALSE(store_event_push(in, sizeof(in), NULL)); // erase failed
    TEST_ASSERT_EQUAL_UINT32(0, store_dropped_count());        // hw failure: no drop
    // The retry succeeds: erase, header, record — the ring keeps going.
    // (Same session: seq continues at 64; the block end was already persisted.)
    uint32_t seq = 0;
    TEST_ASSERT_TRUE(store_event_push(in, sizeof(in), &seq));
    TEST_ASSERT_EQUAL_UINT32(RECS_PER_SECTOR + 1, seq);
}

void test_fault_record_write_cut_torn(void)
{
    uint8_t in[4];
    fill(in, sizeof(in), 1);
    TEST_ASSERT_TRUE(store_event_push(in, sizeof(in), NULL));
    // Cut the second RECORD write 10 bytes in: the record header (CRC
    // included) is programmed but its payload bytes stay 0xFF, so the stored
    // CRC no longer matches — exactly a power cut mid-write.
    store_host_fail_write_after(10);
    TEST_ASSERT_FALSE(store_event_push(in, sizeof(in), NULL));
    TEST_ASSERT_TRUE(store_init());
    store_event_t out[STORE_BATCH_MAX];
    TEST_ASSERT_EQUAL_size_t(1, store_event_batch(out, 10));
    TEST_ASSERT_EQUAL_UINT32(1, out[0].seq);
    TEST_ASSERT_EQUAL_UINT32(0, store_dropped_count());
}

void test_fault_seq_persist_fails_push_fails_and_counts(void)
{
    // Blocker 4: if the seq block end cannot be persisted, the seq might be
    // REUSED after a reboot (silently discarded server-side) — the push must
    // fail and count, never store.
    uint8_t in[4];
    fill(in, sizeof(in), 1);
    // Fresh device: the first push reserves block [1..1000].
    store_host_fail_nvs_set(1); // make that reservation fail
    TEST_ASSERT_FALSE(store_event_push(in, sizeof(in), NULL));
    TEST_ASSERT_EQUAL_UINT32(1, store_dropped_count());
    // Recovery: the retry re-attempts the reservation and succeeds.
    TEST_ASSERT_TRUE(store_event_push(in, sizeof(in), NULL));
    uint32_t seq = 0;
    TEST_ASSERT_TRUE(store_event_push(in, sizeof(in), &seq));
    TEST_ASSERT_EQUAL_UINT32(2, seq);
    // And the seq block end IS now persisted: a reboot never reuses 1..2.
    TEST_ASSERT_TRUE(store_init());
    TEST_ASSERT_EQUAL_UINT32(1001, store_seq_next());
}

void test_fault_nvs_blob_replace_interrupted_keeps_old(void)
{
    store_card_t cards[2] = {{.uid_len = 4, .uid = {0x04, 0xA1, 0xB2, 0xC3}},
                             {.uid_len = 7, .uid = {0x01}, .valid_until = 123}};
    TEST_ASSERT_EQUAL(STORE_OK,
                      store_allow_list_replace(cards, 2, "etag-1", 6));
    store_host_fail_nvs_set(1);
    store_card_t cards2[1] = {{.uid_len = 10, .uid = {0xAA}}};
    TEST_ASSERT_EQUAL(STORE_ERR_NVS, store_allow_list_replace(cards2, 1, "etag-2", 6));
    store_card_t back[4];
    char etag[STORE_ETAG_MAX];
    size_t n = 0, etag_len = 0;
    TEST_ASSERT_EQUAL(STORE_OK,
                      store_allow_list_read(back, 4, &n, etag, sizeof(etag), &etag_len));
    TEST_ASSERT_EQUAL_size_t(2, n); // old list survived the interrupted replace
    TEST_ASSERT_EQUAL_UINT32(123, back[1].valid_until);
}

void test_full_ring_drops_and_counts(void)
{
    uint8_t in[4];
    fill(in, sizeof(in), 1);
    for (size_t i = 0; i < CAPACITY; i++) {
        TEST_ASSERT_TRUE(store_event_push(in, sizeof(in), NULL));
    }
    // Full: door keeps working, events are dropped and counted (§2.4).
    for (int i = 0; i < 3; i++) {
        TEST_ASSERT_FALSE(store_event_push(in, sizeof(in), NULL));
    }
    // Peek: the count survives until the server ACKED the report.
    TEST_ASSERT_EQUAL_UINT32(3, store_dropped_count());
    TEST_ASSERT_EQUAL_UINT32(3, store_dropped_count());
    store_dropped_ack(3);
    TEST_ASSERT_EQUAL_UINT32(0, store_dropped_count());
    // Ack everything: space comes back.
    store_ack(UINT32_MAX);
    TEST_ASSERT_TRUE(store_event_push(in, sizeof(in), NULL));
}

void test_dropped_persisted_at_the_sync_read_point(void)
{
    // Drop counting is RAM + cadence-persisted (per-event NVS writes would
    // be flash wear): the flush point is the sync task's read before each
    // upload — a reboot after that read still reports the drops.
    uint8_t in[4];
    fill(in, sizeof(in), 1);
    for (size_t i = 0; i < CAPACITY + 2; i++) {
        (void)store_event_push(in, sizeof(in), NULL);
    }
    TEST_ASSERT_EQUAL_UINT32(2, store_dropped_count()); // read == flush point
    TEST_ASSERT_TRUE(store_init()); // reboot before the batch was sent
    TEST_ASSERT_EQUAL_UINT32(2, store_dropped_count());
    store_dropped_ack(2);
    TEST_ASSERT_EQUAL_UINT32(0, store_dropped_count());
}

void test_seq_block_persisted_ahead_of_use(void)
{
    // setUp already called store_init(). First allocation reserves [1..1000]
    // by persisting the block END before any seq is handed out.
    TEST_ASSERT_EQUAL_UINT32(1, store_seq_next());
    TEST_ASSERT_EQUAL_UINT32(2, store_seq_next());
    // Wipe only the ring: a reboot must NOT reuse 1..3 — the next seq is the
    // next block (gaps are harmless, reuse would collide server-side, §2.4).
    uint8_t *fl = store_host_flash();
    memset(fl, 0xFF, 3 * 4096);
    TEST_ASSERT_TRUE(store_init());
    TEST_ASSERT_EQUAL_UINT32(1001, store_seq_next());
}

void test_nvs_wipe_new_stream_no_seq_reuse(void)
{
    // Blocker 3: after an NVS wipe the new stream's seqs must never collide
    // with the events still buffered in the ring.
    uint8_t in[4];
    fill(in, sizeof(in), 1);
    for (uint32_t i = 0; i < 5000; i++) {
        TEST_ASSERT_TRUE(store_event_push(in, sizeof(in), NULL));
    }
    uint8_t old_stream[16];
    store_stream_uuid(old_stream);
    store_host_nvs_wipe(); // factory reset of the NVS only; ring survives
    TEST_ASSERT_TRUE(store_init());
    uint8_t new_stream[16];
    store_stream_uuid(new_stream);
    TEST_ASSERT_NOT_EQUAL(0, memcmp(old_stream, new_stream, 16)); // new stream
    uint32_t seq = store_seq_next();
    TEST_ASSERT_TRUE(seq > 5000); // past every seq still in the ring
    store_event_t out[STORE_BATCH_MAX];
    size_t n = store_event_batch(out, 100);
    TEST_ASSERT_EQUAL_size_t(100, n);
    TEST_ASSERT_EQUAL_UINT32(1, out[0].seq); // old events intact, old seqs
}

void test_stream_uuid_regenerated_after_nvs_wipe(void)
{
    uint8_t a[16], b[16], c[16];
    store_stream_uuid(a);
    store_stream_uuid(b);
    TEST_ASSERT_EQUAL_MEMORY(a, b, 16); // stable while NVS holds it
    TEST_ASSERT_EQUAL_UINT8(0x40, a[6] & 0xF0); // RFC 4122 v4 bits
    TEST_ASSERT_EQUAL_UINT8(0x80, a[8] & 0xC0);
    store_host_nvs_wipe();
    TEST_ASSERT_TRUE(store_init()); // init regenerates + persists
    store_stream_uuid(c);
    TEST_ASSERT_NOT_EQUAL(0, memcmp(a, c, 16)); // wipe → new stream (§2.4)
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_capacity_matches_partition);
    RUN_TEST(test_push_batch_roundtrip_and_peek);
    RUN_TEST(test_ack_deletes_through_acked);
    RUN_TEST(test_ack_is_durable_across_reboot);
    RUN_TEST(test_ack_erases_sector_zero_on_first_lap);
    RUN_TEST(test_generation_check_rejects_mismatched_sector);
    RUN_TEST(test_reboot_recovers_ring_and_seq);
    RUN_TEST(test_full_ring_survives_reboot_still_full);
    RUN_TEST(test_torn_record_skipped_and_sector_sealed);
    RUN_TEST(test_encrypted_blank_boots_like_real_flash);
    RUN_TEST(test_fault_header_write_cut);
    RUN_TEST(test_fault_erase_failure_not_counted);
    RUN_TEST(test_fault_record_write_cut_torn);
    RUN_TEST(test_fault_seq_persist_fails_push_fails_and_counts);
    RUN_TEST(test_fault_nvs_blob_replace_interrupted_keeps_old);
    RUN_TEST(test_full_ring_drops_and_counts);
    RUN_TEST(test_dropped_persisted_at_the_sync_read_point);
    RUN_TEST(test_seq_block_persisted_ahead_of_use);
    RUN_TEST(test_nvs_wipe_new_stream_no_seq_reuse);
    RUN_TEST(test_stream_uuid_regenerated_after_nvs_wipe);
    return UNITY_END();
}
