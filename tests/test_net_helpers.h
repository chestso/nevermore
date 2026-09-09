/* Socket helpers shared by the wire tests: WSAStartup on Windows
 * (the tests talk raw sockets directly, not through transport.c),
 * a scratch-dir resolver that works on both platforms, and small
 * MinGW shims. POSIX/Win32 dual. */

#ifndef TEST_NET_HELPERS_H
#define TEST_NET_HELPERS_H

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

#endif /* TEST_NET_HELPERS_H */