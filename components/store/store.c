// Allow-list cache (NVS) and the event ring (raw encrypted partition), §2.8.
// Platform-free: all flash/NVS access goes through store_port.h so the host
// tests run this file unchanged against a RAM flash.
#include "store/store.h"

#include <string.h>

#include "store_port.h"

// --- Ring layout ------------------------------------------------------
//
// The partition is a sequence of 4 KiB sectors. Each sector starts with a
// 32-byte header (magic, generation, CRC); the rest holds fixed-size event
// records. Sectors are used strictly in the order they are (re)used; every
// (re)use bumps the sector's generation by one, so generation - 1 IS the
// sector's ordinal in the append history and a boot reconstructs the whole
// layout from the headers plus the records alone — no per-event metadata
// writes, one sector erase per RECS_PER_SECTOR events (wear-aware, §2.8).
//
// A torn record (power cut mid-write) fails its CRC and is skipped; the
// sector it was in is sealed and the ring continues in the next one. Note
// that with flash encryption a BLANK sector also reads back as garbage, so
// "blank" and "torn" are indistinguishable at boot — either way the sector
// is unusable for appending and gets sealed (host tests reproduce this via
// the keystream mode). store_ack erases each sector as soon as the read
// cursor passes it, so the durable ack state is the set of erased sectors
// and a reboot replays at most one partial sector (≤ 62 events, free to
// resend server-side). The door task's push may still erase when it wraps
// before the sync task acked — bounded by one sector erase (~100 ms worst
// case on ESP32-S3 at 40 MHz flash); the common path erases nothing.
//
// Boot scan cost: one 32-byte header read per sector plus a full pass over
// every dirty sector's records — ≤ 4 MiB of flash reads, well under a
// second at ESP32-S3 flash read speeds, once per boot, before the door
// task starts (store_init runs from app_main).

#define SEC_HDR_SIZE 32u
#define REC_SIZE 64u
#define REC_MAGIC 0xA10Cu
#define SEC_MAGIC 0x5ECCu
#define SEC_VER 1u
#define RECS_PER_SECTOR ((STORE_PORT_SECTOR_SIZE - SEC_HDR_SIZE) / REC_SIZE) // 63
#define MAX_SECTORS 1024u // 4 MiB partition; store_port_flash_size() is checked against this
// (Generation is uint32: reuse #4.29e9 * 63 events is far past device EOL.)

// Sector header: magic u16, ver u8, rsv u8, gen u32, reserved, crc32 over
// the first 28 bytes.
#define SEC_CRC_OFF 28u

// Record header: magic u16, len u8, rsv u8, seq u32, crc32 over bytes 0..7
// plus the len payload bytes at offset 12.
#define REC_HDR_SIZE 12u

typedef struct {
    uint32_t gen;  // 0 = no valid header; else ordinal-of-use + 1
    uint8_t valid; // consecutive valid records from slot 0 (0 when blank)
    uint8_t state; // SECT_*
} sect_t;

enum { SECT_BLANK = 0, SECT_DIRTY = 1, SECT_UNKNOWN = 2 };

static sect_t S[MAX_SECTORS];
static uint32_t n_sectors;
static uint64_t next_abs; // write position (absolute record index)
static uint64_t read_abs; // oldest unacked event
static uint32_t max_gen;
static bool ws_sealed; // write sector has a torn tail: cannot append in it
static bool mounted;
static size_t ring_capacity;
static uint32_t max_ring_seq; // highest seq in the ring (stream/seq recovery)

static uint32_t seq_end;  // persisted reserved-block end (seqs ≤ this are safe)
static uint32_t seq_next; // next seq to hand out

static uint32_t dropped; // full-ring drops not yet acked by the sync task

static uint8_t stream_uuid[16];
static bool have_stream;

