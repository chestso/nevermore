/* tools_websearch.c - web_search: a local SearXNG query tool
 *
 * Port of quoth's quoth-searxng.el: GET {base}/search?q=<q>&format=json
 * from a local SearXNG instance, normalize the JSON into a markdown
 * list of results (engine + score on each, so the model can weigh
 * relevance), sort by score, dedup by URL, cap at max_results, and
 * append the instance's infoboxes/suggestions. No regex anywhere: the
 * URL builder, the encoder and the JSON walk are character-level
 * scans.
 *
 * Async (the event-driven principle): the request rides transport's
 * phase machine exactly as the chat stream does — nm_connect_async,
 * nm_request_queue, nm_read_body — so a tool round never blocks the
 * event loop and the spinner keeps ticking. It is the first async tool
 * that needs WRITE interest (connect, then send), which is why NmTool
 * carries an interest callback; run_command's pipe only ever reads.
 *
 * Reachability (quoth's gating): the first request is the probe. An
 * unreachable instance or a timeout sets the store's `searxng_enabled`
 * RUNTIME value to `off`, so later calls short-circuit with no HTTP
 * request — a dead server must not be hammered on every tool round,
 * and /config shows (and resets) the self-disabling. A success leaves
 * it on; a changed endpoint (the store's `searxng` value differs from
 * the one last probed) clears the latch for a fresh probe. The tool
 * keeps NO endpoint/health copy: the URL and the enabled bool are
 * resolved from the config store at the point of use (no store = the
 * built-in default URL, enabled).
 *
 * Memory: one connection + one growing body buffer per call, reused
 * across steps; the parsed JSON is freed at the end. No per-token
 * allocation.
 */

/* winsock2.h must precede <windows.h>, which nm_clock.h pulls in. */
#ifdef _WIN32
#include <winsock2.h>
#else
#include <sys/select.h>
#include <sys/time.h>
#endif

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "json.h"
#include "nm_config.h" /* the store the tool reads (no proxies) */
#include "tools.h"
#include "tools_internal.h"
#include "transport.h"

#include "nm_clock.h"

#define NM_WEBSEARCH_DEFAULT_URL         "http://127.0.0.1:8888"
#define NM_WEBSEARCH_DEFAULT_TIMEOUT_MS  10000
#define NM_WEBSEARCH_DEFAULT_MAX_RESULTS 8
#define NM_WEBSEARCH_MAX_RESULTS         8 /* quoth's cap (== the default) */
#define NM_WEBSEARCH_BODY_MAX            (4u * 1024u * 1024u)
#define NM_WEBSEARCH_URL_MAX             1024

static int g_timeout_ms; /* 0 = default */

/* The endpoint we last probed. The latch we keep is a "when", not a
 * "what": the URL itself is resolved from the store, but we remember
 * which one the current `searxng_enabled=off` latch belongs to, so a
 * newly pointed endpoint gets a fresh probe (the latch is dropped). */
static char g_probed_url[NM_WEBSEARCH_URL_MAX];

/* ---------------------------------------------------------------- */
/* Runtime knobs (resolved from the store, never copied)             */
/* ---------------------------------------------------------------- */

void nm_tool_web_search_set_timeout_ms(int ms)
{
    g_timeout_ms = ms > 0 ? ms : 0;
}

/* The effective endpoint: the store's `searxng` value, else the
 * built-in default. */
const char *nm_tool_web_search_base_url(void)
{
    NmConfig *c = nm_config_store();
    const char *v = c ? nm_config_resolve(c, NM_CFG_KEY_SEARXNG, NULL)
                      : NULL;
    return (v && *v) ? v : NM_WEBSEARCH_DEFAULT_URL;
}

/* May web_search run? The store's `searxng_enabled` bool, default on. */
int nm_tool_web_search_enabled(void)
{
    NmConfig *c = nm_config_store();
    return c ? nm_config_resolve_bool(c, NM_CFG_KEY_SEARXNG_ENABLED, 1) : 1;
}

/* Record that the instance did not answer: latch `searxng_enabled=off`
 * on the store's RUNTIME layer (visible to /config, resettable, never
 * persisted). No store = nothing to latch (headless/test with no
 * config). */
