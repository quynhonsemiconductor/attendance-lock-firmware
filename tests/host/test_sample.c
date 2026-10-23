// Sample test: proves the harness runs and a component stub links on the host.
// Firmware v1 replaces this with real contract-fixture tests (work item 7).
#include "unity.h"
#include "store/store.h"

void setUp(void) {}
void tearDown(void) {}

void test_ring_capacity_is_positive(void)
{
    TEST_ASSERT_GREATER_THAN_size_t(0, store_ring_capacity());
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_ring_capacity_is_positive);
    return UNITY_END();
}
