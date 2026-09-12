/* wire_recorder.c - the wire debug recorder (docs/WIRE-DEBUG.md)
 *
 * The NmWireTap implementation: NDJSON event lines appended to one
 * file, banner first, secrets redacted at serialization time. The
 * transport knows nothing about files or JSON — this module is the
 * only place the two meet.
 *
 * Memory-reuse principle: one growable scratch buffer per recorder,
 * reused across events (geometric growth, never shrunk); values
 * serialized into it are borrowed pointers into existing buffers
 * (request body, SSE data) with the connection's lifetime. fwrite +
 * fflush per event so `tail -f` shows liveness and a crash loses at
 * most the current line.
 *
 * JSON lines are built with nm_json_set + nm_json_dump — never raw
 * snprintf — because bodies and SSE data carry quotes and backslashes
 * (the backslash-escape rule from AGENTS.md; snprintf'd escapes reach
 * the parser unescaped and reject the whole line).
 */

#include "wire_recorder.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "json.h"

/* The recorder reads (conn_id, xchg, tls) off the connection for the
 * correlation pair — the shared layout include (same pattern as
 * transport.c / transport_socket.c: define-the-layout-here). */
#define NM_TRANSPORT_LAYOUT_HERE
#include "connection_layout.h"

#ifdef HAVE_CONFIG_H
#include "config.h" /* BOBA_VERSION */
#endif

#ifdef _WIN32
#include <direct.h>
#include <process.h>
#include <shlobj.h>
#include <windows.h>
#define NM_GETPID _getpid
#else
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L /* clock_gettime, localtime_r, gmtime_r */
#endif
#include <errno.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#define NM_GETPID getpid
#endif

/* Version string for the banner run line (generated per build).
 * The tests' Makefiles don't build the version header, so fall back
 * to a literal there — the version detail is banner cosmetic. */
#ifdef NM_WIRE_HAS_VERSION_HEADER
#include "nevermore_version.h"
#define NM_WIRE_VERSION NEVERMORE_VERSION
#else
#define NM_WIRE_VERSION "dev"
#endif

#ifndef BOBA_VERSION
#define BOBA_VERSION "unknown"
#endif

/* ---------------------------------------------------------------- */
/* State                                                             */
/* ---------------------------------------------------------------- */

struct NmWireRecorder
{
    FILE *f;
    double t0;             /* wall-clock anchor of line 1 (epoch secs) */
    char url[2048];        /* request URL: scheme://host[:port] + path */
    NmRequestHeader *hdrs; /* copied header array (secret flags ride) */
    size_t n_hdrs;
    size_t hdr_cap;
    char **keys; /* banner "keys configured" names (owned) */
    size_t n_keys;
};

static struct NmWireRecorder g_rec;

/* Monotonic-ish clock: monotonic where the OS offers it, wall clock
 * otherwise. The t field's contract is "seconds since the banner",
 * immune to NTP jumps where possible. */
static double wire_now_wall(void)
{
#ifdef _WIN32
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    /* 100ns ticks since 1601 -> unix seconds */
    unsigned long long t = ((unsigned long long)ft.dwHighDateTime << 32) |
                           ft.dwLowDateTime;
    return (double)t / 10000000.0 - 11644473600.0;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
#endif
}

double nm_wire_recorder_now(void)
{
    if (!g_rec.f)
        return 0.0;
    return wire_now_wall() - g_rec.t0;
}

/* ---------------------------------------------------------------- */
/* Redaction (log-write time only; WIRE-DEBUG §4)                    */
/* ---------------------------------------------------------------- */

#define NM_WIRE_REDACTED "<redacted>"

/* The in-memory request is never touched: the redacted value only
 * exists inside the serialized log line. */
static const char *redact_value(const NmRequestHeader *h)
{
    return h->secret ? NM_WIRE_REDACTED : h->value;
}

/* ---------------------------------------------------------------- */
/* Scratch buffer (one growable buffer, reused across events)        */
/* ---------------------------------------------------------------- */

#define SCRATCH_MIN 1024

