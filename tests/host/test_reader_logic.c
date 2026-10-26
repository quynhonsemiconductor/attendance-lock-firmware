// Reader tap acceptance (host-safe slice).
#include "unity.h"

#include "reader/reader.h"

#define STATE_CAP 256

void setUp(void) {}
void tearDown(void) {}

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

void test_tap_logic_bad_state_rejected(void)
{
    reader_card_t c = { .reader = 0, .uid_len = 4, .uid = { 1 } };
    TEST_ASSERT_FALSE(reader_accept(NULL, STATE_CAP, &c, 1000, 1000, 800));
    uint8_t small[8] = { 0 };
    TEST_ASSERT_FALSE(reader_accept(small, sizeof(small), &c, 1000, 1000, 800));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_tap_logic_debounce_and_cross_read);
    RUN_TEST(test_tap_logic_bad_state_rejected);
    return UNITY_END();
}
