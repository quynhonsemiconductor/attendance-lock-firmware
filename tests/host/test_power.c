// Battery math: divider, thresholds, brownout backoff (host-safe slice).
#include "unity.h"

#include "power/power_calc.h"

void setUp(void) {}
void tearDown(void) {}

void test_divider_math(void)
{
    TEST_ASSERT_EQUAL_UINT32(6000, power_battery_from_raw(600, 10, 1));
    // Fraction-safe path: 600 mV × 11/7.
    TEST_ASSERT_EQUAL_UINT32(600 / 7 * 11 + 600 % 7 * 11 / 7,
                             power_battery_from_raw(600, 11, 7));
    TEST_ASSERT_EQUAL_UINT32(0, power_battery_from_raw(600, 10, 0)); // divide-by-zero guard
}

void test_reading_plausibility_window(void)
{
    // The pack is 4.5-6.5 V; the configured window (Kconfig 3000-9000)
    // absorbs divider and ADC tolerance. Bounds are inclusive.
    TEST_ASSERT_TRUE(power_reading_plausible(4500, 3000, 9000));
    TEST_ASSERT_TRUE(power_reading_plausible(6500, 3000, 9000));
    TEST_ASSERT_TRUE(power_reading_plausible(3000, 3000, 9000));
    TEST_ASSERT_TRUE(power_reading_plausible(9000, 3000, 9000));
    TEST_ASSERT_TRUE(power_reading_plausible(0, 3000, 9000)); // no reading = unknown
    // Round-3 bench: the divider was not wired and the tap floated at
    // 33,9xx mV ("34 V") — implausible high.
    TEST_ASSERT_FALSE(power_reading_plausible(33900, 3000, 9000));
    TEST_ASSERT_FALSE(power_reading_plausible(9001, 3000, 9000));
    // A shorted or half-wired divider reads far below the pack.
    TEST_ASSERT_FALSE(power_reading_plausible(2999, 3000, 9000));
    TEST_ASSERT_FALSE(power_reading_plausible(500, 3000, 9000));
}

void test_release_gate_treats_a_sensor_fault_like_low_battery(void)
{
    // Below the gate (cutoff 4.8 V + 0.2 V margin): deny, as before.
    TEST_ASSERT_TRUE(power_release_blocked(4900, 4800, 200, 3000, 9000));
    // An implausible reading must DENY (fail-secure), not sail through as
    // "34 V" — the round-3 bench regression this arm exists for.
    TEST_ASSERT_TRUE(power_release_blocked(33900, 4800, 200, 3000, 9000));
    TEST_ASSERT_TRUE(power_release_blocked(500, 4800, 200, 3000, 9000));
    // A healthy pack releases normally; no reading (0 mV) never locks out.
    TEST_ASSERT_FALSE(power_release_blocked(5400, 4800, 200, 3000, 9000));
    TEST_ASSERT_FALSE(power_release_blocked(0, 4800, 200, 3000, 9000));
}

void test_low_battery_threshold(void)
{
    TEST_ASSERT_TRUE(power_is_low(4999, 5000)); // alert at ~5.0 V
    TEST_ASSERT_FALSE(power_is_low(5000, 5000));
    TEST_ASSERT_FALSE(power_is_low(0, 5000)); // unknown never alerts
}

void test_release_denied_below_cutoff_plus_margin(void)
{
    // Cutoff 4.8 V + margin 0.2 V: deny under 5.0 V, allow at/above it.
    TEST_ASSERT_TRUE(power_should_deny(4799, 4800, 200));
    TEST_ASSERT_TRUE(power_should_deny(4999, 4800, 200));
    TEST_ASSERT_FALSE(power_should_deny(5000, 4800, 200));
    TEST_ASSERT_FALSE(power_should_deny(0, 4800, 200)); // unknown does not lock out
}

void test_brownout_backoff_doubles_and_caps(void)
{
    power_brownout_t st = { 0, 0 };
    st = power_brownout_on_reset(&st, true);
    TEST_ASSERT_EQUAL_UINT32(1, st.count);
    TEST_ASSERT_EQUAL_UINT32(30000, st.backoff_ms);
    st = power_brownout_on_reset(&st, true);
    TEST_ASSERT_EQUAL_UINT32(2, st.count);
    TEST_ASSERT_EQUAL_UINT32(60000, st.backoff_ms);
    for (int i = 0; i < 10; i++) {
        st = power_brownout_on_reset(&st, true); // doubles to the cap
    }
    TEST_ASSERT_EQUAL_UINT32(16u * 60u * 1000u, st.backoff_ms);
    // A clean power-on clears the counter.
    st = power_brownout_on_reset(&st, false);
    TEST_ASSERT_EQUAL_UINT32(0, st.count);
    TEST_ASSERT_EQUAL_UINT32(0, st.backoff_ms);
}

void test_brownout_blocked_window(void)
{
    TEST_ASSERT_TRUE(power_brownout_in_window(29999, 30000));
    TEST_ASSERT_FALSE(power_brownout_in_window(30000, 30000));
    TEST_ASSERT_FALSE(power_brownout_in_window(0, 0));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_divider_math);
    RUN_TEST(test_reading_plausibility_window);
    RUN_TEST(test_release_gate_treats_a_sensor_fault_like_low_battery);
    RUN_TEST(test_low_battery_threshold);
    RUN_TEST(test_release_denied_below_cutoff_plus_margin);
    RUN_TEST(test_brownout_backoff_doubles_and_caps);
    RUN_TEST(test_brownout_blocked_window);
    return UNITY_END();
}
