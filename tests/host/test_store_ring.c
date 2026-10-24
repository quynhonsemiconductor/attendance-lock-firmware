// Event ring: wraparound, torn records, ack deletion, dropped counting,
// seq block reservation, stream UUID regeneration (§2.8, work item 7).
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
    TEST_ASSERT_EQUAL_UINT8(0x00, fl[32 + 4 * 64 + 8]); // torn slot never resurrected
}

void test_wraparound_reuses_acked_sectors(void)
{
    uint8_t in[4];
    // Fill the whole ring one sector at a time, acking each sector as it
    // completes: the write position re-enters sector 0 at abs CAPACITY and
    // the erased (acked) sector is reused — the wrap, with no data loss.
    for (size_t i = 0; i < CAPACITY; i++) {
        fill(in, sizeof(in), (uint8_t)(i % 256));
        TEST_ASSERT_TRUE(store_event_push(in, sizeof(in), NULL));
        if ((i + 1) % RECS_PER_SECTOR == 0) {
            store_ack((uint32_t)(i + 1));
        }
    }
    fill(in, sizeof(in), 0x42);
    uint32_t seq = 0;
    TEST_ASSERT_TRUE(store_event_push(in, sizeof(in), &seq));
    TEST_ASSERT_EQUAL_UINT32(CAPACITY + 1, seq);
    TEST_ASSERT_TRUE(store_init()); // reboot mid-ring
    store_event_t out[STORE_BATCH_MAX];
    // The oldest surviving sector is 1 (sector 0 was reused): a batch of 100
    // carries seq 64..163 with payloads intact.
    size_t n = store_event_batch(out, STORE_BATCH_MAX);
    TEST_ASSERT_EQUAL_size_t(100, n);
    TEST_ASSERT_EQUAL_UINT32(64, out[0].seq);
    TEST_ASSERT_EQUAL_UINT32(163, out[99].seq);
    for (size_t i = 0; i < n; i++) {
        TEST_ASSERT_EQUAL_UINT8((uint8_t)((out[i].seq - 1) % 256), out[i].bytes[0]);
    }
    TEST_ASSERT_EQUAL_UINT32(0, store_dropped_take());
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
    TEST_ASSERT_EQUAL_UINT32(3, store_dropped_take());
    TEST_ASSERT_EQUAL_UINT32(0, store_dropped_take());
    // Ack everything: space comes back.
    store_ack(UINT32_MAX);
    TEST_ASSERT_TRUE(store_event_push(in, sizeof(in), NULL));
}

void test_dropped_survives_reboot_until_taken(void)
{
    uint8_t in[4];
    fill(in, sizeof(in), 1);
    for (size_t i = 0; i < CAPACITY + 2; i++) {
        (void)store_event_push(in, sizeof(in), NULL);
    }
    TEST_ASSERT_TRUE(store_init()); // reboot before the batch was sent
    TEST_ASSERT_EQUAL_UINT32(2, store_dropped_take());
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

void test_stream_uuid_regenerated_after_nvs_wipe(void)
{
    uint8_t a[16], b[16], c[16];
    store_stream_uuid(a);
    store_stream_uuid(b);
    TEST_ASSERT_EQUAL_MEMORY(a, b, 16); // stable while NVS holds it
    TEST_ASSERT_EQUAL_UINT8(0x40, a[6] & 0xF0); // RFC 4122 v4 bits
    TEST_ASSERT_EQUAL_UINT8(0x80, a[8] & 0xC0);
    store_host_nvs_wipe();
    store_stream_uuid(c);
    TEST_ASSERT_NOT_EQUAL(0, memcmp(a, c, 16)); // wipe → new stream (§2.4)
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_capacity_matches_partition);
    RUN_TEST(test_push_batch_roundtrip_and_peek);
    RUN_TEST(test_ack_deletes_through_acked);
    RUN_TEST(test_reboot_recovers_ring_and_seq);
    RUN_TEST(test_torn_record_skipped_and_sector_sealed);
    RUN_TEST(test_wraparound_reuses_acked_sectors);
    RUN_TEST(test_full_ring_drops_and_counts);
    RUN_TEST(test_dropped_survives_reboot_until_taken);
    RUN_TEST(test_seq_block_persisted_ahead_of_use);
    RUN_TEST(test_stream_uuid_regenerated_after_nvs_wipe);
    return UNITY_END();
}
