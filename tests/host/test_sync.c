// Sync session + contract JSON codec, driven against the contract fixtures
// (tests/fixtures/contract-v1, door terminal contract v1) and a scripted
// transport.
#include "unity.h"

#include <stdio.h>
#include <string.h>

#include "clock/clock.h"
#include "clock_port_host.h"
#include "store/store.h"
#include "store_port_host.h"
#include "sync/sync.h"
#include "sync_port_host.h"

#ifndef FIXTURE_DIR
#define FIXTURE_DIR "."
#endif

void setUp(void)
{
    store_host_reset();
    store_host_seed(0xC0FFEEu);
    TEST_ASSERT_TRUE(store_init());
    clock_host_power_loss();
    clock_host_set_epoch(0);
    clock_host_set_uptime_us(INT64_MIN);
    TEST_ASSERT_TRUE(clock_init());
    sync_host_reset();
}

void tearDown(void) {}

static char *fixture(const char *name)
{
    static char buf[4096];
    snprintf(buf, sizeof(buf), "%s/%s", FIXTURE_DIR, name);
    FILE *f = fopen(buf, "rb");
    TEST_ASSERT_NOT_NULL_MESSAGE(f, buf);
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    return buf;
}

// Strips whitespace outside strings: the fixture's pretty layout vs the
// encoder's compact output — byte-compare after this normalization.
static void minify(char *s)
{
    char *w = s;
    bool in_str = false;
    for (char *r = s; *r; r++) {
        if (*r == '"') {
            in_str = !in_str;
        }
        if (in_str || (*r != ' ' && *r != '\n' && *r != '\r' && *r != '\t')) {
            *w++ = *r;
        }
    }
    *w = '\0';
}

static sync_event_t fixture_event(void)
{
    sync_event_t ev = { 0 };
    ev.boot_id = clock_boot_id();
    ev.uptime_us = 1000000; // matches the pinned clock base in the tests
    ev.reader = 0; // "in"
    ev.uid_len = 4;
    ev.uid[0] = 0x04;
    ev.uid[1] = 0xA1;
    ev.uid[2] = 0xB2;
    ev.uid[3] = 0xC3;
    ev.result = 0; // "allow"
    return ev;
}

// --- JSON codec ---------------------------------------------------------

void test_decode_allow_list_fixture(void)
{
    store_card_t cards[8];
    const char *fx = fixture("allow-list.json");
    bool too_many = true;
    size_t n = sync_decode_allow_list(fx, strlen(fx), cards, 8, &too_many);
    TEST_ASSERT_FALSE(too_many);
    TEST_ASSERT_NOT_EQUAL(SIZE_MAX, n);
    TEST_ASSERT_EQUAL_size_t(1, n);
    TEST_ASSERT_EQUAL_UINT8(4, cards[0].uid_len);
    TEST_ASSERT_EQUAL_UINT8(0x04, cards[0].uid[0]);
    TEST_ASSERT_EQUAL_UINT8(0xC3, cards[0].uid[3]);
    TEST_ASSERT_EQUAL_UINT32(0, cards[0].valid_from); // null windows = 0
    TEST_ASSERT_EQUAL_UINT32(0, cards[0].valid_until);
}

void test_decode_allow_list_unknown_fields_tolerated(void)
{
    // §2.4: fields may be added — a body with unknown fields anywhere must
    // still parse.
    const char *body =
        "{\"schema\":2,\"cards\":[{\"uid\":\"04A1B2C3\",\"label\":\"front\",\"valid_from\":null,"
        "\"meta\":{\"x\":1},\"valid_until\":1729000000}],\"gen\":\"z9\"}";
    store_card_t cards[8];
    bool too_many = false;
    size_t n = sync_decode_allow_list(body, strlen(body), cards, 8, &too_many);
    TEST_ASSERT_NOT_EQUAL(SIZE_MAX, n);
    TEST_ASSERT_FALSE(too_many);
    TEST_ASSERT_EQUAL_size_t(1, n);
    TEST_ASSERT_EQUAL_UINT32(1729000000, cards[0].valid_until);
}

