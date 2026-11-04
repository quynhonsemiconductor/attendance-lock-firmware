// Lock servo math (host-safe slice).
#include "unity.h"

#include "lock/lock_math.h"

void setUp(void) {}

// Move A armed its release at t=700 ms; the real callback fires at or after.
void test_release_waits_for_its_deadline(void)
{
    TEST_ASSERT_FALSE(lock_release_due(699999, 700000));
    TEST_ASSERT_TRUE(lock_release_due(700000, 700000));
    TEST_ASSERT_TRUE(lock_release_due(700050, 700000)); // fired late: still due
    TEST_ASSERT_FALSE(lock_release_due(INT64_MAX - 1, LOCK_NO_RELEASE)); // none pending
}

// The SA/review race: A's callback is dispatched but runs late, while move B
// starts (clears the deadline) and re-arms its own release (B at t=1500 ms).
void test_stale_release_after_a_new_move_does_nothing(void)
{
    // stale A runs between B's start and B's re-arm: no deadline
    TEST_ASSERT_FALSE(lock_release_due(800000, LOCK_NO_RELEASE));
    // stale A runs after B re-armed: B's deadline is still ahead
    TEST_ASSERT_FALSE(lock_release_due(800000, 1500000));
    // B's own callback
    TEST_ASSERT_TRUE(lock_release_due(1500000, 1500000));
}
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
    RUN_TEST(test_release_waits_for_its_deadline);
    RUN_TEST(test_stale_release_after_a_new_move_does_nothing);
    return UNITY_END();
}
