// Clock: base persistence, stamp derivation, HTTP Date parsing, ISO8601,
// deep-sleep/power-cut semantics (§2.4, deep-sleep design).
#include "unity.h"

#include <string.h>

#include "clock/clock.h"
#include "clock_port_host.h"

// 2026-10-23T08:01:02Z (the contract fixture's device_ts).
#define T_FIXTURE 1792742462ULL

void setUp(void)
{
    clock_host_power_loss();
    clock_host_set_epoch(0);
    clock_host_set_uptime_us(INT64_MIN);
    TEST_ASSERT_TRUE(clock_init());
    TEST_ASSERT_FALSE(clock_synced()); // fresh power-on: no clock
}

void tearDown(void) {}

void test_unsynced_stamp_is_uptime_and_flagged(void)
{
    clock_host_set_uptime_us(3600ULL * 1000000); // 1 h after boot
    uint64_t epoch;
    bool synced;
    clock_stamp(3600ULL * 1000000, &epoch, &synced);
    TEST_ASSERT_FALSE(synced);
    TEST_ASSERT_EQUAL_UINT64(3600, epoch); // seconds-since-boot stand-in
}

void test_time_source_sets_base_and_stamps_derive(void)
{
    clock_host_set_uptime_us(100000000); // base set at uptime 100 s
    clock_host_set_uptime_us(100000000);
    clock_on_time_source(T_FIXTURE);
    TEST_ASSERT_TRUE(clock_synced());
    // An event captured BEFORE the sync (the usual same-boot case) derives a
    // timestamp slightly before the base: 100 s of uptime ago.
    uint64_t epoch;
    bool synced;
    clock_stamp(94000000, &epoch, &synced); // captured at uptime 94 s
    TEST_ASSERT_TRUE(synced);
    TEST_ASSERT_EQUAL_UINT64(T_FIXTURE - 6, epoch);
    clock_stamp(160000000, &epoch, &synced); // captured at uptime 160 s
    TEST_ASSERT_EQUAL_UINT64(T_FIXTURE + 60, epoch);
    TEST_ASSERT_TRUE(synced);
}

void test_base_survives_deep_sleep(void)
{
    // Deep sleep: RTC memory keeps the base and the (RTC-backed) uptime
    // keeps counting — a stamp after the wake derives from the same base.
    clock_host_set_uptime_us(100000000);
    clock_on_time_source(T_FIXTURE);
    clock_host_set_uptime_us(100000000 + 300 * 1000000); // uptime continued
    clock_host_reboot_after_deep_sleep();
    TEST_ASSERT_TRUE(clock_init());
    TEST_ASSERT_TRUE(clock_synced());
    uint64_t epoch;
    bool synced;
    clock_stamp(100000000ULL + 300 * 1000000, &epoch, &synced); // 5 min later
    TEST_ASSERT_TRUE(synced);
    TEST_ASSERT_EQUAL_UINT64(T_FIXTURE + 300, epoch);
}

void test_soft_restart_rejects_the_base(void)
{
    // BLOCKER: RTC_NOINIT survives panic/watchdog/esp_restart, but
    // esp_timer restarts at 0 — keeping the base would re-stamp old ring
    // events as synced with a wrong delta. Only a deep-sleep wake may keep
    // it; a soft restart must come up unsynced and re-base.
    clock_host_set_uptime_us(100000000);
    clock_on_time_source(T_FIXTURE);
    clock_host_set_uptime_us(500000); // uptime RESTARTED at ~0
    clock_host_soft_restart();
    TEST_ASSERT_TRUE(clock_init()); // port readable
    TEST_ASSERT_FALSE(clock_synced()); // but the stale base is rejected
    uint64_t epoch;
    bool synced;
    clock_stamp(500000, &epoch, &synced);
    TEST_ASSERT_FALSE(synced);
    TEST_ASSERT_EQUAL_UINT64(1, epoch); // uptime stand-in (0.5 s rounds to 1)
}

void test_power_cut_clears_the_clock(void)
{
    clock_host_set_uptime_us(100000000);
    clock_on_time_source(T_FIXTURE);
    TEST_ASSERT_TRUE(clock_synced());
    clock_host_power_loss();
    TEST_ASSERT_TRUE(clock_init()); // port still readable
    TEST_ASSERT_FALSE(clock_synced()); // but the base is gone: re-sync
    uint64_t epoch;
    bool synced;
    clock_stamp(123456, &epoch, &synced);
    TEST_ASSERT_FALSE(synced);
    TEST_ASSERT_EQUAL_UINT64(0, epoch); // uptime stand-in, not real time
}