void test_decode_allow_list_rejects_malformed(void)
{
    const char *bad[] = {
        "{\"cards\":[{\"uid\":\"04A1\"}]}",       // 3-byte uid: not 8/14/20 hex
        "{\"cards\":[{\"uid\":\"04a1b2c3\"}]}",   // lower-case folded up: OK —
        "{\"cards\":[{\"uid\":\"04A1B2C3\"}",     // truncated
        "{\"cards\":[{\"uid\":123}]}",            // uid not a string
        "{\"cards\":[{\"uid\":\"0GA1B2C3\"}]}",   // not hex
        "{\"cards\":[{\"uid\":\"04A1B2C3\"},]}",  // trailing comma
        "{\"cards\":}",                           // no array
        "{\"cards\":[",                           // unterminated
        "",                                       // empty
        "{\"cards\":[{\"uid\":\"04A1B2C3\"}]}xx", // trailing garbage
    };
    store_card_t cards[8];
    // one intentional exception: lower-case MUST fold to the same card (§2.4
    // uppercase-hex rule is enforced server-side; the terminal is tolerant).
    bool too_many = false;
    size_t n = sync_decode_allow_list(bad[1], strlen(bad[1]), cards, 8, &too_many);
    TEST_ASSERT_NOT_EQUAL(SIZE_MAX, n);
    TEST_ASSERT_EQUAL_UINT8(0x04, cards[0].uid[0]);
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        if (i == 1) {
            continue;
        }
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(
            SIZE_MAX, sync_decode_allow_list(bad[i], strlen(bad[i]), cards, 8, &too_many),
            bad[i]);
    }
    // A body WITHOUT a "cards" key is malformed — decoding it to 0 cards
    // would replace the cache and deny every card in the building.
    TEST_ASSERT_EQUAL_UINT32(SIZE_MAX,
                             sync_decode_allow_list("{}", 2, cards, 8, &too_many));
    // An explicit empty list is legitimate (revoke everyone).
    TEST_ASSERT_NOT_EQUAL(SIZE_MAX,
                          sync_decode_allow_list("{\"cards\":[]}", 12, cards, 8, &too_many));
    // Windows over UINT32_MAX: reject, never truncate.
    TEST_ASSERT_EQUAL_UINT32(
        SIZE_MAX,
        sync_decode_allow_list(
            "{\"cards\":[{\"uid\":\"04A1B2C3\",\"valid_until\":17290000000}]}", 60, cards,
            8, &too_many));
}

void test_decode_events_response_fixture(void)
{
    uint32_t acked = 0, rej[8];
    size_t nrej = 0;
    TEST_ASSERT_TRUE(sync_decode_events_response(fixture("events-response.json"),
                                                 strlen(fixture("events-response.json")), &acked,
                                                 rej, 8, &nrej));
    TEST_ASSERT_EQUAL_UINT32(1234, acked);
    TEST_ASSERT_EQUAL_size_t(0, nrej);
}

void test_decode_events_response_rejected_and_unknown(void)
{
    const char *body = "{\"acked_through\":20,\"extra\":[1,2],\"rejected\":[7,9,100]}";
    uint32_t acked = 0, rej[8];
    size_t nrej = 0;
    TEST_ASSERT_TRUE(sync_decode_events_response(body, strlen(body), &acked, rej, 8, &nrej));
    TEST_ASSERT_EQUAL_UINT32(20, acked);
    TEST_ASSERT_EQUAL_size_t(3, nrej);
    TEST_ASSERT_EQUAL_UINT32(7, rej[0]);
    TEST_ASSERT_EQUAL_UINT32(100, rej[2]);
    // More rejects than the buffer holds: full count still reported.
    const char *r3 = "{\"acked_through\":1,\"rejected\":[1,2,3]}";
    TEST_ASSERT_TRUE(sync_decode_events_response(r3, strlen(r3), &acked, rej, 1, &nrej));
    TEST_ASSERT_EQUAL_size_t(3, nrej);
}

