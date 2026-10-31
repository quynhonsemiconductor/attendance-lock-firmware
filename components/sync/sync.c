// The bounded sync session (§2.4): allow-list poll with If-None-Match/304,
// event batch upload with ack/reject handling, dropped-counter reporting.
// Platform-free — radio, HTTP and credentials go through sync_port.h so the
// host tests run this file unchanged against a scripted transport.
#include "sync/sync.h"

#include <stdio.h>
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

// Outcome of the last session, for the summary line (§ bench logs).
static sync_session_result_t s_last;

static void detail_set(char *dst, size_t cap, const char *src)
{
    size_t n = 0;
    for (; src[n] != '\0' && n + 1 < cap; n++) {
        dst[n] = src[n];
    }
    dst[n] = '\0';
}

// Records the FIRST failure of the session: later steps may fail as a
// consequence (no point reporting a POST timeout after the token was
// refused), and the bench operator acts on the first cause.
static void result_fail(sync_session_result_t *r, sync_stage_t stage, long http_status, int code,
                        const char *detail)
{
    if (r->fail_stage != SYNC_STAGE_NONE) {
        return;
    }
    r->fail_stage = stage;
    r->fail_http_status = http_status;
    r->fail_code = code;
    detail_set(r->fail_detail, sizeof(r->fail_detail), detail);
}

// The user-facing text for a transport stage the port reported.
static void result_fail_transport(sync_session_result_t *r, const sync_fail_t *fail)
{
    switch (fail != NULL ? fail->stage : SYNC_STAGE_NONE) {
    case SYNC_STAGE_TLS:
        result_fail(r, SYNC_STAGE_TLS, 0, fail->code, "TLS handshake failed");
        break;
    case SYNC_STAGE_CONFIG:
        result_fail(r, SYNC_STAGE_CONFIG, 0, fail->code, "request setup failed");
        break;
    case SYNC_STAGE_HTTP:
        // A response that arrived but was unusable (body over the buffer).
        result_fail(r, SYNC_STAGE_HTTP, 0, fail->code, "response body over the buffer");
        break;
    case SYNC_STAGE_WIFI:
    case SYNC_STAGE_IP:
    case SYNC_STAGE_SNTP:
        result_fail(r, fail->stage, 0, fail->code, "no HTTP status");
        break;
    default:
        // The device port reports resolve/connect as SYNC_STAGE_DNS;
        // anything else that reached here has no HTTP status.
        result_fail(r, SYNC_STAGE_DNS, 0, fail != NULL ? fail->code : 0,
                    "cannot resolve or connect");
        break;
    }
}

