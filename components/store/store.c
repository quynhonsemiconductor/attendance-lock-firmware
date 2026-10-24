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
// layout from the headers alone — no per-event flash writes for metadata,
// one sector erase per RECS_PER_SECTOR events (wear-aware, §2.8). A torn
// record (power cut mid-write) fails its CRC and is skipped; the sector it
// was in is sealed and the ring continues in the next one.

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
    uint64_t abs;  // absolute record index of slot 0 (valid when gen != 0)
    uint8_t valid; // consecutive valid records from slot 0 (write sector)
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

static uint32_t seq_end;  // persisted reserved-block end (seqs ≤ this are safe)
static uint32_t seq_next; // next seq to hand out

static uint32_t dropped; // full-ring drops not yet taken by the sync task

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
    memcpy(&h[4], &gen, 4);
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
    memcpy(gen, &h[4], 4);
    return *gen != 0;
}

static void rec_encode(uint8_t *r, uint32_t seq, const uint8_t *payload, size_t len)
{
    memset(r, 0xFF, REC_SIZE);
    r[0] = (uint8_t)(REC_MAGIC & 0xFF);
    r[1] = (uint8_t)(REC_MAGIC >> 8);
    r[2] = (uint8_t)len;
    r[3] = 0xFF;
    memcpy(&r[4], &seq, 4);
    uint32_t c = crc32_upd(0, r, 8);
    c = crc32_upd(c, payload, len);
    memcpy(&r[8], &c, 4);
    memcpy(&r[REC_HDR_SIZE], payload, len);
}

