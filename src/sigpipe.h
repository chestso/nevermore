/* sigpipe.h - a failed socket write is an error, not a signal
 *
 * A `write()` into a peer that has closed raises SIGPIPE at its DEFAULT
 * disposition, and the default action kills the process before the
 * write's `EPIPE` ever reaches its caller. `EPIPE` is what the write
 * RETURNS when the signal is blocked or ignored — the two are not
 * alternatives, on Linux any more than on the BSDs.
 *
 * That matters here because a provider can close on us mid-request (a
 * 413 and a hangup while the body is still going out), and because the
 * TLS handshake and close write into the same fd, so a peer that dies
 * there is the same death. Both are ordinary failures the transport
 * already has a return path for; only the signal turns them into a
 * death.
 *
 * THE MECHANISM IS PER PLATFORM, AND `MSG_NOSIGNAL` ALONE IS NOT ENOUGH:
 *
 *  - BSD/macOS: SO_NOSIGPIPE on the fd (`nm_socket_quiet_sigpipe`).
 *    A socket option beats a per-call flag precisely because EVERY
 *    write on that fd obeys it — including the ones OpenSSL and mbedTLS
 *    make inside SSL_write / ssl_write / the handshake, where we cannot
 *    pass MSG_NOSIGNAL at all.
 *  - Linux: there is no SO_NOSIGPIPE. `MSG_NOSIGNAL` covers our own
 *    send(); the guard below covers everything else, the TLS backends
 *    included. It blocks SIGPIPE for one call, and — the part that is
 *    easy to get wrong — CONSUMES the signal the kernel parked before
 *    restoring the mask, or it is delivered the moment the mask lifts
 *    and the death happens anyway.
 *  - Windows: no SIGPIPE exists; a dead peer is a send() error code the
 *    transport already reports. Both helpers are no-ops.
 *
 * WHY NOT `signal(SIGPIPE, SIG_IGN)` ONCE AT STARTUP: a disposition is
 * inherited state and SIG_IGN survives `execve`. Every tool child
 * (posix_spawn sets no POSIX_SPAWN_SETSIGDEF) and every `!` shell would
 * inherit the ignore, so `yes | head` in a shell would spin on EPIPE
 * instead of dying; and a CLI pipe (`nevermore ask ... | head`) would
 * start printing write errors where dying quietly is the convention.
 * The disposition is not ours to change process-wide.
 *
 * Header-only on purpose (`clock.h` / `colors.h` precedent): two static
 * inline helpers and a struct, so every TU that writes to a socket gets
 * them without a new object file and a `_SOURCES` line in each of the
 * test binaries.
 */

#ifndef NM_SIGPIPE_H
#define NM_SIGPIPE_H

/* The guard is only needed where SO_NOSIGPIPE does not exist AND
 * sigtimedwait does: that is Linux. Darwin has the socket option and no
 * sigtimedwait; Windows has no SIGPIPE. */
#if !defined(_WIN32)
#include <errno.h>
#include <signal.h>
#include <sys/socket.h> /* setsockopt, SOL_SOCKET, SO_NOSIGPIPE */
#include <time.h>       /* struct timespec for the zero-timeout wait */
#endif

/* State for one guarded write. `active` is 1 only when THIS guard
 * blocked SIGPIPE; a caller that had it blocked already owns the signal
 * and nothing is drained or restored on its behalf. */
typedef struct NmSigpipeGuard
{
    int active;
#if defined(__linux__)
    sigset_t saved;
#endif
} NmSigpipeGuard;

#if defined(__linux__)

/* Block SIGPIPE for the write that follows. */
static inline void nm_sigpipe_guard_enter(NmSigpipeGuard *g)
{
    g->active = 0;
    sigset_t set;
    if (sigemptyset(&set) != 0 || sigaddset(&set, SIGPIPE) != 0)
        return;
    if (sigprocmask(SIG_BLOCK, &set, &g->saved) != 0)
        return; /* cannot block: this write goes out unguarded */
    if (sigismember(&g->saved, SIGPIPE))
        return; /* already blocked: the signal is the caller's, not ours */
    g->active = 1;
}

/* Restore the mask, draining the SIGPIPE the write earned first. Pass
 * `saw_epipe` = the write failed with EPIPE; any other outcome leaves
 * nothing pending and only the mask moves back. */
static inline void nm_sigpipe_guard_leave(NmSigpipeGuard *g, int saw_epipe)
{
    if (!g->active)
        return;
    if (saw_epipe) {
        sigset_t set;
        struct timespec zero;
        int saved_errno = errno;
        zero.tv_sec = 0;
        zero.tv_nsec = 0;
        if (sigemptyset(&set) == 0 && sigaddset(&set, SIGPIPE) == 0) {
            /* The kernel posted the signal when write() returned EPIPE.
             * Take it out of the pending set, or it fires on unblock. */
            while (sigtimedwait(&set, NULL, &zero) < 0 && errno == EINTR)
                ;
        }
        errno = saved_errno; /* the caller keeps the write's errno */
    }
    sigprocmask(SIG_SETMASK, &g->saved, NULL);
    g->active = 0;
}

#else /* Windows, Darwin: nothing to block, nothing to drain */

static inline void nm_sigpipe_guard_enter(NmSigpipeGuard *g)
{
    g->active = 0;
}

static inline void nm_sigpipe_guard_leave(NmSigpipeGuard *g, int saw_epipe)
{
    (void)g;
    (void)saw_epipe;
}

#endif

/* Ask the platform to keep this fd's writes from raising SIGPIPE, so a
 * library's internal write (a TLS backend's, whose flag we cannot pass)
 * answers EPIPE like ours. Best-effort: -1 on failure, and the caller's
 * guard still covers our own writes. Apply it where the fd is created,
 * so no later call site can forget it. */
static inline int nm_socket_quiet_sigpipe(int fd)
{
#if !defined(_WIN32) && defined(SO_NOSIGPIPE)
    int one = 1;
    return setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#else
    (void)fd;
    return 0;
#endif
}

#endif /* NM_SIGPIPE_H */
