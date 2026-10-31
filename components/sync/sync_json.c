// JSON codec for the door-terminal v1 contract (§2.4): encode the events
// envelope, decode the allow-list and the events response. Unknown fields
// are skipped on decode (the contract may gain fields). Platform-free,
// libc-free (strtoul-free: explicit digit loops) — host-tested against the
// contract fixtures.
#include "sync/sync.h"

#include <string.h>

#include "clock/clock.h"

// --- small emit helpers ------------------------------------------------

typedef struct {
    char *buf;
    size_t cap;
    size_t len;
    bool ok;
} emit_t;

static void emit_raw(emit_t *e, const char *s, size_t n)
{
    if (!e->ok || e->len + n > e->cap) {
        e->ok = false;
        return;
    }
    memcpy(e->buf + e->len, s, n);
    e->len += n;
}

static void emit_ch(emit_t *e, char c)
{
    emit_raw(e, &c, 1);
}

static void emit_str(emit_t *e, const char *s)
{
    emit_raw(e, s, strlen(s));
}

static void emit_u32(emit_t *e, uint32_t v)
{
    char tmp[10];
    size_t n = 0;
    do {
        tmp[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v > 0);
    for (size_t i = 0; i < n; i++) {
        emit_ch(e, tmp[n - 1 - i]);
    }
}

static void emit_hex_upper(emit_t *e, const uint8_t *bytes, size_t n)
{
    static const char hex[] = "0123456789ABCDEF";
    for (size_t i = 0; i < n; i++) {
        emit_ch(e, hex[bytes[i] >> 4]);
        emit_ch(e, hex[bytes[i] & 0x0F]);
    }
}

// Lower-case hex for the stream UUID.
static void emit_hex_lower(emit_t *e, const uint8_t *bytes, size_t n)
{
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        emit_ch(e, hex[bytes[i] >> 4]);
        emit_ch(e, hex[bytes[i] & 0x0F]);
    }
}

size_t sync_encode_event(const sync_event_t *ev, uint32_t seq, char *out, size_t cap)
{
    // The server enforces {4,7,10}-byte UIDs — an odd-length UID would be
    // rejected per-event and the record would stick in the queue forever.
    if (ev == NULL || out == NULL ||
        (ev->uid_len != 4 && ev->uid_len != 7 && ev->uid_len != 10)) {
        return 0;
    }
    emit_t e = { .buf = out, .cap = cap, .len = 0, .ok = true };
    uint64_t epoch;
    bool synced;
    clock_stamp(ev->boot_id, (int64_t)ev->uptime_us, &epoch, &synced);
    char ts[21];
    clock_iso8601(epoch, ts, sizeof(ts));

    emit_str(&e, "{\"seq\":");
    emit_u32(&e, seq);
    emit_str(&e, ",\"reader\":\"");
    emit_str(&e, ev->reader == 0 ? "in" : "out");
    emit_str(&e, "\",\"uid\":\"");
    emit_hex_upper(&e, ev->uid, ev->uid_len);
    emit_str(&e, "\",\"result\":\"");
    emit_str(&e, ev->result == 0 ? "allow" : "deny");
    emit_str(&e, "\",\"device_ts\":\"");
    emit_raw(&e, ts, 20);
    emit_str(&e, "\",\"time_synced\":");
    emit_str(&e, synced ? "true" : "false");
    emit_ch(&e, '}');
    if (e.ok && e.len < e.cap) {
        e.buf[e.len] = '\0';
    }
    return e.ok ? e.len : 0;
}