void test_encode_event_matches_fixture_fields(void)
{
    // Pin the clock so the encoder derives the fixture's device_ts.
    clock_host_set_uptime_us(1000000);
    clock_on_time_source(1792742462ULL); // base: uptime 1 s = fixture time
    sync_event_t ev = fixture_event();
    char out[256];
    size_t n = sync_encode_event(&ev, 1234, out, sizeof(out));
    TEST_ASSERT_TRUE(n > 0);
    TEST_ASSERT_EQUAL_STRING(
        "{\"seq\":1234,\"reader\":\"in\",\"uid\":\"04A1B2C3\",\"result\":\"allow\","
        "\"device_ts\":\"2026-10-23T08:01:02Z\",\"time_synced\":true}",
        out);
}

void test_encode_event_unsynced_and_denied_out(void)
{
    // No clock: device_ts is the uptime stand-in, time_synced=false.
    clock_host_set_uptime_us(65000000);
    sync_event_t ev = { .reader = 1, .uid_len = 4, .uid = { 0xDE, 0xAD, 0xBE, 0xEF },
                        .result = 1, .uptime_us = 65000000 };
    char out[256];
    TEST_ASSERT_TRUE(sync_encode_event(&ev, 7, out, sizeof(out)) > 0);
    TEST_ASSERT_EQUAL_STRING(
        "{\"seq\":7,\"reader\":\"out\",\"uid\":\"DEADBEEF\",\"result\":\"deny\","
        "\"device_ts\":\"1970-01-01T00:01:05Z\",\"time_synced\":false}",
        out);
}

void test_encode_envelope_matches_fixture(void)
{
    // The fixture event, with the clock pinned so the derived device_ts is
    // the fixture's.
    clock_host_set_uptime_us(1000000);
    clock_on_time_source(1792742462ULL);
    store_event_t ev = { 0 };
    ev.seq = 1234;
    sync_event_t payload = fixture_event();
    memcpy(ev.bytes, &payload, sizeof(payload));
    ev.len = sizeof(payload);
    uint8_t stream[16] = { 0x6f, 0x1a, 0x2b, 0x3c, 0x00, 0x00, 0x40, 0x00,
                           0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01 };
    char out[1024];
    size_t n = sync_encode_envelope(stream, 0, &ev, 1, out, sizeof(out));
    TEST_ASSERT_TRUE(n > 0);
    char want[1024];
    strcpy(want, fixture("events-request.json"));
    minify(want);
    TEST_ASSERT_EQUAL_STRING(want, out);
}

// --- session ------------------------------------------------------------

static void seed_cache(void)
{
    store_card_t c = { .uid_len = 4, .uid = { 0x11 } };
    TEST_ASSERT_EQUAL(STORE_OK, store_allow_list_replace(&c, 1, "\"old-etag\"", 10));
}

void test_session_allow_list_200_replaces_cache(void)
{
    sync_host_enqueue(200, fixture("allow-list.json"), "\"etag-1\"", NULL);
    sync_host_enqueue(200, "{\"acked_through\":0,\"rejected\":[]}", NULL, NULL);
    TEST_ASSERT_TRUE(sync_session_run());
    store_card_t cards[4];
    char etag[STORE_ETAG_MAX];
    size_t n = 0, etag_len = 0;
    TEST_ASSERT_EQUAL(STORE_OK,
                      store_allow_list_read(cards, 4, &n, etag, sizeof(etag), &etag_len));
    TEST_ASSERT_EQUAL_size_t(1, n);
    TEST_ASSERT_EQUAL_MEMORY("\"etag-1\"", etag, 8);
    TEST_ASSERT_EQUAL_STRING("/v1/door/terminal/allow-list", sync_host_last_request.path);
}