static int scratch_reserve(char **buf, size_t *cap, size_t need)
{
    if (need <= *cap)
        return 0;
    size_t nc = *cap ? *cap : SCRATCH_MIN;
    while (nc < need)
        nc *= 2;
    char *nb = realloc(*buf, nc);
    if (!nb)
        return -1;
    *buf = nb;
    *cap = nc;
    return 0;
}

/* Append with NUL guarantee; returns 0 on success. */
static int scratch_put(char **buf, size_t *len, size_t *cap, const char *s,
                       size_t n)
{
    if (scratch_reserve(buf, cap, *len + n + 1))
        return -1;
    memcpy(*buf + *len, s, n);
    *len += n;
    (*buf)[*len] = '\0';
    return 0;
}

static char *g_scratch;
static size_t g_scratch_len;
static size_t g_scratch_cap;

/* ---------------------------------------------------------------- */
/* Line writer                                                       */
/* ---------------------------------------------------------------- */

/* Serialize one event object and append it to the file. The JSON
 * tree (built with nm_json_set) is dumped, the dump is freed, and
 * the tree is freed separately — dump() returns an independent
 * malloc'd string, so both frees are correct. */
static void write_line(NmJson *obj)
{
    if (!g_rec.f || !obj)
        return;
    char *line = nm_json_dump(obj);
    nm_json_free(obj);
    if (!line)
        return;
    fputs(line, g_rec.f);
    free(line);
    fputc('\n', g_rec.f);
    fflush(g_rec.f);
}

static NmJson *new_event(const char *kind, const NmConnection *conn,
                         int with_xchg)
{
    NmJson *o = nm_json_new_object();
    if (!o)
        return NULL;
    nm_json_set(o, "t", nm_json_new_number(nm_wire_recorder_now()));
    nm_json_set(o, "kind", nm_json_new_string(kind));
    nm_json_set(o, "conn",
                nm_json_new_number(conn ? (double)conn->conn_id : 0.0));
    if (with_xchg)
        nm_json_set(o, "xchg", nm_json_new_number((double)conn->xchg));
    return o;
}

/* ---------------------------------------------------------------- */
/* Tap callbacks                                                     */
/* ---------------------------------------------------------------- */

static void tap_connect(const NmConnection *conn, const char *host, int port,
                        NmTransportMode mode)
{
    if (!g_rec.f || !conn)
        return;
    NmJson *o = new_event("connect", conn, 1);
    if (!o)
        return;
    nm_json_set(o, "host", nm_json_new_string(host));
    nm_json_set(o, "port", nm_json_new_number(port));
    nm_json_set(o, "mode", nm_json_new_string(mode == NM_TRANSPORT_TLS ? "tls" : "plain"));
    write_line(o);
}

static void tap_error(const NmConnection *conn, const char *stage,
                      const char *detail)
{
    if (!g_rec.f)
        return;
    NmJson *o = nm_json_new_object();
    if (!o)
        return;
    nm_json_set(o, "t", nm_json_new_number(nm_wire_recorder_now()));
    nm_json_set(o, "kind", nm_json_new_string("error"));
    nm_json_set(o, "conn",
                nm_json_new_number(conn ? (double)conn->conn_id : 0.0));
    /* Correlation: xchg rides along when an exchange was already
     * queued (send/head/body/protocol stages); pre-request failures
     * (dns/connect/tls) omit it — nothing to correlate to yet. */
    if (conn && conn->xchg > 0)
        nm_json_set(o, "xchg", nm_json_new_number((double)conn->xchg));
    /* The wire answered (an error-body path): the head's status is
     * the machine-readable part of the failure. */
    if (conn && conn->resp.status)
        nm_json_set(o, "httpStatus", nm_json_new_number(conn->resp.status));
    nm_json_set(o, "stage", nm_json_new_string(stage));
    nm_json_set(o, "detail", nm_json_new_string(detail));
    write_line(o);
}

/* Copy the queued headers into the recorder (the value pointers
 * (authbuf etc.) do not outlive the request). The secret flag rides
 * along — the request event redacts through the same marker. */
static void stash_headers(const NmRequestHeader *headers, size_t n)
{
    if (scratch_reserve((char **)&g_rec.hdrs, &g_rec.hdr_cap,
                        n * sizeof(NmRequestHeader)) != 0)
        return;
    memcpy(g_rec.hdrs, headers, n * sizeof(NmRequestHeader));
    g_rec.n_hdrs = n;
}