static void ws_mark_unreachable(void)
{
    NmConfig *c = nm_config_store();
    if (c)
        nm_config_runtime_set(c, NM_CFG_KEY_SEARXNG_ENABLED, "off");
}

/* A new endpoint deserves a fresh probe: if the configured URL differs
 * from the one the latch belongs to, drop the self-disabling and
 * remember the new one. */
static void ws_note_endpoint(const char *url)
{
    if (strcmp(url, g_probed_url) == 0)
        return;
    snprintf(g_probed_url, sizeof(g_probed_url), "%s", url);
    NmConfig *c = nm_config_store();
    if (c)
        nm_config_runtime_clear(c, NM_CFG_KEY_SEARXNG_ENABLED);
}

static double timeout_seconds(void)
{
    int ms = g_timeout_ms > 0 ? g_timeout_ms : NM_WEBSEARCH_DEFAULT_TIMEOUT_MS;
    return (double)ms / 1000.0;
}

/* ---------------------------------------------------------------- */
/* Small growable byte buffer (per call, freed with the call)        */
/* ---------------------------------------------------------------- */

typedef struct Buf
{
    char *p;
    size_t len, cap;
} Buf;

static int buf_grow(Buf *b, size_t extra)
{
    if (b->len + extra + 1 <= b->cap)
        return 0;
    size_t ncap = b->cap ? b->cap : 256;
    while (ncap < b->len + extra + 1)
        ncap *= 2;
    char *np = realloc(b->p, ncap);
    if (!np)
        return -1;
    b->p = np;
    b->cap = ncap;
    return 0;
}

static void buf_add(Buf *b, const char *s, size_t n)
{
    if (!s || n == 0 || buf_grow(b, n) != 0)
        return;
    memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = '\0';
}

static void buf_puts(Buf *b, const char *s) { buf_add(b, s, strlen(s)); }

static void buf_addc(Buf *b, char c) { buf_add(b, &c, 1); }

/* One formatted field (a score, a count): routed through vsnprintf,
 * never a forwarded variadic (which is UB). */
static void buf_addf(Buf *b, const char *fmt, ...)
{
    char tmp[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n > 0)
        buf_add(b, tmp,
                (size_t)n < sizeof(tmp) ? (size_t)n : sizeof(tmp) - 1);
}

/* Percent-encode with the RFC 3986 unreserved set untouched (and a
 * space as %20, not a '+': SearXNG takes the query verbatim). */
static void buf_add_encoded(Buf *b, const char *s)
{
    static const char hex[] = "0123456789ABCDEF";
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        unsigned char c = *p;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_' ||
            c == '~') {
            buf_addc(b, (char)c);
        } else {
            char esc[3] = { '%', hex[c >> 4], hex[c & 0x0F] };
            buf_add(b, esc, 3);
        }
    }
}

/* ---------------------------------------------------------------- */
/* URL build / parse                                                 */
/* ---------------------------------------------------------------- */

/* "{base}/search?q=<enc>&format=json[&categories=..][&engines=..]".
 * A trailing slash on the base is folded so a path prefix still yields
 * exactly one separator. Heap text (caller frees). */
static char *build_url(const char *base, const char *query,
                       const char *categories, const char *engines)
{
    Buf b = { 0 };
    size_t blen = strlen(base);
    while (blen > 0 && base[blen - 1] == '/')
        blen--;
    buf_add(&b, base, blen);
    buf_puts(&b, "/search?q=");
    buf_add_encoded(&b, query);
    buf_puts(&b, "&format=json");
    if (categories && *categories) {
        buf_puts(&b, "&categories=");
        buf_add_encoded(&b, categories);
    }
    if (engines && *engines) {
        buf_puts(&b, "&engines=");
        buf_add_encoded(&b, engines);
    }
    return b.p; /* NULL on OOM */
}

/* Decimal port text; -1 on non-digits or overflow. */
static int parse_port(const char *s)
{
    int v = 0;
    if (!s || *s < '0' || *s > '9')
        return -1;
    for (; *s >= '0' && *s <= '9'; s++) {
        v = v * 10 + (*s - '0');
        if (v > 65535)
            return -1;
    }
    return (*s == '\0' || *s == '/') ? v : -1;
}