void test_session_304_keeps_cache_and_sends_etag(void)
{
    seed_cache();
    sync_host_enqueue(304, NULL, NULL, NULL);
    sync_host_enqueue(200, "{\"acked_through\":0,\"rejected\":[]}", NULL, NULL);
    TEST_ASSERT_TRUE(sync_session_run());
    TEST_ASSERT_EQUAL_STRING("\"old-etag\"", sync_host_last_request.if_none_match);
    store_card_t cards[4];
    char etag[STORE_ETAG_MAX];
    size_t n = 0, etag_len = 0;
    TEST_ASSERT_EQUAL(STORE_OK,
                      store_allow_list_read(cards, 4, &n, etag, sizeof(etag), &etag_len));
    TEST_ASSERT_EQUAL_size_t(1, n); // the cached list is untouched
}

void test_session_no_events_skips_post(void)
{
    sync_host_enqueue(304, NULL, NULL, NULL);
    TEST_ASSERT_TRUE(sync_session_run());
    TEST_ASSERT_EQUAL_STRING("GET", sync_host_last_request.method); // no POST ran
}

void test_session_uploads_batch_and_acks(void)
{
    uint8_t payload[sizeof(sync_event_t)];
    sync_event_t ev = fixture_event();
    memcpy(payload, &ev, sizeof(ev));
    for (uint32_t i = 0; i < 3; i++) {
        TEST_ASSERT_TRUE(store_event_push(payload, sizeof(payload), NULL));
    }
    sync_host_enqueue(304, NULL, NULL, NULL);
    sync_host_enqueue(200, "{\"acked_through\":2,\"rejected\":[2]}", NULL, NULL);
    TEST_ASSERT_TRUE(sync_session_run());
    TEST_ASSERT_EQUAL_STRING("POST", sync_host_last_request.method);
    TEST_ASSERT_EQUAL_STRING("test-token", sync_host_last_request.token);
    // Envelope carried the batch: the stream UUID this device generated,
    // 3 events, dropped 0.
    uint8_t stream[16];
    store_stream_uuid(stream);
    char stream_prefix[64];
    strcpy(stream_prefix, "\"stream\":\"");
    static const char hx[] = "0123456789abcdef";
    for (int i = 0; i < 8; i++) { // first 4 bytes of the UUID, then the dash
        stream_prefix[10 + i * 2] = hx[stream[i] >> 4];
        stream_prefix[11 + i * 2] = hx[stream[i] & 0xF];
    }
    stream_prefix[18] = '-';
    stream_prefix[19] = '\0';
    TEST_ASSERT_TRUE(strstr(sync_host_last_request.body, stream_prefix) != NULL);
    TEST_ASSERT_TRUE(strstr(sync_host_last_request.body, "\"dropped\":0") != NULL);
    TEST_ASSERT_TRUE(strstr(sync_host_last_request.body, "\"seq\":3") != NULL);
    // acked_through 2 deletes seqs 1 and 2 (2 was rejected — same fate);
    // seq 3 stays queued.
    store_event_t out[STORE_BATCH_MAX];
    size_t n = store_event_batch(out, 10);
    TEST_ASSERT_EQUAL_size_t(1, n);
    TEST_ASSERT_EQUAL_UINT32(3, out[0].seq);
}

void test_session_reports_dropped_and_acks_it(void)
{
    uint8_t payload[sizeof(sync_event_t)];
    sync_event_t ev = fixture_event();
    memcpy(payload, &ev, sizeof(ev));
    for (size_t i = 0; i < 64512 + 2; i++) { // fill the ring: 2 drops
        (void)store_event_push(payload, sizeof(payload), NULL);
    }
    sync_host_enqueue(304, NULL, NULL, NULL);
    sync_host_enqueue(200, "{\"acked_through\":64512,\"rejected\":[]}", NULL, NULL);
    TEST_ASSERT_TRUE(sync_session_run());
    TEST_ASSERT_TRUE(strstr(sync_host_last_request.body, "\"dropped\":2") != NULL);
    TEST_ASSERT_EQUAL_UINT32(0, store_dropped_count()); // cleared only after the 200
}