static void tap_request(const NmConnection *conn, const char *method,
                        const char *path, const NmRequestHeader *headers,
                        size_t n_headers, const char *body, size_t body_len)
{
    if (!g_rec.f || !conn)
        return;
    stash_headers(headers, n_headers);

    /* Absolute URL: scheme://host[:port] + path. conn->host carries
     * host[:port] already (the Host header source) and tls_host the
     * bare name — the scheme decides the port's defaultness. */
    snprintf(g_rec.url, sizeof(g_rec.url), "%s://%s%s",
             conn->tls ? "https" : "http", conn->host, path);

    NmJson *o = new_event("request", conn, 1);
    if (!o)
        return;
    nm_json_set(o, "method", nm_json_new_string(method));
    nm_json_set(o, "url", nm_json_new_string(g_rec.url));
    nm_json_set(o, "httpVersion", nm_json_new_string("1.1"));
    NmJson *harr = nm_json_new_array();
    for (size_t i = 0; i < g_rec.n_hdrs; i++) {
        NmJson *h = nm_json_new_object();
        nm_json_set(h, "name", nm_json_new_string(g_rec.hdrs[i].name));
        nm_json_set(h, "value",
                    nm_json_new_string(redact_value(&g_rec.hdrs[i])));
        nm_json_push(harr, h);
    }
    nm_json_set(o, "headers", harr);
    if (body && body_len) {
        g_scratch_len = 0; /* each event serializes from zero */
        if (scratch_put(&g_scratch, &g_scratch_len, &g_scratch_cap, body,
                        body_len) == 0)
            nm_json_set(o, "body", nm_json_new_string(g_scratch));
        nm_json_set(o, "bodySize", nm_json_new_number((double)body_len));
    } else {
        nm_json_set(o, "bodySize", nm_json_new_number(0.0));
    }
    write_line(o);
}

static void tap_response_head(const NmConnection *conn, int status,
                              const char *status_text, const char *http_version,
                              const char *content_type, int chunked,
                              long long content_length)
{
    if (!g_rec.f || !conn)
        return;
    NmJson *o = new_event("response-head", conn, 1);
    if (!o)
        return;
    nm_json_set(o, "status", nm_json_new_number(status));
    nm_json_set(o, "statusText", nm_json_new_string(status_text));
    nm_json_set(o, "httpVersion", nm_json_new_string(http_version));
    nm_json_set(o, "contentType", nm_json_new_string(content_type));
    nm_json_set(o, "chunked", nm_json_new_bool(chunked));
    nm_json_set(o, "contentLength", nm_json_new_number((double)content_length));
    /* headers: [] — only the fields above are parsed; the wire head
     * itself is reconstructible from the connection. Kept as an empty
     * array so consumers can rely on the key existing. */
    nm_json_set(o, "headers", nm_json_new_array());
    write_line(o);
}

static void tap_response(const NmConnection *conn, const char *body,
                         size_t body_len)
{
    if (!g_rec.f || !conn)
        return;
    NmJson *o = new_event("response", conn, 1);
    if (!o)
        return;
    if (body && body_len) {
        g_scratch_len = 0; /* each event serializes from zero */
        if (scratch_put(&g_scratch, &g_scratch_len, &g_scratch_cap, body,
                        body_len) == 0)
            nm_json_set(o, "body", nm_json_new_string(g_scratch));
    }
    nm_json_set(o, "bodySize", nm_json_new_number((double)body_len));
    write_line(o);
}

static void tap_stream_event(const NmConnection *conn, const char *event,
                             const char *data, size_t data_len)
{
    if (!g_rec.f || !conn)
        return;
    NmJson *o = new_event("stream-event", conn, 1);
    if (!o)
        return;
    nm_json_set(o, "event", nm_json_new_string(event ? event : "message"));
    if (data && data_len) {
        g_scratch_len = 0; /* each event serializes from zero */
        if (scratch_put(&g_scratch, &g_scratch_len, &g_scratch_cap, data,
                        data_len) == 0)
            nm_json_set(o, "data", nm_json_new_string(g_scratch));
    }
    nm_json_set(o, "dataSize", nm_json_new_number((double)data_len));
    write_line(o);
}