size_t sync_encode_envelope(const uint8_t stream[16], uint32_t dropped, uint32_t misreads,
                            uint16_t battery_mv, bool low_battery, bool battery_fault,
                            const store_event_t *events, size_t n_events, char *out, size_t cap)
{
    if (stream == NULL || out == NULL || (events == NULL && n_events > 0)) {
        return 0;
    }
    if (dropped > SYNC_DROPPED_REPORT_MAX) {
        dropped = SYNC_DROPPED_REPORT_MAX; // server-capped field: report the
        // cap, keep the true count in the store
    }
    emit_t e = { .buf = out, .cap = cap, .len = 0, .ok = true };
    emit_str(&e, "{\"stream\":\"");
    static const uint8_t dash_at[16] = { 4, 6, 8, 10 }; // 8-4-4-4-12 layout
    (void)dash_at;
    for (size_t i = 0; i < 16; i++) {
        if (i == 4 || i == 6 || i == 8 || i == 10) {
            emit_ch(&e, '-');
        }
        emit_hex_lower(&e, &stream[i], 1);
    }
    emit_str(&e, "\",\"dropped\":");
    emit_u32(&e, dropped);
    if (misreads != 0) {
        // Optional (unknown-field-tolerant server): reader UID integrity
        // for this session. Omitted when 0 so a healthy heartbeat keeps the
        // contract fixture's exact bytes.
        emit_str(&e, ",\"misreads\":");
        emit_u32(&e, misreads);
    }
    if (battery_fault) {
        // The reading exists but cannot be a pack voltage (divider fault):
        // never send an implausible battery_mv — the server must not reject
        // the whole batch (round-3 bench finding) or show "34 V".
        emit_str(&e, ",\"battery_fault\":true");
    } else if (battery_mv != 0) { // 0 = no reading this session: omit BOTH
        // fields — the server must not read an absent pack as 0 mV and alert
        emit_str(&e, ",\"battery_mv\":"); // (round-2 review item 1)
        emit_u32(&e, battery_mv);
        emit_str(&e, ",\"low_battery\":");
        emit_str(&e, low_battery ? "true" : "false");
    }
    emit_str(&e, ",\"events\":[");
    for (size_t i = 0; i < n_events; i++) {
        if (i > 0) {
            emit_ch(&e, ',');
        }
        char evbuf[256];
        size_t n = sync_encode_event((const sync_event_t *)events[i].bytes, events[i].seq,
                                     evbuf, sizeof(evbuf));
        if (n == 0) {
            return 0;
        }
        emit_raw(&e, evbuf, n);
    }
    emit_str(&e, "]}");
    if (e.ok && e.len < e.cap) {
        e.buf[e.len] = '\0';
    }
    return e.ok ? e.len : 0;
}

// --- response-body accumulator (round-2 review item 4) ------------------
//
// Bounded sink for streamed body chunks (the ESP port feeds it from
// HTTP_EVENT_ON_DATA). Same rule as the host port: cap-1 usable bytes, and
// once more data arrives than fits the accumulator latches overflow — an
// oversized body must never arrive as a silently truncated 200.

void sync_body_acc_init(sync_body_acc_t *acc, char *buf, size_t cap)
{
    acc->buf = buf;
    acc->cap = cap;
    acc->len = 0;
    acc->overflow = false;
}

bool sync_body_acc_add(sync_body_acc_t *acc, const char *data, size_t n)
{
    if (!acc->overflow && acc->buf != NULL) {
        if (acc->len + n > acc->cap - 1) {
            acc->overflow = true;
        } else {
            memcpy(acc->buf + acc->len, data, n);
            acc->len += n;
        }
    }
    return !acc->overflow; // callers keep draining, then fail the exchange
}

// --- decode: a small skipping JSON scanner ------------------------------
//
// Not a general JSON library: it walks the two documented shapes, tolerates
// unknown fields anywhere, and rejects structurally broken input. Strings in
// these payloads contain no escapes (hex UIDs, ISO timestamps), so a
// backslash inside a string is treated as malformed.

typedef struct {
    const char *s;
    size_t len;
    size_t i;
    bool ok;
} scan_t;

static void scan_ws(scan_t *p)
{
    while (p->i < p->len && (p->s[p->i] == ' ' || p->s[p->i] == '\t' || p->s[p->i] == '\n' ||
                             p->s[p->i] == '\r')) {
        p->i++;
    }
}

static bool scan_ch(scan_t *p, char c)
{
    // A mismatch is a false return, NOT a poison: callers probe ',' and '}'
    // as alternatives — p->ok only dies on the structural failures below.
    scan_ws(p);
    if (p->i < p->len && p->s[p->i] == c) {
        p->i++;
        return true;
    }
    return false;
}

static bool scan_skip_value_d(scan_t *p, int depth);

// Skips one string literal of ANY length (ADR §2.4: unknown fields are
// ignored — their size must not matter). Backslash escapes are stepped over.
static bool scan_skip_string(scan_t *p)
{
    scan_ws(p);
    if (!p->ok || p->i >= p->len || p->s[p->i] != '"') {
        p->ok = false;
        return false;
    }
    p->i++;
    while (p->i < p->len) {
        if (p->s[p->i] == '\\' && p->i + 1 < p->len) {
            p->i += 2;
            continue;
        }
        if (p->s[p->i] == '"') {
            p->i++;
            return true;
        }
        p->i++;
    }
    p->ok = false;
    return false;
}

