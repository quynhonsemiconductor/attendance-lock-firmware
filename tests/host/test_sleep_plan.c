// Deep-sleep gate + timer math (host-safe, battery work 2026-11-03).
#include "unity.h"

#include "sleep/sleep.h"

void setUp(void) {}
void tearDown(void) {}

// Everything quiet: a deep-sleep wake (no boot hold), no console, motion
// long ago.
static sleep_inputs_t quiet(uint32_t now)
{
    sleep_inputs_t in = {
        .now_ms = now,
        .door_idle = true,
        .sync_busy = false,
        .pir_high = false,
        .motion_seen = true,
        .last_motion_ms = 0,
        .awake_ms = 3000,
        .boot_hold_ms = 0,
        .console_seen = false,
        .console_last_ms = 0,
        .console_hold_ms = 60000,
    };
    return in;
}

static void test_quiet_door_sleeps(void)
{
    sleep_inputs_t in = quiet(10000);
    TEST_ASSERT_EQUAL(SLEEP_GO, sleep_decide(&in));
}

static void test_door_activity_wins_over_everything(void)
{
    sleep_inputs_t in = quiet(10000);
    in.door_idle = false;
    in.sync_busy = true;
    in.pir_high = true;
    TEST_ASSERT_EQUAL(SLEEP_STAY_DOOR, sleep_decide(&in));
}

static void test_sync_due_or_running_stays_awake(void)
{
    sleep_inputs_t in = quiet(10000);
    in.sync_busy = true;
    TEST_ASSERT_EQUAL(SLEEP_STAY_SYNC, sleep_decide(&in));
}

static void test_pir_high_stays_awake(void)
{
    sleep_inputs_t in = quiet(10000);
    in.pir_high = true;
    TEST_ASSERT_EQUAL(SLEEP_STAY_PIR, sleep_decide(&in));
}

static void test_motion_window_then_sleep(void)
{
    sleep_inputs_t in = quiet(10000);
    in.last_motion_ms = 8000;
    TEST_ASSERT_EQUAL(SLEEP_STAY_MOTION, sleep_decide(&in)); // 2 s < 3 s
    in.now_ms = 11000;
    TEST_ASSERT_EQUAL(SLEEP_GO, sleep_decide(&in)); // 3 s: window over
}

static void test_no_motion_since_boot_does_not_hold(void)
{
    sleep_inputs_t in = quiet(100); // timer wake: no motion at all
    in.motion_seen = false;
    TEST_ASSERT_EQUAL(SLEEP_GO, sleep_decide(&in));
}

static void test_motion_window_survives_ms_clock_wrap(void)
{
    sleep_inputs_t in = quiet(0x00000400u);
    in.last_motion_ms = 0xFFFFFC00u; // 2 s before, across the wrap
    in.boot_hold_ms = 0;
    TEST_ASSERT_EQUAL(SLEEP_STAY_MOTION, sleep_decide(&in));
}

static void test_cold_boot_console_window(void)
{
    sleep_inputs_t in = quiet(119999);
    in.boot_hold_ms = 120000;
    TEST_ASSERT_EQUAL(SLEEP_STAY_CONSOLE, sleep_decide(&in));
    in.now_ms = 120000;
    TEST_ASSERT_EQUAL(SLEEP_GO, sleep_decide(&in));
}

static void test_console_input_extends_the_window(void)
{
    sleep_inputs_t in = quiet(200000);
    in.console_seen = true;
    in.console_last_ms = 150000;
    TEST_ASSERT_EQUAL(SLEEP_STAY_CONSOLE, sleep_decide(&in)); // 50 s < 60 s
    in.now_ms = 210000;
    TEST_ASSERT_EQUAL(SLEEP_GO, sleep_decide(&in));
}

static void test_timer_until_due_clamped(void)
{
    const uint64_t s = 1000000ull;
    TEST_ASSERT_EQUAL_UINT64(3600 * s, sleep_timer_us(10 * s, 3610 * s, s, 86400 * s));
    TEST_ASSERT_EQUAL_UINT64(s, sleep_timer_us(10 * s, 5 * s, s, 86400 * s)); // past due
    TEST_ASSERT_EQUAL_UINT64(s, sleep_timer_us(10 * s, 10 * s, s, 86400 * s));
    TEST_ASSERT_EQUAL_UINT64(86400 * s, sleep_timer_us(0, 200000 * s, s, 86400 * s));
    // sync never started (due at the far future): the daily cap, not a 1 s loop
    TEST_ASSERT_EQUAL_UINT64(86400 * s, sleep_timer_us(10 * s, UINT64_MAX, s, 86400 * s));
    // degenerate min > max: max wins (documented in sleep.h)
    TEST_ASSERT_EQUAL_UINT64(5 * s, sleep_timer_us(10 * s, 11 * s, 30 * s, 5 * s));
}

static void test_degraded_sleep_caps_the_timer(void)
{
    const uint64_t s = 1000000ull;
    // healthy wake path: the sync timer stands, however long
    TEST_ASSERT_EQUAL_UINT64(7200 * s, sleep_fallback_timer_us(7200 * s, false, 60 * s));
    TEST_ASSERT_EQUAL_UINT64(0, sleep_fallback_timer_us(0, false, 60 * s));
    // stuck PIR / refused ext0-ext1: never longer than the fallback
    TEST_ASSERT_EQUAL_UINT64(60 * s, sleep_fallback_timer_us(7200 * s, true, 60 * s));
    TEST_ASSERT_EQUAL_UINT64(60 * s, sleep_fallback_timer_us(0, true, 60 * s)); // no timer
    TEST_ASSERT_EQUAL_UINT64(5 * s, sleep_fallback_timer_us(5 * s, true, 60 * s)); // sooner stays
}

static void test_boot_hold_after_cold_degraded_and_normal_wakes(void)
{
    TEST_ASSERT_EQUAL_UINT32(120000, sleep_boot_hold_ms(true, false, 120000, 2400));
    TEST_ASSERT_EQUAL_UINT32(120000, sleep_boot_hold_ms(true, true, 120000, 2400));
    // fallback wake: up long enough for the readers to poll
    TEST_ASSERT_EQUAL_UINT32(2400, sleep_boot_hold_ms(false, true, 120000, 2400));
    TEST_ASSERT_EQUAL_UINT32(0, sleep_boot_hold_ms(false, false, 120000, 2400));
}

static void test_verdict_names(void)
{
    TEST_ASSERT_EQUAL_STRING("go", sleep_verdict_name(SLEEP_GO));
    TEST_ASSERT_EQUAL_STRING("motion", sleep_verdict_name(SLEEP_STAY_MOTION));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_quiet_door_sleeps);
    RUN_TEST(test_door_activity_wins_over_everything);
    RUN_TEST(test_sync_due_or_running_stays_awake);
    RUN_TEST(test_pir_high_stays_awake);
    RUN_TEST(test_motion_window_then_sleep);
    RUN_TEST(test_no_motion_since_boot_does_not_hold);
    RUN_TEST(test_motion_window_survives_ms_clock_wrap);
    RUN_TEST(test_cold_boot_console_window);
    RUN_TEST(test_console_input_extends_the_window);
    RUN_TEST(test_timer_until_due_clamped);
    RUN_TEST(test_degraded_sleep_caps_the_timer);
    RUN_TEST(test_boot_hold_after_cold_degraded_and_normal_wakes);
    RUN_TEST(test_verdict_names);
    return UNITY_END();
}