/* Process-global tap, installed once at startup (nm_transport_set_wire_tap). */
static NmWireTap g_tap = {
    tap_connect, tap_request, tap_response_head, tap_response,
    tap_stream_event, tap_error
};

/* ---------------------------------------------------------------- */
/* Banner + file open                                                */
/* ---------------------------------------------------------------- */

/* mkdir -p, character-level path walk (history.c's pattern). */
static int mkdir_p(char *path)
{
#ifdef _WIN32
    size_t len = strlen(path);
    for (size_t i = 1; i <= len; i++) {
        if (path[i] == '\\' || path[i] == '/' || i == len) {
            char saved = path[i];
            path[i] = '\0';
            if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) {
                if (!CreateDirectoryA(path, NULL) &&
                    GetLastError() != ERROR_ALREADY_EXISTS)
                    return -1;
            }
            path[i] = saved;
        }
    }
    return 0;
#else
    size_t len = strlen(path);
    for (size_t i = 1; i <= len; i++) {
        if (path[i] == '/' || i == len) {
            char saved = path[i];
            path[i] = '\0';
            if (mkdir(path, 0755) != 0 && errno != EEXIST)
                return -1;
            path[i] = saved;
        }
    }
    return 0;
#endif
}

/* Default directory per platform, matching history.c's state-dir
 * discovery (WIRE-DEBUG §2). dirname: $XDG_STATE_HOME/nevermore/wire
 * or %LOCALAPPDATA%\nevermore\wire. */
static int wire_default_dir(char *out, size_t cap)
{
#ifdef _WIN32
    char appdata[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_LOCAL_APPDATA, NULL, 0,
                                   appdata)))
        return snprintf(out, cap, "%s\\nevermore\\wire", appdata);
    const char *home = getenv("USERPROFILE");
    if (home)
        return snprintf(out, cap,
                        "%s\\AppData\\Local\\nevermore\\wire", home);
    return snprintf(out, cap, "nevermore\\wire");
#else
    const char *xdg = getenv("XDG_STATE_HOME");
    if (xdg && xdg[0])
        return snprintf(out, cap, "%s/nevermore/wire", xdg);
    const char *home = getenv("HOME");
    return snprintf(out, cap, "%s/.local/state/nevermore/wire",
                    home && *home ? home : "/.local/state/nevermore/wire");
#endif
}

