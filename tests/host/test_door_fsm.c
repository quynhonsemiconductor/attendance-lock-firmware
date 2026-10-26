// The door FSM (host-safe): decision + unlock/hold/relock + feedback
// patterns + the deny overrides. The old per-tap decision tests live here
// too — door_decide moved into door_fsm (PR4b).
#include "unity.h"

#include <string.h>

#include "door_fsm/door_fsm.h"

#define T_NOW 1792742462ULL
#define HOLD_MS 5000u

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

// --- decision ------------------------------------------------------------

void test_permanent_card_allowed_with_and_without_clock(void)
{
    store_card_t cards[1] = { permanent() };
    uint8_t uid[4] = { 0x04, 0xA1, 0xB2, 0xC3 };
    // No-clock rule: permanent cards keep working (§2.4).
    TEST_ASSERT_EQUAL(DOOR_ALLOW, door_decide(cards, 1, false, 0, uid, 4));
    TEST_ASSERT_EQUAL(DOOR_ALLOW, door_decide(cards, 1, true, T_NOW, uid, 4));
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

// --- FSM: boot locked, allow -> hold -> relock ---------------------------

void test_boots_locked(void)
{
    door_fsm_t fsm;
    door_fsm_init(&fsm, HOLD_MS);
    TEST_ASSERT_FALSE(door_fsm_released(&fsm));
    door_out_t out = door_fsm_tick(&fsm, 1000);
    TEST_ASSERT_TRUE(out.engage);
    TEST_ASSERT_FALSE(out.release);
}

void test_allow_unlocks_then_relocks_after_hold(void)
{
    door_fsm_t fsm;
    door_fsm_init(&fsm, HOLD_MS);
    door_out_t out = door_fsm_apply(&fsm, 1000, DOOR_ALLOW);
    TEST_ASSERT_TRUE(out.release);
    TEST_ASSERT_TRUE(out.green);
    TEST_ASSERT_TRUE(out.buzzer); // single beep starts
    TEST_ASSERT_TRUE(door_fsm_released(&fsm));
    // Mid-window: still released, green held.
    out = door_fsm_tick(&fsm, 1000 + HOLD_MS - 1);
    TEST_ASSERT_TRUE(out.release);
    TEST_ASSERT_TRUE(out.green);
    // Past the hold: relocked, feedback dark.
    out = door_fsm_tick(&fsm, 1000 + HOLD_MS);
    TEST_ASSERT_TRUE(out.engage);
    TEST_ASSERT_FALSE(out.release);
    TEST_ASSERT_FALSE(out.green);
    TEST_ASSERT_FALSE(door_fsm_released(&fsm));
}

void test_retap_during_window_extends_it(void)
{
    door_fsm_t fsm;
    door_fsm_init(&fsm, HOLD_MS);
    (void)door_fsm_apply(&fsm, 1000, DOOR_ALLOW);
    (void)door_fsm_apply(&fsm, 4000, DOOR_ALLOW); // re-tap at 3 s
    // 5 s after the FIRST tap the window is not over: the re-tap extended it.
    door_out_t out = door_fsm_tick(&fsm, 6000);
    TEST_ASSERT_TRUE(out.release);
    out = door_fsm_tick(&fsm, 4000 + HOLD_MS);
    TEST_ASSERT_TRUE(out.engage);
    TEST_ASSERT_FALSE(door_fsm_released(&fsm));
}

// --- FSM: denials --------------------------------------------------------

void test_deny_keeps_locked_with_red_and_double_beep(void)
{
    door_fsm_t fsm;
    door_fsm_init(&fsm, HOLD_MS);
    door_out_t out = door_fsm_apply(&fsm, 1000, DOOR_DENY_UNKNOWN);
    TEST_ASSERT_TRUE(out.engage);
    TEST_ASSERT_TRUE(out.red);
    TEST_ASSERT_FALSE(out.green);
    TEST_ASSERT_TRUE(out.buzzer);
    // First beep ends at 1120; gap until 1240; second beep on.
    out = door_fsm_tick(&fsm, 1120);
    TEST_ASSERT_FALSE(out.buzzer);
    out = door_fsm_tick(&fsm, 1240);
    TEST_ASSERT_TRUE(out.buzzer);
    out = door_fsm_tick(&fsm, 1360);
    TEST_ASSERT_FALSE(out.buzzer); // pattern done
    // Red outlives the beeps and clears after 2 s.
    out = door_fsm_tick(&fsm, 2999);
    TEST_ASSERT_TRUE(out.red);
    out = door_fsm_tick(&fsm, 3000);
    TEST_ASSERT_FALSE(out.red);
    TEST_ASSERT_FALSE(door_fsm_released(&fsm));
}

void test_battery_deny_does_not_slam_an_open_window(void)
{
    // A deny (any reason) during an open window: the window keeps running.
    door_fsm_t fsm;
    door_fsm_init(&fsm, HOLD_MS);
    (void)door_fsm_apply(&fsm, 1000, DOOR_ALLOW);
    door_out_t out = door_fsm_apply(&fsm, 2000, DOOR_DENY_BATTERY);
    TEST_ASSERT_TRUE(out.release); // still released
    TEST_ASSERT_FALSE(out.engage); // and the caller must NOT re-engage it
    TEST_ASSERT_TRUE(out.green);
    out = door_fsm_tick(&fsm, 1000 + HOLD_MS);
    TEST_ASSERT_TRUE(out.engage);
}

void test_reader_fault_marks_red_and_keeps_state(void)
{
    door_fsm_t fsm;
    door_fsm_init(&fsm, HOLD_MS);
    door_out_t out = door_fsm_fault(&fsm, 1000);
    TEST_ASSERT_TRUE(out.engage);
    TEST_ASSERT_TRUE(out.red);
    TEST_ASSERT_FALSE(door_fsm_released(&fsm));
    // Fault during an open window: no slam.
    (void)door_fsm_apply(&fsm, 2000, DOOR_ALLOW);
    out = door_fsm_fault(&fsm, 2500);
    TEST_ASSERT_TRUE(out.release);
    TEST_ASSERT_TRUE(door_fsm_released(&fsm));
    out = door_fsm_tick(&fsm, 2000 + HOLD_MS);
    TEST_ASSERT_TRUE(out.engage);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_permanent_card_allowed_with_and_without_clock);
    RUN_TEST(test_unknown_uid_denied);
    RUN_TEST(test_windowed_card_fails_closed_without_clock);
    RUN_TEST(test_windowed_card_by_clock_state);
    RUN_TEST(test_uid_must_match_exactly);
    RUN_TEST(test_boots_locked);
    RUN_TEST(test_allow_unlocks_then_relocks_after_hold);
    RUN_TEST(test_retap_during_window_extends_it);
    RUN_TEST(test_deny_keeps_locked_with_red_and_double_beep);
    RUN_TEST(test_battery_deny_does_not_slam_an_open_window);
    RUN_TEST(test_reader_fault_marks_red_and_keeps_state);
    return UNITY_END();
}
