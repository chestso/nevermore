/* Socket helpers shared by the wire tests: WSAStartup on Windows
 * (the tests talk raw sockets directly, not through transport.c),
 * a scratch-dir resolver that works on both platforms, and small
 * MinGW shims. POSIX/Win32 dual. */

#ifndef TEST_NET_HELPERS_H
#define TEST_NET_HELPERS_H

#include <stdio.h>
#include <sys/stat.h>
#include <sys/types.h>
#ifdef _WIN32
#include <direct.h> /* _chdir/_getpid */
#include <process.h>
#define getpid      _getpid
#define mkdir(d, m) _mkdir(d)
#endif

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>

/* MinGW shims: close() -> closesocket(), usleep() -> Sleep(). */
#define close(s)   closesocket(s)
#define usleep(us) Sleep((DWORD)((us) / 1000))

/* Every socket call in the tests needs WSA initialized first. */
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
#if defined(__GNUC__) || defined(__clang__)
#define TEST_NET_HELPERS_UNUSED __attribute__((unused))
#else
#define TEST_NET_HELPERS_UNUSED
#endif

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

#endif /* TEST_NET_HELPERS_H */