static void banner_wall_time(char *out, size_t cap)
{
#ifdef _WIN32
    SYSTEMTIME st;
    GetSystemTime(&st);
    snprintf(out, cap, "%04d-%02d-%02dT%02d:%02d:%02dZ", st.wYear, st.wMonth,
             st.wDay, st.wHour, st.wMinute, st.wSecond);
#else
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm;
    gmtime_r(&ts.tv_sec, &tm);
    snprintf(out, cap, "%04d-%02d-%02dT%02d:%02d:%02dZ", tm.tm_year + 1900,
             tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
#endif
}

/* ISO timestamp for the default filename (local time; the banner
 * carries the UTC anchor). */
static void filename_stamp(char *out, size_t cap)
{
#ifdef _WIN32
    SYSTEMTIME st;
    GetLocalTime(&st);
    snprintf(out, cap, "%04d%02d%02d-%02d%02d%02d", st.wYear, st.wMonth,
             st.wDay, st.wHour, st.wMinute, st.wSecond);
#else
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm;
    localtime_r(&ts.tv_sec, &tm);
    snprintf(out, cap, "%04d%02d%02d-%02d%02d%02d", tm.tm_year + 1900,
             tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
#endif
}

int nm_wire_recorder_init(const char *provider, const char *model)
{
    const char *spec = getenv("NEVERMORE_DEBUG_WIRE");
    if (!spec || !*spec)
        return 0; /* off: zero overhead, no tap */

    char path[4096];
    if (strcmp(spec, "1") == 0) {
        char dir[2048];
        wire_default_dir(dir, sizeof(dir));
        char stamp[32];
        filename_stamp(stamp, sizeof(stamp));
        snprintf(path, sizeof(path), "%s/nevermore-wire-%s-%d.ndjson", dir,
                 stamp, (int)NM_GETPID());
    } else {
        snprintf(path, sizeof(path), "%s", spec);
    }

    /* Ensure the parent directory exists (mkdir -p on the dirname). */
    char dirbuf[4096];
    snprintf(dirbuf, sizeof(dirbuf), "%s", path);
    char *sep = strrchr(dirbuf, '/');
    char *bslash = strrchr(dirbuf, '\\');
    char *d = (sep && bslash) ? (sep > bslash ? sep : bslash)
                              : (sep ? sep : bslash);
    if (d) {
        *d = '\0';
        if (dirbuf[0] && mkdir_p(dirbuf) != 0) {
            fprintf(stderr, "nevermore: wire debug: could not create %s\n",
                    dirbuf);
            return -1;
        }
    }

    FILE *f = fopen(path, "w");
    if (!f) {
        fprintf(stderr,
                "nevermore: wire debug: could not open %s — recording off\n",
                path);
        return -1;
    }

    /* Re-arm: keep the key names collected via
     * nm_wire_recorder_set_env_keys (called BEFORE init), zero the
     * struct, close any previously-open log. */
    FILE *prev = g_rec.f;
    char **prev_keys = g_rec.keys;
    size_t prev_n_keys = g_rec.n_keys;
    memset(&g_rec, 0, sizeof(g_rec));
    if (prev)
        fclose(prev);
    g_rec.f = f;
    g_rec.keys = prev_keys;
    g_rec.n_keys = prev_n_keys;
    g_rec.t0 = wire_now_wall();

    nm_transport_set_wire_tap(&g_tap);

    /* Banner: plain text, '#'-prefixed so NDJSON consumers filter it
     * with grep -v '^#'. Everything a human needs to orient sits in
     * this one screenful. */
    char when[64];
    banner_wall_time(when, sizeof(when));
    fprintf(f, "# nevermore wire debug — one JSON event per line below.\n");
    fprintf(f, "# run %s pid %d nevermore %s (boba %s)\n", when,
            (int)NM_GETPID(), NM_WIRE_VERSION, BOBA_VERSION);
    fprintf(f, "# provider %s model %s\n", provider ? provider : "(none)",
            model && *model ? model : "(none)");
    fprintf(f, "# keys configured:");
    for (size_t i = 0; i < g_rec.n_keys; i++)
        fprintf(f, " %s", g_rec.keys[i]);
    fprintf(f, " (names only — values never logged)\n");
    fprintf(f, "# t is monotonic seconds from the first line of this file.\n");
    fprintf(f, "# WARNING: bodies contain conversation + file content; "
               "treat as sensitive.\n");
    fprintf(f, "# redaction: the auth header is marked at construction → "
               "%s\n",
            NM_WIRE_REDACTED);
    fprintf(f, "# replay:   curl -X POST 'URL' -H 'Header: value' ... "
               "-d 'body'  (from any \"request\" line)\n");
    fflush(f);
    return 1;
}

void nm_wire_recorder_set_env_keys(const char *const *names, size_t n)
{
    /* Collected before init writes the banner; names are copied. */
    char **keys = malloc((n ? n : 1) * sizeof(char *));
    if (!keys)
        return;
    size_t k = 0;
    for (size_t i = 0; i < n; i++) {
        char *copy = names[i] ? strdup(names[i]) : NULL;
        if (copy)
            keys[k++] = copy;
    }
    g_rec.keys = keys;
    g_rec.n_keys = k;
}

void nm_wire_recorder_shutdown(void)
{
    nm_transport_set_wire_tap(NULL);
    if (g_rec.f) {
        fflush(g_rec.f);
        fclose(g_rec.f);
        g_rec.f = NULL;
    }
    free(g_scratch);
    g_scratch = NULL;
    g_scratch_len = 0;
    g_scratch_cap = 0;
    free(g_rec.hdrs);
    g_rec.hdrs = NULL;
    g_rec.hdr_cap = 0;
    g_rec.n_hdrs = 0;
    if (g_rec.keys) {
        for (size_t i = 0; i < g_rec.n_keys; i++)
            free(g_rec.keys[i]);
        free(g_rec.keys);
        g_rec.keys = NULL;
        g_rec.n_keys = 0;
    }
}