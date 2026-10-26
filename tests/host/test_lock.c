// Lock servo math (host-safe slice).
#include "unity.h"

#include "lock/lock_math.h"

void setUp(void) {}
void tearDown(void) {}

void test_servo_duty_math(void)
{
    // 50 Hz, 14 bit: period 20000 µs, max 16383 ticks.
    TEST_ASSERT_EQUAL_UINT32(819, lock_servo_duty_ticks(1000, 50, 14)); // 1000/20000*16383
    TEST_ASSERT_EQUAL_UINT32(1638, lock_servo_duty_ticks(2000, 50, 14));
    TEST_ASSERT_EQUAL_UINT32(0, lock_servo_duty_ticks(0, 50, 14)); // 0 µs is a valid pulse
    // An out-of-range pulse returns the maximum (never drives past the
    // mechanical stop).
    TEST_ASSERT_EQUAL_UINT32(4096, lock_servo_duty_ticks(5000, 50, 14)); // 25 % of period
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_servo_duty_math);
    return UNITY_END();
}