// Reads a string literal into out. A string that does not FIT is fully
// consumed and *too_long = true (callers treat it as an unknown field);
// malformed input is false.
static bool scan_string_relaxed(scan_t *p, char *out, size_t cap, bool *too_long)
{
    scan_ws(p);
    *too_long = false;
    if (!p->ok || p->i >= p->len || p->s[p->i] != '"') {
        p->ok = false;
        return false;
    }
    p->i++;
    size_t n = 0;
    while (p->i < p->len && p->s[p->i] != '"') {
        if (p->s[p->i] == '\\' && p->i + 1 < p->len) { // escaped char: cannot be
            p->i += 2;                            // a known-field name/value
            *too_long = true;
            continue;
        }
        if (n + 1 < cap) {
            out[n++] = p->s[p->i];
        } else {
            *too_long = true;
        }
        p->i++;
    }
    if (p->i >= p->len) {
        p->ok = false;
        return false;
    }
    p->i++; // closing quote
    out[n < cap ? n : cap - 1] = '\0';
    return true;
}

static bool scan_skip_value(scan_t *p)
{
    return scan_skip_value_d(p, 0);
}

// Reads a string literal into out (NUL-terminated, truncation = failure).
static bool scan_string(scan_t *p, char *out, size_t cap)
{
    scan_ws(p);
    if (!p->ok || p->i >= p->len || p->s[p->i] != '"') {
        p->ok = false;
        return false;
    }
    p->i++;
    size_t n = 0;
    while (p->i < p->len && p->s[p->i] != '"') {
        if (p->s[p->i] == '\\' || n + 1 >= cap) {
            p->ok = false;
            return false;
        }
        out[n++] = p->s[p->i++];
    }
    if (p->i >= p->len) {
        p->ok = false;
        return false;
    }
    p->i++; // closing quote
    out[n] = '\0';
    return true;
}

static bool scan_uint(scan_t *p, uint64_t *out)
{
    scan_ws(p);
    uint64_t v = 0;
    size_t start = p->i;
    while (p->i < p->len && p->s[p->i] >= '0' && p->s[p->i] <= '9') {
        if (v > (UINT64_MAX - 9) / 10) {
            p->ok = false;
            return false;
        }
        v = v * 10 + (uint64_t)(p->s[p->i++] - '0');
    }
    if (p->i == start) {
        p->ok = false;
        return false;
    }
    *out = v;
    return true;
}

#define SKIP_MAX_DEPTH 16

// Skips one JSON value of any type (object, array, string, number,
// true/false/null) — the unknown-field tolerance (§2.4). Depth-capped so
// nested garbage cannot overflow the sync task's stack.
static bool scan_skip_value_d(scan_t *p, int depth)
{
    if (depth > SKIP_MAX_DEPTH) {
        p->ok = false;
        return false;
    }
    scan_ws(p);
    if (p->i >= p->len) {
        p->ok = false;
        return false;
    }
    char c = p->s[p->i];
    if (c == '"') {
        return scan_skip_string(p); // any length: unknown fields are ignored
    }
    if (c == '{' || c == '[') {
        char close = c == '{' ? '}' : ']';
        p->i++;
        scan_ws(p);
        if (p->i < p->len && p->s[p->i] == close) {
            p->i++;
            return true;
        }
        for (;;) {
            if (c == '{') {
                char key[64];
                bool tl;
                if (!scan_string_relaxed(p, key, sizeof(key), &tl) || !scan_ch(p, ':')) {
                    return false;
                }
                (void)tl; // keys inside unknown objects are unknown too
            }
            if (!scan_skip_value_d(p, depth + 1)) {
                return false;
            }
            scan_ws(p);
            if (p->i >= p->len) {
                p->ok = false;
                return false;
            }
            if (p->s[p->i] == ',') {
                p->i++;
                continue;
            }
            if (p->s[p->i] == close) {
                p->i++;
                return true;
            }
            p->ok = false;
            return false;
        }
    }
    if (c == 't' || c == 'f' || c == 'n') { // true / false / null
        while (p->i < p->len && p->s[p->i] >= 'a' && p->s[p->i] <= 'z') {
            p->i++;
        }
        return p->i > 0;
    }
    // number
    size_t start = p->i;
    while (p->i < p->len) {
        char k = p->s[p->i];
        bool part = (k >= '0' && k <= '9') || k == '-' || k == '+' || k == '.' || k == 'e' ||
                    k == 'E';
        if (!part) {
            break;
        }
        p->i++;
    }
    return p->i > start;
}