void test_session_failed_post_keeps_batch_and_dropped(void)
{
    uint8_t payload[sizeof(sync_event_t)];
    sync_event_t ev = fixture_event();
    memcpy(payload, &ev, sizeof(ev));
    TEST_ASSERT_TRUE(store_event_push(payload, sizeof(payload), NULL));
    sync_host_enqueue(304, NULL, NULL, NULL);
    sync_host_enqueue(500, NULL, NULL, NULL); // POST fails server-side
    TEST_ASSERT_FALSE(sync_session_run());
    store_event_t out[STORE_BATCH_MAX];
    TEST_ASSERT_EQUAL_size_t(1, store_event_batch(out, 10)); // still queued
    sync_host_enqueue(304, NULL, NULL, NULL); // next session serves it again
    sync_host_enqueue(200, "{\"acked_through\":1,\"rejected\":[]}", NULL, NULL);
    TEST_ASSERT_TRUE(sync_session_run());
    TEST_ASSERT_EQUAL_size_t(0, store_event_batch(out, 10));
}

void test_session_transport_failure_keeps_state(void)
{
    uint8_t payload[sizeof(sync_event_t)];
    sync_event_t ev = fixture_event();
    memcpy(payload, &ev, sizeof(ev));
    TEST_ASSERT_TRUE(store_event_push(payload, sizeof(payload), NULL));
    TEST_ASSERT_FALSE(sync_session_run()); // empty script: Wi-Fi/GET fails
    store_event_t out[STORE_BATCH_MAX];
    TEST_ASSERT_EQUAL_size_t(1, store_event_batch(out, 10));
}

void test_session_oversized_body_is_transport_failure(void)
{
    // A body that does not fit the response buffer must FAIL, never arrive
    // as a silently truncated 200 (a truncated allow-list would freeze
    // revocations). The buffer is sized for STORE_CARDS_MAX cards; this
    // body is bigger.
    static char big[20 * 1024];
    memset(big, 'x', sizeof(big) - 1);
    sync_host_enqueue(200, big, NULL, NULL);
    TEST_ASSERT_FALSE(sync_session_run());
}

void test_session_clamps_ack_to_the_batch(void)
{
    uint8_t payload[sizeof(sync_event_t)];
    sync_event_t ev = fixture_event();
    memcpy(payload, &ev, sizeof(ev));
    for (uint32_t i = 0; i < 150; i++) { // two batches' worth
        TEST_ASSERT_TRUE(store_event_push(payload, sizeof(payload), NULL));
    }
    sync_host_enqueue(304, NULL, NULL, NULL);
    sync_host_enqueue(200, "{\"acked_through\":4294967295,\"rejected\":[]}", NULL, NULL);
    TEST_ASSERT_TRUE(sync_session_run());
    // A hostile/buggy acked_through must not delete the unqueued 50.
    store_event_t out[STORE_BATCH_MAX];
    size_t n = store_event_batch(out, 10);
    TEST_ASSERT_EQUAL_size_t(10, n);
    TEST_ASSERT_EQUAL_UINT32(101, out[0].seq);
}

