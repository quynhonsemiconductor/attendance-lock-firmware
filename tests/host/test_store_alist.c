// Allow-list cache: compact binary blob (cards + ETag), atomic replace,
// loud oversize refusal, size negotiation on read.
#include "unity.h"

#include <string.h>

#include "store/store.h"
#include "store_port_host.h"

void setUp(void)
{
    store_host_reset();
    TEST_ASSERT_TRUE(store_init());
}

void tearDown(void) {}

static store_card_t card4(void)
{
    store_card_t c = {.uid_len = 4, .uid = {0x04, 0xA1, 0xB2, 0xC3}};
    return c;
}

static store_card_t card7(void)
{
    store_card_t c = {.uid_len = 7, .uid = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07},
                      .valid_until = 123};
    return c;
}

static store_card_t card10(void)
{
    store_card_t c = {.uid_len = 10,
                      .uid = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x11, 0x22, 0x33, 0x44, 0x55},
                      .valid_from = 1000,
                      .valid_until = 2000};
    return c;
}

void test_absent_read(void)
{
    store_card_t out[4];
    size_t n = 0, etag_len = 0;
    TEST_ASSERT_EQUAL(STORE_ABSENT,
                      store_allow_list_read(out, 4, &n, NULL, 0, &etag_len));
}

void test_replace_and_read_roundtrip(void)
{
    store_card_t cards[3] = {card4(), card7(), card10()};
    TEST_ASSERT_EQUAL(STORE_OK, store_allow_list_replace(cards, 3, "W/\"abc123\"", 10));
    store_card_t out[4];
    char etag[STORE_ETAG_MAX];
    size_t n = 0, etag_len = 0;
    TEST_ASSERT_EQUAL(STORE_OK,
                      store_allow_list_read(out, 4, &n, etag, sizeof(etag), &etag_len));
    TEST_ASSERT_EQUAL_size_t(3, n);
    TEST_ASSERT_EQUAL_size_t(10, etag_len);
    TEST_ASSERT_EQUAL_MEMORY("W/\"abc123\"", etag, 10);
    TEST_ASSERT_EQUAL_UINT8(4, out[0].uid_len);
    TEST_ASSERT_EQUAL_UINT8(0x04, out[0].uid[0]);
    TEST_ASSERT_EQUAL_UINT32(0, out[0].valid_from); // absent windows stay 0
    TEST_ASSERT_EQUAL_UINT32(0, out[0].valid_until);
    TEST_ASSERT_EQUAL_UINT8(7, out[1].uid_len);
    TEST_ASSERT_EQUAL_UINT32(123, out[1].valid_until);
    TEST_ASSERT_EQUAL_UINT8(10, out[2].uid_len);
    TEST_ASSERT_EQUAL_UINT32(1000, out[2].valid_from);
    TEST_ASSERT_EQUAL_UINT32(2000, out[2].valid_until);
    // Replace is atomic: the new list is readable, whole, after one call.
    store_card_t none[1];
    TEST_ASSERT_EQUAL(STORE_OK, store_allow_list_replace(none, 0, "", 0));
    TEST_ASSERT_EQUAL(STORE_OK,
                      store_allow_list_read(out, 4, &n, etag, sizeof(etag), &etag_len));
    TEST_ASSERT_EQUAL_size_t(0, n);
    TEST_ASSERT_EQUAL_size_t(0, etag_len);
}

void test_read_negotiates_sizes(void)
{
    store_card_t cards[3] = {card4(), card7(), card10()};
    TEST_ASSERT_EQUAL(STORE_OK, store_allow_list_replace(cards, 3, "etag-xyz", 8));
    // Too-small card buffer: the stored sizes come back, retry succeeds.
    store_card_t one[1];
    size_t n = 0, etag_len = 0;
    TEST_ASSERT_EQUAL(STORE_ERR_TOO_SMALL,
                      store_allow_list_read(one, 1, &n, NULL, 0, &etag_len));
    TEST_ASSERT_EQUAL_size_t(3, n);
    TEST_ASSERT_EQUAL_size_t(8, etag_len);
    static store_card_t big[16];
    char etag[STORE_ETAG_MAX];
    TEST_ASSERT_EQUAL(STORE_OK,
                      store_allow_list_read(big, 16, &n, etag, sizeof(etag), &etag_len));
    TEST_ASSERT_EQUAL_size_t(3, n);
}