// Parses "null" | <uint>; *is_null set accordingly.
static bool scan_uint_or_null(scan_t *p, uint64_t *out, bool *is_null)
{
    scan_ws(p);
    if (p->i + 4 <= p->len && strncmp(p->s + p->i, "null", 4) == 0) {
        p->i += 4;
        *is_null = true;
        *out = 0;
        return true;
    }
    *is_null = false;
    return scan_uint(p, out);
}

size_t sync_decode_allow_list(const char *body, size_t len, store_card_t *out,
                              size_t max_cards, bool *too_many)
{
    scan_t p = { .s = body, .len = len, .i = 0, .ok = true };
    char key[24];
    size_t n = 0;
    bool have_cards = false, too_many_local = false;
    if (too_many != NULL) {
        *too_many = false;
    }
    if (!scan_ch(&p, '{')) {
        return SIZE_MAX;
    }
    scan_ws(&p);
    if (p.i < p.len && p.s[p.i] == '}') {
        p.i++;
    } else {
        for (;;) {
            bool key_tl;
            if (!scan_string_relaxed(&p, key, sizeof(key), &key_tl) || !scan_ch(&p, ':')) {
                return SIZE_MAX;
            }
            if (key_tl) { // unknown key of any length: skip its value
                if (!scan_skip_value(&p)) {
                    return SIZE_MAX;
                }
            } else if (strcmp(key, "cards") == 0) {
                have_cards = true;
                scan_ws(&p);
                if (p.i >= p.len || p.s[p.i] != '[') {
                    return SIZE_MAX;
                }
                p.i++;
                scan_ws(&p);
                if (p.i < p.len && p.s[p.i] == ']') {
                    p.i++;
                } else {
                    for (;;) {
                        store_card_t card = { 0 };
                        scan_ws(&p);
                        if (p.i >= p.len || p.s[p.i] != '{') {
                            return SIZE_MAX;
                        }
                        p.i++;
                        scan_ws(&p);
                        if (p.i < p.len && p.s[p.i] == '}') {
                            p.i++;
                        } else {
                            for (;;) {
                                char ck[16];
                                char uid[24] = { 0 };
                                uint64_t win;
                                bool is_null;
                                bool ck_tl;
                                if (!scan_string_relaxed(&p, ck, sizeof(ck), &ck_tl) ||
                                    !scan_ch(&p, ':')) {
                                    return SIZE_MAX;
                                }
                                if (ck_tl) { // unknown key of any length
                                    if (!scan_skip_value(&p)) {
                                        return SIZE_MAX;
                                    }
                                } else if (strcmp(ck, "uid") == 0) {
                                    if (!scan_string(&p, uid, sizeof(uid))) {
                                        return SIZE_MAX;
                                    }
                                    size_t ul = strlen(uid);
                                    if ((ul != 8 && ul != 14 && ul != 20) || n >= max_cards) {
                                        return SIZE_MAX;
                                    }
                                    card.uid_len = (uint8_t)(ul / 2);
                                    for (size_t b = 0; b < card.uid_len; b++) {
                                        char hi = uid[b * 2], lo = uid[b * 2 + 1];
                                        if (hi >= 'a' && hi <= 'f') {
                                            hi = (char)(hi - 'a' + 'A');
                                        }
                                        if (lo >= 'a' && lo <= 'f') {
                                            lo = (char)(lo - 'a' + 'A');
                                        }
                                        int hv = (hi >= '0' && hi <= '9') ? hi - '0'
                                                 : (hi >= 'A' && hi <= 'F') ? hi - 'A' + 10
                                                                            : -1;
                                        int lv = (lo >= '0' && lo <= '9') ? lo - '0'
                                                 : (lo >= 'A' && lo <= 'F') ? lo - 'A' + 10
                                                                            : -1;
                                        if (hv < 0 || lv < 0) {
                                            return SIZE_MAX;
                                        }
                                        card.uid[b] = (uint8_t)(hv * 16 + lv);
                                    }
                                } else if (strcmp(ck, "valid_from") == 0) {
                                    if (!scan_uint_or_null(&p, &win, &is_null) ||
                                        (!is_null && win > UINT32_MAX)) {
                                        return SIZE_MAX; // reject, never truncate
                                    }
                                    card.valid_from = is_null ? 0 : (uint32_t)win;
                                } else if (strcmp(ck, "valid_until") == 0) {
                                    if (!scan_uint_or_null(&p, &win, &is_null) ||
                                        (!is_null && win > UINT32_MAX)) {
                                        return SIZE_MAX;
                                    }
                                    card.valid_until = is_null ? 0 : (uint32_t)win;
                                } else {
                                    if (!scan_skip_value(&p)) {
                                        return SIZE_MAX;
                                    }
                                }
                                if (scan_ch(&p, ',')) {
                                    continue;
                                }
                                if (scan_ch(&p, '}')) {
                                    break;
                                }
                                return SIZE_MAX;
                            }
                        }
                        if (n < max_cards) {
                            out[n++] = card; // uid validated above
                        } else {
                            too_many_local = true; // keep counting: loud, not silent
                        }
                        if (scan_ch(&p, ',')) {
                            continue;
                        }
                        if (scan_ch(&p, ']')) {
                            break;
                        }
                        return SIZE_MAX;
                    }
                }
            } else {
                if (!scan_skip_value(&p)) {
                    return SIZE_MAX;
                }
            }
            if (scan_ch(&p, ',')) {
                continue;
            }
            if (scan_ch(&p, '}')) {
                break;
            }
            return SIZE_MAX;
        }
    }
    scan_ws(&p);
    if (p.i != p.len || !have_cards) {
        // A missing "cards" key is MALFORMED, not an empty list — treating
        // it as 0 cards would deny every card in the building.
        return SIZE_MAX;
    }
    if (too_many != NULL) {
        *too_many = too_many_local;
    }
    return n;
}

