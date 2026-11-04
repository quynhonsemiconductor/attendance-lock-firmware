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

// PIR-gated cadence (battery work 2026-11-01): fast 400 ms polls while there
// is motion and for 10 s after it (1 s tap-to-read target), 5 s otherwise.
#define ACTIVE 10000u
#define POLL 400u
#define IDLE 5000u

void test_scan_plan_idle_before_any_motion(void)
{
    reader_scan_plan_t p = reader_scan_plan(1000, false, false, 0, ACTIVE, POLL, IDLE);
    TEST_ASSERT_FALSE(p.fast);
    TEST_ASSERT_EQUAL_UINT32(IDLE, p.wait_ms);
}

void test_scan_plan_fast_while_pir_high(void)
{
    // High level wins even with a stale last-motion stamp.
    reader_scan_plan_t p = reader_scan_plan(90000, true, true, 1000, ACTIVE, POLL, IDLE);
    TEST_ASSERT_TRUE(p.fast);
    TEST_ASSERT_EQUAL_UINT32(POLL, p.wait_ms);
}

void test_scan_plan_window_after_motion_then_idle(void)
{
    // Person stopped moving at t=20000 (finding the card): still fast.
    reader_scan_plan_t p = reader_scan_plan(20000 + ACTIVE - 1, false, true, 20000, ACTIVE, POLL,
                                            IDLE);
    TEST_ASSERT_TRUE(p.fast);
    TEST_ASSERT_EQUAL_UINT32(POLL, p.wait_ms);
    // Window over: back to the fallback cadence.
    p = reader_scan_plan(20000 + ACTIVE, false, true, 20000, ACTIVE, POLL, IDLE);
    TEST_ASSERT_FALSE(p.fast);
    TEST_ASSERT_EQUAL_UINT32(IDLE, p.wait_ms);
}

void test_scan_plan_survives_ms_clock_wrap(void)
{
    // Motion 1 s before the uint32 ms clock wraps, checked 2 s after.
    uint32_t last = 0xFFFFFFFFu - 999u;
    reader_scan_plan_t p = reader_scan_plan(2000, false, true, last, ACTIVE, POLL, IDLE);
    TEST_ASSERT_TRUE(p.fast);
    p = reader_scan_plan(last + ACTIVE, false, true, last, ACTIVE, POLL, IDLE);
    TEST_ASSERT_FALSE(p.fast);
}

// --- stuck PIR (deep-sleep gate) ------------------------------------------

void test_pir_stuck_only_after_a_long_unbroken_high(void)
{
    TEST_ASSERT_FALSE(reader_pir_stuck(false, 0, 120000, 60000)); // low: never
    TEST_ASSERT_FALSE(reader_pir_stuck(true, 1000, 60999, 60000));
    TEST_ASSERT_TRUE(reader_pir_stuck(true, 1000, 61000, 60000));
    TEST_ASSERT_FALSE(reader_pir_stuck(true, 0, 999999, 0)); // 0 = feature off (READER_PIR_STUCK_S=0)
}

void test_pir_stuck_survives_ms_clock_wrap(void)
{
    TEST_ASSERT_FALSE(reader_pir_stuck(true, 0xFFFFF000u, 0x00001000u, 60000));
    TEST_ASSERT_TRUE(reader_pir_stuck(true, 0xFFFFF000u, 0x00010000u, 60000));
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
    RUN_TEST(test_scan_plan_idle_before_any_motion);
    RUN_TEST(test_scan_plan_fast_while_pir_high);
    RUN_TEST(test_scan_plan_window_after_motion_then_idle);
    RUN_TEST(test_scan_plan_survives_ms_clock_wrap);
    RUN_TEST(test_pir_stuck_only_after_a_long_unbroken_high);
    RUN_TEST(test_pir_stuck_survives_ms_clock_wrap);
    return UNITY_END();
}