/* scheme://host[:port][/path] -> host (heap), port, mode, path (heap,
 * "" when the base carries none). 0 = ok, -1 = malformed. Bracketed
 * IPv6 hosts are accepted. */
static int parse_url(const char *url, char **host_out, int *port_out,
                     NmTransportMode *mode_out, char **path_out)
{
    const char *sep = strstr(url, "://");
    if (!sep)
        return -1;
    size_t slen = (size_t)(sep - url);
    NmTransportMode mode;
    int defport;
    if (slen == 4 && strncmp(url, "http", 4) == 0) {
        mode = NM_TRANSPORT_PLAIN;
        defport = 80;
    } else if (slen == 5 && strncmp(url, "https", 5) == 0) {
        mode = NM_TRANSPORT_TLS;
        defport = 443;
    } else {
        return -1;
    }
    const char *rest = sep + 3;
    const char *slash = strchr(rest, '/');
    size_t alen = slash ? (size_t)(slash - rest) : strlen(rest);
    if (alen == 0)
        return -1;

    const char *hstart = rest;
    size_t hlen = alen;
    int port = defport;
    if (rest[0] == '[') {
        const char *close = memchr(rest, ']', alen);
        if (!close)
            return -1;
        hstart = rest + 1;
        hlen = (size_t)(close - (rest + 1));
        if ((size_t)(close - rest) + 1 < alen && close[1] == ':')
            port = parse_port(close + 2);
    } else {
        const char *colon = memchr(rest, ':', alen);
        if (colon) {
            hlen = (size_t)(colon - rest);
            port = parse_port(colon + 1);
        }
    }
    if (hlen == 0 || port <= 0)
        return -1;

    char *host = malloc(hlen + 1);
    if (!host)
        return -1;
    memcpy(host, hstart, hlen);
    host[hlen] = '\0';
    char *path = strdup(slash ? slash : "");
    if (!path) {
        free(host);
        return -1;
    }
    *host_out = host;
    *port_out = port;
    *mode_out = mode;
    *path_out = path;
    return 0;
}

/* ---------------------------------------------------------------- */
/* Response normalization (quoth's search-result rendering)          */
/* ---------------------------------------------------------------- */

static double score_of(const NmJson *r)
{
    const NmJson *s = nm_json_get(r, "score");
    return nm_json_type(s) == NM_JSON_NUMBER ? nm_json_num(s) : 0.0;
}

/* One result block:
 *
 *   Result [engine: E, score: S]:
 *   # TITLE
 *   URL
 *   CONTENT
 */
static void format_block(Buf *b, const NmJson *r)
{
    const char *title = nm_json_str(nm_json_get(r, "title"));
    const char *url = nm_json_str(nm_json_get(r, "url"));
    const char *content = nm_json_str(nm_json_get(r, "content"));

    Buf eng = { 0 };
    const NmJson *es = nm_json_get(r, "engines");
    if (nm_json_type(es) == NM_JSON_ARRAY) {
        for (size_t i = 0; i < nm_json_len(es); i++) {
            const char *en = nm_json_str(nm_json_at(es, i));
            if (!en || !*en)
                continue;
            if (eng.len)
                buf_puts(&eng, ", ");
            buf_puts(&eng, en);
        }
    }
    if (!eng.len) {
        /* simplified payloads carry the singular `engine` */
        const char *en = nm_json_str(nm_json_get(r, "engine"));
        buf_puts(&eng, en && *en ? en : "unknown");
    }

    char sc[32];
    const NmJson *score = nm_json_get(r, "score");
    if (nm_json_type(score) == NM_JSON_NUMBER)
        snprintf(sc, sizeof(sc), "%g", nm_json_num(score));
    else
        snprintf(sc, sizeof(sc), "unknown");

    buf_addf(b, "Result [engine: %s, score: %s]:\n", eng.p ? eng.p : "unknown",
             sc);
    buf_puts(b, "# ");
    buf_puts(b, title ? title : "");
    buf_puts(b, "\n");
    buf_puts(b, url ? url : "");
    buf_puts(b, "\n");
    buf_puts(b, content ? content : "");
    free(eng.p);
}

/* Join a string array's non-empty members with ", " into `b`,
 * prefixed by `label`; nothing when the array is absent/empty. */
