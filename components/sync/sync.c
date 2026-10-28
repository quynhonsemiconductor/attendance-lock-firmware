// The bounded sync session (§2.4): allow-list poll with If-None-Match/304,
// event batch upload with ack/reject handling, dropped-counter reporting.
// Platform-free — radio, HTTP and credentials go through sync_port.h so the
// host tests run this file unchanged against a scripted transport.
#include "sync/sync.h"

#include <string.h>

#include "sdkconfig.h"

#include "clock/clock.h"
#include "store/store.h"
#include "sync_port.h"

#ifndef SYNC_HOST_BUILD
#include "esp_log.h"
static const char *TAG = "sync";
#define SYNC_LOGW(...) ESP_LOGW(TAG, __VA_ARGS__)
#else
#define SYNC_LOGW(...) do { } while (0)
#endif

// Response buffers: the allow-list buffer is SYNC_RESP_CAP (sync.h — worst
// case 256 cards of JSON); post_resp is sized per review for the ack/reject
// body. Static: sync-task-owned.
#define POST_RESP_CAP 2048

// Session buffers: file-scope, never on the sync task's stack; all uses are
// single-task (the door task never touches the store via this path).
static char s_resp[SYNC_RESP_CAP];
static char s_body[16384]; // 100 events × ~120 bytes each
static store_card_t s_cards[256];
static store_event_t s_batch[STORE_BATCH_MAX];
static uint32_t s_rejected[STORE_BATCH_MAX];

