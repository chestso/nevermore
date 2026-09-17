/* Socket helpers shared by the wire tests: WSAStartup on Windows
 * (the tests talk raw sockets directly, not through transport.c),
 * a scratch-dir resolver that works on both platforms, small MinGW
 * shims, and the offline-catalog pin every app/provider test needs.
 * POSIX/Win32 dual. */

#ifndef TEST_NET_HELPERS_H
#define TEST_NET_HELPERS_H

#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>
#ifdef _WIN32
#include <direct.h> /* _chdir/_getpid */
#include <process.h>
#define getpid      _getpid
#define mkdir(d, m) _mkdir(d)
#endif

#if defined(__GNUC__) || defined(__clang__)
#define TEST_NET_HELPERS_UNUSED __attribute__((unused))
#else
#define TEST_NET_HELPERS_UNUSED
#endif

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>

/* MinGW shims: close() -> closesocket(), usleep() -> Sleep(). */
#define close(s)   closesocket(s)
#define usleep(us) Sleep((DWORD)((us) / 1000))

/* Every socket call in the tests needs WSA initialized first. */
static int test_wsa_init(void) TEST_NET_HELPERS_UNUSED;
static int test_wsa_init(void)
{
    WSADATA d;
    return WSAStartup(MAKEWORD(2, 2), &d) == 0 ? 0 : -1;
}
#else
#include <arpa/inet.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>

static int test_wsa_init(void) TEST_NET_HELPERS_UNUSED;
static int test_wsa_init(void)
{
    return 0; /* POSIX: nothing to init */
}
#endif

/* Pin the process cwd to a scratch directory (per-pid temp dir).
 *
 * Why: agents discover AGENTS.md from the working directory. A test
 * that runs inside the repo tree would pick up the repo's own
 * AGENTS.md — which is git-excluded, so CI checkouts would behave
 * differently from a dev box (and the extra ~20 KiB changes the
 * captured wire request size). Test agents must start somewhere
 * with no context files.
 *
 * Returns 0 on success. Call from main() before any agent is built.
 * Some test binaries (json, sse, wire) never build one, so the
 * definition is marked unused — -Wall stays clean either way. */
static int test_chdir_to_scratch(void) TEST_NET_HELPERS_UNUSED;
static int test_chdir_to_scratch(void)
{
    static char dir[512];
#ifdef _WIN32
    snprintf(dir, sizeof(dir), "C:/Users/Public/nm-test-cwd-%d",
             (int)getpid());
    mkdir(dir, 0755);
    return _chdir(dir) == 0 ? 0 : -1;
#else
    snprintf(dir, sizeof(dir), "/tmp/nm-test-cwd-%d", (int)getpid());
    mkdir(dir, 0755);
    return chdir(dir) == 0 ? 0 : -1;
#endif
}

/* Pin the model catalogs offline for the whole test process.
 *
 * Why: a catalog lookup with NO endpoint override resolves the
 * provider's DEFAULT base (api.ollama.com, opencode.ai, ...) and,
 * since the live gate is on unless NM_NO_LIVE_CATALOG is set, goes
 * straight to the real service. That breaks the make-check rule
 * (canned loopback servers only), and it is not a benign extra
 * round-trip: the fetched list REPLACES the static catalog the tests
 * assert against (the 2026-09-17 flake — model ids differed per run
 * in test_chat_app, and a slow fetch blew the per-test watchdog),
 * the probe blocks, and a networked CI runner takes the same path
 * because no workflow exports the gate.
 *
 * NM_NO_LIVE_CATALOG is the production knob nm_live_catalog_enabled()
 * reads; setting it here rather than asking every caller to export it
 * keeps "tests never probe a default base" true by construction. The
 * canned-base fetches the wire tests DO want are unaffected — an
 * explicit base_url is honored whatever the gate says.
 *
 * Returns 0 on success. Call from main() before any app, agent or
 * provider call, and pair it with TEST_OFFLINE_CATALOG_PIN_CHECK. */
static int test_pin_offline_catalog(void) TEST_NET_HELPERS_UNUSED;
static int test_pin_offline_catalog(void)
{
#ifdef _WIN32
    return _putenv_s("NM_NO_LIVE_CATALOG", "1") == 0 ? 0 : -1;
#else
    return setenv("NM_NO_LIVE_CATALOG", "1", 1) == 0 ? 0 : -1;
#endif
}

/* Is the pin in effect? (The tripwire's question.) */
static int test_offline_catalog_pinned(void) TEST_NET_HELPERS_UNUSED;
static int test_offline_catalog_pinned(void)
{
    return getenv("NM_NO_LIVE_CATALOG") != NULL ? 1 : 0;
}

/* Tripwire for the pin above: expand ONCE at file scope, then
 * RUN_TEST(test_offline_catalog_is_pinned) as the FIRST test in a
 * binary that touches provider catalogs. A dropped
 * test_pin_offline_catalog() call then fails loudly instead of
 * quietly probing a real endpoint. Uses test_helpers.h's counter, so
 * include that first. */
#define TEST_OFFLINE_CATALOG_PIN_CHECK()                      \
    static void test_offline_catalog_is_pinned(void)          \
    {                                                         \
        if (test_offline_catalog_pinned())                    \
            return;                                           \
        fprintf(stderr,                                       \
                "  FAIL: %s: live catalog not pinned - call " \
                "test_pin_offline_catalog() in main\n",       \
                __FILE__);                                    \
        test_fail_count++;                                    \
    }

#endif /* TEST_NET_HELPERS_H */