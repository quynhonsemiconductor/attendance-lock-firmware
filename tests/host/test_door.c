// Door decision + lock servo math + reader tap logic (host-safe slices).
#include "unity.h"

#include <string.h>

#include "door/door.h"
#include "lock/lock_math.h"
#include "reader/reader.h"

#define T_NOW 1792742462ULL

void setUp(void) {}
void tearDown(void) {}

static store_card_t permanent(void)
{
    store_card_t c = { .uid_len = 4, .uid = { 0x04, 0xA1, 0xB2, 0xC3 } };
    return c;
}

static store_card_t windowed(uint32_t from, uint32_t until)
{
    store_card_t c = { .uid_len = 4, .uid = { 0x11, 0x22, 0x33, 0x44 },
                       .valid_from = from, .valid_until = until };
    return c;
}

void test_permanent_card_allowed_with_and_without_clock(void)
{
    store_card_t cards[1] = { permanent() };
    uint8_t uid[4] = { 0x04, 0xA1, 0xB2, 0xC3 };
    // No-clock rule: permanent cards keep working (§2.4).
    TEST_ASSERT_EQUAL(DOOR_ALLOW,
                      door_decide(cards, 1, false, 0, uid, 4));
    TEST_ASSERT_EQUAL(DOOR_ALLOW,
                      door_decide(cards, 1, true, T_NOW, uid, 4));
}

void test_unknown_uid_denied(void)
{
    store_card_t cards[1] = { permanent() };
    uint8_t uid[4] = { 0xDE, 0xAD, 0xBE, 0xEF };
    TEST_ASSERT_EQUAL(DOOR_DENY_UNKNOWN, door_decide(cards, 1, true, T_NOW, uid, 4));
}

void test_windowed_card_fails_closed_without_clock(void)
{
    store_card_t cards[1] = { windowed(T_NOW - 1000, T_NOW + 1000) };
    uint8_t uid[4] = { 0x11, 0x22, 0x33, 0x44 };
    // Temporary cards fail closed offline — even when the window would be open.
    TEST_ASSERT_EQUAL(DOOR_DENY_NOCLOCK, door_decide(cards, 1, false, T_NOW, uid, 4));
}

void test_windowed_card_by_clock_state(void)
{
    uint8_t uid[4] = { 0x11, 0x22, 0x33, 0x44 };
    store_card_t open_now[1] = { windowed(T_NOW - 1000, T_NOW + 1000) };
    TEST_ASSERT_EQUAL(DOOR_ALLOW, door_decide(open_now, 1, true, T_NOW, uid, 4));
    store_card_t not_yet[1] = { windowed(T_NOW + 1000, 0) };
    TEST_ASSERT_EQUAL(DOOR_DENY_WINDOW, door_decide(not_yet, 1, true, T_NOW, uid, 4));
    store_card_t expired[1] = { windowed(0, T_NOW - 1) };
    TEST_ASSERT_EQUAL(DOOR_DENY_WINDOW, door_decide(expired, 1, true, T_NOW, uid, 4));
}

void test_uid_must_match_exactly(void)
{
    store_card_t cards[1] = { permanent() };
    uint8_t uid[4] = { 0x04, 0xA1, 0xB2, 0xC4 };
    TEST_ASSERT_EQUAL(DOOR_DENY_UNKNOWN, door_decide(cards, 1, true, T_NOW, uid, 4));
}

// --- lock servo math ----------------------------------------------------

void test_servo_duty_math(void)
{
    // 50 Hz, 14 bit → period 20000 µs, max 16383.
    TEST_ASSERT_EQUAL_UINT32(819, lock_servo_duty_ticks(1000, 50, 14)); // 1000/20000*16383
    TEST_ASSERT_EQUAL_UINT32(1638, lock_servo_duty_ticks(2000, 50, 14));
    // Clamped at both ends.
    TEST_ASSERT_EQUAL_UINT32(0, lock_servo_duty_ticks(0, 50, 14));
    TEST_ASSERT_EQUAL_UINT32(4096, lock_servo_duty_ticks(5000, 50, 14)); // 25% duty
}

// --- reader tap logic ---------------------------------------------------

#define STATE_CAP 256

void test_tap_logic_debounce_and_cross_read(void)
{
    uint8_t state[STATE_CAP] = { 0 };
    reader_card_t c = { .reader = 0, .uid_len = 4, .uid = { 1, 2, 3, 4 } };
    // First tap accepted.
    TEST_ASSERT_TRUE(reader_accept(state, sizeof(state), &c, 1000, 1000, 800));
    // Held card re-polled 200 ms later: suppressed.
    TEST_ASSERT_FALSE(reader_accept(state, sizeof(state), &c, 1200, 1000, 800));
    // After the debounce window: accepted again (a second tap).
    TEST_ASSERT_TRUE(reader_accept(state, sizeof(state), &c, 2500, 1000, 800));
    // Same card on the OTHER reader within the cross-read window: suppressed.
    reader_card_t other = { .reader = 1, .uid_len = 4, .uid = { 1, 2, 3, 4 } };
    TEST_ASSERT_FALSE(reader_accept(state, sizeof(state), &other, 2600, 1000, 800));
    // A different card passes.
    reader_card_t d = { .reader = 0, .uid_len = 4, .uid = { 9, 9, 9, 9 } };
    TEST_ASSERT_TRUE(reader_accept(state, sizeof(state), &d, 2600, 1000, 800));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_permanent_card_allowed_with_and_without_clock);
    RUN_TEST(test_unknown_uid_denied);
    RUN_TEST(test_windowed_card_fails_closed_without_clock);
    RUN_TEST(test_windowed_card_by_clock_state);
    RUN_TEST(test_uid_must_match_exactly);
    RUN_TEST(test_servo_duty_math);
    RUN_TEST(test_tap_logic_debounce_and_cross_read);
    return UNITY_END();
}
