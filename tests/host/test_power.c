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
    RUN_TEST(test_low_battery_threshold);
    RUN_TEST(test_release_denied_below_cutoff_plus_margin);
    RUN_TEST(test_brownout_backoff_doubles_and_caps);
    RUN_TEST(test_brownout_blocked_window);
    return UNITY_END();
}