void sync_session_result(sync_session_result_t *out)
{
    if (out != NULL) {
        *out = s_last;
    }
}

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
    sync_session_result_t r;
    memset(&r, 0, sizeof(r));

    char url[128], token[128];
    if (!sync_port_creds(url, sizeof(url), token, sizeof(token))) {
        result_fail(&r, SYNC_STAGE_CONFIG, 0, 0,
                    "unprovisioned — run `set url` and `set token`");
        s_last = r;
        return false; // unprovisioned: nothing to do
    }
    // Reader UID integrity since the previous session: reported even when
    // this session fails, so a bad reader is visible from the log alone.
    r.misreads = sync_port_take_misreads();
    sync_fail_t fail = { SYNC_STAGE_NONE, 0 };
    if (!sync_port_wifi_up(&fail)) {
        if (fail.stage == SYNC_STAGE_IP) {
            result_fail(&r, SYNC_STAGE_IP, 0, fail.code, "associated, no IP (DHCP)");
        } else if (fail.stage == SYNC_STAGE_CONFIG) {
            result_fail(&r, SYNC_STAGE_CONFIG, 0, fail.code,
                        "no Wi-Fi credentials — run `set wifi ssid` and `set wifi pass`");
        } else {
            result_fail(&r, SYNC_STAGE_WIFI, 0, fail.code, "radio did not connect");
        }
        s_last = r;
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
    fail = (sync_fail_t){ SYNC_STAGE_NONE, 0 };
    if (!sync_port_http("GET", url, "/v1/door/terminal/allow-list", token, etag, NULL, 0, &status,
                        s_resp, sizeof(s_resp), &resp_len, resp_etag, sizeof(resp_etag), date,
                        sizeof(date), &fail)) {
        ok = false; // transport failure: keep everything, backoff
        result_fail_transport(&r, &fail);
    } else {
        r.allow_list_status = status;
        if (status == 200) {
            bool too_many = false;
            size_t n = sync_decode_allow_list(s_resp, resp_len, s_cards,
                                              sizeof(s_cards) / sizeof(s_cards[0]), &too_many);
            if (n == SIZE_MAX) {
                SYNC_LOGW("allow-list body malformed (len=%u)", (unsigned)resp_len);
                ok = false; // the old cache stays: an empty-looking body must
                            // never deny every card
                result_fail(&r, SYNC_STAGE_HTTP, 200, 0, "allow-list body malformed");
            } else if (too_many) {
                SYNC_LOGW("allow-list too large: count>%u", (unsigned)STORE_CARDS_MAX);
                ok = false; // loud: the old cache stays, sync flags it upstream
                result_fail(&r, SYNC_STAGE_HTTP, 200, 0, "allow-list too large");
            } else if (store_allow_list_replace(s_cards, n, resp_etag, strlen(resp_etag)) !=
                       STORE_OK) {
                SYNC_LOGW("allow-list cache write failed");
                ok = false;
                result_fail(&r, SYNC_STAGE_HTTP, 200, 0, "allow-list cache write failed");
            }
        } else if (status == 304) {
            // unchanged — the cached list stays active (§2.4)
        } else if (status == 401 || status == 403) {
            SYNC_LOGW("terminal credentials refused: status=%ld", status);
            ok = false; // credentials refused: retry next session
            result_fail(&r, SYNC_STAGE_HTTP, status, 0,
                        "terminal token refused: check `set token`");
        } else {
            ok = false; // 5xx / anything else: keep state
            result_fail(&r, SYNC_STAGE_HTTP, status, 0, "unexpected status");
        }
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
    bool have_time = false;
    if (clock_system_epoch() >= CLOCK_SNTP_EPOCH_FLOOR) {
        clock_on_time_source(clock_system_epoch());
        have_time = true;
    } else if (date[0] != '\0') {
        uint64_t epoch;
        if (clock_parse_http_date(date, &epoch)) {
            clock_on_time_source(epoch);
            have_time = true;
        }
    }
    if (!have_time) {
        // Not a session failure (contract: device_ts 1970 + time_synced
        // false is correct), but the bench operator must see why.
        r.time_unsynced = true;
        SYNC_LOGW("no time source (SNTP and Date failed): events stamped 1970, time_synced=false");
    }

    // --- events: drain in batches of ≤100, oldest first -------------------
    uint8_t stream[16];
    store_stream_uuid(stream);
    uint16_t battery_mv = 0;
    bool low_battery = false;
    bool battery_fault = false;
    sync_port_battery(&battery_mv, &low_battery, &battery_fault); // live reading, sent
    // every session: the server's terminal TTL (150 min > the 60 min
    // interval) and battery badge key on the envelope fields.
    uint32_t dropped_from_store = store_dropped_count(); // rides the FIRST
    bool dropped_pending = dropped_from_store > 0;       // envelope only;
    // acked against the store counter after that 200.
    const int64_t drain_start_us = clock_uptime_us();
    uint32_t last_sent_seq = 0; // distinct sent events (a re-sent batch
                                // after a partial ack is not re-counted)
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
        for (size_t i = 0; i < n_good; i++) {
            if (s_batch[i].seq > last_sent_seq) {
                last_sent_seq = s_batch[i].seq;
                r.events_sent++;
            }
        }
        // A batch of ONLY corrupt records can never upload — waiting for a
        // server ack would wedge the ring in ok=false/backoff forever. Clear
        // the poison seqs locally; the dropped REPORT still rides this
        // envelope (round-2 review item 2).
        bool cleared_poison = false;
        if (n_good == 0 && bad_max_seq > 0) {
            store_ack(bad_max_seq);
            cleared_poison = true;
            SYNC_LOGW("batch all-corrupt: cleared through seq=%u locally",
                      (unsigned)bad_max_seq);
        }
        size_t body_len = sync_encode_envelope(stream, dropped, r.misreads, battery_mv,
                                               low_battery, battery_fault, s_batch, n_good,
                                               s_body, sizeof(s_body));
        if (body_len == 0) {
            ok = false;
            result_fail(&r, SYNC_STAGE_CONFIG, 0, 0, "events envelope too large");
            break;
        }
        long post_status = 0;
        static char post_resp[POST_RESP_CAP];
        size_t post_resp_len = 0;
        fail = (sync_fail_t){ SYNC_STAGE_NONE, 0 };
        if (!sync_port_http("POST", url, "/v1/door/terminal/events", token, "", s_body, body_len,
                            &post_status, post_resp, sizeof(post_resp), &post_resp_len, NULL, 0,
                            NULL, 0, &fail)) {
            ok = false; // nothing acked: the batch stays queued (peek semantics)
            result_fail_transport(&r, &fail);
            break;
        }
        if (post_status != 200) {
            ok = false;
            if (post_status == 401 || post_status == 403) {
                result_fail(&r, SYNC_STAGE_HTTP, post_status, 0,
                            "terminal token refused: check `set token`");
            } else if (post_status == 422) {
                // Round-3 bench finding: the body names the rejected field
                // (battery_mv out of range); 120 bytes is what fits a line.
                result_fail(&r, SYNC_STAGE_HTTP, 422, 0, "server rejected the envelope");
                SYNC_LOGW("events envelope rejected (422): %.120s", post_resp);
            } else {
                result_fail(&r, SYNC_STAGE_HTTP, post_status, 0, "unexpected status");
            }
            break;
        }
        uint32_t acked = 0;
        size_t rejected_len = 0;
        if (!sync_decode_events_response(post_resp, post_resp_len, &acked, s_rejected,
                                         sizeof(s_rejected) / sizeof(s_rejected[0]),
                                         &rejected_len)) {
            ok = false; // malformed 200: resend is free (ON CONFLICT DO NOTHING)
            result_fail(&r, SYNC_STAGE_HTTP, 200, 0, "events response malformed");
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
        for (size_t i = 0; i < n_good; i++) {
            if (s_batch[i].seq <= acked) {
                r.events_acked++; // rejected seqs are acked too: same fate
            }
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
        if (acked == 0 && n > 0 && !cleared_poison) {
            // 200 but nothing accepted: re-sending the same batch inside
            // this session would only hammer the server — back off to the
            // next session (the ring keeps everything). A cleared-poison
            // round DID make progress, poison or not.
            ok = false;
            result_fail(&r, SYNC_STAGE_HTTP, 200, 0, "200 but nothing acked");
            break;
        }
    }

    clock_sntp_stop();
    sync_port_wifi_down();
    r.ok = ok;
    s_last = r;
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

static const char *stage_name(sync_stage_t stage)
{
    switch (stage) {
    case SYNC_STAGE_CONFIG: return "config";
    case SYNC_STAGE_WIFI:   return "wifi";
    case SYNC_STAGE_IP:     return "ip";
    case SYNC_STAGE_SNTP:   return "sntp";
    case SYNC_STAGE_DNS:    return "dns";
    case SYNC_STAGE_TLS:    return "tls";
    case SYNC_STAGE_HTTP:   return "http";
    default:                return "unknown";
    }
}

// One line per session, success included (round-3 bench finding: the log
// said nothing about WHY a session failed or WHEN the next would run).
// "next in" is rounded up; "backoff" counts consecutive failures capped at
// the 5 backoff steps the delay function knows.
size_t sync_format_summary(const sync_session_result_t *r, uint32_t consecutive_fails,
                           uint32_t next_delay_ms, char *out, size_t cap)
{
    if (r == NULL || out == NULL || cap == 0) {
        return 0;
    }
    char al[16];
    if (r->allow_list_status == 0) {
        detail_set(al, sizeof(al), "none"); // the GET never produced a status
    } else {
        snprintf(al, sizeof(al), "%ld", r->allow_list_status);
    }
    uint32_t backoff = consecutive_fails > 5 ? 5 : consecutive_fails;
    uint32_t secs = (next_delay_ms + 999u) / 1000u;
    int n;
    if (r->ok) {
        n = snprintf(out, cap,
                     "session ok%s allow-list=%s events=%u/%u misreads=%u next in %u s "
                     "(backoff %u/5)",
                     r->time_unsynced ? " (sntp: no time source)" : "", al,
                     (unsigned)r->events_acked, (unsigned)r->events_sent, (unsigned)r->misreads,
                     (unsigned)secs, (unsigned)backoff);
    } else {
        char stage[24];
        if (r->fail_stage == SYNC_STAGE_HTTP && r->fail_http_status != 0) {
            snprintf(stage, sizeof(stage), "http %ld", r->fail_http_status);
        } else if (r->fail_stage == SYNC_STAGE_HTTP) {
            detail_set(stage, sizeof(stage), "http");
        } else {
            detail_set(stage, sizeof(stage), stage_name(r->fail_stage));
        }
        char why[160];
        if ((r->fail_stage == SYNC_STAGE_DNS || r->fail_stage == SYNC_STAGE_TLS) &&
            r->fail_code != 0) {
            snprintf(why, sizeof(why), "%s, code 0x%x", r->fail_detail, r->fail_code);
        } else {
            detail_set(why, sizeof(why), r->fail_detail);
        }
        n = snprintf(out, cap,
                     "session failed (%s: %s) allow-list=%s events=%u/%u misreads=%u next in %u s "
                     "(backoff %u/5)",
                     stage, why, al, (unsigned)r->events_acked, (unsigned)r->events_sent,
                     (unsigned)r->misreads, (unsigned)secs, (unsigned)backoff);
    }
    if (n < 0 || (size_t)n >= cap) {
        out[0] = '\0';
        return 0;
    }
    return (size_t)n;
}