void test_session_poison_record_dropped_not_blocking(void)
{
    uint8_t payload[sizeof(sync_event_t)];
    sync_event_t ev = fixture_event();
    memcpy(payload, &ev, sizeof(ev));
    TEST_ASSERT_TRUE(store_event_push(payload, sizeof(payload), NULL)); // seq 1
    TEST_ASSERT_TRUE(store_event_push(payload, sizeof(payload), NULL)); // seq 2
    uint8_t poison[sizeof(sync_event_t)];
    sync_event_t bad = { 0 }; // uid_len 0: fails to encode
    memcpy(poison, &bad, sizeof(bad));
    TEST_ASSERT_TRUE(store_event_push(poison, sizeof(poison), NULL)); // seq 3
    TEST_ASSERT_TRUE(store_event_push(payload, sizeof(payload), NULL)); // seq 4
    sync_host_enqueue(304, NULL, NULL, NULL);
    sync_host_enqueue(200, "{\"acked_through\":4,\"rejected\":[]}", NULL, NULL);
    TEST_ASSERT_TRUE(sync_session_run());
    // The envelope carried the 3 good records, not the poison.
    TEST_ASSERT_TRUE(strstr(sync_host_last_request.body, "\"seq\":4") != NULL);
    // The poison was dropped+counted, and everything through seq 4 is gone:
    // the queue is empty — a bad record never blocks it (§2.4).
    store_event_t out[STORE_BATCH_MAX];
    TEST_ASSERT_EQUAL_size_t(0, store_event_batch(out, 10));
    // The poison's drop went out WITH the batch and was acked with it.
    TEST_ASSERT_EQUAL_UINT32(0, store_dropped_count());
}

void test_session_rebases_every_session(void)
{
    // Drift correction: SNTP (system clock past the floor) re-bases EVERY
    // session, not just the first.
    clock_host_set_epoch(1792742462ULL);
    sync_host_enqueue(200, "{\"cards\":[]}", NULL, "Wed, 23 Oct 2026 08:01:02 GMT");
    TEST_ASSERT_TRUE(sync_session_run());
    uint64_t epoch;
    bool synced;
    clock_stamp(clock_boot_id(), clock_uptime_us(), &epoch, &synced);
    TEST_ASSERT_TRUE(synced);
    TEST_ASSERT_EQUAL_UINT64(1792742462ULL, epoch);
    // Next session: the system clock moved on (SNTP), the Date header is
    // stale — the SNTP time must win.
    clock_host_set_epoch(1792742462ULL + 600);
    sync_host_enqueue(200, "{\"cards\":[]}", NULL, "Wed, 23 Oct 2026 08:01:02 GMT");
    TEST_ASSERT_TRUE(sync_session_run());
    clock_stamp(clock_boot_id(), clock_uptime_us(), &epoch, &synced);
    TEST_ASSERT_EQUAL_UINT64(1792742462ULL + 600, epoch);
}

void test_decode_allow_list_long_unknown_keys_and_strings(void)
{
    // ADR §2.4: unknown fields are ignored — their LENGTH must not matter.
    static char body[20000];
    snprintf(body, sizeof(body),
             "{\"%6000s\":true,\"cards\":[{\"uid\":\"04A1B2C3\",\"%3000s\":"
             "\"%3000s\",\"note\":\"ok\"}],\"tail\":\"%5000s\"}",
             "K", "K", "V", "V");
    store_card_t cards[8];
    bool too_many = false;
    size_t n = sync_decode_allow_list(body, strlen(body), cards, 8, &too_many);
    TEST_ASSERT_NOT_EQUAL(SIZE_MAX, n);
    TEST_ASSERT_FALSE(too_many);
    TEST_ASSERT_EQUAL_size_t(1, n);
    TEST_ASSERT_EQUAL_UINT8(0x04, cards[0].uid[0]);
}

void test_session_unprovisioned_skips_everything(void)
{
    sync_host_set_provisioned(false);
    TEST_ASSERT_FALSE(sync_session_run());
    TEST_ASSERT_EQUAL_UINT8(0, sync_host_last_request.method[0]); // no request at all
}

void test_session_401_keeps_events_queued(void)
{
    uint8_t payload[sizeof(sync_event_t)];
    sync_event_t ev = fixture_event();
    memcpy(payload, &ev, sizeof(ev));
    TEST_ASSERT_TRUE(store_event_push(payload, sizeof(payload), NULL));
    sync_host_enqueue(401, NULL, NULL, NULL);
    sync_host_enqueue(401, NULL, NULL, NULL);
    TEST_ASSERT_FALSE(sync_session_run()); // refused: retry next session
    store_event_t out[STORE_BATCH_MAX];
    TEST_ASSERT_EQUAL_size_t(1, store_event_batch(out, 10));
}