static void format_string_array(Buf *b, const char *label, const NmJson *arr)
{
    if (nm_json_type(arr) != NM_JSON_ARRAY)
        return;
    Buf t = { 0 };
    for (size_t i = 0; i < nm_json_len(arr); i++) {
        const char *s = nm_json_str(nm_json_at(arr, i));
        if (!s || !*s)
            continue;
        if (t.len)
            buf_puts(&t, ", ");
        buf_puts(&t, s);
    }
    if (t.len) {
        if (b->len)
            buf_puts(b, "\n\n");
        buf_puts(b, label);
        buf_puts(b, t.p);
    }
    free(t.p);
}

/* SearXNG infoboxes: each entry's `infobox` (or `title`) becomes an
 * "Info: a, b" line. */
static void format_infoboxes(Buf *b, const NmJson *ib)
{
    if (nm_json_type(ib) != NM_JSON_ARRAY)
        return;
    Buf t = { 0 };
    for (size_t i = 0; i < nm_json_len(ib); i++) {
        const NmJson *o = nm_json_at(ib, i);
        const char *title = nm_json_str(nm_json_get(o, "infobox"));
        if (!title || !*title)
            title = nm_json_str(nm_json_get(o, "title"));
        if (!title || !*title)
            continue;
        if (t.len)
            buf_puts(&t, ", ");
        buf_puts(&t, title);
    }
    if (t.len) {
        if (b->len)
            buf_puts(b, "\n\n");
        buf_puts(b, "Info: ");
        buf_puts(b, t.p);
    }
    free(t.p);
}

static void normalize(const NmJson *doc, Buf *b, long max)
{
    const NmJson *raw = nm_json_get(doc, "results");
    size_t n = nm_json_type(raw) == NM_JSON_ARRAY ? nm_json_len(raw) : 0;

    const NmJson **items = n ? malloc(n * sizeof(*items)) : NULL;
    double *scores = n ? malloc(n * sizeof(*scores)) : NULL;
    if (n && (!items || !scores)) {
        free(items);
        free(scores);
        return;
    }
    for (size_t i = 0; i < n; i++) {
        items[i] = nm_json_at(raw, i);
        scores[i] = items[i] ? score_of(items[i]) : 0.0;
    }
    /* Stable insertion sort, descending by score (ties keep result
     * order — quoth sorts the same way). */
    for (size_t i = 1; i < n; i++) {
        const NmJson *it = items[i];
        double sc = scores[i];
        size_t j = i;
        while (j > 0 && scores[j - 1] < sc) {
            items[j] = items[j - 1];
            scores[j] = scores[j - 1];
            j--;
        }
        items[j] = it;
        scores[j] = sc;
    }

    /* Dedup by URL keeping the highest score (the sorted order makes
     * the first occurrence the best), capped at `max`. */
    const char **seen = NULL;
    size_t nseen = 0, cap_seen = 0;
    for (size_t i = 0; i < n && (long)nseen < max; i++) {
        const NmJson *r = items[i];
        if (!r)
            continue;
        const char *url = nm_json_str(nm_json_get(r, "url"));
        int dup = 0;
        for (size_t k = 0; k < nseen; k++) {
            if ((!url && !seen[k]) ||
                (url && seen[k] && strcmp(url, seen[k]) == 0)) {
                dup = 1;
                break;
            }
        }
        if (dup)
            continue;
        if (nseen == cap_seen) {
            size_t ncap = cap_seen ? cap_seen * 2 : 8;
            const char **ns = realloc(seen, ncap * sizeof(*ns));
            if (!ns)
                break; /* no room to track more: stop deduping */
            seen = ns;
            cap_seen = ncap;
        }
        seen[nseen++] = url;
        if (b->len)
            buf_puts(b, "\n\n");
        format_block(b, r);
    }
    free(seen);
    free(items);
    free(scores);

    format_infoboxes(b, nm_json_get(doc, "infoboxes"));
    format_string_array(b, "Suggestions: ", nm_json_get(doc, "suggestions"));
}

/* ---------------------------------------------------------------- */
/* The async step machine                                            */
/* ---------------------------------------------------------------- */

struct NmToolExec
{
    NmConnection *conn;
    char *buf; /* response body, grown geometrically */
    size_t len, cap;
    long max_results;
    double deadline;
    int done;
    NmToolResult result; /* terminal result, handed out once */
};

