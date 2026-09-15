/* conversation_id.c - per-conversation routing id (provider support)
 *
 * One implementation of nm_conversation_id_new (declared in
 * provider_internal.h). It cannot live in agent.c as an early draft
 * placed it: the providers call it too, and the provider-only test
 * link sets deliberately do not link the agent (and with it the
 * session/tools stack). A tiny standalone TU keeps the seam's
 * "one declaration, one implementation" rule without dragging the
 * agent graph into every provider test.
 *
 * The id is a provider-scoped routing hint (x-opencode-session), not
 * a secret: it has no authentication role. It only has to be stable
 * per conversation and non-colliding across concurrent clients
 * (docs/OPENCODE-PROVIDER-DESIGN.md §3), which is why the no-new-
 * library fallback below is safe for every platform.
 *
 * Entropy, decided (design §1a: no new link libraries):
 *   - POSIX: /dev/urandom — an OS file, not a dependency. Read once;
 *     failure falls through to the mix rather than failing.
 *   - Windows: a 128-bit splitmix64 mix of clock + pid + counter +
 *     the address of a file-static sentinel. No BCryptGenRandom, no
 *     -lbcrypt (dropped outright against the dependency budget); no
 *     heap allocation is made just to take an address.
 * Neither path blocks: the whole thing is open/read once or a few
 * arithmetic ops.
 */

#include "provider_internal.h"

#include <stdint.h>
#include <string.h>

#include "nm_clock.h"

#ifdef _WIN32
#include <process.h>
#define NM_GETPID _getpid
#else
#include <fcntl.h>
#include <unistd.h>
#define NM_GETPID getpid
#endif

/* splitmix64: a well-mixed, non-crypto 64-bit generator. */
static unsigned long long splitmix64(unsigned long long *state)
{
    unsigned long long z = (*state += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

void nm_conversation_id_new(char out[NM_CONVERSATION_ID_LEN])
{
    unsigned char bytes[16];
    int have = 0;

#ifndef _WIN32
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) {
        ssize_t n = read(fd, bytes, sizeof(bytes));
        close(fd);
        if (n == (ssize_t)sizeof(bytes))
            have = 1;
    }
#endif
    if (!have) {
        static unsigned long long counter;
        static const char sentinel;
        unsigned long long seed =
            (unsigned long long)(nm_monotonic_seconds() * 1e9) ^
            ((unsigned long long)NM_GETPID() << 32) ^ (++counter) ^
            (unsigned long long)(uintptr_t)&sentinel;
        unsigned long long a = splitmix64(&seed);
        unsigned long long b = splitmix64(&seed);
        memcpy(bytes, &a, 8);
        memcpy(bytes + 8, &b, 8);
    }

    /* "nm-" + 32 lowercase hex (35 chars + NUL). */
    static const char hex[] = "0123456789abcdef";
    out[0] = 'n';
    out[1] = 'm';
    out[2] = '-';
    for (int i = 0; i < 16; i++) {
        out[3 + i * 2] = hex[bytes[i] >> 4];
        out[4 + i * 2] = hex[bytes[i] & 0xf];
    }
    out[35] = '\0';
}