void test_failed_replace_keeps_old_list_loudly(void)
{
    store_card_t cards[2] = {card4(), card7()};
    TEST_ASSERT_EQUAL(STORE_OK, store_allow_list_replace(cards, 2, "e1", 2));
    // Bad args.
    store_card_t bad = {.uid_len = 5};
    TEST_ASSERT_EQUAL(STORE_ERR_ARG, store_allow_list_replace(&bad, 1, "e", 1));
    TEST_ASSERT_EQUAL(STORE_ERR_ARG, store_allow_list_replace(cards, 2, "e", STORE_ETAG_MAX + 1));
    // NVS failure: loud error, old list intact (a revocation freeze must be
    // visible to the sync task, never silent).
    store_host_fail_nvs_set(1);
    TEST_ASSERT_EQUAL(STORE_ERR_NVS, store_allow_list_replace(cards, 2, "e2", 2));
    store_card_t out[4];
    char etag[STORE_ETAG_MAX];
    size_t n = 0, etag_len = 0;
    TEST_ASSERT_EQUAL(STORE_OK,
                      store_allow_list_read(out, 4, &n, etag, sizeof(etag), &etag_len));
    TEST_ASSERT_EQUAL_size_t(2, n);
    TEST_ASSERT_EQUAL_MEMORY("e1", etag, 2);
}

void test_oversize_is_too_many_and_keeps_old(void)
{
    // STORE_CARDS_MAX entries fit; MAX+1 is refused BEFORE any NVS write,
    // with the old list still active — the loud refusal is what keeps
    // revocations flowing (sync logs it and flags the terminal).
    static store_card_t fits[STORE_CARDS_MAX];
    for (size_t i = 0; i < STORE_CARDS_MAX; i++) {
        fits[i].uid_len = 4;
        fits[i].uid[0] = (uint8_t)i;
    }
    TEST_ASSERT_EQUAL(STORE_OK, store_allow_list_replace(fits, STORE_CARDS_MAX, "full", 4));
    static store_card_t over[STORE_CARDS_MAX + 1];
    for (size_t i = 0; i < STORE_CARDS_MAX + 1; i++) {
        over[i].uid_len = 4;
        over[i].uid[0] = (uint8_t)i;
    }
    TEST_ASSERT_EQUAL(STORE_ERR_TOO_MANY,
                      store_allow_list_replace(over, STORE_CARDS_MAX + 1, "over", 4));
    // Small buffer negotiates; full buffer verifies the old list + etag.
    store_card_t probe[2];
    size_t n = 0, etag_len = 0;
    TEST_ASSERT_EQUAL(STORE_ERR_TOO_SMALL,
                      store_allow_list_read(probe, 2, &n, NULL, 0, &etag_len));
    TEST_ASSERT_EQUAL_size_t(STORE_CARDS_MAX, n);
    TEST_ASSERT_EQUAL_size_t(4, etag_len);
    static store_card_t out[STORE_CARDS_MAX];
    char etag[STORE_ETAG_MAX];
    TEST_ASSERT_EQUAL(STORE_OK,
                      store_allow_list_read(out, STORE_CARDS_MAX, &n, etag, sizeof(etag), &etag_len));
    TEST_ASSERT_EQUAL_size_t(STORE_CARDS_MAX, n);
    TEST_ASSERT_EQUAL_MEMORY("full", etag, 4);
}

void test_cache_survives_reboot(void)
{
    store_card_t cards[1] = {card10()};
    TEST_ASSERT_EQUAL(STORE_OK, store_allow_list_replace(cards, 1, "keep", 4));
    TEST_ASSERT_TRUE(store_init()); // reboot
    store_card_t out[2];
    char etag[STORE_ETAG_MAX];
    size_t n = 0, etag_len = 0;
    TEST_ASSERT_EQUAL(STORE_OK,
                      store_allow_list_read(out, 2, &n, etag, sizeof(etag), &etag_len));
    TEST_ASSERT_EQUAL_size_t(1, n);
    TEST_ASSERT_EQUAL_UINT32(2000, out[0].valid_until);
    TEST_ASSERT_EQUAL_MEMORY("keep", etag, 4);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_absent_read);
    RUN_TEST(test_replace_and_read_roundtrip);
    RUN_TEST(test_read_negotiates_sizes);
    RUN_TEST(test_failed_replace_keeps_old_list_loudly);
    RUN_TEST(test_oversize_is_too_many_and_keeps_old);
    RUN_TEST(test_cache_survives_reboot);
    return UNITY_END();
}