// One session:
//   Wi-Fi up → SNTP window starts → GET allow-list (ETag/304; a Date header
//   is the time fallback) → POST events in batches of ≤100 until the ring
//   is empty, the drain budget is spent or a round stops making progress
//   (the first POST runs every session — an empty ring still heartbeats the
//   measured battery) → SNTP stops → Wi-Fi down. Any step may fail; the
//   ring and the cache keep their state (§2.1: the door works without
//   opshub).
bool sync_session_run(void)
{
    char url[128], token[128];
    if (!sync_port_creds(url, sizeof(url), token, sizeof(token))) {
        return false; // unprovisioned: nothing to do
    }
    if (!sync_port_wifi_up()) {
        return false;
    }
    clock_sntp_start(); // bounded window, stopped below

    bool ok = true;

    // --- allow-list: If-None-Match with the ETag of the cached list -------
    char etag[STORE_ETAG_MAX] = { 0 };
    size_t etag_len = 0;
    store_allow_list_read(NULL, 0, NULL, etag, sizeof(etag), &etag_len);

    size_t resp_len = 0;
    long status = 0;
    char resp_etag[STORE_ETAG_MAX] = { 0 };
    char date[40] = { 0 };
    if (!sync_port_http("GET", url, "/v1/door/terminal/allow-list", token, etag, NULL, 0, &status,
                        s_resp, sizeof(s_resp), &resp_len, resp_etag, sizeof(resp_etag), date,
                        sizeof(date))) {
        ok = false; // transport failure: keep everything, backoff
    } else if (status == 200) {
        bool too_many = false;
        size_t n = sync_decode_allow_list(s_resp, resp_len, s_cards,
                                          sizeof(s_cards) / sizeof(s_cards[0]), &too_many);
        if (n == SIZE_MAX) {
            SYNC_LOGW("allow-list body malformed (len=%u)", (unsigned)resp_len);
            ok = false; // the old cache stays: an empty-looking body must
                        // never deny every card
        } else if (too_many) {
            SYNC_LOGW("allow-list too large: count>%u", (unsigned)STORE_CARDS_MAX);
            ok = false; // loud: the old cache stays, sync flags it upstream
        } else if (store_allow_list_replace(s_cards, n, resp_etag, strlen(resp_etag)) !=
                   STORE_OK) {
            SYNC_LOGW("allow-list cache write failed");
            ok = false;
        }
    } else if (status == 304) {
        // unchanged — the cached list stays active (§2.4)
    } else if (status == 401 || status == 403) {
        SYNC_LOGW("terminal credentials refused: status=%ld", status);
        ok = false; // credentials refused: retry next session
    } else {
        ok = false; // 5xx / anything else: keep state
    }

    // Bounded wait (≤ 5 s) for SNTP to actually converge before the re-base
    // decision — starting it and reading immediately would almost always
    // fall through to the Date header.
    for (int i = 0; i < 50 && clock_system_epoch() < CLOCK_SNTP_EPOCH_FLOOR; i++) {
        sync_port_delay_ms(100);
    }
    // Re-base EVERY session (drift correction): SNTP time wins when the
    // system clock is past the floor, otherwise the Date header — before
    // the events are stamped.
    if (clock_system_epoch() >= CLOCK_SNTP_EPOCH_FLOOR) {
        clock_on_time_source(clock_system_epoch());
    } else if (date[0] != '\0') {
        uint64_t epoch;
        if (clock_parse_http_date(date, &epoch)) {
            clock_on_time_source(epoch);
        }
    }

    // --- events: drain in batches of ≤100, oldest first -------------------
    uint8_t stream[16];
    store_stream_uuid(stream);
    uint16_t battery_mv = 0;
    bool low_battery = false;
    sync_port_battery(&battery_mv, &low_battery); // live reading, sent
    // every session: the server's terminal TTL (150 min > the 60 min
    // interval) and battery badge key on the envelope fields.
    uint32_t dropped_from_store = store_dropped_count(); // rides the FIRST
    bool dropped_pending = dropped_from_store > 0;       // envelope only;
    // acked against the store counter after that 200.
    const int64_t drain_start_us = clock_uptime_us();
    static char s_ev_probe[256];
    for (size_t round = 0;; round++) {
        size_t n = store_event_batch(s_batch, STORE_BATCH_MAX);
        if (round > 0 && n == 0) {
            break; // drained (round 0 always POSTs: the heartbeat)
        }
        if (round > 0 &&
            (uint64_t)(clock_uptime_us() - drain_start_us) / 1000u >= SYNC_DRAIN_BUDGET_MS) {
            SYNC_LOGW("drain budget spent: queued events remain");
            break; // session stays bounded; the next session continues
        }
        // A record that fails to encode (corrupt payload) is dropped and
        // counted locally — it must never block the queue (§2.4). It is
        // excluded from the envelope here and deleted together with the
        // ack below; a repeated failure over-counts drops once per session.
        size_t n_good = 0;
        uint32_t bad_max_seq = 0;
        uint32_t dropped = dropped_pending ? dropped_from_store : 0;
        for (size_t i = 0; i < n; i++) {
            if (sync_encode_event((const sync_event_t *)s_batch[i].bytes, s_batch[i].seq,
                                  s_ev_probe, sizeof(s_ev_probe)) == 0) {
                dropped++; // poison records are counted and REPORTED, but
                if (s_batch[i].seq > bad_max_seq) { // never acked against the
                    bad_max_seq = s_batch[i].seq;  // store's own counter
                }
                SYNC_LOGW("event encode failed: seq=%u", s_batch[i].seq);
                continue;
            }
            s_batch[n_good++] = s_batch[i];
        }
        size_t body_len = sync_encode_envelope(stream, dropped, battery_mv, low_battery, s_batch,
                                               n_good, s_body, sizeof(s_body));
        long post_status = 0;
        static char post_resp[POST_RESP_CAP];
        size_t post_resp_len = 0;
        if (body_len == 0 ||
            !sync_port_http("POST", url, "/v1/door/terminal/events", token, "", s_body, body_len,
                            &post_status, post_resp, sizeof(post_resp), &post_resp_len, NULL, 0,
                            NULL, 0)) {
            ok = false; // nothing acked: the batch stays queued (peek semantics)
            break;
        }
        if (post_status != 200) {
            ok = false;
            break;
        }
        uint32_t acked = 0;
        size_t rejected_len = 0;
        if (!sync_decode_events_response(post_resp, post_resp_len, &acked, s_rejected,
                                         sizeof(s_rejected) / sizeof(s_rejected[0]),
                                         &rejected_len)) {
            ok = false; // malformed 200: resend is free (ON CONFLICT DO NOTHING)
            break;
        }
        // Never ack past what this batch actually carried.
        uint32_t batch_max = n_good > 0 ? s_batch[n_good - 1].seq : 0;
        if (bad_max_seq > batch_max) {
            batch_max = bad_max_seq;
        }
        if (acked > batch_max) {
            acked = batch_max;
        }
        store_ack(acked); // accepted AND rejected seqs die together
        if (dropped_pending) {
            store_dropped_ack(dropped_from_store); // only the store-read
            dropped_pending = false; // value: the poison additions above were
            // already REPORTED in this envelope and must not be subtracted
            // from a counter that never included them
        }
        if (rejected_len > 0) {
            SYNC_LOGW("events rejected: count=%u", (unsigned)rejected_len);
        }
        if (acked == 0 && n > 0) {
            // 200 but nothing accepted: re-sending the same batch inside
            // this session would only hammer the server — back off to the
            // next session (the ring keeps everything).
            ok = false;
            break;
        }
    }

    clock_sntp_stop();
    sync_port_wifi_down();
    return ok;
}

void sync_init(void)
{
    sync_port_task_start();
}

// Delay until the next session: the configured interval when the last one
// was clean; on failure an exponential backoff (30 s doubling, capped at
// 8 min) plus 0-25 % jitter so a fleet of terminals does not stampede one
// server. Deep sleep replaces this delay once the main loop arms it (PR 4).
uint32_t sync_next_delay_ms(bool last_ok, uint32_t consecutive_fails)
{
    const uint32_t interval = CONFIG_SYNC_INTERVAL_MIN * 60u * 1000u;
    if (last_ok || consecutive_fails == 0) {
        return interval;
    }
    uint32_t shift = consecutive_fails - 1;
    if (shift > 4) {
        shift = 4;
    }
    uint32_t backoff = 30000u << shift;
    uint32_t jitter = sync_port_random_u32() % (backoff / 4 + 1);
    return backoff + jitter;
}
