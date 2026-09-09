#ifndef NM_TEST_HELPERS_H
#define NM_TEST_HELPERS_H

/* Ported from portty/coffer house pattern: RUN_TEST / TEST_SUMMARY
 * macros + ASSERT_* helpers. Every test binary is standalone and
 * links libnevermore.a. Pass -v for verbose output. */

#include <stdio.h>
#include <string.h>

#if defined(__GNUC__) || defined(__clang__)
#define TEST_HELPERS_UNUSED __attribute__((unused))
#else
#define TEST_HELPERS_UNUSED
#endif

static TEST_HELPERS_UNUSED int test_pass_count = 0;
static TEST_HELPERS_UNUSED int test_fail_count = 0;

#define ASSERT_TRUE(expr)                                                      \
    do {                                                                       \
        if (!(expr)) {                                                         \
            fprintf(stderr, "  FAIL: %s:%d: %s\n", __FILE__, __LINE__, #expr); \
            test_fail_count++;                                                 \
            return;                                                            \
        }                                                                      \
    } while (0)

#define ASSERT_FALSE(expr) ASSERT_TRUE(!(expr))

#define ASSERT_EQ(a, b)                                                           \
    do {                                                                          \
        if ((a) != (b)) {                                                         \
            fprintf(stderr, "  FAIL: %s:%d: %s == %s (%lld != %lld)\n", __FILE__, \
                    __LINE__, #a, #b, (long long)(a), (long long)(b));            \
            test_fail_count++;                                                    \
            return;                                                               \
        }                                                                         \
    } while (0)

#define ASSERT_NULL(a)     ASSERT_TRUE((a) == NULL)
#define ASSERT_NOT_NULL(a) ASSERT_TRUE((a) != NULL)

#define ASSERT_STR_EQ(a, b)                                                 \
    do {                                                                    \
        const char *_a = (a);                                               \
        const char *_b = (b);                                               \
        if (_a == NULL && _b == NULL)                                       \
            break;                                                          \
        if (_a == NULL || _b == NULL || strcmp(_a, _b) != 0) {              \
            fprintf(stderr, "  FAIL: %s:%d: %s == %s (\"%s\" != \"%s\")\n", \
                    __FILE__, __LINE__, #a, #b, _a ? _a : "(null)",         \
                    _b ? _b : "(null)");                                    \
            test_fail_count++;                                              \
            return;                                                         \
        }                                                                   \
    } while (0)

#define RUN_TEST(fn)                      \
    do {                                  \
        int _before = test_fail_count;    \
        fn();                             \
        if (test_fail_count == _before) { \
            printf("  PASS: %s\n", #fn);  \
            test_pass_count++;            \
        } else {                          \
            printf("  FAIL: %s\n", #fn);  \
        }                                 \
    } while (0)

#define TEST_SUMMARY()                            \
    do {                                          \
        printf("\n%d passed, %d failed\n",        \
               test_pass_count, test_fail_count); \
        return test_fail_count > 0 ? 1 : 0;       \
    } while (0)

#endif // NM_TEST_HELPERS_H
