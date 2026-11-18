/* test_keepalive_pin.h - the suite's keepalive pin
 *
 * Every test binary that dials pins `keepalive` OFF on its config
 * store, so the Phase 6 default-ON flip — and a developer's
 * NEVERMORE_KEEPALIVE=on environment — never reaches the suite: the
 * canned servers are single-accept, and under a pool a reply without
 * `Connection: close` is a keep-alive invitation (CONTRIBUTING's
 * canned-server rule). `tests/test_keepalive.c` is the opt-in: it
 * sets the key per test on the same RUNTIME layer the pin uses.
 *
 * The runtime layer is the TOP layer (runtime > cli > env > shadow >
 * user), so the pin outvotes everything a developer could export.
 *
 * Separate from test_net_helpers.h on purpose: that header is
 * included by binaries that do not link conf.c (a pure-FS test's
 * scratch-dir helpers); this one is only for the dialing mains, and
 * the tripwire below is what makes a dropped pin loud instead of a
 * Phase 6 surprise.
 */

#ifndef TEST_KEEPALIVE_PIN_H
#define TEST_KEEPALIVE_PIN_H

#include "conf.h"

static int test_keepalive_pin_done;

/* Call right after the store is created (and again for every store a
 * test installs mid-run — test_provider swaps them). */
static void test_net_pin_keepalive_off(NmConfig *store)
{
    if (!store)
        return;
    nm_config_runtime_set(store, NM_CFG_KEY_KEEPALIVE, "off");
    test_keepalive_pin_done = 1;
}

/* Expand ONCE at file scope, then RUN_TEST as the FIRST test in the
 * binary (the TEST_OFFLINE_CATALOG_PIN_CHECK precedent). */
#define TEST_KEEPALIVE_PIN_CHECK()                         \
    static void test_keepalive_pin_is_set(void)            \
    {                                                      \
        if (test_keepalive_pin_done)                       \
            return;                                        \
        fprintf(stderr,                                    \
                "  FAIL: %s: keepalive not pinned - call " \
                "test_net_pin_keepalive_off() in main\n",  \
                __FILE__);                                 \
        test_fail_count++;                                 \
    }

#endif /* TEST_KEEPALIVE_PIN_H */
