// Allow-list cache: atomic replace, failure keeps the old list, size caps.
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

void test_no_cache_read_fails(void)
{
    char buf[64];
    size_t len;
    TEST_ASSERT_FALSE(store_allow_list_read(buf, sizeof(buf), &len));
}

void test_replace_and_read_roundtrip(void)
{
    const char *body1 = "{\"cards\":[{\"uid\":\"04A1B2C3\"}]}";
    TEST_ASSERT_TRUE(store_allow_list_replace(body1, strlen(body1)));
    char buf[128];
    size_t len = 0;
    TEST_ASSERT_TRUE(store_allow_list_read(buf, sizeof(buf), &len));
    TEST_ASSERT_EQUAL_size_t(strlen(body1), len);
    TEST_ASSERT_EQUAL_MEMORY(body1, buf, len);
    // Replace is atomic: the new list is readable, whole, after one call.
    const char *body2 = "{\"cards\":[]}";
    TEST_ASSERT_TRUE(store_allow_list_replace(body2, strlen(body2)));
    len = 0;
    TEST_ASSERT_TRUE(store_allow_list_read(buf, sizeof(buf), &len));
    TEST_ASSERT_EQUAL_size_t(strlen(body2), len);
    TEST_ASSERT_EQUAL_MEMORY(body2, buf, len);
}

void test_failed_replace_keeps_old_list(void)
{
    const char *body = "{\"cards\":[{\"uid\":\"04A1B2C3\"}]}";
    TEST_ASSERT_TRUE(store_allow_list_replace(body, strlen(body)));
    TEST_ASSERT_FALSE(store_allow_list_replace(body, 0)); // empty
    TEST_ASSERT_FALSE(store_allow_list_replace(body, STORE_ALLOW_LIST_MAX + 1));
    char buf[128];
    size_t len = 0;
    TEST_ASSERT_TRUE(store_allow_list_read(buf, sizeof(buf), &len));
    TEST_ASSERT_EQUAL_size_t(strlen(body), len);
    TEST_ASSERT_EQUAL_MEMORY(body, buf, len);
}

void test_max_size_boundary(void)
{
    static char big[STORE_ALLOW_LIST_MAX];
    memset(big, 'x', sizeof(big) - 1);
    TEST_ASSERT_TRUE(store_allow_list_replace(big, sizeof(big) - 1));
    static char buf[STORE_ALLOW_LIST_MAX];
    size_t len = 0;
    TEST_ASSERT_TRUE(store_allow_list_read(buf, sizeof(buf), &len));
    TEST_ASSERT_EQUAL_size_t(sizeof(big) - 1, len);
    TEST_ASSERT_TRUE(store_allow_list_replace(big, sizeof(big))); // one over
    TEST_ASSERT_FALSE(store_allow_list_read == NULL); // keep the old list valid
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_no_cache_read_fails);
    RUN_TEST(test_replace_and_read_roundtrip);
    RUN_TEST(test_failed_replace_keeps_old_list);
    RUN_TEST(test_max_size_boundary);
    return UNITY_END();
}
