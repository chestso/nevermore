/* test_provider.c - provider registry tests. Runs offline. */

#include <stdio.h>

#include "nevermore/provider.h"
#include "test_helpers.h"

static void test_provider_registry_complete(void)
{
    ASSERT_NOT_NULL(nm_provider_get(NM_PROVIDER_HYPER));
    ASSERT_NOT_NULL(nm_provider_get(NM_PROVIDER_OLLAMA));
    ASSERT_NOT_NULL(nm_provider_get(NM_PROVIDER_OPENAI));
    ASSERT_NOT_NULL(nm_provider_get(NM_PROVIDER_OPENROUTER));
}

static void test_provider_lookup_by_name(void)
{
    ASSERT_NOT_NULL(nm_provider_by_name("hyper"));
    ASSERT_NOT_NULL(nm_provider_by_name("ollama"));
    ASSERT_NOT_NULL(nm_provider_by_name("openai"));
    ASSERT_NOT_NULL(nm_provider_by_name("openrouter"));
    ASSERT_NULL(nm_provider_by_name("nope"));
}

int main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;
    printf("test_provider:\n");
    RUN_TEST(test_provider_registry_complete);
    RUN_TEST(test_provider_lookup_by_name);
    TEST_SUMMARY();
}