static int ws_reserve(NmToolExec *e, size_t extra)
{
    if (e->len + extra + 1 <= e->cap)
        return 0;
    size_t ncap = e->cap ? e->cap : 8192;
    while (ncap < e->len + extra + 1)
        ncap *= 2;
    char *nb = realloc(e->buf, ncap);
    if (!nb)
        return -1;
    e->buf = nb;
    e->cap = ncap;
    return 0;
}

static void ws_fail(NmToolExec *e, const char *msg)
{
    nm_tool_result_free(&e->result);
    e->result = nm_tool_result_error(msg);
    e->done = 1;
}

static NmToolExec *fail_exec(NmToolResult r)
{
    NmToolExec *e = calloc(1, sizeof(*e));
    if (!e) {
        nm_tool_result_free(&r);
        return NULL;
    }
    e->done = 1;
    e->result = r;
    return e;
}

/* Response head + body are in: check the status, parse and render. */
static void ws_finalize(NmToolExec *e)
{
    const NmResponse *resp = nm_response(e->conn);
    int status = resp ? resp->status : 0;
    if (status < 200 || status >= 300) {
        char msg[128];
        snprintf(msg, sizeof(msg),
                 "web_search: SearXNG returned HTTP %d (is format=json "
                 "enabled in search.formats?)",
                 status);
        ws_fail(e, msg);
        ws_mark_unreachable();
        return;
    }
    if (e->len == 0) {
        ws_fail(e, "web_search: SearXNG returned an empty response");
        ws_mark_unreachable();
        return;
    }
    const char *jerr = NULL;
    NmJson *doc = nm_json_parse(e->buf, e->len, &jerr);
    if (!doc) {
        ws_fail(e, "web_search: SearXNG returned malformed JSON");
        ws_mark_unreachable();
        return;
    }
    Buf out = { 0 };
    normalize(doc, &out, e->max_results);
    nm_json_free(doc);
    e->result = nm_tool_format_result(out.p, 0);
    free(out.p);
    e->done = 1;
    /* A success leaves `searxng_enabled` on (it was on to get here). */
}

/* Hand the terminal result to the caller exactly once. */
static NmToolStatus ws_take(NmToolExec *e, NmToolResult *out)
{
    *out = e->result;
    e->result = (NmToolResult){ 0, NULL };
    return NM_TOOL_DONE;
}

static NmToolStatus ws_step(NmToolExec *e, NmToolResult *out)
{
    if (!e) {
        *out = nm_tool_result_error("internal: null web_search exec");
        return NM_TOOL_DONE;
    }
    if (e->done)
        return ws_take(e, out);

    if (nm_monotonic_seconds() >= e->deadline) {
        ws_fail(e, "web_search: SearXNG request timed out");
        ws_mark_unreachable();
        return ws_take(e, out);
    }

    NmTransportStatus ts = nm_connection_step(e->conn);
    if (ts == NM_TRANSPORT_PENDING)
        return NM_TOOL_RUNNING;
    if (ts != NM_TRANSPORT_OK) {
        const char *err = nm_connection_last_error(e->conn);
        char msg[256];
        snprintf(msg, sizeof(msg), "web_search: SearXNG unreachable: %s",
                 err && *err ? err : "connection failed");
        ws_fail(e, msg);
        ws_mark_unreachable();
        return ws_take(e, out);
    }

    for (;;) {
        if (e->len >= NM_WEBSEARCH_BODY_MAX) {
            ws_fail(e, "web_search: SearXNG response exceeded the size cap");
            ws_mark_unreachable();
            return ws_take(e, out);
        }
        if (ws_reserve(e, 4096) != 0) {
            ws_fail(e, "web_search: out of memory");
            return ws_take(e, out);
        }
        long n =
            nm_read_body(e->conn, e->buf + e->len, e->cap - e->len - 1);
        if (n == NM_READ_WOULD_BLOCK)
            return NM_TOOL_RUNNING;
        if (n < 0) {
            const char *err = nm_connection_last_error(e->conn);
            char msg[256];
            snprintf(msg, sizeof(msg), "web_search: SearXNG unreachable: %s",
                     err && *err ? err : "read failed");
            ws_fail(e, msg);
            ws_mark_unreachable();
            return ws_take(e, out);
        }
        if (n == 0)
            break; /* body complete */
        e->len += (size_t)n;
        e->buf[e->len] = '\0';
    }

    ws_finalize(e);
    return ws_take(e, out);
}

