/* addr_cache.h - the name service's answers, remembered
 *
 * The connect walk needs an address list, and the only portable way to
 * get one is getaddrinfo — a call that BLOCKS for as long as the
 * machine's resolver takes. That is not a small number in practice: a
 * name whose other family has no answer costs the statistical A+AAAA
 * round-trip (200-300 ms measured on this box for a provider that
 * answers in 4 ms over A alone), and a network that drops DNS sits out
 * `timeout x attempts` — seconds — with no deadline anywhere in this
 * program to cut it short. The walk runs on the UI thread.
 *
 * So the fix is not a thread: it is not asking twice. This is a small
 * process-local table of what the resolver said, keyed by host and port,
 * consulted before the question. A provider's rounds, its catalog fetch
 * and a retry all dial the same name, so one answer carries a whole turn
 * — and the first ask lands where the user is already waiting for the
 * network (the handoff paint in chat_app.c is what makes even that one
 * invisible).
 *
 * Two rules, and the durability of the thing rests on there being only
 * two:
 *
 *   - a remembered answer EXPIRES. Nothing else invalidates: no judging
 *     whether an address was good (a stale dial is bounded by the walk's
 *     per-attempt budget exactly as a fresh dead one is), no negative
 *     caching (a failed resolve stores nothing, so a name service that
 *     comes back is believed at once), no persistence (a process starts
 *     knowing nothing, so a boot-time misresolution cannot survive a
 *     restart).
 *   - it stores the RESOLVER'S OWN ORDER and nothing else. Which address
 *     to dial first is the walk's decision, re-made per walk from the
 *     family latch (see transport.h), so a latch earned after a name was
 *     remembered still reorders it. The cache holds facts; the walk holds
 *     policy.
 *
 * A convenience, never a requirement: a miss, an expired entry, a name
 * too long for the key, a full table — all mean "ask", which is today's
 * behavior. And it is the UI thread's state: not locked, not thread-safe,
 * single-writer by the transport's own shape.
 */

#ifndef NM_ADDR_CACHE_H
#define NM_ADDR_CACHE_H

/* sockaddr_storage for the stored targets (guarded: the includer's
 * platform headers define it — winsock2 on Windows, sys/socket.h
 * elsewhere, the connection layout's precedent). */
#ifndef _WIN32
#include <sys/socket.h>
#else
#include <winsock2.h>
#endif

/* How many addresses of one name are remembered. DELIBERATELY more than
 * one walk dials (NM_CONNECT_MAX_ADDRS): the walk's cap lands AFTER the
 * family ordering, so a list pre-truncated to the walk's cap would take
 * choices away from that ordering. transport_socket.c ties the two
 * numbers together at build time. */
#define NM_ADDR_CACHE_MAX_ADDRS 16

/* One minute. Long enough to cover a turn's rounds, its tool rounds, and
 * the retry after a failed one — short enough that a host which moved is
 * back on its own name well inside a human's patience. A CDN rotating is
 * the case the number answers for; the cost of being wrong about it is
 * one bounded attempt, not a broken connection (the walk still has every
 * remembered address to move on through). */
#define NM_ADDR_CACHE_TTL_SECONDS 60.0

/* Copy the remembered resolver-order addresses for host:port into
 * out_addrs/out_lens, at most max_addrs of them. Returns the count, or 0
 * when there is no live entry for that exact host AND port — a miss is
 * not an error, it is "ask". An expired entry is reported as a miss and
 * its slot becomes free. */
int nm_addr_cache_get(const char *host, int port,
                      struct sockaddr_storage *out_addrs, unsigned *out_lens,
                      int max_addrs);

/* Remember host:port's resolver-order list. A name too long for the key
 * is not remembered at all rather than remembered under a truncated
 * spelling — answering for a different name than was asked is the one
 * thing a cache may never do. Anything the table cannot hold is simply
 * not held. */
void nm_addr_cache_put(const char *host, int port,
                       const struct sockaddr_storage *addrs,
                       const unsigned *lens, int n_addrs);

#endif /* NM_ADDR_CACHE_H */