// Explicit little-endian helpers (no host-endian memcpy of integers).
static void put_u32le(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint32_t get_u32le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// Absolute record index where a physical sector's slot 0 sits in the append
// history (derived from the generation — no stored cursor).
static uint64_t sector_abs(uint32_t p)
{
    return (uint64_t)(S[p].gen - 1) * RECS_PER_SECTOR;
}

// --- CRC32 (poly 0xEDB88320, no table: called only on push/batch/ack) ---

static uint32_t crc32_upd(uint32_t crc, const uint8_t *p, size_t n)
{
    crc = ~crc;
    while (n--) {
        crc ^= *p++;
        for (int i = 0; i < 8; i++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

// --- Sector / record encoding -----------------------------------------

static uint32_t sec_crc(const uint8_t *h)
{
    return crc32_upd(0, h, SEC_CRC_OFF);
}

static void sec_encode(uint8_t *h, uint32_t gen)
{
    memset(h, 0xFF, SEC_HDR_SIZE);
    h[0] = (uint8_t)(SEC_MAGIC & 0xFF);
    h[1] = (uint8_t)(SEC_MAGIC >> 8);
    h[2] = SEC_VER;
    h[3] = 0xFF;
    put_u32le(&h[4], gen);
    uint32_t c = sec_crc(h);
    memcpy(&h[SEC_CRC_OFF], &c, 4);
}

static bool sec_parse(const uint8_t *h, uint32_t *gen)
{
    uint16_t magic = (uint16_t)h[0] | ((uint16_t)h[1] << 8);
    if (magic != SEC_MAGIC || h[2] != SEC_VER) {
        return false;
    }
    uint32_t stored;
    memcpy(&stored, &h[SEC_CRC_OFF], 4);
    if (stored != sec_crc(h)) {
        return false;
    }
    *gen = get_u32le(&h[4]);
    return *gen != 0;
}

static void rec_encode(uint8_t *r, uint32_t seq, const uint8_t *payload, size_t len)
{
    memset(r, 0xFF, REC_SIZE);
    r[0] = (uint8_t)(REC_MAGIC & 0xFF);
    r[1] = (uint8_t)(REC_MAGIC >> 8);
    r[2] = (uint8_t)len;
    r[3] = 0xFF;
    put_u32le(&r[4], seq);
    uint32_t c = crc32_upd(0, r, 8);
    c = crc32_upd(c, payload, len);
    memcpy(&r[8], &c, 4);
    memcpy(&r[REC_HDR_SIZE], payload, len);
}

// Decodes the record at the given absolute index; false when blank/torn or
// when the sector's generation does not match the index (a record left over
// from an earlier era of the ring must never be served, §2.4).
static bool rec_at(uint64_t abs_idx, uint32_t *seq, uint8_t *payload, size_t *len)
{
    uint32_t p = (uint32_t)((abs_idx / RECS_PER_SECTOR) % n_sectors);
    uint32_t slot = (uint32_t)(abs_idx % RECS_PER_SECTOR);
    if (S[p].gen == 0 || sector_abs(p) != abs_idx - slot) {
        return false;
    }
    uint32_t off = p * STORE_PORT_SECTOR_SIZE + SEC_HDR_SIZE + slot * REC_SIZE;
    uint8_t r[REC_SIZE];
    if (!store_port_flash_read(off, r, REC_SIZE)) {
        return false;
    }
    uint16_t magic = (uint16_t)r[0] | ((uint16_t)r[1] << 8);
    if (magic != REC_MAGIC || r[2] > STORE_EVENT_MAX) {
        return false;
    }
    uint32_t stored;
    memcpy(&stored, &r[8], 4);
    uint32_t c = crc32_upd(0, r, 8);
    c = crc32_upd(c, &r[REC_HDR_SIZE], r[2]);
    if (stored != c) {
        return false; // torn write: skipped (§2.8)
    }
    *seq = get_u32le(&r[4]);
    memcpy(payload, &r[REC_HDR_SIZE], r[2]);
    *len = r[2];
    return true;
}

// A sealed sector leaves an unreadable gap at its tail; the queue head jumps
// to the next sector (the torn events are gone — that is what "torn" means).
static void normalize_head(void)
{
    uint8_t payload[STORE_EVENT_MAX];
    size_t len;
    uint32_t seq;
    while (read_abs < next_abs && !rec_at(read_abs, &seq, payload, &len)) {
        read_abs = (read_abs / RECS_PER_SECTOR + 1) * RECS_PER_SECTOR;
    }
}

// --- Boot recovery -----------------------------------------------------

static bool scan_ring(void)
{
    memset(S, 0, sizeof(S));
    uint8_t h[SEC_HDR_SIZE];
    n_sectors = (uint32_t)(store_port_flash_size() / STORE_PORT_SECTOR_SIZE);
    if (n_sectors == 0 || n_sectors > MAX_SECTORS) {
        return false;
    }
    uint32_t ws = 0, rs = 0;
    uint32_t ws_gen = 0, rs_gen = UINT32_MAX;
    for (uint32_t p = 0; p < n_sectors; p++) {
        if (!store_port_flash_read(p * STORE_PORT_SECTOR_SIZE, h, SEC_HDR_SIZE)) {
            return false;
        }
        uint32_t gen;
        if (sec_parse(h, &gen)) {
            S[p].gen = gen;
            S[p].state = SECT_DIRTY;
            if (gen > ws_gen) {
                ws_gen = gen;
                ws = p;
            }
            if (gen < rs_gen) {
                rs_gen = gen;
                rs = p;
            }
        } else {
            // Blank or garbage: blank stays until first use, garbage (which
            // includes an encrypted-blank sector and a torn header) is
            // erased before first use.
            bool blank = true;
            for (uint32_t i = 0; i < SEC_HDR_SIZE; i++) {
                if (h[i] != 0xFF) {
                    blank = false;
                    break;
                }
            }
            S[p].state = blank ? SECT_BLANK : SECT_UNKNOWN;
        }
    }
    max_gen = ws_gen;
    max_ring_seq = 0;
    if (ws_gen == 0) {
        // Fresh ring.
        next_abs = 0;
        read_abs = 0;
        ws_sealed = false;
        return true;
    }
    // Scan every dirty sector's records from slot 0: consecutive-valid count
    // (the full-ring check needs it for ALL sectors, not just the write
    // sector) and the highest seq in the ring (stream/seq recovery).
    uint8_t payload[STORE_EVENT_MAX];
    size_t len;
    uint32_t seq;
    for (uint32_t p = 0; p < n_sectors; p++) {
        if (S[p].state != SECT_DIRTY) {
            continue;
        }
        uint64_t base = sector_abs(p);
        uint32_t v = 0;
        while (v < RECS_PER_SECTOR && rec_at(base + v, &seq, payload, &len)) {
            if (seq > max_ring_seq) {
                max_ring_seq = seq;
            }
            v++;
        }
        S[p].valid = (uint8_t)v;
    }
    next_abs = sector_abs(ws) + S[ws].valid;
    ws_sealed = (S[ws].valid < RECS_PER_SECTOR);
    read_abs = sector_abs(rs); // durable ack state = erased sectors (§ack)
    normalize_head();
    return true;
}

// --- Sector advance (wrap) ----------------------------------------------

// Advances the write position into the next sector, erasing it when needed.
// *full = the next sector still holds unacked events — overwriting it would
// lose data, so the push is refused and counted (§2.4). Flash failures also
// refuse the push but do NOT count (hardware trouble, not a full ring).
static bool ring_advance(bool *full)
{
    uint64_t logical;
    if (ws_sealed) {
        // Skip the sealed sector's dead tail: the write position jumps to the
        // next sector boundary.
        logical = next_abs / RECS_PER_SECTOR + 1;
    } else {
        logical = next_abs / RECS_PER_SECTOR;
    }
    uint32_t p = (uint32_t)(logical % n_sectors);
    sect_t *s = &S[p];
    *full = false;
    if (s->state == SECT_DIRTY && sector_abs(p) + s->valid > read_abs) {
        *full = true;
        return false;
    }
    if (s->state != SECT_BLANK) {
        if (!store_port_flash_erase(p * STORE_PORT_SECTOR_SIZE)) {
            return false;
        }
        s->state = SECT_BLANK;
    }
    uint8_t h[SEC_HDR_SIZE];
    uint32_t gen = max_gen + 1;
    sec_encode(h, gen);
    if (!store_port_flash_write(p * STORE_PORT_SECTOR_SIZE, h, SEC_HDR_SIZE)) {
        s->state = SECT_UNKNOWN; // partial header: unusable until re-erased
        return false;
    }
    s->gen = gen;
    s->valid = 0;
    s->state = SECT_DIRTY;
    max_gen = gen;
    next_abs = logical * RECS_PER_SECTOR;
    ws_sealed = false;
    return true;
}

// --- NVS-backed small state --------------------------------------------

#define NVS_KEY_ALIST "alist"
#define NVS_KEY_STREAM "stream"
#define NVS_KEY_SEQ_END "seqe"
#define NVS_KEY_DROP "drop"

// Shared 16 KiB scratch for allow-list encode/decode — file-scope, never on
// a task stack; every use is under the component lock.
static uint8_t al_blob[9 + STORE_ETAG_MAX + STORE_CARDS_MAX * 20];

// --- Allow-list blob ----------------------------------------------------
//
// [0..3]  magic 'A','L','1',0
// [4..7]  count (u32 LE)
// [8]     etag length (u8)
// [9..]   etag bytes
// [.]     entries, 20 bytes each: uid_len u8, uid[10], valid_from u32,
//         valid_until u32 (LE), 1 pad byte.

#define AL_MAGIC "AL1\x00"

static size_t al_encode(uint8_t *blob, const store_card_t *cards, size_t n, const char *etag,
                        size_t etag_len)
{
    memcpy(blob, AL_MAGIC, 4);
    put_u32le(&blob[4], (uint32_t)n);
    blob[8] = (uint8_t)etag_len;
    memcpy(&blob[9], etag, etag_len);
    uint8_t *e = &blob[9 + etag_len];
    for (size_t i = 0; i < n; i++) {
        e[0] = cards[i].uid_len;
        memcpy(&e[1], cards[i].uid, STORE_UID_MAX);
        put_u32le(&e[11], cards[i].valid_from);
        put_u32le(&e[15], cards[i].valid_until);
        e[19] = 0;
        e += 20;
    }
    return (size_t)(9 + etag_len) + n * 20;
}

// --- Public API ---------------------------------------------------------

bool store_init(void)
{
    if (!store_port_lock_init()) {
        return false;
    }
    store_port_lock();
    bool ok = false;
    do {
        uint32_t v = 0;
        if (store_port_nvs_get_u32(NVS_KEY_DROP, &v) == STORE_PORT_ERROR) {
            break;
        }
        dropped = v;
        if (!scan_ring()) {
            break;
        }
        v = 0; // ABSENT leaves v untouched — must not inherit the dropped value
        if (store_port_nvs_get_u32(NVS_KEY_SEQ_END, &v) == STORE_PORT_ERROR) {
            break;
        }
        seq_end = v;
        // The seq block continues past the persisted block end AND past
        // everything still in the ring: after an NVS wipe the new stream's
        // seqs can never collide with the old events still buffered (§2.4).
        seq_next = seq_end > max_ring_seq ? seq_end + 1 : max_ring_seq + 1;
        // Stream UUID: load once, cached in RAM.
        size_t len = 16;
        store_port_nvs_res_t r = store_port_nvs_get_blob(NVS_KEY_STREAM, stream_uuid, &len);
        if (r == STORE_PORT_ERROR) {
            break;
        }
        if (r == STORE_PORT_FOUND && len == 16) {
            have_stream = true;
        } else {
            store_port_random_bytes(stream_uuid, 16);
            stream_uuid[6] = (uint8_t)((stream_uuid[6] & 0x0F) | 0x40); // RFC 4122 v4
            stream_uuid[8] = (uint8_t)((stream_uuid[8] & 0x3F) | 0x80);
            if (!store_port_nvs_set_blob(NVS_KEY_STREAM, stream_uuid, 16)) {
                break; // a stream we cannot persist would regenerate per boot
            }
            have_stream = true;
        }
        ring_capacity = (size_t)n_sectors * RECS_PER_SECTOR;
        mounted = true;
        ok = true;
    } while (0);
    store_port_unlock();
    return ok;
}

size_t store_ring_capacity(void)
{
    return ring_capacity;
}

store_status_t store_allow_list_replace(const store_card_t *cards, size_t n, const char *etag,
                                        size_t etag_len)
{
    if (cards == NULL && n > 0) {
        return STORE_ERR_ARG;
    }
    if (etag == NULL && etag_len > 0) {
        return STORE_ERR_ARG;
    }
    if (etag_len > STORE_ETAG_MAX) {
        return STORE_ERR_ARG;
    }
    for (size_t i = 0; i < n; i++) {
        if (cards[i].uid_len != 4 && cards[i].uid_len != 7 && cards[i].uid_len != 10) {
            return STORE_ERR_ARG;
        }
    }
    if (n > STORE_CARDS_MAX) {
        return STORE_ERR_TOO_MANY; // LOUD: a stale list must be flagged, not silent
    }
    store_port_lock();
    // al_blob is shared: encode, write and decode all happen under the lock.
    size_t len = al_encode(al_blob, cards, n, etag, etag_len);
    // NVS blob replace is atomic on commit (the port appends the new entry
    // before freeing the old), so a power cut leaves the old list active —
    // and every failure path here returns an error the caller surfaces.
    bool ok = store_port_nvs_set_blob(NVS_KEY_ALIST, al_blob, len);
    store_port_unlock();
    return ok ? STORE_OK : STORE_ERR_NVS;
}

store_status_t store_allow_list_read(store_card_t *out, size_t max_cards, size_t *n_out,
                                     char *etag, size_t etag_cap, size_t *etag_len_out)
{
    size_t len = sizeof(al_blob);
    store_port_lock();
    store_port_nvs_res_t r = store_port_nvs_get_blob(NVS_KEY_ALIST, al_blob, &len);
    // al_blob is shared: the whole decode stays under the lock.
    store_status_t st = STORE_OK;
    if (r == STORE_PORT_ABSENT) {
        st = STORE_ABSENT;
    } else if (r == STORE_PORT_ERROR) {
        st = STORE_ERR_NVS;
    } else if (len < 9 || memcmp(al_blob, AL_MAGIC, 4) != 0) {
        st = STORE_ERR_NVS; // unknown format: treated as no valid cache
    }
    size_t count = 0, etag_len = 0;
    if (st == STORE_OK) {
        count = get_u32le(&al_blob[4]);
        etag_len = al_blob[8];
        if (count > STORE_CARDS_MAX || 9 + etag_len + count * 20 > len) {
            st = STORE_ERR_NVS; // corrupt/truncated: caller re-syncs
        }
    }
    store_status_t too_small = STORE_OK;
    if (count > max_cards || etag_len > etag_cap) {
        too_small = STORE_ERR_TOO_SMALL; // retryable with the sizes above
    }
    if (n_out != NULL) {
        *n_out = count;
    }
    if (etag_len_out != NULL) {
        *etag_len_out = etag_len;
    }
    if (st != STORE_OK) {
        store_port_unlock();
        return st;
    }
    // Each out-buffer that fits is filled even on TOO_SMALL — callers that
    // only want the ETag (If-None-Match) pass max_cards = 0.
    if (etag != NULL && etag_len <= etag_cap) {
        memcpy(etag, &al_blob[9], etag_len);
    }
    if (too_small != STORE_OK) {
        store_port_unlock();
        return too_small;
    }
    if (out != NULL) {
        const uint8_t *e = &al_blob[9 + etag_len];
        for (size_t i = 0; i < count; i++) {
            out[i].uid_len = e[0];
            memcpy(out[i].uid, &e[1], STORE_UID_MAX);
            out[i].valid_from = get_u32le(&e[11]);
            out[i].valid_until = get_u32le(&e[15]);
            e += 20;
        }
    }
    store_port_unlock();
    return STORE_OK;
}

void store_stream_uuid(uint8_t out[16])
{
    memcpy(out, stream_uuid, 16);
}

static uint32_t seq_alloc_locked(void);

uint32_t store_seq_next(void)
{
    store_port_lock();
    uint32_t out = seq_alloc_locked();
    store_port_unlock();
    return out;
}

static uint32_t seq_alloc_locked(void)
{
    if (seq_next > seq_end) {
        // Persist the new block end BEFORE handing out its first seq: a crash
        // right after costs a gap, never a reuse (§2.4 — a reused seq would
        // be silently discarded server-side as a duplicate).
        uint32_t new_end = ((seq_next + STORE_SEQ_BLOCK - 1) / STORE_SEQ_BLOCK) * STORE_SEQ_BLOCK;
        store_port_lock();
        bool ok = store_port_nvs_set_u32(NVS_KEY_SEQ_END, new_end);
        store_port_unlock();
        if (!ok) {
            return 0; // unreserved seq: caller must not store the event
        }
        seq_end = new_end;
    }
    return seq_next++;
}

bool store_event_push(const uint8_t *event, size_t len, uint32_t *seq_out)
{
    if (!mounted || event == NULL || len == 0 || len > STORE_EVENT_MAX) {
        return false;
    }
    store_port_lock();
    bool ok = false;
    do {
        uint32_t slot = (uint32_t)(next_abs % RECS_PER_SECTOR);
        if (slot == 0 || ws_sealed) {
            bool full = false;
            if (!ring_advance(&full)) {
                if (full) {
                    // Ring full: the door keeps working, the event is dropped
                    // and counted (§2.4). RAM-only here — persisting per
                    // dropped event is flash wear; the sync session flushes
                    // the count to NVS when it reads it for the next upload.
                    dropped++;
                }
                break;
            }
            slot = (uint32_t)(next_abs % RECS_PER_SECTOR);
        }
        uint32_t seq = store_seq_next();
        if (seq == 0) {
            // The seq reservation could not be persisted: without it a reboot
            // might REUSE this seq (silently dropped server-side). Treat the
            // push as failed and count it — staying silent is worse (§2.4).
            dropped++;
            store_port_nvs_set_u32(NVS_KEY_DROP, dropped); // rare: poison record
            break;
        }
        uint8_t r[REC_SIZE];
        rec_encode(r, seq, event, len);
        uint32_t p = (uint32_t)((next_abs / RECS_PER_SECTOR) % n_sectors);
        uint32_t off = p * STORE_PORT_SECTOR_SIZE + SEC_HDR_SIZE + slot * REC_SIZE;
        if (!store_port_flash_write(off, r, REC_SIZE)) {
            ws_sealed = true; // partial program: seal the sector, next push advances
            break;
        }
        S[p].valid++;
        next_abs++;
        if (seq_out != NULL) {
            *seq_out = seq;
        }
        ok = true;
    } while (0);
    store_port_unlock();
    return ok;
}

// Erase every sector the ack cursor fully passed (durable ack state, §2.8:
// same total wear, done on the sync task).
static void erase_passed_sectors(uint64_t from, uint64_t to)
{
    // First boundary strictly after `from`, clamped to ≥ RECS_PER_SECTOR:
    // boundary b erases the sector holding records [b-63, b), so b starts at
    // one full sector even when from == 0 (first lap, sector 0).
    uint64_t b = (from + RECS_PER_SECTOR - 1) / RECS_PER_SECTOR * RECS_PER_SECTOR;
    if (b < RECS_PER_SECTOR) {
        b = RECS_PER_SECTOR;
    }
    for (; b <= to; b += RECS_PER_SECTOR) {
        uint32_t p = (uint32_t)((b / RECS_PER_SECTOR - 1) % n_sectors);
        if (S[p].state == SECT_DIRTY) {
            if (store_port_flash_erase(p * STORE_PORT_SECTOR_SIZE)) {
                S[p].state = SECT_BLANK;
                S[p].valid = 0;
                // gen stays: the generation check in rec_at keeps any stale
                // read from resurrecting it
            }
        }
    }
}

size_t store_event_batch(store_event_t *out, size_t max_events)
{
    if (max_events > STORE_BATCH_MAX) {
        max_events = STORE_BATCH_MAX;
    }
    store_port_lock();
    // Peek only: a batch that never got uploaded must be servable again, so
    // the cursor advances in store_ack(), not here.
    normalize_head();
    size_t n = 0;
    while (n < max_events && read_abs + n < next_abs) {
        if (!rec_at(read_abs + n, &out[n].seq, out[n].bytes, &out[n].len)) {
            break; // defensive: unreadable head — stop, never skip silently
        }
        n++;
    }
    store_port_unlock();
    return n;
}

void store_ack(uint32_t acked_through)
{
    store_port_lock();
    // The batch is the oldest events in seq order, so one walk deletes
    // exactly the uploaded batch — accepted AND rejected seqs (§2.4: one bad
    // event never blocks the queue).
    normalize_head();
    uint64_t from = read_abs;
    uint8_t payload[STORE_EVENT_MAX];
    size_t len;
    uint32_t seq;
    while (read_abs < next_abs) {
        if (!rec_at(read_abs, &seq, payload, &len) || seq > acked_through) {
            break;
        }
        read_abs++;
    }
    normalize_head();
    erase_passed_sectors(from, read_abs);
    store_port_unlock();
}

uint32_t store_dropped_count(void)
{
    // Read == flush point: the sync session reads this before each upload,
    // which is the cadence the count is persisted at (a crash before then
    // only under-reports drops, never loses queued events).
    store_port_nvs_set_u32(NVS_KEY_DROP, dropped);
    return dropped;
}

void store_dropped_ack(uint32_t n)
{
    store_port_lock();
    dropped = (dropped > n) ? dropped - n : 0;
    // Best-effort: a lost clear only over-reports drops once (additive).
    store_port_nvs_set_u32(NVS_KEY_DROP, dropped);
    store_port_unlock();
}
