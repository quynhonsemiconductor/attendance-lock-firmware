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

void test_tap_logic_full_table_evicts_least_recent(void)
{
    uint8_t state[STATE_CAP] = { 0 };
    // Fill all 8 slots: cards 1..8 at 1000, 1100, ... 1700.
    for (uint8_t k = 1; k <= 8; k++) {
        reader_card_t c = { .reader = 0, .uid_len = 4, .uid = { k, k, k, k } };
        TEST_ASSERT_TRUE(reader_accept(state, sizeof(state), &c, 900 + 100u * k, 1000, 800));
    }
    // Card 1 (slot 0) tapped again: now the most recent entry.
    reader_card_t c1 = { .reader = 0, .uid_len = 4, .uid = { 1, 1, 1, 1 } };
    TEST_ASSERT_TRUE(reader_accept(state, sizeof(state), &c1, 2500, 1000, 800));
    // A 9th card evicts card 2 (oldest), not slot 0.
    reader_card_t c9 = { .reader = 0, .uid_len = 4, .uid = { 9, 9, 9, 9 } };
    TEST_ASSERT_TRUE(reader_accept(state, sizeof(state), &c9, 2600, 1000, 800));
    // Card 1 still held: its debounce state survived.
    TEST_ASSERT_FALSE(reader_accept(state, sizeof(state), &c1, 2700, 1000, 800));
}

// Bench 2026-10-31: three real cards produced three ghost UIDs on reader 1
// (seq 1010-1012) that went to opshub as unknown-card events.

void test_bcc_accepts_a_valid_anticoll_frame(void)
{
    // UID 2B14C87E with the ISO 14443-3 BCC (XOR of the four bytes).
    uint8_t uid[4] = { 0x2B, 0x14, 0xC8, 0x7E };
    uint8_t bcc = (uint8_t)(0x2B ^ 0x14 ^ 0xC8 ^ 0x7E);
    TEST_ASSERT_TRUE(reader_bcc_ok(uid, bcc));
}

void test_bcc_rejects_a_corrupted_anticoll_frame(void)
{
    uint8_t uid[4] = { 0x2B, 0x14, 0xC8, 0x7E };
    uint8_t bcc = (uint8_t)(0x2B ^ 0x14 ^ 0xC8 ^ 0x7E);
    // One corrupted UID bit no longer matches the card's BCC: the driver
    // rejects the read (counted misread, no event, no beep).
    uint8_t bad[4] = { 0x2B, 0x15, 0xC8, 0x7E };
    TEST_ASSERT_FALSE(reader_bcc_ok(bad, bcc));
    // A corrupted BCC byte is rejected too.
    TEST_ASSERT_FALSE(reader_bcc_ok(uid, (uint8_t)(bcc ^ 0x80)));
    TEST_ASSERT_FALSE(reader_bcc_ok(NULL, bcc));
}

void test_confirm_read_rejects_a_flapping_uid(void)
{
    // Two independent reads inside one poll must agree; a one-off corrupted
    // read (the ghost-UID shape) differs and is rejected.
    uint8_t real[4] = { 0x2B, 0x14, 0xC8, 0x7E };
    uint8_t ghost[4] = { 0xAA, 0x53, 0xD2, 0x04 };
    TEST_ASSERT_TRUE(reader_uid_confirmed(real, real, 4));
    TEST_ASSERT_FALSE(reader_uid_confirmed(real, ghost, 4));
    TEST_ASSERT_FALSE(reader_uid_confirmed(NULL, real, 4));
    TEST_ASSERT_FALSE(reader_uid_confirmed(real, real, 7)); // CL1 only: no 7/10-byte UIDs
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
    RUN_TEST(test_tap_logic_full_table_evicts_least_recent);
    RUN_TEST(test_tap_logic_bad_state_rejected);
    RUN_TEST(test_bcc_accepts_a_valid_anticoll_frame);
    RUN_TEST(test_bcc_rejects_a_corrupted_anticoll_frame);
    RUN_TEST(test_confirm_read_rejects_a_flapping_uid);
    return UNITY_END();
}