/* The connection's own wait source (a socket, with the phase's
 * connect/send/response interest bits). */
static int ws_source(NmToolExec *e, NmSource *out)
{
    if (!e || e->done || !e->conn)
        return 0;
    NmSource s = nm_connection_interest(e->conn);
    if (s.handle < 0 || s.flags == 0)
        return 0;
    *out = s;
    return 1;
}

static void ws_end(NmToolExec *e)
{
    if (!e)
        return;
    if (e->conn)
        nm_connection_close(e->conn);
    free(e->buf);
    nm_tool_result_free(&e->result);
    free(e);
}

/* ---------------------------------------------------------------- */
/* begin (arg validation -> connect -> queue)                        */
/* ---------------------------------------------------------------- */

static NmToolExec *ws_begin(const NmTool *tool, const char *args_json,
                            void *userdata)
{
    (void)tool;
    (void)userdata;

    /* Disabled short-circuit: no HTTP request (quoth's "do not hammer
     * a dead server"). The store's `searxng_enabled` is off either
     * because a previous probe failed (the tool latched it) or because
     * the user turned it off — both mean "do not dial". */
    if (!nm_tool_web_search_enabled())
        return fail_exec(nm_tool_result_error(
            "web_search: SearXNG is unreachable (cached for this session); "
            "start the local server or reset searxng_enabled"));

    /* A newly pointed endpoint re-enables the probe. */
    ws_note_endpoint(nm_tool_web_search_base_url());

    const char *jerr = NULL;
    NmJson *args = args_json && *args_json
                       ? nm_json_parse(args_json, strlen(args_json), &jerr)
                       : NULL;
    if (!args || nm_json_type(args) != NM_JSON_OBJECT) {
        nm_json_free(args);
        return fail_exec(nm_tool_result_error(
            "web_search: arguments are not a JSON object"));
    }
    const char *query = nm_json_str(nm_json_get(args, "query"));
    if (!query || !*query) {
        nm_json_free(args);
        return fail_exec(nm_tool_result_error("web_search: missing query"));
    }
    long max = NM_WEBSEARCH_DEFAULT_MAX_RESULTS;
    const NmJson *jm = nm_json_get(args, "max_results");
    if (nm_json_type(jm) == NM_JSON_NUMBER) {
        max = (long)nm_json_num(jm);
        if (max < 1)
            max = 1;
        if (max > NM_WEBSEARCH_MAX_RESULTS)
            max = NM_WEBSEARCH_MAX_RESULTS;
    }
    const char *categories = nm_json_str(nm_json_get(args, "categories"));
    const char *engines = nm_json_str(nm_json_get(args, "engines"));

    char *url = build_url(nm_tool_web_search_base_url(), query, categories,
                          engines);
    nm_json_free(args);
    if (!url)
        return fail_exec(nm_tool_result_error("web_search: out of memory"));

    char *host = NULL, *path = NULL;
    int port = 0;
    NmTransportMode mode = NM_TRANSPORT_PLAIN;
    if (parse_url(url, &host, &port, &mode, &path) != 0) {
        free(url);
        char msg[NM_WEBSEARCH_URL_MAX + 64];
        snprintf(msg, sizeof(msg), "web_search: invalid SearXNG URL: %s",
                 nm_tool_web_search_base_url());
        return fail_exec(nm_tool_result_error(msg));
    }
    free(url);

    NmToolExec *e = calloc(1, sizeof(*e));
    if (!e) {
        free(host);
        free(path);
        return NULL;
    }
    e->deadline = nm_monotonic_seconds() + timeout_seconds();
    e->max_results = max;

    NmConnectInfo ci;
    ci.status = NM_TRANSPORT_OK;
    ci.detail[0] = '\0';
    e->conn = nm_connect_async(host, port, mode, &ci);
    free(host);
    if (!e->conn) {
        char msg[256];
        snprintf(msg, sizeof(msg), "web_search: SearXNG unreachable: %s",
                 ci.detail[0] ? ci.detail : "connect failed");
        ws_mark_unreachable();
        free(path);
        ws_fail(e, msg);
        return e;
    }
    NmTransportStatus ts =
        nm_request_queue(e->conn, "GET", path, NULL, 0, NULL, 0);
    free(path);
    if (ts != NM_TRANSPORT_OK) {
        ws_fail(e, "web_search: could not queue the SearXNG request");
        return e;
    }
    return e;
}

