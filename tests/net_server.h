/* net_server.h - the suite's one canned server: a scripted pump
 *
 * One thread, one poll, no timers. A canned server is a script of
 * ROUNDS - a round per completed request - and a pump the test's own
 * drive loop services on every wake: nonblocking accept, drain, act,
 * flush. Nothing here ever sleeps and nothing here ever spawns: the
 * wait (nm_ts_wait) is the ONE poll that covers both ends - the
 * client's fd with its declared interest, and the server's fds with
 * theirs - so a drive reacts to whichever end moved.
 *
 * Why not a worker thread: a thread is a second order of interleavings
 * the test has to reason about, and every one of those orders is a
 * platform flake waiting to happen (the v0.16.0 tag run died on a
 * shutdown()-on-a-listener shape Linux tolerates and BSD/Windows
 * reject, delivered by exactly such a thread). The pump delivers the
 * same shapes deterministically: a DROP round closes the connection
 * slot in the harness's own thread, before the client's next step, so
 * a redial sees the same corpse on every stack.
 *
 * When the script runs out of rounds, the LAST round repeats - a
 * persistent canned peer (the keep-alive reuse tests live on exactly
 * that). A completed request consumes a round whether it arrived on a
 * fresh connection or a held-open one, and a connection that closes
 * before any request byte lands consumes NOTHING (EOF on the slot, no
 * round spent, no OS dependence - the workaround the threaded
 * accept loops used to need).
 */

#ifndef NM_TEST_NET_SERVER_H
#define NM_TEST_NET_SERVER_H

#include <stddef.h>
#include <stdint.h>

/* What a scripted round does once its request has landed. */
typedef enum
{
    NM_TS_SERVE = 0, /* write the reply, then hold or close the socket
                      * per the round's close_after_reply */
    NM_TS_DROP,      /* read the request, close WITHOUT answering - the
                      * FIN-in-flight shape a stale reuse dies on */
    NM_TS_DIE,       /* close the listener too: the next dial is refused,
                      * deterministically, on every stack */
    NM_TS_STALL,     /* read the request, then nothing - no bytes, no
                      * close. The silent peer a budget must outlive; the
                      * TEST moves the (virtual) clock to the verdict */
    NM_TS_HOLD       /* read the request, then PARK the socket: no reply,
                      * no close, and the connection slot frees for the
                      * next dial - the held-open peer a test serves
                      * AROUND (a catalog GET the app must keep waiting
                      * on while the chat's POSTs get answered). A parked
                      * socket is closed by nm_ts_stop/nm_ts_kill and is
                      * never polled again; a test that parks more than
                      * NM_TS_PARK_MAX just gets the extra closed. */
} NmTsAction;

typedef struct NmTsServer NmTsServer;

/* The per-round extension point: called with the whole request once it
 * completes, BEFORE the action runs. Route by path, record evidence, or
 * rewrite the answer with nm_ts_reply_override() - the provider and
 * credit families ride this instead of growing new server shapes. */
typedef void (*NmTsOnRequest)(NmTsServer *s, int round,
                              const char *req, size_t len);

typedef struct
{
    /* head+body, whole. NULL with DROP / DIE / STALL (nothing to
     * write). A one-exchange reply MUST carry `Connection: close`
     * itself - build it with test_send_reply()/test_reply_with_close
     * so it cannot be forgotten. */
    const char *reply;
    NmTsAction action;     /* NM_TS_SERVE by zero */
    int close_after_reply; /* 1 = close the socket once the reply is fully
                            * written; 0 = hold it open for the next
                            * exchange on it (the pool's reuse shape) */
    int chunk_events;      /* 1 = SSE pacing: one `data:` event per pump
                            * pass instead of the whole body at once - a
                            * mid-round observer sees each event land,
                            * where a thread had to usleep for the same */
    NmTsOnRequest on_request;
} NmTsRound;

/* Bind a nonblocking loopback listener on an ephemeral port and arm
 * the script. Returns the server (heap, opaque), or NULL. The caller
 * keeps the pointer and passes it to every call; nm_ts_stop frees it. */
NmTsServer *nm_ts_start(const NmTsRound *script, int n_rounds);

/* One nonblocking pass over every ready server event: accept, drain
 * what arrived, act on a completed request, flush pending reply bytes.
 * Never blocks, never waits - the drive loop calls it between steps
 * and inside every wait wake. */
void nm_ts_pump(NmTsServer *s);

/* THE wait: poll the client's fd (its declared interest, NM_INTEREST_*)
 * together with the server's fds, up to timeout_ms, then pump. Returns
 * poll's answer: >0 = something was ready (and the server has been
 * serviced), 0 = the slice elapsed quietly, -1 = poll error. A stalled
 * drive shows up as repeated 0s - print a diagnostic and fail the
 * assertion, never spin forever. */
int nm_ts_wait(NmTsServer *s, intptr_t fd, unsigned interest,
               int timeout_ms);

/* Close both server fds and KEEP the server: the drive keeps working
 * (its waits cover just the client's fd now), and the port stays
 * readable - the "nothing listens anymore" shape. nm_ts_stop frees
 * what this closes. */
void nm_ts_kill(NmTsServer *s);

/* Close both server fds (a kill, if you did not already) and free the
 * server. The CLIENT'S own cleanup (a pool flush) stays the caller's:
 * this module does not know what the client owns. */
void nm_ts_stop(NmTsServer *s);

/* Rewrite what the CURRENT round serves. Only meaningful from an
 * on_request hook (before the action armed its bytes); the string is
 * borrowed, not copied, so keep it alive until the round is written. */
void nm_ts_reply_override(NmTsServer *s, const char *reply);

/* A chunked SSE reply built from RAW events (each a complete
 * "data: ...\n\n" line pair): the 200 head, one computed chunk per
 * event, and the terminator. hold_tail omits the terminator - the
 * round's socket then stays open with nothing more coming (pair it
 * with close_after_reply = 0), the shape a silence-budget test needs.
 * Returns malloc'd bytes; the caller frees when the server is done
 * (the round borrows the string for its life). */
char *nm_ts_sse_chunked(const char *events, int hold_tail);

int nm_ts_port(const NmTsServer *s);     /* the bound port */
int nm_ts_accepts(const NmTsServer *s);  /* connections taken */
int nm_ts_requests(const NmTsServer *s); /* completed requests */

/* The request that completed as round `round`, whole (head + body),
 * or NULL when it fell outside the capture ring (the last 16 requests
 * are kept). A request that only truncated into the ring is what it
 * is - the evidence buffers are capped, not the wire. */
const char *nm_ts_request(const NmTsServer *s, int round);

/* Held sockets a NM_TS_HOLD round parked: capped, because parking is a
 * SHAPE a script asks for (a held GET beside a served chat), not a
 * client class - a script that parks more than this just gets the
 * extras closed. */
#define NM_TS_PARK_MAX 4

#endif /* NM_TEST_NET_SERVER_H */
