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
    if (ev == NULL || out == NULL || ev->uid_len < 4 || ev->uid_len > 10) {
        return 0;
    }
    emit_t e = { .buf = out, .cap = cap, .len = 0, .ok = true };
    uint64_t epoch;
    bool synced;
    clock_stamp((int64_t)ev->uptime_us, &epoch, &synced);
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

size_t sync_encode_envelope(const uint8_t stream[16], uint32_t dropped,
                            const store_event_t *events, size_t n_events, char *out, size_t cap)
{
    if (stream == NULL || out == NULL || (events == NULL && n_events > 0)) {
        return 0;
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

// Skips one JSON value of any type (object, array, string, number,
// true/false/null) — the unknown-field tolerance (§2.4).
static bool scan_skip_value(scan_t *p)
{
    scan_ws(p);
    if (p->i >= p->len) {
        p->ok = false;
        return false;
    }
    char c = p->s[p->i];
    if (c == '"') {
        char sink[64]; // unknown strings longer than this: fail-closed (the
        return scan_string(p, sink, sizeof(sink)); // contract has none)
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
                if (!scan_string(p, key, sizeof(key)) || !scan_ch(p, ':')) {
                    return false;
                }
            }
            if (!scan_skip_value(p)) {
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

size_t sync_decode_allow_list(const char *body, size_t len, store_card_t *out, size_t max_cards)
{
    scan_t p = { .s = body, .len = len, .i = 0, .ok = true };
    char key[24];
    size_t n = 0;
    if (!scan_ch(&p, '{')) {
        return SIZE_MAX;
    }
    scan_ws(&p);
    if (p.i < p.len && p.s[p.i] == '}') {
        p.i++;
    } else {
        for (;;) {
            if (!scan_string(&p, key, sizeof(key)) || !scan_ch(&p, ':')) {
                return SIZE_MAX;
            }
            if (strcmp(key, "cards") == 0) {
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
                                if (!scan_string(&p, ck, sizeof(ck)) || !scan_ch(&p, ':')) {
                                    return SIZE_MAX;
                                }
                                if (strcmp(ck, "uid") == 0) {
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
                                    if (!scan_uint_or_null(&p, &win, &is_null)) {
                                        return SIZE_MAX;
                                    }
                                    card.valid_from = is_null ? 0 : (uint32_t)win;
                                } else if (strcmp(ck, "valid_until") == 0) {
                                    if (!scan_uint_or_null(&p, &win, &is_null)) {
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
                            return SIZE_MAX; // more cards than the caller takes: loud
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
    if (p.i != p.len) {
        return SIZE_MAX; // trailing garbage
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
        if (!scan_string(&p, key, sizeof(key)) || !scan_ch(&p, ':')) {
            return false;
        }
        if (strcmp(key, "acked_through") == 0) {
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
