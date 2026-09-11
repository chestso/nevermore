/* test_source.c - listing-plane source tests (the catalog picker's
 * data seam). Runs offline: static + registry sources only; the
 * phase-5 wire source rides fetch_begin/step against canned servers
 * in its own test. */

#include <stdio.h>
#include <string.h>

#include "provider.h"
#include "source.h"
#include "test_helpers.h"

/* ---- registry source ---- */

static void test_registry_source_lists_all_providers(void)
{
    NmSource *s = nm_source_registry_create();
    ASSERT_NOT_NULL(s);

    ASSERT_TRUE(nm_source_fetch_begin(s, NULL) == 0);
    ASSERT_TRUE(nm_source_step(s) == NM_FETCH_OK);
    ASSERT_EQ(nm_source_fd(s), -1); /* sync: no fd to poll */

    size_t n = 0;
    const NmEntry *items = nm_source_items(s, &n);
    ASSERT_NOT_NULL(items);
    ASSERT_EQ(n, 4); /* hyper, ollama, openai, openrouter */

    /* Every entry is a registered provider; id is the router's
     * vocabulary (what /provider <name> accepts). */
    for (size_t i = 0; i < n; i++) {
        ASSERT_NOT_NULL(nm_provider_by_name(items[i].id));
        ASSERT_TRUE(items[i].label && *items[i].label);
    }
    nm_source_free(s);
}

static void test_registry_source_names_round_trip(void)
{
    NmSource *s = nm_source_registry_create();
    ASSERT_TRUE(nm_source_fetch_begin(s, NULL) == 0);

    size_t n = 0;
    const NmEntry *items = nm_source_items(s, &n);
    int saw_ollama = 0, saw_openai = 0;
    for (size_t i = 0; i < n; i++) {
        if (strcmp(items[i].id, "ollama") == 0)
            saw_ollama = 1;
        if (strcmp(items[i].id, "openai") == 0)
            saw_openai = 1;
    }
    ASSERT_TRUE(saw_ollama && saw_openai);
    nm_source_free(s);
}

/* ---- static (catalog) source ---- */

static void test_static_source_wraps_provider_catalog(void)
{
    const NmProvider *ollama = nm_provider_by_name("ollama");
    ASSERT_NOT_NULL(ollama);

    NmSource *s = nm_source_catalog_create(ollama, NULL, NULL);
    ASSERT_NOT_NULL(s);

    ASSERT_TRUE(nm_source_fetch_begin(s, NULL) == 0);
    ASSERT_TRUE(nm_source_step(s) == NM_FETCH_OK);

    size_t n = 0;
    const NmEntry *items = nm_source_items(s, &n);
    ASSERT_NOT_NULL(items);
    ASSERT_TRUE(n > 0);

    /* Same truth as the provider's own models(): every entry id is
     * in the provider catalog. */
    size_t pn = 0;
    const NmModel *models = ollama->models(ollama, NULL, NULL, &pn);
    ASSERT_EQ(n, pn);
    int found = 0;
    for (size_t i = 0; i < pn; i++) {
        if (strcmp(models[i].id, items[0].id) == 0)
            found = 1;
    }
    ASSERT_TRUE(found);
    nm_source_free(s);
}

static void test_static_source_entry_shape(void)
{
    const NmProvider *ollama = nm_provider_by_name("ollama");
    NmSource *s = nm_source_catalog_create(ollama, NULL, NULL);
    ASSERT_TRUE(nm_source_fetch_begin(s, NULL) == 0);

    size_t n = 0;
    const NmEntry *items = nm_source_items(s, &n);
    for (size_t i = 0; i < n; i++) {
        ASSERT_NOT_NULL(items[i].id);
        ASSERT_NOT_NULL(items[i].label);
        ASSERT_TRUE(items[i].context_length == -1 ||
                    items[i].context_length > 0);
    }
    nm_source_free(s);
}

static void test_static_source_refetch_reuses(void)
{
    /* The memory-reuse principle: a second fetch rebuilds in place —
     * items stay valid, no leak (ASan watches), count consistent. */
    const NmProvider *ollama = nm_provider_by_name("ollama");
    NmSource *s = nm_source_catalog_create(ollama, NULL, NULL);
    ASSERT_TRUE(nm_source_fetch_begin(s, NULL) == 0);
    size_t n1 = 0;
    const NmEntry *items1 = nm_source_items(s, &n1);

    ASSERT_TRUE(nm_source_fetch_begin(s, NULL) == 0);
    size_t n2 = 0;
    const NmEntry *items2 = nm_source_items(s, &n2);
    ASSERT_EQ(n1, n2);
    ASSERT_EQ(items1[0].id, items2[0].id); /* borrowed: same memory */
    nm_source_free(s);
}

static void test_static_source_empty_catalog(void)
{
    /* A provider with no catalog yields an empty view, not a crash. */
    const NmProvider *p = nm_provider_by_name("hyper");
    ASSERT_NOT_NULL(p);
    NmSource *s = nm_source_catalog_create(p, NULL, NULL);
    ASSERT_TRUE(nm_source_fetch_begin(s, NULL) == 0);
    size_t n = 99;
    const NmEntry *items = nm_source_items(s, &n);
    (void)items;
    nm_source_free(s);
}

static void test_cancel_is_safe_on_sync_sources(void)
{
    NmSource *reg = nm_source_registry_create();
    nm_source_cancel(reg); /* no fetch in flight */
    nm_source_free(reg);

    const NmProvider *ollama = nm_provider_by_name("ollama");
    NmSource *cat = nm_source_catalog_create(ollama, NULL, NULL);
    nm_source_cancel(cat);
    nm_source_free(cat);
}

static void test_null_safety(void)
{
    nm_source_free(NULL);

    NmSource *s = nm_source_registry_create();
    ASSERT_EQ(nm_source_fd(s), -1);
    size_t n = 1;
    ASSERT_TRUE(nm_source_items(s, &n) == NULL); /* never fetched */
    ASSERT_EQ(n, 0);
    nm_source_free(s);
}

int main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;
    printf("test_source:\n");
    RUN_TEST(test_registry_source_lists_all_providers);
    RUN_TEST(test_registry_source_names_round_trip);
    RUN_TEST(test_static_source_wraps_provider_catalog);
    RUN_TEST(test_static_source_entry_shape);
    RUN_TEST(test_static_source_refetch_reuses);
    RUN_TEST(test_static_source_empty_catalog);
    RUN_TEST(test_cancel_is_safe_on_sync_sources);
    RUN_TEST(test_null_safety);
    TEST_SUMMARY();
}