/* ---------------------------------------------------------------- */
/* execute (direct callers: begin + a readiness pump)                */
/* ---------------------------------------------------------------- */

/* Blocking readiness wait for the direct-call pump. The event-driven
 * path (the agent) never reaches it — boba or nm_agent_turn owns the
 * wait; this only serves nm_toolset_execute callers (tests, tools
 * without a loop). The timeout lets the deadline be checked even when
 * the fd never becomes readable (an accepted-but-silent server). */
static int wait_ready(int fd, unsigned interest, int timeout_ms)
{
    fd_set r, w;
    FD_ZERO(&r);
    FD_ZERO(&w);
    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    if (interest & NM_INTEREST_READ)
        FD_SET(fd, &r);
    if (interest & NM_INTEREST_WRITE)
        FD_SET(fd, &w);
#ifdef _WIN32
    return select(0, (interest & NM_INTEREST_READ) ? &r : NULL,
                  (interest & NM_INTEREST_WRITE) ? &w : NULL, NULL, &tv);
#else
    return select(fd + 1, &r, &w, NULL, &tv);
#endif
}

static NmToolResult ws_execute(const NmTool *tool, const char *args_json,
                               void *userdata)
{
    NmToolExec *e = ws_begin(tool, args_json, userdata);
    if (!e)
        return nm_tool_result_error("web_search: out of memory");
    for (;;) {
        NmToolResult r = { 0, NULL };
        if (ws_step(e, &r) == NM_TOOL_DONE) {
            ws_end(e);
            return r;
        }
        NmSource src = { -1, 0, NM_SRC_FD };
        if (!ws_source(e, &src)) {
            /* No wait target but not done: a begin that queued nothing.
             * The deadline still bounds it — step again promptly. */
            ws_end(e);
            return nm_tool_result_error("web_search: request stalled");
        }
        wait_ready((int)src.handle, src.flags, 50);
    }
}

/* ---------------------------------------------------------------- */
/* Vtable                                                            */
/* ---------------------------------------------------------------- */

/* The per-request deadline as "ms until the agent should step again".
 * The event-driven (TUI) path never polls: the fd is only readable when
 * the peer speaks, so an instance that accepts the connection and never
 * answers would otherwise never be re-stepped. The agent folds this into
 * nm_agent_next_timeout_ms and the runtime's tick fires the step. */
static int ws_deadline_ms(const NmToolExec *e)
{
    if (!e)
        return -1;
    if (e->done)
        return 0; /* wanted now, to hand out the terminal result */
    double left = (e->deadline - nm_monotonic_seconds()) * 1000.0;
    if (left <= 0.0)
        return 0;
    if (left >= 2147483000.0)
        return 2147483000;
    return (int)left; /* truncated: never exceeds the budget */
}

static const char web_search_schema[] =
    "{\"type\":\"object\",\"properties\":{"
    "\"query\":{\"type\":\"string\",\"description\":\"Search query.\"},"
    "\"max_results\":{\"type\":\"integer\",\"description\":\"Maximum number "
    "of results (1-8, default 8).\"},"
    "\"categories\":{\"type\":\"string\",\"description\":\"Comma-separated "
    "SearXNG categories (e.g. news,science); empty = all.\"},"
    "\"engines\":{\"type\":\"string\",\"description\":\"Comma-separated "
    "SearXNG engines (e.g. duckduckgo,wikipedia); empty = the instance "
    "default.\"}},"
    "\"required\":[\"query\"]}";

const NmTool nm_tool_web_search = {
    .name = "web_search",
    .description = "Search the web through a local SearXNG instance and "
                   "return ranked result titles, URLs and snippets",
    .emoji = "🌐",
    .params_schema = web_search_schema,
    .execute = ws_execute,
    .begin = ws_begin,
    .step = ws_step,
    .source = ws_source,
    .deadline_ms = ws_deadline_ms,
    .end = ws_end,
};