// Decodes the record at the given absolute index; false when blank/torn.
static bool rec_at(uint64_t abs_idx, uint32_t *seq, uint8_t *payload, size_t *len)
{
    uint32_t p = (uint32_t)((abs_idx / RECS_PER_SECTOR) % n_sectors);
    uint32_t slot = (uint32_t)(abs_idx % RECS_PER_SECTOR);
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
    *seq = (uint32_t)r[4] | ((uint32_t)r[5] << 8) | ((uint32_t)r[6] << 16) | ((uint32_t)r[7] << 24);
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
            S[p].abs = (uint64_t)(gen - 1) * RECS_PER_SECTOR;
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
            // Blank or garbage: blank stays until first use, garbage is
            // erased before first use (factory-fresh partitions included).
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
    if (ws_gen == 0) {
        // Fresh ring.
        next_abs = 0;
        read_abs = 0;
        ws_sealed = false;
        return true;
    }
    // Count consecutive valid records in the write sector; a torn tail
    // (power cut mid-write) seals the sector.
    uint8_t payload[STORE_EVENT_MAX];
    size_t len;
    uint32_t seq;
    uint32_t v = 0;
    while (v < RECS_PER_SECTOR && rec_at(S[ws].abs + v, &seq, payload, &len)) {
        v++;
    }
    S[ws].valid = (uint8_t)v;
    next_abs = S[ws].abs + v;
    ws_sealed = (v < RECS_PER_SECTOR);
    read_abs = S[rs].abs; // nothing is acked across a boot
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
    if (s->state == SECT_DIRTY && s->abs + s->valid > read_abs) {
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
        return false;
    }
    s->gen = gen;
    s->abs = logical * RECS_PER_SECTOR;
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

static bool nvs_dropped_load(void)
{
    // Missing key = fresh device = 0 (port contract).
    dropped = 0;
    return store_port_nvs_get_u32(NVS_KEY_DROP, &dropped);
}

// --- Public API ---------------------------------------------------------

bool store_init(void)
{
    if (!nvs_dropped_load()) {
        return false;
    }
    if (!scan_ring()) {
        return false;
    }
    uint32_t end = 0; // missing key = fresh device = 0 (port contract)
    if (!store_port_nvs_get_u32(NVS_KEY_SEQ_END, &end)) {
        return false;
    }
    seq_end = end;
    seq_next = end + 1;
    ring_capacity = (size_t)n_sectors * RECS_PER_SECTOR;
    mounted = true;
    return true;
}

size_t store_ring_capacity(void)
{
    return ring_capacity;
}

bool store_allow_list_replace(const char *body, size_t len)
{
    if (body == NULL || len == 0 || len > STORE_ALLOW_LIST_MAX) {
        return false;
    }
    // NVS blob replace is atomic on commit (the port appends the new entry
    // before freeing the old), so a power cut leaves the old list active.
    return store_port_nvs_set_blob(NVS_KEY_ALIST, body, len);
}

bool store_allow_list_read(char *out, size_t cap, size_t *len_out)
{
    size_t len = cap;
    if (!store_port_nvs_get_blob(NVS_KEY_ALIST, out, &len)) {
        return false;
    }
    if (len_out != NULL) {
        *len_out = len;
    }
    return true;
}

void store_stream_uuid(uint8_t out[16])
{
    size_t len = 16;
    if (store_port_nvs_get_blob(NVS_KEY_STREAM, out, &len) && len == 16) {
        return;
    }
    store_port_random_bytes(out, 16);
    out[6] = (uint8_t)((out[6] & 0x0F) | 0x40); // RFC 4122 v4 bits
    out[8] = (uint8_t)((out[8] & 0x3F) | 0x80);
    store_port_nvs_set_blob(NVS_KEY_STREAM, out, 16);
}

uint32_t store_seq_next(void)
{
    if (seq_next > seq_end) {
        // Persist the new block end BEFORE handing out its first seq: a crash
        // right after costs a gap, never a reuse (§2.4 — a reused seq would
        // be silently discarded server-side as a duplicate).
        seq_end = ((seq_next + STORE_SEQ_BLOCK - 1) / STORE_SEQ_BLOCK) * STORE_SEQ_BLOCK;
        store_port_nvs_set_u32(NVS_KEY_SEQ_END, seq_end);
    }
    return seq_next++;
}

bool store_event_push(const uint8_t *event, size_t len, uint32_t *seq_out)
{
    if (!mounted || event == NULL || len == 0 || len > STORE_EVENT_MAX) {
        return false;
    }
    uint32_t slot = (uint32_t)(next_abs % RECS_PER_SECTOR);
    if (slot == 0 || ws_sealed) {
        bool full = false;
        if (!ring_advance(&full)) {
            if (full) {
                // Ring full: the door keeps working, the event is dropped and
                // counted (§2.4). Persisted, so it survives until it is sent.
                dropped++;
                store_port_nvs_set_u32(NVS_KEY_DROP, dropped);
            }
            return false;
        }
        slot = (uint32_t)(next_abs % RECS_PER_SECTOR);
    }
    uint32_t seq = store_seq_next();
    uint8_t r[REC_SIZE];
    rec_encode(r, seq, event, len);
    uint32_t p = (uint32_t)((next_abs / RECS_PER_SECTOR) % n_sectors);
    uint32_t off = p * STORE_PORT_SECTOR_SIZE + SEC_HDR_SIZE + slot * REC_SIZE;
    if (!store_port_flash_write(off, r, REC_SIZE)) {
        ws_sealed = true; // partial program: seal the sector, next push advances
        return false;
    }
    S[p].valid++;
    next_abs++;
    if (seq_out != NULL) {
        *seq_out = seq;
    }
    return true;
}

size_t store_event_batch(store_event_t *out, size_t max_events)
{
    if (max_events > STORE_BATCH_MAX) {
        max_events = STORE_BATCH_MAX;
    }
    // Peek only: a batch that never got uploaded must be servable again, so
    // the cursor advances in store_ack(), not here.
    normalize_head();
    size_t n = 0;
    while (n < max_events && read_abs + n < next_abs) {
        size_t len;
        if (!rec_at(read_abs + n, &out[n].seq, out[n].bytes, &len)) {
            break; // defensive: unreadable head — stop, never skip silently
        }
        n++;
    }
    return n;
}

void store_ack(uint32_t acked_through)
{
    // The batch is the oldest events in seq order, so one walk deletes
    // exactly the uploaded batch — accepted AND rejected seqs (§2.4: one bad
    // event never blocks the queue).
    normalize_head();
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
}

uint32_t store_dropped_take(void)
{
    uint32_t v = dropped;
    dropped = 0;
    // Persisted per increment; the clear is best-effort — a lost clear only
    // over-reports drops once after a reboot (server-side, additive).
    store_port_nvs_set_u32(NVS_KEY_DROP, 0);
    return v;
}