void test_resync_rebases_the_stamps(void)
{
    clock_host_set_uptime_us(100000000);
    clock_on_time_source(T_FIXTURE);
    // Hours later the session re-syncs (SNTP or Date header): the base moves
    // to NOW, stamps before it still derive backwards.
    clock_host_set_uptime_us(100000000ULL + 1800 * 1000000);
    clock_on_time_source(T_FIXTURE + 1805); // server says 1805 s later
    uint64_t epoch;
    bool synced;
    // Captured 5 s before the re-sync: derives from the NEW base backwards.
    clock_stamp(100000000ULL + 1795 * 1000000, &epoch, &synced);
    TEST_ASSERT_EQUAL_UINT64(T_FIXTURE + 1800, epoch);
}

void test_http_date_fixture(void)
{
    uint64_t epoch = 0;
    TEST_ASSERT_TRUE(clock_parse_http_date("Wed, 23 Oct 2026 08:01:02 GMT", &epoch));
    TEST_ASSERT_EQUAL_UINT64(T_FIXTURE, epoch);
}

void test_http_date_rejects_garbage(void)
{
    uint64_t epoch;
    const char *bad[] = {
        "",
        "23 Oct 2026 08:01:02 GMT",            // no day-name/comma
        "Wed, 23 Xyz 2026 08:01:02 GMT",       // bad month
        "Wed, 32 Oct 2026 08:01:02 GMT",       // bad day
        "Wed, 23 Oct 26 08:01:02 GMT",         // short year
        "Wed, 23 Oct 2026 25:01:02 GMT",       // bad hour
        "Wed, 23 Oct 2026 08:61:02 GMT",       // bad minute
        "Wed, 23 Oct 2026 08:01:02 UTC",       // not GMT
        "Wed, 23 Oct 2026 08:01:02",           // no zone
        "Wed Oct 23 2026 08:01:02 GMT",        // missing comma
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        TEST_ASSERT_FALSE_MESSAGE(clock_parse_http_date(bad[i], &epoch), bad[i]);
    }
}

void test_http_date_leap_day_and_year_bounds(void)
{
    uint64_t epoch = 0;
    TEST_ASSERT_TRUE(clock_parse_http_date("Sat, 29 Feb 2020 00:00:00 GMT", &epoch));
    TEST_ASSERT_EQUAL_UINT64(1582934400ULL, epoch);
    TEST_ASSERT_FALSE(clock_parse_http_date("Fri, 29 Feb 2021 00:00:00 GMT", &epoch));
    TEST_ASSERT_FALSE(clock_parse_http_date("Wed, 23 Oct 2019 08:01:02 GMT", &epoch)); // pre-2020
    TEST_ASSERT_FALSE(clock_parse_http_date("Thu, 01 Jan 1970 00:00:00 GMT", &epoch)); // pre-2020
}

void test_iso8601_roundtrip(void)
{
    const struct { const char *date; const char *iso; } samples[] = {
        { "Wed, 23 Oct 2026 08:01:02 GMT", "2026-10-23T08:01:02Z" },
        { "Sat, 29 Feb 2020 00:00:00 GMT", "2020-02-29T00:00:00Z" },
        { "Thu, 31 Dec 2099 23:59:59 GMT", "2099-12-31T23:59:59Z" },
    };
    for (size_t i = 0; i < sizeof(samples) / sizeof(samples[0]); i++) {
        uint64_t epoch = 0;
        TEST_ASSERT_TRUE(clock_parse_http_date(samples[i].date, &epoch));
        char iso[21];
        clock_iso8601(epoch, iso, sizeof(iso));
        TEST_ASSERT_EQUAL_STRING_MESSAGE(samples[i].iso, iso, samples[i].date);
    }
}

void test_iso8601_buffer_guard(void)
{
    char small[8] = {0};
    clock_iso8601(T_FIXTURE, small, sizeof(small)); // must not write
    TEST_ASSERT_EQUAL_UINT8(0, small[0]);
    char ok[21];
    clock_iso8601(T_FIXTURE, ok, sizeof(ok));
    TEST_ASSERT_EQUAL_STRING("2026-10-23T08:01:02Z", ok);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_unsynced_stamp_is_uptime_and_flagged);
    RUN_TEST(test_time_source_sets_base_and_stamps_derive);
    RUN_TEST(test_base_survives_deep_sleep);
    RUN_TEST(test_soft_restart_rejects_the_base);
    RUN_TEST(test_power_cut_clears_the_clock);
    RUN_TEST(test_resync_rebases_the_stamps);
    RUN_TEST(test_http_date_fixture);
    RUN_TEST(test_http_date_rejects_garbage);
    RUN_TEST(test_http_date_leap_day_and_year_bounds);
    RUN_TEST(test_iso8601_roundtrip);
    RUN_TEST(test_iso8601_buffer_guard);
    return UNITY_END();
}
