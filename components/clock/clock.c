// Time base + stamping (§2.4): SNTP or the server Date header set a persisted
// {epoch, uptime-at-set} base; device timestamps are derived from it at
// upload time. Platform-free — time sources and persistence go through
// clock_port.h so the host tests run this file unchanged.
#include "clock/clock.h"

#include <string.h>

#include "clock_port.h"

// Persisted state (clock_state_t lives in clock_port.h — one definition for
// the logic and the RTC layout). Magics guard against power-on garbage.
#define BASE_MAGIC 0x434C4B31u // "CLK1"
#define BOOT_MAGIC 0x424F4F54u // "BOOT"

static clock_state_t st;
static bool port_ok;

int64_t clock_uptime_us(void)
{
    return clock_port_uptime_us();
}

bool clock_init(void)
{
    size_t len = 0;
    clock_port_res_t r = clock_port_state_load(&st, sizeof(st), &len);
    port_ok = (r != CLOCK_PORT_ERROR);
    bool have_state = port_ok && r == CLOCK_PORT_FOUND && len == sizeof(st) &&
                      st.magic == BASE_MAGIC && st.boot_magic == BOOT_MAGIC;
    if (!port_ok) {
        return false;
    }
    if (have_state && clock_port_deep_sleep_wake()) {
        // Deep sleep: the RTC timer kept counting — bases and boot id stand.
        return true;
    }
    if (have_state) {
        // Soft restart (panic/watchdog/esp_restart): esp_timer restarted at
        // 0, so the current base is stale — keep it one slot back under the
        // OLD segment id (records from before the restart still derive from
        // it) and start a NEW segment, unsynced.
        st.prev_valid = st.cur_valid;
        st.prev_boot_id = st.cur_boot_id;
        st.prev_epoch = st.cur_epoch;
        st.prev_uptime_us = st.cur_uptime_us;
        st.cur_valid = 0;
        clock_port_random_bytes(&st.cur_boot_id, sizeof(st.cur_boot_id));
        if (st.cur_boot_id == 0 || st.cur_boot_id == st.prev_boot_id) {
            st.cur_boot_id = st.prev_boot_id + 1; // ids must differ
        }
    } else {
        // Power-on (or unreadable RTC): new boot id, no bases, no clock.
        memset(&st, 0, sizeof(st));
        st.magic = BASE_MAGIC;
        st.boot_magic = BOOT_MAGIC;
        clock_port_random_bytes(&st.cur_boot_id, sizeof(st.cur_boot_id));
        if (st.cur_boot_id == 0) {
            st.cur_boot_id = 1; // 0 stays "no segment"
        }
    }
    if (!clock_port_state_store(&st, sizeof(st))) {
        clock_port_log("clock state persist failed — stamps may reset on reboot");
    }
    return true;
}

// --- HTTP Date (IMF-fixdate) parsing, libc-free -------------------------
//
// "Wed, 23 Oct 2026 08:01:02 GMT" — the format HTTP/1.1 servers send for
// Date. Days-from-civil (Howard Hinnant's algorithm) maps to epoch seconds.

static int month_from_name(const char *s)
{
    static const char *names[12] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                     "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
    for (int i = 0; i < 12; i++) {
        if (s[0] == names[i][0] && s[1] == names[i][1] && s[2] == names[i][2]) {
            return i + 1;
        }
    }
    return 0;
}