void test_session_uses_date_header_when_unsynced(void)
{
    // SNTP did not converge in this session; the response Date header is the
    // fallback (§2.4) — later events stamp synced from it.
    sync_host_enqueue(200, "{\"cards\":[]}", NULL, "Wed, 23 Oct 2026 08:01:02 GMT");
    TEST_ASSERT_TRUE(sync_session_run());
    TEST_ASSERT_TRUE(clock_synced());
    uint64_t epoch;
    bool synced;
    clock_stamp(clock_boot_id(), clock_uptime_us(), &epoch, &synced);
    TEST_ASSERT_TRUE(synced);
    TEST_ASSERT_EQUAL_UINT64(1792742462ULL, epoch);
}

// --- backoff ------------------------------------------------------------

void test_next_delay_interval_when_clean(void)
{
    TEST_ASSERT_EQUAL_UINT32(60u * 60 * 1000, sync_next_delay_ms(true, 0));
    TEST_ASSERT_EQUAL_UINT32(60u * 60 * 1000, sync_next_delay_ms(true, 5));
}

void test_next_delay_backoff_grows_and_caps(void)
{
    // Jitter is deterministic in the host port (random = 42).
    uint32_t d1 = sync_next_delay_ms(false, 1);
    TEST_ASSERT_TRUE(d1 >= 30000 && d1 <= 37500);
    uint32_t d2 = sync_next_delay_ms(false, 2);
    TEST_ASSERT_TRUE(d2 >= 60000 && d2 <= 75000);
    TEST_ASSERT_TRUE(d2 > d1);
    uint32_t d9 = sync_next_delay_ms(false, 9);
    TEST_ASSERT_TRUE(d9 >= 480000 && d9 <= 600000); // capped at 8 min + jitter
    TEST_ASSERT_EQUAL_UINT32(sync_next_delay_ms(false, 20), d9);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_decode_allow_list_fixture);
    RUN_TEST(test_decode_allow_list_unknown_fields_tolerated);
    RUN_TEST(test_decode_allow_list_rejects_malformed);
    RUN_TEST(test_decode_events_response_fixture);
    RUN_TEST(test_decode_events_response_rejected_and_unknown);
    RUN_TEST(test_encode_event_matches_fixture_fields);
    RUN_TEST(test_encode_event_unsynced_and_denied_out);
    RUN_TEST(test_encode_envelope_matches_fixture);
    RUN_TEST(test_session_allow_list_200_replaces_cache);
    RUN_TEST(test_session_304_keeps_cache_and_sends_etag);
    RUN_TEST(test_session_no_events_skips_post);
    RUN_TEST(test_session_uploads_batch_and_acks);
    RUN_TEST(test_session_reports_dropped_and_acks_it);
    RUN_TEST(test_session_failed_post_keeps_batch_and_dropped);
    RUN_TEST(test_session_transport_failure_keeps_state);
    RUN_TEST(test_session_oversized_body_is_transport_failure);
    RUN_TEST(test_session_clamps_ack_to_the_batch);
    RUN_TEST(test_session_poison_record_dropped_not_blocking);
    RUN_TEST(test_session_rebases_every_session);
    RUN_TEST(test_decode_allow_list_long_unknown_keys_and_strings);
    RUN_TEST(test_session_unprovisioned_skips_everything);
    RUN_TEST(test_session_401_keeps_events_queued);
    RUN_TEST(test_session_uses_date_header_when_unsynced);
    RUN_TEST(test_next_delay_interval_when_clean);
    RUN_TEST(test_next_delay_backoff_grows_and_caps);
    return UNITY_END();
}