bool sync_decode_events_response(const char *body, size_t len, uint32_t *acked_through,
                                 uint32_t *out_rejected, size_t max_rejected,
                                 size_t *rejected_len)
{
    scan_t p = { .s = body, .len = len, .i = 0, .ok = true };
    char key[24];
    bool have_acked = false;
    if (rejected_len != NULL) {
        *rejected_len = 0;
    }
    if (!scan_ch(&p, '{')) {
        return false;
    }
    scan_ws(&p);
    if (p.i < p.len && p.s[p.i] == '}') {
        return false; // acked_through is required
    }
    for (;;) {
        bool key_tl;
        if (!scan_string_relaxed(&p, key, sizeof(key), &key_tl) || !scan_ch(&p, ':')) {
            return false;
        }
        if (key_tl) { // unknown key of any length: skip its value
            if (!scan_skip_value(&p)) {
                return false;
            }
        } else if (strcmp(key, "acked_through") == 0) {
            uint64_t v;
            if (!scan_uint(&p, &v) || v > UINT32_MAX) {
                return false;
            }
            *acked_through = (uint32_t)v;
            have_acked = true;
        } else if (strcmp(key, "rejected") == 0) {
            scan_ws(&p);
            if (p.i >= p.len || p.s[p.i] != '[') {
                return false;
            }
            p.i++;
            scan_ws(&p);
            if (p.i < p.len && p.s[p.i] == ']') {
                p.i++;
            } else {
                for (;;) {
                    uint64_t v;
                    if (!scan_uint(&p, &v) || v > UINT32_MAX) {
                        return false;
                    }
                    if (out_rejected != NULL && *rejected_len < max_rejected) {
                        out_rejected[*rejected_len] = (uint32_t)v;
                    }
                    if (rejected_len != NULL) {
                        (*rejected_len)++;
                    }
                    if (scan_ch(&p, ',')) {
                        continue;
                    }
                    if (scan_ch(&p, ']')) {
                        break;
                    }
                    return false;
                }
            }
        } else {
            if (!scan_skip_value(&p)) {
                return false;
            }
        }
        if (scan_ch(&p, ',')) {
            continue;
        }
        if (scan_ch(&p, '}')) {
            break;
        }
        return false;
    }
    scan_ws(&p);
    return have_acked && p.i == p.len;
}