// Days from 1970-01-01 to y-m-d (proleptic Gregorian).
static int64_t days_from_civil(int y, int m, int d)
{
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (unsigned)((153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1);
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

static const char *skip_spaces(const char *s)
{
    while (*s == ' ' || *s == '\t') {
        s++;
    }
    return s;
}

static bool parse_uint(const char **s, int digits, uint64_t *out)
{
    uint64_t v = 0;
    for (int i = 0; i < digits; i++) {
        if (**s < '0' || **s > '9') {
            return false;
        }
        v = v * 10 + (uint64_t)(*(*s)++ - '0');
    }
    *out = v;
    return true;
}

bool clock_synced(void)
{
    return st.cur_valid != 0;
}

uint32_t clock_boot_id(void)
{
    return st.cur_boot_id;
}

void clock_on_time_source(uint64_t epoch_now)
{
    st.cur_valid = 1;
    st.cur_epoch = epoch_now;
    st.cur_uptime_us = clock_port_uptime_us();
    if (!clock_port_state_store(&st, sizeof(st))) {
        clock_port_log("clock state persist failed — stamps may reset on reboot");
    }
}

void clock_stamp(uint32_t seg_boot_id, int64_t uptime_us, uint64_t *epoch_out, bool *synced_out)
{
    const uint64_t *epoch = NULL;
    const int64_t *base_uptime = NULL;
    if (seg_boot_id == st.cur_boot_id && st.cur_valid) {
        epoch = &st.cur_epoch;
        base_uptime = &st.cur_uptime_us;
    } else if (seg_boot_id == st.prev_boot_id && st.prev_valid) {
        // A record from BEFORE a soft restart derives from the kept base.
        epoch = &st.prev_epoch;
        base_uptime = &st.prev_uptime_us;
    }
    if (epoch != NULL) {
        int64_t delta_us = uptime_us - *base_uptime;
        // Round to nearest (an event captured BEFORE the sync — the usual
        // same-segment case — lands slightly before base.epoch).
        int64_t delta_s = delta_us >= 0 ? (delta_us + 500000) / 1000000
                                        : -((-delta_us + 500000) / 1000000);
        *epoch_out = (uint64_t)((int64_t)*epoch + delta_s);
        *synced_out = true;
    } else {
        // No usable clock for this segment: seconds-since-boot counted from
        // the Unix epoch. Stable and monotonic; time_synced=false tells the
        // server the stamp is relative.
        *epoch_out = (uint64_t)((uptime_us + 500000) / 1000000);
        *synced_out = false;
    }
}

uint64_t clock_system_epoch(void)
{
    return clock_port_now_epoch();
}

bool clock_parse_http_date(const char *s, uint64_t *epoch_out)
{
    if (s == NULL) {
        return false;
    }
    s = skip_spaces(s);
    // day-name "," — accepted but not validated beyond the comma: the day of
    // week carries no information the epoch needs.
    while (*s && *s != ',') {
        s++;
    }
    if (*s != ',') {
        return false;
    }
    s = skip_spaces(s + 1);
    uint64_t day, year, hour, min, sec;
    if (!parse_uint(&s, 2, &day) || *s != ' ' || day == 0 || day > 31) {
        return false;
    }
    s = skip_spaces(s + 1);
    int month = month_from_name(s);
    if (month == 0 || s[3] != ' ') {
        return false;
    }
    s += 4;
    // Before 2020 = garbage (a device built this decade); also keeps the
    // days-from-civil result away from uint64 wrap territory.
    if (!parse_uint(&s, 4, &year) || year < 2020 || year > 9999 || *s != ' ') {
        return false;
    }
    s++;
    if (!parse_uint(&s, 2, &hour) || *s != ':' || hour > 23) {
        return false;
    }
    s++;
    if (!parse_uint(&s, 2, &min) || *s != ':' || min > 59) {
        return false;
    }
    s++;
    if (!parse_uint(&s, 2, &sec) || sec > 60) { // 60 = leap second, tolerated
        return false;
    }
    s = skip_spaces(s);
    if (!(s[0] == 'G' && s[1] == 'M' && s[2] == 'T')) {
        return false;
    }
    // Day-in-month validation (leap years included): "29 Feb 2021" is not
    // a date, and a parser that accepts it invents time.
    static const int mdays[13] = { 0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    int maxd = mdays[month];
    if (month == 2 && ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0)) {
        maxd = 29;
    }
    if ((int)day > maxd) {
        return false;
    }
    int64_t days = days_from_civil((int)year, month, (int)day);
    *epoch_out = (uint64_t)(days * 86400 + (int64_t)hour * 3600 + min * 60 + sec);
    return true;
}

void clock_iso8601(uint64_t epoch, char *out, size_t cap)
{
    if (cap < 21 || out == NULL) {
        return;
    }
    // civil_from_days, inverse of days_from_civil.
    int64_t days = (int64_t)epoch / 86400;
    uint64_t secs = epoch % 86400;
    days += 719468;
    int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    uint64_t doe = (uint64_t)(days - era * 146097);
    uint64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t y = (int64_t)yoe + era * 400;
    uint64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    uint64_t mp = (5 * doy + 2) / 153;
    uint64_t d = doy - (153 * mp + 2) / 5 + 1;
    uint64_t m = mp + (mp < 10 ? 3 : -9);
    y += (m <= 2);
    uint64_t yy = (uint64_t)y;
    out[0] = (char)('0' + yy / 1000 % 10);
    out[1] = (char)('0' + yy / 100 % 10);
    out[2] = (char)('0' + yy / 10 % 10);
    out[3] = (char)('0' + yy % 10);
    out[4] = '-';
    out[5] = (char)('0' + m / 10);
    out[6] = (char)('0' + m % 10);
    out[7] = '-';
    out[8] = (char)('0' + d / 10);
    out[9] = (char)('0' + d % 10);
    out[10] = 'T';
    out[11] = (char)('0' + secs / 36000 % 10);
    out[12] = (char)('0' + secs / 3600 % 10);
    out[13] = ':';
    out[14] = (char)('0' + secs / 600 % 6);
    out[15] = (char)('0' + secs / 60 % 10);
    out[16] = ':';
    out[17] = (char)('0' + secs / 10 % 6);
    out[18] = (char)('0' + secs % 10);
    out[19] = 'Z';
    out[20] = '\0';
}

// --- Bounded SNTP session (device-only mechanics behind the port) -------

void clock_sntp_start(void)
{
    clock_port_sntp_start(); // servers resolved from Kconfig in the port
}

void clock_sntp_stop(void)
{
    clock_port_sntp_stop();
}
