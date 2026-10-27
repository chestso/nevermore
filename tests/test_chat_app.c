/* test_chat_app.c - the inline chat component against canned SSE
 * servers, driven through boba without a terminal.
 *
 * The component's contract under test:
 *   - submit -> nm_agent_start; step-driven turn prints the user
 *     echo and the answer into the runtime's output FILE* (captured
 *     via tmpfile), never through view()
 *   - the submit's own beat paints the handoff: the emptied composer and
 *     the thinking row, before a step or a tick (starting a turn can
 *     block on the name service, and the user must not watch their own
 *     line while it does)
 *   - line-buffered transcript: deltas split across batches continue
 *     on ONE scrollback line; the partial tail renders in the frame
 *     (view) while streaming; complete lines reach the output
 *   - /quit /model /models /provider /help as character-level
 *     commands; the model popup selects and applies a model
 *   - Ctrl+C cancels an in-flight turn and lands back in IDLE
 *   - connection failure prints an error and returns to IDLE
 *
 * The runtime is created with .output = tmpfile and driven by hand:
 * tui_runtime_send for keys, tui_runtime_flush for frames, the app's
 * own step/pump helpers for the agent — no tui_runtime_run (that
 * needs a tty; the event-loop wiring itself is main.c's job).
 */

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <boba/dynamic_buffer.h>
#include <boba/input_parser.h>
#include <boba/msg.h>
#include <boba/runtime.h>
#include <boba/stream.h>
#include <boba/terminal_profile.h>
#include <boba/unicode.h>

#include "chat_app.h"
#include "agent.h"
#include "conf.h"
#include "reminder.h"
#include "authinfo.h"
#include "colors.h"
#include "image_bytes.h"
#include "proc.h"
#include "fake_clock.h" /* nm_test_clock_advance_ms (virtual deadlines) */
#include "test_helpers.h"
#include "test_net_helpers.h"

/* MinGW has no setenv (POSIX); the tests only ever set/replace. */
static void test_setenv(const char *name, const char *value)
{
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

static void test_unsetenv(const char *name)
{
#ifdef _WIN32
    _putenv_s(name, "");
#else
    unsetenv(name);
#endif
}

/* ---------------------------------------------------------------- */
/* Canned server (test_agent's, single-round convenience wrapper)   */
/* ---------------------------------------------------------------- */

#define REQ_CAP 16384

/* Tool fixture path — string-pasted into the SSE literal (forward
 * slashes on Windows keep the JSON string escape-free, per the house
 * lessons). */
#ifdef _WIN32
#define FIXTURE_PATH "C:/Users/Public/nm-test-chat.txt"
#else
#define FIXTURE_PATH "/tmp/nm-test-chat.txt"
#endif

/* mkdir shim for the config scratch dirs (MinGW has no mkdir(d, mode)). */
#ifdef _WIN32
#define chat_mkdir(d) _mkdir(d)
#else
#define chat_mkdir(d) mkdir((d), 0755)
#endif

/* The app's agent source handle as an int (always the stream's
 * socket/fd: these tests drive a stream, never a process job). */
static int app_fd(NmChatApp *app)
{
    return (int)nm_chat_app_source(app).handle;
}

static char g_request[REQ_CAP];

struct ServerScript
{
    const char *sse[4];
    int n_rounds;
    int port;
    int fd;
    int delay_us;              /* between events of each round (0 = none) */
    int stall_at_end;          /* keep the last round's connection open (no
                                * terminating chunk) until the peer closes */
    const char *prism_head;    /* extra response-head lines, in wire form
                                * (CRLF-terminated) — a routed round's
                                * `X-Prism-*` headers. NULL = the direct
                                * case. */
    const char *prism_trailer; /* extra trailer-section lines, in wire
                                * form — the savings a routed round
                                * reports AFTER the final chunk. NULL =
                                * the empty trailer section. */
    int terminator_delay_ms;   /* hold between the last event and the
                                * 0-chunk: the late-trailer shape, where
                                * the gateway computes the figure after
                                * [DONE]. 0 = the terminator rides the
                                * last event's read (today's gateway). */

    /* Rendezvous pacing — the deterministic replacement for delay_us
     * for tests that need ONE event per client step. With `paced` set,
     * the server sends event N only after the test has granted permit
     * N (see server_paced_init / server_ack): a handshake, not a
     * guessed sleep, so the interleaving is the test's to decide and
     * costs no wall clock. `released` unblocks a server left waiting
     * when the test stops driving (teardown, an early break). */
    int paced;
    pthread_mutex_t lock;
    pthread_cond_t cv;
    int permits;  /* events the test has let through */
    int released; /* test is done driving: send the rest */
};

/* Arm the rendezvous (call right after memset'ing the script). */
static void server_paced_init(struct ServerScript *sc)
{
    pthread_mutex_init(&sc->lock, NULL);
    pthread_cond_init(&sc->cv, NULL);
    sc->paced = 1;
}

/* Let one more event through: the test calls this once per step it
 * takes, so the client sees one event per step. */
static void server_ack(struct ServerScript *sc)
{
    if (!sc->paced)
        return;
    pthread_mutex_lock(&sc->lock);
    sc->permits++;
    pthread_cond_broadcast(&sc->cv);
    pthread_mutex_unlock(&sc->lock);
}

/* Stop pacing: whatever is left goes out at once (teardown path). */
static void server_release(struct ServerScript *sc)
{
    if (!sc->paced)
        return;
    pthread_mutex_lock(&sc->lock);
    sc->released = 1;
    pthread_cond_broadcast(&sc->cv);
    pthread_mutex_unlock(&sc->lock);
}

/* Block until event `idx` may be sent (paced mode only). */
static void server_wait_permit(struct ServerScript *sc, int idx)
{
    if (!sc->paced)
        return;
    pthread_mutex_lock(&sc->lock);
    while (sc->permits <= idx && !sc->released)
        pthread_cond_wait(&sc->cv, &sc->lock);
    pthread_mutex_unlock(&sc->lock);
}

/* Send one SSE body as a chunked response — the canned server's wire
 * shape. `sc` may be NULL: no pacing, no inter-event delay, no extra
 * head lines (the servers that need a held catalog request have their
 * own thread). The 0-chunk terminator is the caller's (the stalling
 * rounds send none). */
static void sse_send_chunked(int cfd, const char *body, struct ServerScript *sc)
{
    char head[256];
    int hl = snprintf(head, sizeof(head),
                      "HTTP/1.1 200 OK\r\n"
                      "Content-Type: text/event-stream\r\n"
                      "%s"
                      "Transfer-Encoding: chunked\r\n\r\n",
                      (sc && sc->prism_head) ? sc->prism_head : "");
    send(cfd, head, (size_t)hl, 0);
    size_t bl = strlen(body);
    size_t off = 0;
    int ev_idx = 0;
    while (off < bl) {
        const char *ev_end = strstr(body + off, "\n\n");
        size_t ev_len =
            ev_end ? (size_t)(ev_end - (body + off)) + 2 : bl - off;
        /* Paced: event N goes out only once the test has stepped N
         * times (so the client sees one event per step). */
        if (sc)
            server_wait_permit(sc, ev_idx);
        char chunk[REQ_CAP];
        int cl = snprintf(chunk, sizeof(chunk), "%zx\r\n", ev_len);
        memcpy(chunk + cl, body + off, ev_len);
        cl += (int)ev_len;
        memcpy(chunk + cl, "\r\n", 2);
        cl += 2;
        size_t cs = 0;
        while (cs < (size_t)cl) {
            long n = send(cfd, chunk + cs, (size_t)cl - cs, 0);
            if (n <= 0)
                break;
            cs += (size_t)n;
        }
        if (sc && sc->delay_us)
            usleep((unsigned)sc->delay_us);
        off += ev_len;
        ev_idx++;
    }
}

static void *chat_server_thread(void *arg)
{
    struct ServerScript *sc = arg;
    for (int round = 0; round < sc->n_rounds; round++) {
        /* Bounded accept: a turn cancelled during the async connect
         * or send phase may never produce a deliverable connection
         * (macOS/BSD drop it from the backlog; Linux delivers EOF).
         * Wait bounded, then treat a no-show as end of script. */
        struct timeval atv = { 2, 0 };
        fd_set arfds;
        FD_ZERO(&arfds);
        FD_SET(sc->fd, &arfds);
        if (select(sc->fd + 1, &arfds, NULL, NULL, &atv) <= 0)
            return NULL; /* cancelled before connecting: fine */
        int cfd = accept(sc->fd, NULL, NULL);
        if (cfd < 0)
            return NULL;
        /* Drain the request; capture it. */
        char req[REQ_CAP];
        size_t got = 0;
        while (got < sizeof(req) - 1) {
            long n = recv(cfd, req + got, sizeof(req) - 1 - got, 0);
            if (n <= 0)
                break;
            got += (size_t)n;
            if (strstr(req, "\r\n\r\n") && got > 4 && req[got - 1] == '}')
                break;
        }
        req[got] = '\0';
        if (got < REQ_CAP)
            snprintf(g_request, REQ_CAP, "%s", req);

        const char *body = sc->sse[round];
        if (!body || !*body) {
            /* Stalling round: keep the connection open, send nothing.
             * A closed peer is READABLE (EOF), so drain: 0 = gone. */
            while (1) {
                struct timeval tv = { 0, 500 * 1000 };
                fd_set rfds;
                FD_ZERO(&rfds);
                FD_SET(cfd, &rfds);
                if (select(cfd + 1, &rfds, NULL, NULL, &tv) <= 0)
                    continue; /* keep stalling until the peer closes */
                char sink[256];
                long n = recv(cfd, sink, sizeof(sink), 0);
                if (n <= 0)
                    break; /* peer closed */
            }
            close(cfd);
            continue;
        }

        sse_send_chunked(cfd, body, sc);
        if (sc->stall_at_end && round == sc->n_rounds - 1) {
            /* Hold the connection open without the terminal chunk —
             * the client stays mid-stream until it cancels. A closed
             * peer is READABLE (EOF), so drain bytes: 0 = peer gone. */
            while (1) {
                struct timeval tv = { 0, 500 * 1000 };
                fd_set rfds;
                FD_ZERO(&rfds);
                FD_SET(cfd, &rfds);
                if (select(cfd + 1, &rfds, NULL, NULL, &tv) <= 0)
                    continue;
                char sink[256];
                long n = recv(cfd, sink, sizeof(sink), 0);
                if (n <= 0)
                    break; /* peer closed (cancel) */
            }
            close(cfd);
            continue;
        }
        /* The 0-chunk terminator + trailer section. The hold makes the
         * trailer arrive in a segment AFTER [DONE] was read (the
         * framing tail — a server that computes the figure after the
         * stream ends, docs/HYPER-API.md §3.5.2). */
        if (sc->terminator_delay_ms)
            usleep((unsigned)sc->terminator_delay_ms * 1000);
        send(cfd, "0\r\n", 3, 0);
        if (sc->prism_trailer)
            send(cfd, sc->prism_trailer, strlen(sc->prism_trailer), 0);
        send(cfd, "\r\n", 2, 0);
        close(cfd);
    }
    return NULL;
}

static int server_bind(int *port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) < 0)
        return -1;
    socklen_t l = sizeof(a);
    if (getsockname(fd, (struct sockaddr *)&a, &l) < 0) {
        close(fd);
        return -1;
    }
    *port = ntohs(a.sin_port);
    if (listen(fd, 8) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* ---------------------------------------------------------------- */
/* App harness: runtime over a tmpfile output, hand-driven          */
/* ---------------------------------------------------------------- */

#define OUT_CAP (256 * 1024)

typedef struct AppHarness
{
    NmChatApp *app;
    TuiRuntime *rt;
    FILE *out;
    char *text; /* snapshot of everything written */
    /* Optional rendezvous partner: when set, every step this harness
     * takes grants the scripted server one more event (see
     * server_paced_init). Tests that need one event per step set it
     * instead of a delay_us guess. */
    struct ServerScript *pacer;
} AppHarness;

/* Read the whole output FILE* into a heap string (rewinds nothing —
 * the file is opened r+ so we can read and reset). */
static const char *harness_read(AppHarness *h)
{
    fflush(h->out);
    long pos = ftell(h->out);
    rewind(h->out);
    size_t n = fread(h->text, 1, OUT_CAP - 1, h->out);
    h->text[n] = '\0';
    /* Restore the write position so later writes append. */
    fseek(h->out, pos, SEEK_SET);
    return h->text;
}

static void harness_free(AppHarness *h)
{
    /* The runtime owns the app (component->free). */
    if (h->rt)
        tui_runtime_free(h->rt);
    if (h->out)
        fclose(h->out);
    free(h->text);
    free(h);
}

/* The spinner's color tier comes from the terminal profile's
 * env-derived truecolor hint (COLORTERM, or a direct-color TERM), so the
 * TEST PROCESS's own environment must not decide a frame's bytes: pin
 * the 16-color tier — the blink — before the runtime exists. The probe
 * reads the environment when it starts (the first flush, inside
 * harness_new), so harness_new_rgb flips COLORTERM on for the duration
 * of the call and back off after: the profile keeps the hint it read,
 * and the tests that assert the pulse get the RGB tier. */
static void pin_color_env(int truecolor)
{
    if (truecolor) {
        test_setenv("COLORTERM", "truecolor");
    } else {
        test_unsetenv("COLORTERM");
        test_setenv("TERM", "xterm-256color"); /* never a -direct/-truecolor */
    }
}

/* Create the app + runtime pair wired like main.c does it. Provider
 * defaults to "ollama:local" (static catalog + openai_client wire). */
static AppHarness *harness_new_full(const char *provider, const char *model,
                                    const char *base_url, int truecolor)
{
    pin_color_env(truecolor);
    /* A test starts from a known instant: deadlines are all relative,
     * but a stale advance from a previous test would make a failure
     * depend on test order. */
    nm_test_clock_reset();
    AppHarness *h = calloc(1, sizeof(*h));
    if (!h)
        return NULL;
    h->text = malloc(OUT_CAP);
    h->out = tmpfile();
    h->app = nm_chat_app_new(provider, model);
    if (!h->text || !h->out || !h->app) {
        harness_free(h);
        return NULL;
    }

    TuiRuntimeConfig cfg = {
        .raw_mode = 0,
        .output = h->out,
    };
    h->rt = tui_runtime_create(
        (TuiComponent *)nm_chat_app_component(h->app), h->app, &cfg);
    if (!h->rt) {
        harness_free(h);
        return NULL;
    }
    nm_chat_app_set_runtime(h->app, h->rt);
    if (base_url)
        nm_chat_app_set_endpoint(h->app, base_url, NULL);

    /* The event loop's first flush (run() does this before reading). */
    tui_runtime_flush(h->rt);

    /* The probe has read the environment: put the process back on the
     * pinned 16-color tier, so one RGB harness cannot leak its hint
     * into the next test's frames. */
    pin_color_env(0);
    return h;
}

/* The default harness: the 16-color tier (the dot blinks). */
static AppHarness *harness_new(const char *provider, const char *model,
                               const char *base_url)
{
    return harness_new_full(provider, model, base_url, 0);
}

/* The same harness on a truecolor terminal (the dot pulses). */
static AppHarness *harness_new_rgb(const char *provider, const char *model,
                                   const char *base_url)
{
    return harness_new_full(provider, model, base_url, 1);
}

/* Type a string into the textinput (plain chars); each key is sent
 * through the runtime like the event loop would, flushing after
 * (the stdin-ready branch flushes each iteration). */
static void harness_type(AppHarness *h, const char *s)
{
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        tui_runtime_send(h->rt, tui_msg_char(*p, 0));
        tui_runtime_flush(h->rt);
    }
}

/* Feed raw terminal bytes through the input parser and into the runtime,
 * the way the event loop's read does — the wire is the parser's input,
 * so a test about an ENCODING (a kitty CSI u key) starts here. */
static void harness_wire(AppHarness *h, const char *bytes)
{
    TuiInputParser *p = tui_input_parser_create();
    if (!p)
        return;
    TuiMsg msgs[8];
    int n = tui_input_parser_parse(p, (const unsigned char *)bytes,
                                   strlen(bytes), msgs, 8);
    for (int i = 0; i < n; i++) {
        tui_runtime_send(h->rt, msgs[i]);
        tui_runtime_flush(h->rt);
    }
    tui_input_parser_free(p);
}

/* ---------------------------------------------------------------- */
/* Config write-back harness                                         */
/* ---------------------------------------------------------------- */

/* The provider-name validator main.c installs (config.c has no registry
 * of its own). */
static int chat_valid_provider(const char *name)
{
    return nm_provider_by_name(name) != NULL;
}

static char g_cfg_root[600];
static char g_cfg_user[700];
static char g_cfg_shadow[700];

/* Pin both config paths into the test's own scratch dir — never the
 * real ~/.config or ~/.local/state. The two files are removed first,
 * for the reason test_config.c pins its own the same way: the scratch
 * root is PID-keyed and a Wine run reuses the previous run's PID, so a
 * leftover SHADOW leaks into this run — the app loads it and re-emits
 * what it finds, and an exact-content shadow assert then fails on a
 * file this run never wrote. */
static void pin_cfg_paths(const char *sub)
{
    snprintf(g_cfg_root, sizeof(g_cfg_root), "%s/cfg-%s",
             test_scratch_dir(), sub);
    chat_mkdir(g_cfg_root);
    snprintf(g_cfg_user, sizeof(g_cfg_user), "%s/config", g_cfg_root);
    snprintf(g_cfg_shadow, sizeof(g_cfg_shadow), "%s/shadow", g_cfg_root);
    remove(g_cfg_user);
    remove(g_cfg_shadow);
    nm_config_set_paths(g_cfg_user, g_cfg_shadow);
}

static const char *cfg_read_shadow(void)
{
    static char buf[4096];
    buf[0] = '\0';
    FILE *f = fopen(g_cfg_shadow, "rb");
    if (!f)
        return buf;
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    return buf;
}

static int cfg_file_present(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return 0;
    fclose(f);
    return 1;
}

static void write_file_at(const char *path, const char *content)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "  setup: cannot write %s\n", path);
        return;
    }
    fwrite(content, 1, strlen(content), f);
    fclose(f);
}

/* The user config's bytes, verbatim (the "never touched" assertion). */
static const char *cfg_user_bytes(void)
{
    static char buf[4096];
    buf[0] = '\0';
    FILE *f = fopen(g_cfg_user, "rb");
    if (!f)
        return buf;
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    return buf;
}

/* Load a config for `h` and wire it exactly like main.c does: install
 * it on the app (which publishes it as the process store, so the
 * machinery — agent, connect walk, web_search — resolves every setting
 * from it at the point of use). Nothing is pushed. */
static NmConfig *cfg_for(AppHarness *h)
{
    NmConfig *cfg = nm_config_load();
    nm_config_set_env(cfg);
    nm_chat_app_set_config(h->app, cfg);
    return cfg;
}

/* The connect knobs and the family latch live in the config store; the
 * tests drive them on the runtime layer, the same way /config and the
 * walk's own latch do. A scratch store (no file I/O) is used when a
 * test has no config of its own — begin/end around the test so one
 * test's store never leaks into the next. */
static NmConfig *g_scratch_cfg;

static void scratch_store_begin(void)
{
    g_scratch_cfg = nm_config_new();
    nm_config_set_store(g_scratch_cfg);
}

static void scratch_store_end(void)
{
    if (nm_config_store() == g_scratch_cfg)
        nm_config_set_store(NULL);
    nm_config_free(g_scratch_cfg);
    g_scratch_cfg = NULL;
}

static void store_set(const char *key, const char *value)
{
    NmConfig *c = nm_config_store();
    if (c)
        nm_config_runtime_set(c, key, value);
}

static void store_clear(const char *key)
{
    NmConfig *c = nm_config_store();
    if (c)
        nm_config_runtime_clear(c, key);
}

static void knobs_set_timeout(int ms)
{
    char b[32];
    snprintf(b, sizeof(b), "%d", ms);
    store_set(NM_CFG_KEY_CONNECT_TIMEOUT, b);
}

static void knobs_set_skip_families(int mask)
{
    store_set(NM_CFG_KEY_SKIP_FAMILIES, nm_family_name(mask));
}

static void harness_enter(AppHarness *h)
{
    tui_runtime_send(h->rt, tui_msg_key(TUI_KEY_ENTER, 0, 0));
    tui_runtime_flush(h->rt);
}

#ifndef _WIN32
/* The agent's own entry in the app's wait set. The set is an array now
 * (agent stream/exec fd first, then one READ entry per process
 * session), so a test that wants "the agent's interest" looks up its
 * fd rather than assuming slot 0 is a single connection.
 *
 * POSIX-only: every caller pairs the flags with a select() on the fd,
 * so the Windows build (where a job's source is a waitable HANDLE, not
 * a socket) never reaches here. */
static unsigned app_interest(NmChatApp *app)
{
    NmSource e[TUI_IO_SOURCE_MAX];
    size_t n = nm_chat_app_interest(app, e, TUI_IO_SOURCE_MAX);
    int afd = app_fd(app);
    for (size_t i = 0; i < n; i++) {
        if (e[i].handle == afd)
            return e[i].flags;
    }
    return 0;
}
#endif /* !_WIN32 */

/* Wait up to `timeout_ms` on an arbitrary source, the way boba's loop
 * does. A Windows process job's readiness object is a waitable event, so
 * select() cannot be used on it — it would fail at once and every caller
 * here would spin instead of waiting (run_command and the job tools both
 * ride that mechanism). */
static void app_wait_src(const NmSource *sp, int timeout_ms)
{
    NmSource s = *sp;
#ifdef _WIN32
    if (s.kind == NM_SRC_HANDLE) {
        WaitForSingleObject((HANDLE)s.handle, (DWORD)timeout_ms);
        return;
    }
    fd_set r, w;
    struct timeval tv = { timeout_ms / 1000, (timeout_ms % 1000) * 1000 };
    FD_ZERO(&r);
    FD_ZERO(&w);
    if (s.flags & NM_INTEREST_READ)
        FD_SET((SOCKET)s.handle, &r);
    if (s.flags & NM_INTEREST_WRITE)
        FD_SET((SOCKET)s.handle, &w);
    select(0, (s.flags & NM_INTEREST_READ) ? &r : NULL,
           (s.flags & NM_INTEREST_WRITE) ? &w : NULL, NULL, &tv);
#else
    fd_set r, w;
    struct timeval tv = { timeout_ms / 1000, (timeout_ms % 1000) * 1000 };
    FD_ZERO(&r);
    FD_ZERO(&w);
    int fd = (int)s.handle;
    if (s.flags & NM_INTEREST_READ)
        FD_SET(fd, &r);
    if (s.flags & NM_INTEREST_WRITE)
        FD_SET(fd, &w);
    select(fd + 1, (s.flags & NM_INTEREST_READ) ? &r : NULL,
           (s.flags & NM_INTEREST_WRITE) ? &w : NULL, NULL, &tv);
#endif
}

/* Wait up to `timeout_ms` for the app's agent source. */
static void app_wait(AppHarness *h, int timeout_ms)
{
    NmSource s = nm_chat_app_source(h->app);
    if (s.handle < 0 || !s.flags)
        return;
    app_wait_src(&s, timeout_ms);
}

/* One step, plus the rendezvous grant the step just consumed (a paced
 * server sends the next event only when the test asks for it). */
static void harness_step_ack(AppHarness *h)
{
    nm_chat_app_step(h->app);
    if (h->pacer)
        server_ack(h->pacer);
}

/* Drive an in-flight catalog fetch the way the event loop does: wait on
 * the app's source, then hand it to external_ready (the popup opens on
 * the terminal step). Bounded; 0 when no fetch is in flight. */
static int harness_pump_catalog(AppHarness *h, int max_spins)
{
    for (int i = 0; i < max_spins; i++) {
        NmSource s = nm_chat_app_source(h->app);
        if (s.handle < 0 || !s.flags)
            return 0;
        app_wait_src(&s, 10);
        nm_chat_app_external_ready(h->app, s.handle, s.flags);
        tui_runtime_flush(h->rt);
    }
    return -1;
}

/* Drive the agent to completion the way the runtime's external-fd
 * loop would: poll fd -> step. Bounded. */
/* Drive the agent to completion the way the runtime's external-fd
 * loop would: wait on the app's source -> step. Bounded. */
static int harness_drive(AppHarness *h, int max_spins)
{
    for (int i = 0; i < max_spins; i++) {
        NmAgentState st = nm_chat_app_state(h->app);
        if (st == NM_AGENT_DONE || st == NM_AGENT_ERROR ||
            st == NM_AGENT_IDLE)
            return 0;
        NmSource s = nm_chat_app_source(h->app);
        if (s.handle >= 0 && s.flags) {
            app_wait(h, 10);
            harness_step_ack(h);
        } else if (s.handle < 0) {
            /* Tool phase (announce/execute): no source, step makes
             * progress immediately. */
            harness_step_ack(h);
        } else {
            usleep(10 * 1000);
        }
        tui_runtime_flush(h->rt);
    }
    return -1;
}

/* Milliseconds from a portable wall clock: a test's own bound, never
 * the clock under test (that one is tests/fake_clock.c). */
static long long wall_ms(void)
{
#ifdef _WIN32
    return (long long)GetTickCount64();
#else
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (long long)tv.tv_sec * 1000 + tv.tv_usec / 1000;
#endif
}

/* Wait up to `timeout_ms` on one source; 1 when it became ready, 0 on
 * a timeout. The ready/timed-out distinction is what the event loop's
 * two branches turn on (an fd event vs the tick). */
static int app_wait_ready(const NmSource *sp, int timeout_ms)
{
    NmSource s = *sp;
#ifdef _WIN32
    if (s.kind == NM_SRC_HANDLE) {
        DWORD r = WaitForSingleObject((HANDLE)s.handle, (DWORD)timeout_ms);
        return r == WAIT_OBJECT_0 ? 1 : 0;
    }
    fd_set r, w;
    struct timeval tv = { timeout_ms / 1000, (timeout_ms % 1000) * 1000 };
    FD_ZERO(&r);
    FD_ZERO(&w);
    if (s.flags & NM_INTEREST_READ)
        FD_SET((SOCKET)s.handle, &r);
    if (s.flags & NM_INTEREST_WRITE)
        FD_SET((SOCKET)s.handle, &w);
    return select(0, (s.flags & NM_INTEREST_READ) ? &r : NULL,
                  (s.flags & NM_INTEREST_WRITE) ? &w : NULL, NULL,
                  &tv) > 0
               ? 1
               : 0;
#else
    fd_set r, w;
    struct timeval tv = { timeout_ms / 1000, (timeout_ms % 1000) * 1000 };
    FD_ZERO(&r);
    FD_ZERO(&w);
    int fd = (int)s.handle;
    if (s.flags & NM_INTEREST_READ)
        FD_SET(fd, &r);
    if (s.flags & NM_INTEREST_WRITE)
        FD_SET(fd, &w);
    return select(fd + 1, (s.flags & NM_INTEREST_READ) ? &r : NULL,
                  (s.flags & NM_INTEREST_WRITE) ? &w : NULL, NULL,
                  &tv) > 0
               ? 1
               : 0;
#endif
}

/* Drive a turn the way boba's EVENT LOOP does: wait on the app's wait
 * target, run the app's ready branch when it fires, and the app's tick
 * when the wait times out.
 *
 * The plain harness_drive steps on EVERY iteration, and that HIDES a
 * stall: the tool phase has no deadline, so a step is the only thing
 * that can move it — and in the real loop a step comes from an fd
 * event or from a due deadline, never from the passage of time. A turn
 * that waits for a step nobody makes looks alive to harness_drive and
 * dead to the user.
 *
 * Bounded by WALL CLOCK (a ready fd costs microseconds, so an
 * iteration count would not bound the wait). */
static int harness_drive_events(AppHarness *h, int budget_ms)
{
    long long t0 = wall_ms();
    for (;;) {
        NmAgentState st = nm_chat_app_state(h->app);
        if (st == NM_AGENT_DONE || st == NM_AGENT_ERROR ||
            st == NM_AGENT_IDLE)
            return 0;
        if (wall_ms() - t0 > (long long)budget_ms)
            return -1;
        NmSource s = nm_chat_app_source(h->app);
        int wait_ms = nm_chat_app_tick_ms(h->app);
        if (wait_ms < 0)
            wait_ms = 50; /* no cadence: a short poll */
        int ready = 0;
        if (s.handle >= 0 && s.flags)
            ready = app_wait_ready(&s, wait_ms);
        if (ready)
            nm_chat_app_external_ready(h->app, s.handle, s.flags);
        else
            nm_chat_app_tick(h->app);
        tui_runtime_flush(h->rt);
    }
}

/* One step + frame repaint (for mid-stream assertions). Steps until
 * the round's text collector has content (bounded) — the server's
 * accept+send races the poll, and readability alone can fire before
 * any payload bytes have landed. */
/* One step, plus the flush the event loop would do. No payload wait:
 * for tests that intentionally STALL (no SSE payload at all), where
 * harness_step_once would burn its whole budget waiting for a tail
 * that never comes (5 s of the watchdog's 10 s per binary). */
/* Drive a turn to its end on VIRTUAL time: tick at the app's reported
 * cadence, advancing the fake clock by that cadence — the timer-driven
 * sibling of harness_drive (which waits on real I/O). For a turn whose
 * progress comes from a DEADLINE (a connect walk's per-attempt budget,
 * the stream-inactivity timeout) this is exact and instant, and it is
 * the same drive boba's loop performs: the tick IS the timer. Returns 0
 * when the turn ended, -1 when the tick budget ran out. */
static int harness_drive_virtual(AppHarness *h, int max_ticks)
{
    for (int i = 0; i < max_ticks; i++) {
        NmAgentState st = nm_chat_app_state(h->app);
        if (st == NM_AGENT_DONE || st == NM_AGENT_ERROR ||
            st == NM_AGENT_IDLE)
            return 0;
        int wait = nm_chat_app_tick_ms(h->app);
        if (wait < 0)
            wait = 5;
        nm_test_clock_advance_ms(wait);
        nm_chat_app_tick(h->app);
        tui_runtime_flush(h->rt);
    }
    return -1;
}

/* One step, plus the flush the event loop would do. No payload wait:
 * for tests that intentionally STALL (no SSE payload at all), where
 * harness_step_once would burn its whole budget waiting for a tail
 * that never comes (5 s of the watchdog's 10 s per binary). */
static void harness_single_step(AppHarness *h)
{
    app_wait(h, 50);
    harness_step_ack(h);
    tui_runtime_flush(h->rt);
}

/* Step until the app's collector has tail bytes, then flush. Bounded
 * (100 × 50 ms select) — the payload-waiting sibling of
 * harness_single_step. */
/* Step until the app's collector has tail bytes, then flush. Bounded
 * (100 × 50 ms wait) — the payload-waiting sibling of
 * harness_single_step. */
static void harness_step_once(AppHarness *h)
{
    for (int i = 0; i < 100; i++) {
        if (nm_chat_app_source(h->app).handle < 0)
            break;
        app_wait(h, 50);
        harness_step_ack(h);
        if (nm_chat_app_tail_len(h->app) > 0)
            break;
    }
    tui_runtime_flush(h->rt);
}

/* The exact bytes boba's span path paints for one styled span (the status
 * line and the prompt are both TuiStyle span sets, not app byte writers),
 * so a test derives the expected bytes from the same color role the app
 * declares — that is what makes the assertion about the ROLE, not a
 * literal. Caller frees. */
/* One SGR role prefix + text, as the 16-COLOR harness paints it. The
 * byte writers resolve a role through nm_sgr() for the terminal's tier,
 * so an assertion on bytes has to spell the tier it pinned:
 * harness_new is 16-color (a palette-index SGR), harness_new_rgb is
 * 24-bit. A small ring, so a call site needs no free — these are
 * assertions, not data. */
static const char *sgr16(NmSgrRole role, const char *text)
{
    static char ring[4][512];
    static int i;
    char *b = ring[i++ & 3];
    snprintf(b, sizeof(ring[0]), "%s%s", nm_sgr(role, NM_COLOR_16), text);
    return b;
}

static char *span_bytes(TuiColor color, const char *text)
{
    TuiStyle s = tui_style_foreground(tui_style_new(), color);
    s.inline_ = 1;
    return tui_style_render(&s, text);
}

/* The status row's right-hand half, exactly as boba's layout paints it:
 * the rule filling the slack (gutter Comment), then the identity
 * segment's own pad_left blank and its text (nm_color_status_identity).
 * `left` is the row's left chrome text and `identity` the identity block
 * — the row's width is the harness's terminal (80), so the rule takes
 * what is left of it. Caller frees. */
static char *status_row_bytes(const char *left, const char *identity)
{
    int rule = 80 - (int)tui_utf8_display_width(left) - 1 /* pad_left */ -
               (int)tui_utf8_display_width(identity);
    if (rule < 1)
        rule = 1;
    char rule_text[256 * 3 + 1];
    size_t o = 0;
    for (int i = 0; i < rule; i++) {
        rule_text[o++] = (char)0xe2;
        rule_text[o++] = (char)0x94;
        rule_text[o++] = (char)0x80; /* U+2500 */
    }
    rule_text[o] = '\0';

    char ident_text[512];
    snprintf(ident_text, sizeof(ident_text), " %s", identity);

    char *rule_span = span_bytes(nm_color_gutter(NM_COLOR_16), rule_text);
    char *ident_span = span_bytes(nm_color_status_identity(NM_COLOR_16), ident_text);
    if (!rule_span || !ident_span) {
        free(rule_span);
        free(ident_span);
        return NULL;
    }
    size_t n = strlen(rule_span) + strlen(ident_span) + 1;
    char *row = (char *)malloc(n);
    if (row)
        snprintf(row, n, "%s%s", rule_span, ident_span);
    free(rule_span);
    free(ident_span);
    return row;
}

/* The gauge's fields as the row paints them: ONE SPAN PER FIELD, in
 * declaration order. The counts and the rate carry the TIER's color
 * (nm_color_gutter at rest, Orange/Red under pressure), while the output
 * count carries its own (nm_color_gauge_output) — it is not a window
 * fact, so it must not change color with the window. An empty field is a
 * span that is simply not there.
 *
 * `depth` is the tier the harness pinned: harness_new is 16-color (so the
 * output span is a palette-index SGR), harness_new_rgb truecolor (a 24-bit
 * one). Caller frees. */
static char *gauge_bytes(TuiColor tier, const char *counts, const char *out,
                         const char *rate, NmColorDepth depth)
{
    int has_out = out && out[0];
    int has_rate = rate && rate[0];
    char *a = span_bytes(tier, counts);
    char *b = has_out ? span_bytes(nm_color_gauge_output(depth), out) : NULL;
    char *c = has_rate ? span_bytes(tier, rate) : NULL;
    if (!a || (has_out && !b) || (has_rate && !c)) {
        free(a);
        free(b);
        free(c);
        return NULL;
    }
    size_t n = strlen(a) + (b ? strlen(b) : 0) + (c ? strlen(c) : 0) + 1;
    char *row = (char *)malloc(n);
    if (row)
        snprintf(row, n, "%s%s%s", a, b ? b : "", c ? c : "");
    free(a);
    free(b);
    free(c);
    return row;
}

/* The same, on the row at rest (the gauge's tier is Comment). */
static char *gauge_bytes_rest(const char *counts, const char *out,
                              const char *rate, NmColorDepth depth)
{
    return gauge_bytes(nm_color_gutter(depth), counts, out, rate, depth);
}

/* The harness's identity for a provider/model pair: the ONE spelling the
 * status row's right block and the startup banner share. */
#define IDENT(p, m) p " \xc2\xb7 " m

/* The frame's POPUP region: everything painted after the input's prompt
 * (the popup is the last thing the composer paints). The status row's
 * identity block carries the active model id too, so "the picker does
 * not list X" must not scan the whole frame. */
static const char *frame_popup(const char *frame, const char *prompt_span)
{
    const char *last = NULL;
    for (const char *p = strstr(frame, prompt_span); p;
         p = strstr(p + 1, prompt_span))
        last = p;
    return last ? last + strlen(prompt_span) : frame;
}

/* The status row as the frame paints it: the bytes between the
 * statusline's own lead and the "\r\n" the composer puts after it (the
 * input's own lead follows). NULL when the frame carries no chrome row
 * (the submit frame). Caller frees.
 *
 * The prompt span anchors it: the input's lead is the "\r\x1b[K" right
 * before the prompt, and the row's own lead is the nearest "\r" before
 * that (the row itself carries no carriage return). */
static char *frame_status_row(const char *frame, const char *prompt_span)
{
    const char *p = strstr(frame, prompt_span);
    if (!p || p - frame < 6)
        return NULL;
    if (memcmp(p - 4, "\r\x1b[K", 4) != 0)
        return NULL;
    const char *end = p - 4;
    if (memcmp(end - 2, "\r\n", 2) != 0)
        return NULL; /* no chrome row: the input opens the frame's row */
    end -= 2;
    const char *start = end;
    while (start > frame && start[-1] != '\r')
        start--;
    if (start == frame || end - start < 3)
        return NULL;
    start += 3; /* past the row's own "\x1b[K" (the "\r" is behind us) */
    size_t n = (size_t)(end - start);
    char *row = (char *)malloc(n + 1);
    if (!row)
        return NULL;
    memcpy(row, start, n);
    row[n] = '\0';
    return row;
}

/* The SGR the identity block is painted with (its span's prefix), for
 * "is there any identity on this row?" assertions. Caller frees. */
static char *status_identity_sgr(void)
{
    char *s = span_bytes(nm_color_status_identity(NM_COLOR_16), "x");
    if (s) {
        char *x = strchr(s, 'x');
        if (x)
            *x = '\0';
    }
    return s;
}

/* ---------------------------------------------------------------- */
/* Tests                                                            */
/* ---------------------------------------------------------------- */

static void test_submit_echoes_and_prints_answer(void)
{
    struct ServerScript sc;
    memset(&sc, 0, sizeof(sc));
    sc.n_rounds = 1;
    sc.sse[0] =
        "data: {\"choices\":[{\"delta\":{\"content\":\"Hello world\"}}]}\n\n"
        "data: [DONE]\n\n";
    sc.fd = server_bind(&sc.port);
    ASSERT_TRUE(sc.fd >= 0);
    pthread_t th;
    pthread_create(&th, NULL, chat_server_thread, &sc);

    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1", sc.port);
    AppHarness *h = harness_new("ollama:local", "test-model", base);
    ASSERT_NOT_NULL(h);

    harness_type(h, "hi there");
    harness_enter(h);
    ASSERT_EQ(nm_chat_app_state(h->app), NM_AGENT_STREAMING);
    ASSERT_EQ(harness_drive(h, 500), 0);
    ASSERT_EQ(nm_chat_app_state(h->app), NM_AGENT_DONE);

    const char *out = harness_read(h);
    /* The user's message lands in the scrollback with the echo marker. */
    ASSERT_TRUE(strstr(out, "hi there") != NULL);
    /* The streamed answer lands in the scrollback. */
    ASSERT_TRUE(strstr(out, "Hello world") != NULL);
    /* The answer is terminated as a whole line (line-buffered). */
    ASSERT_TRUE(strstr(out, "Hello world\r\n") != NULL);
    /* The request rode the configured model. */
    ASSERT_TRUE(strstr(g_request, "\"model\":\"test-model\"") != NULL);

    harness_free(h);
    pthread_join(th, NULL);
    close(sc.fd);
}

/* The "remove if too many" half: a body that ends in blank lines — here
 * an unterminated fence, whose trailing blanks boba would otherwise
 * commit verbatim — still yields exactly ONE blank line, never a stack.
 * Defined after the raw responder helpers below. */
static void test_separator_collapses_trailing_blank_lines(void);

/* Reasoning then content: the separator follows each run — one blank
 * line between the (dim) reasoning and the answer, one after the
 * answer — and never doubles at the turn end. Defined after the raw
 * responder helpers below. */
static void test_separator_between_reasoning_and_answer(void);

static void test_delta_line_continuation_is_preserved(void)
{
    /* The regression test for the line-buffer protocol: two deltas
     * split across readable events must continue on ONE scrollback
     * line — no forced newline, no interleaved spinner row. The
     * server's rendezvous lets the app see exactly one delta per step
     * (a handshake, not a guessed delay). */
    struct ServerScript sc;
    memset(&sc, 0, sizeof(sc));
    server_paced_init(&sc);
    sc.n_rounds = 1;
    sc.sse[0] =
        "data: {\"choices\":[{\"delta\":{\"content\":\"Hello \"}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{\"content\":\"world\"}}]}\n\n"
        "data: [DONE]\n\n";
    sc.fd = server_bind(&sc.port);
    ASSERT_TRUE(sc.fd >= 0);
    pthread_t th;
    pthread_create(&th, NULL, chat_server_thread, &sc);

    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1", sc.port);
    AppHarness *h = harness_new("ollama:local", "test-model", base);
    ASSERT_NOT_NULL(h);
    h->pacer = &sc;

    harness_type(h, "continue");
    harness_enter(h);
    ASSERT_EQ(harness_drive(h, 500), 0);

    const char *out = harness_read(h);
    ASSERT_TRUE(strstr(out, "Hello world\r\n") != NULL);
    /* The continuation never landed on a fresh transcript line (the
     * frame's own bytes legitimately contain "Hello \r\n" as the
     * tail-row separator — the regression is the *transcript* split,
     * which would print "world" as the start of the next line). */
    ASSERT_TRUE(strstr(out, "Hello \r\nworld") == NULL);

    server_release(&sc);
    harness_free(h);
    pthread_join(th, NULL);
    close(sc.fd);
}

/* Regression (bugs/streaming-dupes-and-cursor-move-to-top-of-terminal.md):
 * with an EMPTY tail (the whole "thinking" phase), the busy frame
 * must not begin with \r\n — that painted a phantom leading blank
 * row between the submitted prompt and the spinner, and boba's
 * newline row counting then tracked one row more than the spinner
 * ever used. */
static void test_busy_frame_with_empty_tail_has_no_phantom_row(void)
{
    struct ServerScript sc;
    memset(&sc, 0, sizeof(sc));
    sc.n_rounds = 1;
    /* Stalling round: no SSE payload at all — the app sits in
     * STREAMING state with an empty tail ("thinking"). */
    sc.sse[0] = "";
    sc.stall_at_end = 1;
    sc.fd = server_bind(&sc.port);
    ASSERT_TRUE(sc.fd >= 0);
    pthread_t th;
    pthread_create(&th, NULL, chat_server_thread, &sc);

    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1", sc.port);
    AppHarness *h = harness_new("ollama:local", "test-model", base);
    ASSERT_NOT_NULL(h);

    harness_type(h, "hello");
    harness_enter(h);
    /* One step: this round never sends a payload, so the payload-wait
     * pump would just spin out its budget. */
    harness_single_step(h);
    nm_chat_app_tick(h->app); /* advance the spinner (the run loop's tick) */
    tui_runtime_flush(h->rt);

    ASSERT_EQ(nm_chat_app_state(h->app), NM_AGENT_STREAMING);
    ASSERT_EQ(nm_chat_app_tail_len(h->app), 0u);
    const char *frame = tui_runtime_render(h->rt);
    ASSERT_NOT_NULL(frame);
    /* The frame's first row is the spinner itself: no leading
     * line separator (the phantom row). */
    ASSERT_TRUE(strncmp(frame, "\r\n", 2) != 0);
    /* The busy frame carries the INPUT AREA: its status row (the
     * spinner's glyph in its own color, the context gauge with no usage
     * and no known limit yet, the separator rule filling the row, and
     * the right-aligned identity block) on one row, then the accent
     * prompt on the next — the input is always where the next prompt is
     * gathered (R1). What the turn is doing is the glyph's STYLE, so
     * there is no label beside it. */
    char *glyph = span_bytes(tui_color_ansi(10), "\xc2\xb7 "); /* · */
    char *gauge = span_bytes(nm_color_gutter(NM_COLOR_16), "-/- ");
    char *prompt = span_bytes(nm_color_prompt(NM_COLOR_16), "\xe2\x9d\xaf ");
    char *rule =
        status_row_bytes("\xc2\xb7 -/- ", IDENT("ollama:local", "test-model"));
    ASSERT_NOT_NULL(glyph);
    ASSERT_NOT_NULL(gauge);
    ASSERT_NOT_NULL(prompt);
    ASSERT_NOT_NULL(rule);
    ASSERT_TRUE(strstr(frame, glyph) != NULL);
    ASSERT_TRUE(strstr(frame, gauge) != NULL);
    ASSERT_TRUE(strstr(frame, prompt) != NULL);
    /* No busy label anywhere in the paint: the glyph is the whole
     * activity readout. */
    ASSERT_TRUE(strstr(frame, "thinking") == NULL);
    /* The order: [glyph][gauge][rule][identity] on the status row — the
     * gauge is the row's one fixed landmark (only the constant-width
     * glyph sits left of it), the rule fills the row out to the terminal
     * width, and the identity closes it. */
    char joined[512];
    snprintf(joined, sizeof(joined), "%s%s", glyph, gauge);
    ASSERT_TRUE(strstr(frame, joined) != NULL);
    ASSERT_TRUE(strstr(frame, rule) != NULL);
    /* The row is exactly the terminal width and the identity's last
     * column is the row's last column: the right edge is flush. */
    char ident_text[128];
    snprintf(ident_text, sizeof(ident_text), " %s",
             IDENT("ollama:local", "test-model"));
    char *identity = span_bytes(nm_color_status_identity(NM_COLOR_16), ident_text);
    ASSERT_NOT_NULL(identity);
    ASSERT_TRUE(strstr(frame, identity) != NULL);
    free(identity);
    /* The prompt opens the NEXT row — the status row is a separator, so
     * the prompt never shares it. The composer emits the row, then its
     * own "\r\n"; the input's leading "\r" + EL clears the row it lands
     * on (the separator the input used to emit itself). */
    char next_row[512];
    snprintf(next_row, sizeof(next_row), "\r\n\r\033[K%s", prompt);
    ASSERT_TRUE(strstr(frame, next_row) != NULL);
    free(glyph);
    free(gauge);
    free(prompt);
    free(rule);
    /* And once the tail grows, the tail row is frame row 0 too —
     * the first tail row renders where the spinner was, and the
     * spinner moves below it (no blank row in between). */
    /* (covered structurally: tail row 0 emits "\r" + EL, spinner
     * separated by exactly one "\r\n" — see render_tail_rows) */

    harness_free(h);
    /* Teardown cancels the agent; the runtime free closes the socket
     * and releases the server's stall loop (its `sc` is on this
     * frame). Join so the thread never outlives it. */
    pthread_join(th, NULL);
    close(sc.fd);
}

/* The dot on a TRUECOLOR terminal: the glyph's color is the pulse's,
 * frame by frame — it starts at the trough (the terminal background,
 * so the dot fades out of the background) and reaches the peak (Dracula
 * Green) one cycle's midpoint later, at the 100 ms cadence. The tier
 * comes from the terminal profile's env-derived hint, which is what
 * harness_new_rgb pins. */
static void test_dot_pulses_on_a_truecolor_terminal(void)
{
    struct ServerScript sc;
    memset(&sc, 0, sizeof(sc));
    sc.n_rounds = 1;
    sc.sse[0] = ""; /* stalling round: the app sits in STREAMING */
    sc.stall_at_end = 1;
    sc.fd = server_bind(&sc.port);
    ASSERT_TRUE(sc.fd >= 0);
    pthread_t th;
    pthread_create(&th, NULL, chat_server_thread, &sc);

    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1", sc.port);
    AppHarness *h = harness_new_rgb("ollama:local", "test-model", base);
    ASSERT_NOT_NULL(h);

    harness_type(h, "hello");
    harness_enter(h);
    harness_single_step(h);
    ASSERT_EQ(nm_chat_app_state(h->app), NM_AGENT_STREAMING);

    /* Tick 0: the ramp's first glyph (·) at the pulse's trough (the
     * terminal background the palette assumes — the dot is fading in
     * from it). */
    nm_chat_app_tick(h->app);
    tui_runtime_flush(h->rt);
    const char *frame = tui_runtime_render(h->rt);
    ASSERT_NOT_NULL(frame);
    char *low = span_bytes(tui_color_rgb(40, 42, 54), "\xc2\xb7 ");
    ASSERT_NOT_NULL(low);
    ASSERT_TRUE(strstr(frame, low) != NULL);
    free(low);

    /* Eight ticks on: the peak — the palette's Green #50FA7B, on the
     * ramp's big dot (step 2 of the ping-pong). */
    for (int i = 0; i < 8; i++)
        nm_chat_app_tick(h->app);
    tui_runtime_flush(h->rt);
    frame = tui_runtime_render(h->rt);
    ASSERT_NOT_NULL(frame);
    char *peak = span_bytes(tui_color_rgb(80, 250, 123), "\xe2\x97\x8f ");
    ASSERT_NOT_NULL(peak);
    ASSERT_TRUE(strstr(frame, peak) != NULL);
    free(peak);

    /* The cadence is the style's, and the agent's own deadline is the
     * only thing that may shorten it. */
    ASSERT_EQ(nm_chat_app_tick_ms(h->app), 100);

    harness_free(h);
    pthread_join(th, NULL);
    close(sc.fd);
}

/* The handoff (the Enter -> repaint lag): the beat that submits paints
 * the EMPTIED composer and the thinking row, before a step and before a
 * tick. Starting a turn resolves the provider's host on the UI thread
 * (the transport's connect walk needs the address list before it can arm
 * a non-blocking connect), so on a slow name service the work between the
 * keypress and the turn's first frame is hundreds of milliseconds — and
 * what the user watched for that long was their own line, still sitting
 * in the composer. The paint must therefore happen before the send does
 * any work, not after.
 *
 * The turn's first tick then hands back the SAME frame the handoff
 * painted: declaring busy is a read of the animation, not an advance of
 * its phase, so the paint cannot steal the pulse's first beat. */
static void test_submit_paints_the_handoff_frame(void)
{
    struct ServerScript sc;
    memset(&sc, 0, sizeof(sc));
    sc.n_rounds = 1;
    sc.sse[0] = ""; /* stalling: the turn sits in the thinking phase */
    sc.stall_at_end = 1;
    sc.fd = server_bind(&sc.port);
    ASSERT_TRUE(sc.fd >= 0);
    pthread_t th;
    pthread_create(&th, NULL, chat_server_thread, &sc);

    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1", sc.port);
    AppHarness *h = harness_new_rgb("ollama:local", "test-model", base);
    ASSERT_NOT_NULL(h);

    harness_type(h, "hello");
    harness_enter(h); /* no step, no tick: the submit's own beat only */

    /* The send did start the turn (this is the beat that does it). */
    ASSERT_EQ(nm_chat_app_state(h->app), NM_AGENT_STREAMING);

    const char *frame = tui_runtime_render(h->rt);
    ASSERT_NOT_NULL(frame);
    /* The composer is empty: the sent line left the live region — it is
     * in the scrollback, echoed once by finish_inline. */
    ASSERT_TRUE(strstr(frame, "hello") == NULL);
    /* The status row already says thinking, in the pulse's own first
     * frame: the trough the dot fades in from. */
    char *trough = span_bytes(tui_color_rgb(40, 42, 54), "\xc2\xb7 ");
    ASSERT_NOT_NULL(trough);
    ASSERT_TRUE(strstr(frame, trough) != NULL);

    /* And the animation is where a turn's animation starts: the tick
     * that follows paints the same frame, because the handoff read it
     * rather than ticking it. */
    nm_chat_app_tick(h->app);
    tui_runtime_flush(h->rt);
    frame = tui_runtime_render(h->rt);
    ASSERT_NOT_NULL(frame);
    ASSERT_TRUE(strstr(frame, trough) != NULL);
    free(trough);

    harness_free(h);
    pthread_join(th, NULL);
    close(sc.fd);
}

/* The dot on a 16-COLOR terminal: no pulse — the bright ANSI16 green
 * holds — and the BLINK carries the animation. The dark phase declares
 * its two columns as spaces rather than dropping the segment, so the
 * row behind the glyph does not move: the same rule length, the same
 * identity, one column of content swapped. */
static void test_dot_blinks_and_holds_its_column_on_16_colors(void)
{
    struct ServerScript sc;
    memset(&sc, 0, sizeof(sc));
    sc.n_rounds = 1;
    sc.sse[0] = "";
    sc.stall_at_end = 1;
    sc.fd = server_bind(&sc.port);
    ASSERT_TRUE(sc.fd >= 0);
    pthread_t th;
    pthread_create(&th, NULL, chat_server_thread, &sc);

    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1", sc.port);
    AppHarness *h = harness_new("ollama:local", "test-model", base);
    ASSERT_NOT_NULL(h);

    harness_type(h, "hello");
    harness_enter(h);
    harness_single_step(h);
    ASSERT_EQ(nm_chat_app_state(h->app), NM_AGENT_STREAMING);

    /* The lit phase (tick 0): the dot, in the bright green. */
    nm_chat_app_tick(h->app);
    tui_runtime_flush(h->rt);
    const char *frame = tui_runtime_render(h->rt);
    ASSERT_NOT_NULL(frame);
    char *lit = span_bytes(tui_color_ansi(10), "\xc2\xb7 ");
    ASSERT_NOT_NULL(lit);
    ASSERT_TRUE(strstr(frame, lit) != NULL);
    free(lit);

    /* Ten ticks on: the dark phase — the glyph's two columns are spaces
     * in the same color, and the row's LAYOUT is byte-identical to the
     * lit one (same left chrome width, so the same rule). */
    for (int i = 0; i < 10; i++)
        nm_chat_app_tick(h->app);
    tui_runtime_flush(h->rt);
    frame = tui_runtime_render(h->rt);
    ASSERT_NOT_NULL(frame);
    char *dark = span_bytes(tui_color_ansi(10), "  ");
    char *dark_row =
        status_row_bytes("  -/- ", IDENT("ollama:local", "test-model"));
    char *lit_glyph = span_bytes(tui_color_ansi(10), "\xc2\xb7 ");
    ASSERT_NOT_NULL(dark);
    ASSERT_NOT_NULL(dark_row);
    ASSERT_NOT_NULL(lit_glyph);
    ASSERT_TRUE(strstr(frame, dark) != NULL);
    ASSERT_TRUE(strstr(frame, dark_row) != NULL);
    ASSERT_TRUE(strstr(frame, lit_glyph) == NULL); /* the dot is dark */
    free(dark);
    free(dark_row);
    free(lit_glyph);

    /* The 16-color tier still ticks at the dot's cadence (the pulse and
     * the blink share the style's clock). */
    ASSERT_EQ(nm_chat_app_tick_ms(h->app), 100);

    harness_free(h);
    pthread_join(th, NULL);
    close(sc.fd);
}

/* The spinner stops the moment the round completes (DONE), and the
 * status row must go glyphless then — not hold the last animated glyph
 * until the next turn starts. The idle cadence (nm_chat_app_tick_ms) is
 * -1 once the turn ends, so the loop never ticks again to refresh the
 * cached frame; the DONE transition clears it (the invariant
 * compose_status reads: glyphless when nothing runs). */
static void test_spinner_clears_when_round_completes(void)
{
    struct ServerScript sc;
    memset(&sc, 0, sizeof(sc));
    sc.n_rounds = 1;
    sc.sse[0] =
        "data: {\"choices\":[{\"delta\":{\"content\":\"ok\"}}]}\n\n"
        "data: [DONE]\n\n";
    sc.fd = server_bind(&sc.port);
    ASSERT_TRUE(sc.fd >= 0);
    pthread_t th;
    pthread_create(&th, NULL, chat_server_thread, &sc);

    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1", sc.port);
    AppHarness *h = harness_new("ollama:local", "test-model", base);
    ASSERT_NOT_NULL(h);

    harness_type(h, "hi");
    harness_enter(h);
    ASSERT_EQ(nm_chat_app_state(h->app), NM_AGENT_STREAMING);

    /* Tick the spinner once mid-stream: an animated glyph is now cached
     * in the frame (the very condition the bug leaves lingering). */
    nm_chat_app_tick(h->app);
    tui_runtime_flush(h->rt);
    const char *frame = tui_runtime_render(h->rt);
    ASSERT_NOT_NULL(frame);
    char *glyph = span_bytes(tui_color_ansi(10), "\xc2\xb7 "); /* · */
    ASSERT_NOT_NULL(glyph);
    ASSERT_TRUE(strstr(frame, glyph) != NULL);
    free(glyph);

    /* Drive the round to completion. harness_drive steps + flushes but
     * never ticks, so the only thing that can clear the cached glyph is
     * the DONE transition itself — exactly what the fix does. */
    ASSERT_EQ(harness_drive(h, 500), 0);
    ASSERT_EQ(nm_chat_app_state(h->app), NM_AGENT_DONE);
    tui_runtime_flush(h->rt);
    frame = tui_runtime_render(h->rt);
    ASSERT_NOT_NULL(frame);

    /* The status row is glyphless now: the gauge, the rule and the
     * identity stand without a spinner (the idle-row layout), and the
     * glyph that was animated above is gone. */
    char *gauge = span_bytes(nm_color_gutter(NM_COLOR_16), "-/- ");
    char *rule = status_row_bytes("-/- ", IDENT("ollama:local", "test-model"));
    char *dead = span_bytes(tui_color_ansi(10), "\xc2\xb7 ");
    ASSERT_NOT_NULL(gauge);
    ASSERT_NOT_NULL(rule);
    ASSERT_NOT_NULL(dead);
    char joined[512];
    snprintf(joined, sizeof(joined), "%s%s", gauge, rule);
    ASSERT_TRUE(strstr(frame, joined) != NULL);
    ASSERT_TRUE(strstr(frame, dead) == NULL);
    free(gauge);
    free(rule);
    free(dead);

    harness_free(h);
    pthread_join(th, NULL);
    close(sc.fd);
}

/* The ramp glyphs are one display cell each in boba's width table — the
 * status row's layout and the cursor arithmetic rest on that, and a
 * table change (or a glyph swap for an East-Asian-Wide one) must be
 * caught here rather than as a drifting row. */
static void test_spinner_glyphs_are_one_cell(void)
{
    ASSERT_EQ(tui_utf8_display_width("\xe2\x97\x8f"), 1); /* ● U+25CF */
    ASSERT_EQ(tui_utf8_display_width("\xe2\x80\xa2"), 1); /* • U+2022 */
    ASSERT_EQ(tui_utf8_display_width("\xc2\xb7"), 1);     /* · U+00B7 */
}

static void test_streaming_frame_shows_tail_and_spinner(void)
{
    struct ServerScript sc;
    memset(&sc, 0, sizeof(sc));
    sc.n_rounds = 1;
    /* No [DONE] and a held connection: the stream stays open mid-turn,
     * so the app has live tail content on the frame. */
    sc.sse[0] =
        "data: {\"choices\":[{\"delta\":{\"content\":\"streaming tail "
        "without newline yet\"}}]}\n\n";
    sc.stall_at_end = 1;
    sc.fd = server_bind(&sc.port);
    ASSERT_TRUE(sc.fd >= 0);
    pthread_t th;
    pthread_create(&th, NULL, chat_server_thread, &sc);

    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1", sc.port);
    AppHarness *h = harness_new("ollama:local", "test-model", base);
    ASSERT_NOT_NULL(h);
    /* The startup preflight note (a keyed provider with no key) is a
     * committed unit from BEFORE the stream: baseline it. */
    unsigned committed0 =
        tui_transcript_commit_count(nm_chat_app_transcript(h->app));

    harness_type(h, "go");
    harness_enter(h);
    harness_step_once(h);
    nm_chat_app_tick(h->app); /* advance the spinner (the run loop's tick) */
    tui_runtime_flush(h->rt);

    /* Mid-stream: state STREAMING, the tail is LIVE-REGION content
     * (frame) and the INPUT ROW is rendered too — the submitted text is
     * cleared, but the status line + prompt are always there (R1). */
    ASSERT_EQ(nm_chat_app_state(h->app), NM_AGENT_STREAMING);
    const char *frame = tui_runtime_render(h->rt);
    ASSERT_NOT_NULL(frame);
    ASSERT_TRUE(strstr(frame, "streaming tail") != NULL);
    ASSERT_TRUE(strstr(frame, "go") == NULL); /* submitted text cleared */
    char *prompt = span_bytes(nm_color_prompt(NM_COLOR_16), "\xe2\x9d\xaf ");
    char *gauge = span_bytes(nm_color_gutter(NM_COLOR_16), "-/- ");
    ASSERT_NOT_NULL(prompt);
    ASSERT_NOT_NULL(gauge);
    ASSERT_TRUE(strstr(frame, prompt) != NULL); /* the input row is here */
    ASSERT_TRUE(strstr(frame, gauge) != NULL);
    free(prompt);
    free(gauge);
    /* A spinner glyph is on the frame, painted in its own color: the
     * 16-color tier's bright green (this harness pins the tier). */
    char *dot = span_bytes(tui_color_ansi(10), "\xc2\xb7 ");
    ASSERT_NOT_NULL(dot);
    ASSERT_TRUE(strstr(frame, dot) != NULL);
    free(dot);

    /* The tail is live-region content: nothing has committed to the
     * scrollback since the turn began. This is a real state assertion
     * (the transcript's own commit count), not a byte-scan proxy. */
    ASSERT_EQ(tui_transcript_commit_count(nm_chat_app_transcript(h->app)),
              committed0);
    const char *out = harness_read(h);
    ASSERT_TRUE(strstr(out, "\r\nstreaming tail without newline yet\r\n") == NULL);

    harness_free(h);
    /* The app was cancelled by teardown: the runtime free tears the
     * socket down, which releases the server's stall loop. Join so
     * the thread never outlives this frame (its `sc` is on it). */
    pthread_join(th, NULL);
    close(sc.fd);
}

/* ---------------------------------------------------------------- */
/* Context gauge (P2): provider-reported usage in the status line    */
/* ---------------------------------------------------------------- */

/* The output marker (U+2193) is ONE CELL under every width model — that
 * is the whole reason it is a plain arrow and not a VS15 cluster like the
 * ⚡ rate beside it. Pin the measurement, because the row's column math
 * (the rule's fill, the identity's right edge) is built on it: a boba
 * regression that widened the arrow would leave the gauge's bytes
 * unchanged and shift every cell after it.
 *
 * Measured against all three models EXPLICITLY, never the ambient one: a
 * dev box and a CI runner resolve the ambient model differently, so the
 * zero-suffix call would be a coin flip (the same reason the ⚡ test names
 * its model). */
static void test_output_marker_is_one_cell_under_every_model(void)
{
    ASSERT_EQ(
        tui_utf8_display_width_model("\xe2\x86\x93", TUI_WIDTH_MODEL_GRID), 1);
    ASSERT_EQ(
        tui_utf8_display_width_model("\xe2\x86\x93", TUI_WIDTH_MODEL_WCWIDTH),
        1);
    ASSERT_EQ(
        tui_utf8_display_width_model("\xe2\x86\x93", TUI_WIDTH_MODEL_UTS51), 1);
    /* And the ⚡ cluster it sits beside is the contrast that makes the
     * point: two cells under the grid rule without its selector. */
    ASSERT_EQ(tui_utf8_display_width_model("\xe2\x9a\xa1", TUI_WIDTH_MODEL_GRID),
              2);
}

/* The idle frame carries the gauge, the rule and the identity (Q4) and it
 * says "unknown" honestly: no usage reported yet, and the catalog carries
 * no window for this model. */
static void test_context_gauge_unknown_reads_as_dash(void)
{
    AppHarness *h = harness_new("ollama:local", "test-model", NULL);
    ASSERT_NOT_NULL(h);

    const char *frame = tui_runtime_render(h->rt);
    ASSERT_NOT_NULL(frame);
    char *gauge = span_bytes(nm_color_gutter(NM_COLOR_16), "-/- ");
    char *prompt = span_bytes(nm_color_prompt(NM_COLOR_16), "\xe2\x9d\xaf ");
    char *rule = status_row_bytes("-/- ", IDENT("ollama:local", "test-model"));
    ASSERT_NOT_NULL(gauge);
    ASSERT_NOT_NULL(prompt);
    ASSERT_NOT_NULL(rule);
    /* The idle status row is the gauge, the rule and the identity, with no
     * slot and no busy chrome (Q4); the prompt opens the NEXT row, so the
     * gauge's width never moves it. */
    char joined[512];
    snprintf(joined, sizeof(joined), "%s%s", gauge, rule);
    ASSERT_TRUE(strstr(frame, joined) != NULL);
    char next_row[512];
    snprintf(next_row, sizeof(next_row), "\r\n\r\033[K%s", prompt);
    ASSERT_TRUE(strstr(frame, next_row) != NULL);
    free(gauge);
    free(prompt);
    free(rule);

    /* /context spells the same state out. */
    harness_type(h, "/context");
    harness_enter(h);
    const char *out = harness_read(h);
    ASSERT_TRUE(strstr(out, "context: limit unknown") != NULL);
    ASSERT_TRUE(strstr(out, "context: no usage reported by the provider "
                            "yet") != NULL);

    harness_free(h);
}

/* A multi-row input keeps the status line on ONE row and aligns
 * continuation rows under the prompt's text column. The status chrome
 * (spinner + gauge + label) is status about the TURN, not per visual
 * row: it never repeats down a wrapped input, and since it is a row of
 * its own, the continuation column is the prompt's width alone. */
static void test_multiline_input_continuation_aligns_under_prompt(void)
{
    AppHarness *h = harness_new("ollama:local", "test-model", NULL);
    ASSERT_NOT_NULL(h);

    harness_type(h, "hello");
    /* Shift+Enter inserts a newline (multiline input). */
    tui_runtime_send(h->rt, tui_msg_key(TUI_KEY_ENTER, 0, TUI_MOD_SHIFT));
    harness_type(h, "world");

    const char *frame = tui_runtime_render(h->rt);
    ASSERT_NOT_NULL(frame);

    /* The status row (gauge + rule) appears exactly once. */
    char *gauge = span_bytes(nm_color_gutter(NM_COLOR_16), "-/- ");
    ASSERT_NOT_NULL(gauge);
    const char *seed = strstr(frame, gauge);
    ASSERT_NOT_NULL(seed);
    ASSERT_TRUE(strstr(seed + 1, gauge) == NULL);

    /* The prompt opens its own row; the continuation row indents by
     * exactly the prompt's width, so its text aligns under the first
     * row's text. */
    char *prompt = span_bytes(nm_color_prompt(NM_COLOR_16), "\xe2\x9d\xaf ");
    char *cont = span_bytes(nm_color_prompt(NM_COLOR_16), "  ");
    ASSERT_NOT_NULL(prompt);
    ASSERT_NOT_NULL(cont);
    char row1[512], row2[512];
    snprintf(row1, sizeof(row1), "%shello", prompt);
    snprintf(row2, sizeof(row2), "%sworld", cont);
    ASSERT_TRUE(strstr(frame, row1) != NULL);
    ASSERT_TRUE(strstr(frame, row2) != NULL);

    free(gauge);
    free(prompt);
    free(cont);
    harness_free(h);
}

/* The keyboard protocol tier is the terminal's answer, resolved at the
 * point of use: `auto` declares the protocol's flag 1 only once the
 * probe says the terminal speaks it, and the declaration is what makes
 * the runtime push it. Before the answer there is nothing to declare —
 * which changes nothing, since a terminal sends the legacy encodings
 * until we push. */
static void test_kbd_tier_follows_the_terminal(void)
{
    AppHarness *h = harness_new("ollama:local", "test-model", NULL);
    ASSERT_NOT_NULL(h);
    /* Started like the real loop (tui_runtime_run), so the stop path
     * that pops the flags is live. */
    tui_runtime_start(h->rt);

    /* The probe has not resolved: no push. (The tmpfile terminal never
     * answers; the real loop's deadline is what resolves it.) */
    tui_runtime_flush(h->rt);
    const char *frame = harness_read(h);
    ASSERT_TRUE(strstr(frame, "\033[?u") != NULL);   /* asked */
    ASSERT_TRUE(strstr(frame, "\033[>25u") == NULL); /* nothing declared */

    /* The terminal answers: it speaks the protocol, so the full tier is
     * declared (1 | 8 | 16 = 25). */
    h->rt->probe_state = 3;
    h->rt->profile.resolved = 1;
    h->rt->profile.kbd_protocol = 1;
    tui_runtime_flush(h->rt);
    frame = harness_read(h);
    ASSERT_TRUE(strstr(frame, "\033[>25u") != NULL);

    /* And the flags are popped at stop, so the shell does not inherit
     * them. */
    tui_runtime_stop(h->rt);
    frame = harness_read(h);
    ASSERT_TRUE(strstr(frame, "\033[<u") != NULL);

    harness_free(h);
}

/* `kbd = off` is the escape hatch and it wins over the terminal's
 * answer: a terminal whose implementation misbehaves gets the legacy
 * encodings, never the push. `kbd = on` is the other override — declare
 * it without waiting for a probe that a late-answering terminal never
 * gives. */
static void test_kbd_mode_overrides_the_probe(void)
{
    AppHarness *h = harness_new("ollama:local", "test-model", NULL);
    ASSERT_NOT_NULL(h);
    NmConfig *cfg = cfg_for(h);
    ASSERT_NOT_NULL(cfg);
    ASSERT_EQ(nm_config_runtime_set(cfg, NM_CFG_KEY_KBD, "off"), 0);

    h->rt->probe_state = 3;
    h->rt->profile.resolved = 1;
    h->rt->profile.kbd_protocol = 1;
    tui_runtime_flush(h->rt);
    ASSERT_TRUE(strstr(harness_read(h), "\033[>25u") == NULL);

    ASSERT_EQ(nm_config_runtime_set(cfg, NM_CFG_KEY_KBD, "on"), 0);
    tui_runtime_flush(h->rt);
    ASSERT_TRUE(strstr(harness_read(h), "\033[>25u") != NULL);

    nm_config_free(cfg);
    harness_free(h);
}

/* Shift+Enter is the newline key once the full tier is declared — the
 * terminal sends it as CSI 13;2u, which the parser turns into ENTER +
 * SHIFT — and a capital that arrives with its reported text (CSI
 * 97;2;65u) lands as the capital. */
static void test_kbd_full_tier_gives_shift_enter(void)
{
    AppHarness *h = harness_new("ollama:local", "test-model", NULL);
    ASSERT_NOT_NULL(h);
    h->rt->probe_state = 3;
    h->rt->profile.resolved = 1;
    h->rt->profile.kbd_protocol = 1;
    tui_runtime_flush(h->rt); /* declares the tier */
    /* The tier this test is about: flags 1|8|16 pushed (8 is what makes
     * Shift+Enter tellable from Enter). */
    ASSERT_TRUE(strstr(harness_read(h), "\033[>25u") != NULL);

    harness_type(h, "one");
    /* Shift+Enter: a newline, not a submit. */
    tui_runtime_send(h->rt, tui_msg_key(TUI_KEY_ENTER, 0, TUI_MOD_SHIFT));
    harness_type(h, "two");
    ASSERT_STR_EQ(tui_textinput_text(nm_chat_app_textinput(h->app)),
                  "one\ntwo");
    ASSERT_EQ(nm_chat_app_state(h->app), NM_AGENT_IDLE);

    /* A text key that carries its text (the terminal's associated
     * text) inserts what the key produced. */
    TuiMsg cap = tui_msg_key(TUI_KEY_NONE, 'A', TUI_MOD_SHIFT);
    cap.data.key.text[0] = 'A';
    cap.data.key.text_len = 1;
    tui_runtime_send(h->rt, cap);
    ASSERT_STR_EQ(tui_textinput_text(nm_chat_app_textinput(h->app)),
                  "one\ntwoA");

    harness_free(h);
}

/* A lock modifier is not a modifier — it is keyboard STATE, and the
 * report-all flag makes the terminal put it on EVERY key event. The
 * user's own box has Num Lock on, so their kitty sends `CSI 97;133u`
 * for Ctrl+A and `CSI 13;130u` for Shift+Enter (128 = num_lock): a
 * decoder that knows only the values 2..8 reads those as NO modifiers,
 * which is "Ctrl+Shift does nothing" and "Shift+Enter submits". The
 * modifiers must survive the lock bits. */
static void test_kbd_wire_lock_modifiers_keep_the_modifiers(void)
{
    AppHarness *h = harness_new("ollama:local", "test-model", NULL);
    ASSERT_NOT_NULL(h);
    h->rt->probe_state = 3;
    h->rt->profile.resolved = 1;
    h->rt->profile.kbd_protocol = 1;
    tui_runtime_flush(h->rt); /* declares the tier */
    ASSERT_TRUE(strstr(harness_read(h), "\033[>25u") != NULL);

    harness_type(h, "xy");
    /* Shift+Enter with Num Lock on: a newline at the cursor, not a
     * submit. */
    harness_wire(h, "\033[13;130u");
    ASSERT_STR_EQ(tui_textinput_text(nm_chat_app_textinput(h->app)),
                  "xy\n");
    ASSERT_EQ(nm_chat_app_state(h->app), NM_AGENT_IDLE);

    /* Ctrl+A with Num Lock on: cursor to the start of the line, NOT a
     * typed 'a' — so the next character lands before "ab". */
    harness_type(h, "ab");
    harness_wire(h, "\033[97;133u");
    harness_type(h, "z");
    ASSERT_STR_EQ(tui_textinput_text(nm_chat_app_textinput(h->app)),
                  "xy\nzab");

    harness_free(h);
}

/* The declared tier's own traffic, fed as the WIRE BYTES the terminal
 * sends. Flag 8 reports the modifier keys THEMSELVES (a Shift press is
 * CSI 57441;2u) — a functional key, not text — and flag 16 reports a
 * text key's text beside its unshifted code. Both halves matter: the
 * Shift press must reach the input as NOTHING — before this the PUA
 * codepoint went in as text, so every capital arrived preceded by an
 * invisible character — and the capital must arrive exactly once. */
static void test_kbd_wire_shift_press_types_nothing(void)
{
    AppHarness *h = harness_new("ollama:local", "test-model", NULL);
    ASSERT_NOT_NULL(h);
    h->rt->probe_state = 3;
    h->rt->profile.resolved = 1;
    h->rt->profile.kbd_protocol = 1;
    tui_runtime_flush(h->rt); /* declares the tier */
    ASSERT_TRUE(strstr(harness_read(h), "\033[>25u") != NULL);

    harness_wire(h, "\033[57441;2u");   /* left shift down */
    harness_wire(h, "\033[72;2;72u");   /* H: key code 72, text "H" */
    harness_wire(h, "\033[57441;2:3u"); /* left shift up (a release) */
    harness_wire(h, "\033[105;1;105u"); /* i */
    ASSERT_STR_EQ(tui_textinput_text(nm_chat_app_textinput(h->app)), "Hi");

    /* And a grapheme arrives whole: 'e' + COMBINING ACUTE is ONE key
     * event carrying both codepoints in its text field. */
    harness_wire(h, "\033[101;1;101:769u");
    ASSERT_STR_EQ(tui_textinput_text(nm_chat_app_textinput(h->app)),
                  "Hie\xcc\x81");

    harness_free(h);
}

/* /config reports the terminal's ANSWER for `auto`: the store value
 * alone would not say whether the protocol is actually in use, which is
 * the one thing the row is for. */
static void test_config_reports_the_kbd_tier(void)
{
    AppHarness *h = harness_new("ollama:local", "test-model", NULL);
    ASSERT_NOT_NULL(h);
    NmConfig *cfg = cfg_for(h);
    ASSERT_NOT_NULL(cfg);

    harness_type(h, "/config");
    harness_enter(h);
    const char *out = harness_read(h);
    ASSERT_TRUE(strstr(out, "kbd") != NULL);
    ASSERT_TRUE(strstr(out, "terminal: legacy encodings") != NULL);
    size_t seen = strlen(out);

    /* The terminal answers: the same row now reports the protocol. */
    h->rt->probe_state = 3;
    h->rt->profile.resolved = 1;
    h->rt->profile.kbd_protocol = 1;
    harness_type(h, "/config");
    harness_enter(h);
    out = harness_read(h);
    ASSERT_TRUE(strstr(out + seen, "terminal: kitty protocol") != NULL);

    nm_config_free(cfg);
    harness_free(h);
}

/* A bracketed paste lands in the input as TEXT: every line ending a * terminal sends for the same copied text (CR, CRLF, LF) is one
 * newline, and no part of the payload submits. A terminal without
 * bracketed paste delivers the same bytes as Enter presses — which is
 * exactly the submit this must not be. */
static void test_paste_lands_as_text_with_newlines(void)
{
    AppHarness *h = harness_new("ollama:local", "test-model", NULL);
    ASSERT_NOT_NULL(h);

    harness_type(h, "before ");
    const char *payload = "one\rtwo\r\nthree\nfour";
    TuiMsg paste = tui_msg_paste(strdup(payload), strlen(payload));
    tui_runtime_send(h->rt, paste);
    tui_msg_free(&paste);

    ASSERT_STR_EQ(tui_textinput_text(nm_chat_app_textinput(h->app)),
                  "before one\ntwo\nthree\nfour");
    /* Nothing submitted: the whole block is still the prompt. */
    ASSERT_EQ(nm_chat_app_state(h->app), NM_AGENT_IDLE);

    harness_free(h);
}

/* A usage-carrying round fills the gauge from the provider's numbers —
 * used from the wire, limit from the catalog, cached from the
 * prompt_tokens_details breakdown — and colors it by how full the
 * window is. */
static void test_context_gauge_reports_usage_and_limit(void)
{
    struct ServerScript sc;
    memset(&sc, 0, sizeof(sc));
    sc.n_rounds = 1;
    /* Hyper rides usage on the finish_reason chunk or a standalone
     * choices:[] chunk; either way the agent keeps the LAST report. */
    sc.sse[0] =
        "data: {\"choices\":[{\"delta\":{\"content\":\"hello\"}}]}\n\n"
        "data: {\"choices\":[],\"usage\":{\"prompt_tokens\":12400,"
        "\"completion_tokens\":5,\"total_tokens\":12405,"
        "\"prompt_tokens_details\":{\"cached_tokens\":8100}}}\n\n"
        "data: [DONE]\n\n";
    sc.fd = server_bind(&sc.port);
    ASSERT_TRUE(sc.fd >= 0);
    pthread_t th;
    pthread_create(&th, NULL, chat_server_thread, &sc);

    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1", sc.port);
    /* hyper's static catalog carries gpt-oss-120b's window (131072), so
     * the denominator is real catalog metadata, not an estimate. */
    AppHarness *h = harness_new("hyper", "gpt-oss-120b", base);
    ASSERT_NOT_NULL(h);

    harness_type(h, "hi");
    harness_enter(h);
    ASSERT_EQ(harness_drive(h, 500), 0);
    ASSERT_EQ(nm_chat_app_state(h->app), NM_AGENT_DONE);

    /* The idle frame's gauge: 12.4k of 131k, then the SESSION's output
     * count (the one round's 5 completion tokens — the accumulated
     * field, read from the same accessor /context spells out), then the
     * cache-read rate (the one round's 8100 read / 12400 input =
     * 65.3 %). The rate marker is the small ⚡ — U+26A1 + VS15 (the
     * text-presentation request), so the assert pins the selector: a
     * bare ⚡ is a prefix of it and would slip back in unnoticed.
     *
     * The cluster measures ONE cell — that is the whole reason for the
     * selector, and the row's column math (the rule's fill, the
     * identity's right edge) is built on it. Pin the width here too: a
     * boba regression that re-widened the cluster would leave this
     * frame's bytes unchanged but shift every cell after the gauge.
     *
     * The width is measured against the UTS #51 rule EXPLICITLY, not
     * against the ambient model: the marker is written for the rule
     * (a VS15 cluster takes one cell where UTS #51 defines a text
     * glyph), but a terminal's own rule may disagree (xterm and
     * Windows Terminal keep the base's width, so they draw ⚡︎ two
     * cells wide). The zero-suffix call answers with whatever model is
     * in force — the process's, on a dev box, and the runner's, in CI —
     * so the assert would be a coin flip. The _model form is the same
     * measurement under a named rule. */
    ASSERT_EQ(tui_utf8_display_width_model("\xe2\x9a\xa1\xef\xb8\x8e",
                                           TUI_WIDTH_MODEL_UTS51),
              1);
    ASSERT_EQ(tui_utf8_display_width_model("\xe2\x9a\xa1",
                                           TUI_WIDTH_MODEL_UTS51),
              2);
    const char *frame = tui_runtime_render(h->rt);
    ASSERT_NOT_NULL(frame);
    char *gauge = gauge_bytes_rest("12.4k/131k ", "\xe2\x86\x93"
                                                  "5 ",
                                   "\xe2\x9a\xa1\xef\xb8\x8e"
                                   "65.3% ",
                                   NM_COLOR_16);
    ASSERT_NOT_NULL(gauge);
    ASSERT_TRUE(strstr(frame, gauge) != NULL);
    free(gauge);

    /* /context is the spelled-out breakdown, the per-round lines plus
     * the session accrual. */
    harness_type(h, "/context");
    harness_enter(h);
    const char *out = harness_read(h);
    ASSERT_TRUE(strstr(out, "context: limit 131,072 tokens (model "
                            "gpt-oss-120b)") != NULL);
    ASSERT_TRUE(strstr(out, "context: 12,400 / 131,072 tokens used "
                            "(9.5%)") != NULL);
    ASSERT_TRUE(strstr(out, "context: 8,100 tokens cached (65.3% of the "
                            "prompt)") != NULL);
    ASSERT_TRUE(strstr(out, "context: session 1 round, 12,400 input / 5 "
                            "output tokens") != NULL);
    ASSERT_TRUE(strstr(out, "context: session cache read 8,100 tokens "
                            "(65.3% of the 12,400 input)") != NULL);
    /* No write count on this shape: the line is absent. */
    ASSERT_TRUE(strstr(out, "session cache write") == NULL);

    harness_free(h);
    pthread_join(th, NULL);
    close(sc.fd);
}

/* The ⚡ rate is CUMULATIVE over the session, not a per-round number:
 * round 1 reports no cache fact at all (the gauge shows the counts alone), then
 * two read-reporting rounds move the rate to the session's own
 * read/base. A reported 0 counts as a miss (its input in the base). */
static void test_context_gauge_cache_rate_is_cumulative(void)
{
    struct ServerScript sc;
    memset(&sc, 0, sizeof(sc));
    sc.n_rounds = 3;
    /* No prompt_tokens_details: no read reported. */
    sc.sse[0] =
        "data: {\"choices\":[{\"delta\":{\"content\":\"a\"}}]}\n\n"
        "data: {\"choices\":[],\"usage\":{\"prompt_tokens\":8936,"
        "\"completion_tokens\":153,\"total_tokens\":9089}}\n\n"
        "data: [DONE]\n\n";
    sc.sse[1] =
        "data: {\"choices\":[{\"delta\":{\"content\":\"b\"}}]}\n\n"
        "data: {\"choices\":[],\"usage\":{\"prompt_tokens\":11322,"
        "\"completion_tokens\":164,\"total_tokens\":11486,"
        "\"prompt_tokens_details\":{\"cached_tokens\":9088,"
        "\"cache_write_tokens\":2048}}}\n\n"
        "data: [DONE]\n\n";
    sc.sse[2] =
        "data: {\"choices\":[{\"delta\":{\"content\":\"c\"}}]}\n\n"
        "data: {\"choices\":[],\"usage\":{\"prompt_tokens\":10000,"
        "\"completion_tokens\":100,\"total_tokens\":10100,"
        "\"prompt_tokens_details\":{\"cached_tokens\":0}}}\n\n"
        "data: [DONE]\n\n";
    sc.fd = server_bind(&sc.port);
    ASSERT_TRUE(sc.fd >= 0);
    pthread_t th;
    pthread_create(&th, NULL, chat_server_thread, &sc);

    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1", sc.port);
    /* hyper's static catalog carries gpt-oss-120b's window (131072), so
     * the denominator is real catalog metadata resolved at the point of
     * use — no push. */
    AppHarness *h = harness_new("hyper", "gpt-oss-120b", base);
    ASSERT_NOT_NULL(h);

    /* Round 1: usage, but no cache read ever reported => no ⚡ at all
     * (never a fabricated 0 %) and the session line says so. The ↓ count
     * IS there: this round reported output, which is what the marker
     * needs — the two fields' conditions are independent. */
    harness_type(h, "one");
    harness_enter(h);
    ASSERT_EQ(harness_drive(h, 500), 0);
    const char *frame = tui_runtime_render(h->rt);
    ASSERT_NOT_NULL(frame);
    char *gauge = gauge_bytes_rest("8.9k/131k ", "\xe2\x86\x93"
                                                 "153 ",
                                   NULL,
                                   NM_COLOR_16);
    ASSERT_NOT_NULL(gauge);
    ASSERT_TRUE(strstr(frame, gauge) != NULL);
    free(gauge);
    ASSERT_TRUE(strstr(frame, "\xe2\x9a\xa1\xef\xb8\x8e") == NULL);
    harness_type(h, "/context");
    harness_enter(h);
    const char *out = harness_read(h);
    ASSERT_TRUE(strstr(out, "context: session 1 round, 8,936 input / 153 "
                            "output tokens") != NULL);
    ASSERT_TRUE(strstr(out, "context: session cache: not reported") != NULL);

    /* Round 2: 9088 / 11322 = 80.3 %. */
    harness_type(h, "two");
    harness_enter(h);
    ASSERT_EQ(harness_drive(h, 500), 0);
    frame = tui_runtime_render(h->rt);
    ASSERT_NOT_NULL(frame);
    gauge = gauge_bytes_rest("11.3k/131k ", "\xe2\x86\x93"
                                            "317 ",
                             "\xe2\x9a\xa1\xef\xb8\x8e"
                             "80.3% ",
                             NM_COLOR_16);
    ASSERT_NOT_NULL(gauge);
    ASSERT_TRUE(strstr(frame, gauge) != NULL);
    free(gauge);

    /* Round 3: a miss (0 read) joins the base => 9088 / 21322 = 42.6 %,
     * cumulative, and the write count shows as an absolute number. */
    harness_type(h, "three");
    harness_enter(h);
    ASSERT_EQ(harness_drive(h, 500), 0);
    frame = tui_runtime_render(h->rt);
    ASSERT_NOT_NULL(frame);
    gauge = gauge_bytes_rest("10k/131k ", "\xe2\x86\x93"
                                          "417 ",
                             "\xe2\x9a\xa1\xef\xb8\x8e"
                             "42.6% ",
                             NM_COLOR_16);
    ASSERT_NOT_NULL(gauge);
    ASSERT_TRUE(strstr(frame, gauge) != NULL);
    free(gauge);
    harness_type(h, "/context");
    harness_enter(h);
    out = harness_read(h);
    ASSERT_TRUE(strstr(out, "context: session 3 rounds, 30,258 input / 417 "
                            "output tokens") != NULL);
    ASSERT_TRUE(strstr(out, "context: session cache read 9,088 tokens "
                            "(42.6% of the 21,322 input)") != NULL);
    ASSERT_TRUE(strstr(out, "context: session cache write 2,048 tokens") != NULL);

    harness_free(h);
    pthread_join(th, NULL);
    close(sc.fd);
}

/* The ↓ field appears only once a round has REPORTED output — and a
 * reported 0 is a report, not an unknown. The two halves of that rule are
 * one test because they are the same latch: a usage object that omits
 * completion_tokens leaves the ledger's output side unseen (no marker),
 * and one that carries a 0 latches it (marker, reading "0").
 *
 * The cache rate rides the same rounds and is INDEPENDENT: round 1
 * reports a read but no output, so the ⚡ is there while the ↓ is not —
 * which is what makes the pair a pair of conditions rather than one. */
static void test_context_gauge_output_marker_waits_for_a_report(void)
{
    struct ServerScript sc;
    memset(&sc, 0, sizeof(sc));
    sc.n_rounds = 2;
    /* Round 1: a read count, NO completion_tokens. */
    sc.sse[0] =
        "data: {\"choices\":[{\"delta\":{\"content\":\"a\"}}]}\n\n"
        "data: {\"choices\":[],\"usage\":{\"prompt_tokens\":1000,"
        "\"prompt_tokens_details\":{\"cached_tokens\":500}}}\n\n"
        "data: [DONE]\n\n";
    /* Round 2: output reported, and it is 0. */
    sc.sse[1] =
        "data: {\"choices\":[{\"delta\":{\"content\":\"b\"}}]}\n\n"
        "data: {\"choices\":[],\"usage\":{\"prompt_tokens\":2000,"
        "\"completion_tokens\":0,\"total_tokens\":2000}}\n\n"
        "data: [DONE]\n\n";
    sc.fd = server_bind(&sc.port);
    ASSERT_TRUE(sc.fd >= 0);
    pthread_t th;
    pthread_create(&th, NULL, chat_server_thread, &sc);

    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1", sc.port);
    AppHarness *h = harness_new("hyper", "gpt-oss-120b", base);
    ASSERT_NOT_NULL(h);

    /* Round 1: the counts and the ⚡ rate, and no ↓ anywhere in the paint
     * — the ledger has no output fact, and the row omits rather than
     * printing a fabricated "↓0". */
    harness_type(h, "one");
    harness_enter(h);
    ASSERT_EQ(harness_drive(h, 500), 0);
    const char *frame = tui_runtime_render(h->rt);
    ASSERT_NOT_NULL(frame);
    char *gauge = gauge_bytes_rest("1k/131k ", NULL,
                                   "\xe2\x9a\xa1\xef\xb8\x8e"
                                   "50.0% ",
                                   NM_COLOR_16);
    ASSERT_NOT_NULL(gauge);
    ASSERT_TRUE(strstr(frame, gauge) != NULL);
    free(gauge);
    ASSERT_TRUE(strstr(frame, "\xe2\x86\x93") == NULL);

    /* Round 2: completion_tokens 0 is a REPORT — the marker appears and
     * reads 0, while the rate is unmoved (round 2 reported no read, so it
     * joins neither cache operand). /context's exact spelling agrees on
     * the same accessor. */
    harness_type(h, "two");
    harness_enter(h);
    ASSERT_EQ(harness_drive(h, 500), 0);
    frame = tui_runtime_render(h->rt);
    ASSERT_NOT_NULL(frame);
    gauge = gauge_bytes_rest("2k/131k ", "\xe2\x86\x93"
                                         "0 ",
                             "\xe2\x9a\xa1\xef\xb8\x8e"
                             "50.0% ",
                             NM_COLOR_16);
    ASSERT_NOT_NULL(gauge);
    ASSERT_TRUE(strstr(frame, gauge) != NULL);
    free(gauge);
    harness_type(h, "/context");
    harness_enter(h);
    const char *out = harness_read(h);
    ASSERT_TRUE(strstr(out, "context: session 2 rounds, 3,000 input / 0 "
                            "output tokens") != NULL);

    harness_free(h);
    pthread_join(th, NULL);
    close(sc.fd);
}

/* The output marker's color is the TIER's business. A 16-color terminal
 * gets the spec's ANSI palette INDEX — a `36` SGR, never a 24-bit
 * sequence, which is the whole reason the tier exists — and a truecolor
 * one the spec's Cyan hex. The two are the same color under Dracula's
 * own terminal palette (the spec's AnsiCyan is #8BE9FD, Cyan's own
 * value), which is what makes the 16-color rendering an EXACT one rather
 * than a nearest-neighbour guess at a hex.
 *
 * Both tiers are driven through the real path — the same canned round on
 * a 16-color harness and then on a truecolor one — so this pins the
 * app's CHOICE of depth, not just the role's two spellings. */
static void test_output_marker_color_follows_the_terminal_tier(void)
{
    /* The role itself: one color, two spellings. */
    TuiColor rgb = nm_color_gauge_output(NM_COLOR_RGB);
    TuiColor ansi = nm_color_gauge_output(NM_COLOR_16);
    ASSERT_EQ(rgb.type, TUI_COLOR_RGB);
    ASSERT_EQ(rgb.v.rgb.r, NM_DRACULA_CYAN_R);
    ASSERT_EQ(rgb.v.rgb.g, NM_DRACULA_CYAN_G);
    ASSERT_EQ(rgb.v.rgb.b, NM_DRACULA_CYAN_B);
    ASSERT_EQ(ansi.type, TUI_COLOR_ANSI16);
    ASSERT_EQ(ansi.v.ansi, NM_DRACULA_ANSI_CYAN);
    ASSERT_EQ(NM_DRACULA_ANSI_CYAN, 6); /* AnsiCyan, the spec's slot */

    struct ServerScript sc;
    memset(&sc, 0, sizeof(sc));
    sc.n_rounds = 2; /* one turn per tier */
    for (int i = 0; i < 2; i++)
        sc.sse[i] =
            "data: {\"choices\":[{\"delta\":{\"content\":\"hi\"}}]}\n\n"
            "data: {\"choices\":[],\"usage\":{\"prompt_tokens\":1000,"
            "\"completion_tokens\":5,\"total_tokens\":1005,"
            "\"prompt_tokens_details\":{\"cached_tokens\":500}}}\n\n"
            "data: [DONE]\n\n";
    sc.fd = server_bind(&sc.port);
    ASSERT_TRUE(sc.fd >= 0);
    pthread_t th;
    pthread_create(&th, NULL, chat_server_thread, &sc);

    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1", sc.port);

    /* The 16-color tier: the palette index, and the 24-bit sequence
     * nowhere in the paint. */
    AppHarness *h16 = harness_new("hyper", "gpt-oss-120b", base);
    ASSERT_NOT_NULL(h16);
    harness_type(h16, "hi");
    harness_enter(h16);
    ASSERT_EQ(harness_drive(h16, 500), 0);
    const char *frame = tui_runtime_render(h16->rt);
    ASSERT_NOT_NULL(frame);
    char *gauge = gauge_bytes_rest("1k/131k ", "\xe2\x86\x93"
                                               "5 ",
                                   "\xe2\x9a\xa1\xef\xb8\x8e"
                                   "50.0% ",
                                   NM_COLOR_16);
    ASSERT_NOT_NULL(gauge);
    ASSERT_TRUE(strstr(frame, gauge) != NULL);
    free(gauge);
    ASSERT_TRUE(strstr(frame, "\033[36m") != NULL); /* the index's SGR */
    ASSERT_TRUE(strstr(frame, "\033[38;2;139;233;253m") == NULL);
    harness_free(h16);

    /* The truecolor tier: the spec's Cyan hex. */
    AppHarness *h32 = harness_new_rgb("hyper", "gpt-oss-120b", base);
    ASSERT_NOT_NULL(h32);
    harness_type(h32, "hi");
    harness_enter(h32);
    ASSERT_EQ(harness_drive(h32, 500), 0);
    frame = tui_runtime_render(h32->rt);
    ASSERT_NOT_NULL(frame);
    gauge = gauge_bytes_rest("1k/131k ", "\xe2\x86\x93"
                                         "5 ",
                             "\xe2\x9a\xa1\xef\xb8\x8e"
                             "50.0% ",
                             NM_COLOR_RGB);
    ASSERT_NOT_NULL(gauge);
    ASSERT_TRUE(strstr(frame, gauge) != NULL);
    free(gauge);
    ASSERT_TRUE(strstr(frame, "\033[38;2;139;233;253m") != NULL);
    harness_free(h32);

    pthread_join(th, NULL);
    close(sc.fd);
}

/* The gauge's buffer holds the WORST its fields can print, not the worst
 * seen in practice. A long rate needs a SMALL cache base while a long
 * count needs a LARGE prompt, so driving the row past its old 64 bytes
 * takes two rounds — and every value here is a power of two, which is
 * what makes the JSON numbers land on the long exactly (a decimal past
 * 2^53 is not representable as a double, so the parse would round it and
 * the expected string would be a guess).
 *
 * The property is the COMPLETE ⚡ cluster: a field is written whole or
 * not at all, so an undersized buffer DROPS the marker. A truncated one
 * would be worse — cut before the VS15 it is two cells wide, and the
 * row's column math would reserve the wrong columns.
 *
 * The worst case is a `long`'s worst case, and `long` is 32-BIT on
 * Windows (LLP64): there the three fields cannot reach 64 bytes at all,
 * so there is nothing to trap and the test returns early. The values
 * below are 2^61/2^62, which a 32-bit long cannot hold — casting them
 * would be UB, not a smaller number. */
static void test_context_gauge_survives_the_worst_case_counts(void)
{
    if (sizeof(long) < 8) {
        /* Say so out loud: this sub-test is vacuous here, and a
         * vacuous PASS that says nothing is how a platform difference
         * hides. */
        fprintf(stderr, "  (skip: a %zu-byte long cannot reach the "
                        "buffer's old bound)\n",
                sizeof(long));
        return;
    }

    struct ServerScript sc;
    memset(&sc, 0, sizeof(sc));
    sc.n_rounds = 2;
    /* Round 1: a 1-token prompt (the rate's denominator) with a huge
     * read, and half the output. */
    sc.sse[0] =
        "data: {\"choices\":[{\"delta\":{\"content\":\"a\"}}]}\n\n"
        "data: {\"choices\":[],\"usage\":{\"prompt_tokens\":1,"
        "\"completion_tokens\":2305843009213693952,"
        "\"prompt_tokens_details\":{\"cached_tokens\":4611686018427387904}"
        "}}\n\n"
        "data: [DONE]\n\n";
    /* Round 2: the huge prompt the gauge's used count shows (2^62), and
     * the other half of the output — the sum stays inside a long. */
    sc.sse[1] =
        "data: {\"choices\":[{\"delta\":{\"content\":\"b\"}}]}\n\n"
        "data: {\"choices\":[],\"usage\":{\"prompt_tokens\":4611686018427387904,"
        "\"completion_tokens\":2305843009213693952}}\n\n"
        "data: [DONE]\n\n";
    sc.fd = server_bind(&sc.port);
    ASSERT_TRUE(sc.fd >= 0);
    pthread_t th;
    pthread_create(&th, NULL, chat_server_thread, &sc);

    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1", sc.port);
    AppHarness *h = harness_new("hyper", "gpt-oss-120b", base);
    ASSERT_NOT_NULL(h);

    harness_type(h, "one");
    harness_enter(h);
    ASSERT_EQ(harness_drive(h, 500), 0);
    harness_type(h, "two");
    harness_enter(h);
    ASSERT_EQ(harness_drive(h, 500), 0);

    /* 16 + 1 + 4 + (1+3+16) + (1+6+24) + NUL = 74 bytes: the old 64-byte
     * buffer cannot hold it, and the ⚡ field would be refused. Tier 2
     * (past 95 % of a known window) is the colour of a used count this
     * far over the catalog's — and the tier's own arithmetic is a double,
     * so it does not overflow where a long multiply would. */
    const char *frame = tui_runtime_render(h->rt);
    ASSERT_NOT_NULL(frame);
    char *gauge = gauge_bytes(
        nm_color_gutter_warn_hot(NM_COLOR_16), "4611686018427.3M/131k ",
        "\xe2\x86\x93"
        "4611686018427.3M ",
        "\xe2\x9a\xa1\xef\xb8\x8e"
        "461168601842738790400.0% ",
        NM_COLOR_16);
    ASSERT_NOT_NULL(gauge);
    ASSERT_TRUE(strstr(frame, gauge) != NULL);
    free(gauge);

    harness_free(h);
    pthread_join(th, NULL);
    close(sc.fd);
}

/* The gauge's tier tracks how full the window is: Orange past ~85 % of a
 * KNOWN limit, Red past ~95 % (Comment at rest). The window is the
 * catalog's (hyper's gpt-oss-120b, 131072 — the same shipped row the two
 * gauge tests above resolve), so the tiers are exercised through the real
 * path, not a pushed fixture. */
static void test_context_gauge_warns_near_the_limit(void)
{
    struct ServerScript sc;
    memset(&sc, 0, sizeof(sc));
    sc.n_rounds = 2;
    sc.sse[0] =
        "data: {\"choices\":[{\"delta\":{\"content\":\"a\"}}]}\n\n"
        "data: {\"choices\":[],\"usage\":{\"prompt_tokens\":115000,"
        "\"completion_tokens\":1,\"total_tokens\":115001}}\n\n"
        "data: [DONE]\n\n";
    sc.sse[1] =
        "data: {\"choices\":[{\"delta\":{\"content\":\"b\"}}]}\n\n"
        "data: {\"choices\":[],\"usage\":{\"prompt_tokens\":126000,"
        "\"completion_tokens\":1,\"total_tokens\":126001}}\n\n"
        "data: [DONE]\n\n";
    sc.fd = server_bind(&sc.port);
    ASSERT_TRUE(sc.fd >= 0);
    pthread_t th;
    pthread_create(&th, NULL, chat_server_thread, &sc);

    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1", sc.port);
    AppHarness *h = harness_new("hyper", "gpt-oss-120b", base);
    ASSERT_NOT_NULL(h);

    harness_type(h, "one");
    harness_enter(h);
    ASSERT_EQ(harness_drive(h, 500), 0);
    const char *frame = tui_runtime_render(h->rt);
    ASSERT_NOT_NULL(frame);
    char *warn =
        gauge_bytes(nm_color_gutter_warn(NM_COLOR_16), "115k/131k ", "\xe2\x86\x93"
                                                                     "1 ",
                    NULL, NM_COLOR_16);
    ASSERT_NOT_NULL(warn);
    ASSERT_TRUE(strstr(frame, warn) != NULL);
    free(warn);

    harness_type(h, "two");
    harness_enter(h);
    ASSERT_EQ(harness_drive(h, 500), 0);
    frame = tui_runtime_render(h->rt);
    ASSERT_NOT_NULL(frame);
    char *hot = gauge_bytes(nm_color_gutter_warn_hot(NM_COLOR_16), "126k/131k ",
                            "\xe2\x86\x93"
                            "2 ",
                            NULL, NM_COLOR_16);
    ASSERT_NOT_NULL(hot);
    ASSERT_TRUE(strstr(frame, hot) != NULL);
    free(hot);

    harness_free(h);
    pthread_join(th, NULL);
    close(sc.fd);
}

/* ---------------------------------------------------------------- */
/* Status row identity: the right-aligned `provider · model` block   */
/* ---------------------------------------------------------------- */

/* The identity block's model follows /model, and the row it lands in is
 * re-declared (boba's change detection is on the declarations, so a new
 * model id is a new row). */
static void test_status_row_identity_follows_model(void)
{
    AppHarness *h = harness_new("ollama:local", "test-model", NULL);
    ASSERT_NOT_NULL(h);

    const char *frame = tui_runtime_render(h->rt);
    ASSERT_NOT_NULL(frame);
    char *before = span_bytes(nm_color_status_identity(NM_COLOR_16),
                              " " IDENT("ollama:local", "test-model"));
    ASSERT_NOT_NULL(before);
    ASSERT_TRUE(strstr(frame, before) != NULL);
    free(before);

    /* The exact-set form: this test is about the IDENTITY ROW changing,
     * not about catalog membership. (And not a prefix of the old id,
     * which would make the "old identity is gone" check below vacuous.) */
    harness_type(h, "/model ! other-model");
    harness_enter(h);
    ASSERT_STR_EQ(nm_chat_app_model(h->app), "other-model");

    frame = tui_runtime_render(h->rt);
    ASSERT_NOT_NULL(frame);
    char *after = span_bytes(nm_color_status_identity(NM_COLOR_16),
                             " " IDENT("ollama:local", "other-model"));
    ASSERT_NOT_NULL(after);
    ASSERT_TRUE(strstr(frame, after) != NULL);
    /* The old identity is gone (the declarations changed, not just the
     * text inside one of them). */
    ASSERT_TRUE(strstr(frame, IDENT("ollama:local", "test-model")) == NULL);
    free(after);

    harness_free(h);
}

/* A /provider switch re-resolves the model for the NEW provider: the
 * identity follows the provider, and a provider with no model memory
 * shows the honest "(no model)" rather than the previous provider's id
 * (the store is the per-provider model memory — see switch_provider). */
static void test_status_row_identity_follows_provider(void)
{
    pin_cfg_paths("statusrow-provider");
    AppHarness *h = harness_new("hyper", "gpt-oss-120b", NULL);
    ASSERT_NOT_NULL(h);
    NmConfig *cfg = cfg_for(h); /* an empty store: no model memory */

    harness_type(h, "/provider ollama:local");
    harness_enter(h);
    ASSERT_STR_EQ(nm_chat_app_provider(h->app), "ollama:local");

    const char *frame = tui_runtime_render(h->rt);
    ASSERT_NOT_NULL(frame);
    char *identity = span_bytes(nm_color_status_identity(NM_COLOR_16),
                                " " IDENT("ollama:local", "(no model)"));
    ASSERT_NOT_NULL(identity);
    ASSERT_TRUE(strstr(frame, identity) != NULL);
    /* The previous provider's id is nowhere on the row. */
    ASSERT_TRUE(strstr(frame, IDENT("hyper", "gpt-oss-120b")) == NULL);
    free(identity);

    nm_config_free(cfg);
    harness_free(h);
}

/* A model that was never set reads as the ONE "(no model)" spelling —
 * the row never shows an empty identity. */
static void test_status_row_identity_no_model(void)
{
    AppHarness *h = harness_new("ollama:local", NULL, NULL);
    ASSERT_NOT_NULL(h);
    ASSERT_NULL(nm_chat_app_model(h->app));

    const char *frame = tui_runtime_render(h->rt);
    ASSERT_NOT_NULL(frame);
    char *identity = span_bytes(nm_color_status_identity(NM_COLOR_16),
                                " " IDENT("ollama:local", "(no model)"));
    ASSERT_NOT_NULL(identity);
    ASSERT_TRUE(strstr(frame, identity) != NULL);
    free(identity);

    /* The label seam is the same spelling the notices use. */
    ASSERT_STR_EQ(nm_chat_app_model_label(h->app), "(no model)");

    harness_free(h);
}

/* A narrow terminal: boba elides the identity (head kept, `…`-marked)
 * before it touches the rule, and the chrome (the gauge) is never cut.
 * The row is still exactly the terminal width. */
static void test_status_row_identity_elides_on_narrow_terminal(void)
{
    /* A long, catalog-unknown id: the identity is 57 columns and the left
     * chrome 4 (the gauge reads "-/- ", no known window). */
    AppHarness *h = harness_new(
        "openrouter", "vendor/very-long-model-identifier-abcdefghij", NULL);
    ASSERT_NOT_NULL(h);
    tui_runtime_send(h->rt, tui_msg_window_size(46, 24));
    tui_runtime_drain(h->rt);
    tui_runtime_flush(h->rt);

    const char *frame = tui_runtime_render(h->rt);
    ASSERT_NOT_NULL(frame);
    char *prompt = span_bytes(nm_color_prompt(NM_COLOR_16), "\xe2\x9d\xaf ");
    ASSERT_NOT_NULL(prompt);
    char *row = frame_status_row(frame, prompt);
    ASSERT_NOT_NULL(row);
    ASSERT_EQ(tui_utf8_display_width_ansi(row, strlen(row)), 46u);

    /* The gauge is untouched (its own span, at full text). */
    char *gauge = span_bytes(nm_color_gutter(NM_COLOR_16), "-/- ");
    ASSERT_NOT_NULL(gauge);
    ASSERT_TRUE(strstr(row, gauge) != NULL);
    free(gauge);
    /* The rule survives (at least one column) ... */
    ASSERT_TRUE(strstr(row, "\xe2\x94\x80") != NULL);
    /* ... and the identity is elided, head kept and marked. */
    ASSERT_TRUE(strstr(row, "openrouter \xc2\xb7 vendor/very-long-") != NULL);
    ASSERT_TRUE(strstr(row, "\xe2\x80\xa6") != NULL);
    ASSERT_TRUE(strstr(row, "abcdefghij") == NULL);
    free(prompt);
    free(row);

    harness_free(h);
}

/* Narrower still: the identity gives everything, is dropped whole, and
 * the row falls back to [chrome][rule] — the separator contract is the
 * last thing standing. */
static void test_status_row_identity_dropped_when_short(void)
{
    AppHarness *h = harness_new(
        "openrouter", "vendor/very-long-model-identifier-abcdefghij", NULL);
    ASSERT_NOT_NULL(h);
    /* 6 columns: the 4-column gauge and a one-column rule, with no room
     * left for the identity's own one-column floor plus its pad. */
    tui_runtime_send(h->rt, tui_msg_window_size(6, 24));
    tui_runtime_drain(h->rt);
    tui_runtime_flush(h->rt);

    const char *frame = tui_runtime_render(h->rt);
    ASSERT_NOT_NULL(frame);
    char *prompt = span_bytes(nm_color_prompt(NM_COLOR_16), "\xe2\x9d\xaf ");
    ASSERT_NOT_NULL(prompt);
    char *row = frame_status_row(frame, prompt);
    ASSERT_NOT_NULL(row);
    ASSERT_EQ(tui_utf8_display_width_ansi(row, strlen(row)), 6u);
    ASSERT_TRUE(strstr(row, "openrouter") == NULL);
    ASSERT_TRUE(strstr(row, "vendor/") == NULL);
    /* No identity bytes at all — not even the elided head or the pad. */
    char *sgr = status_identity_sgr();
    ASSERT_NOT_NULL(sgr);
    ASSERT_TRUE(strstr(row, sgr) == NULL);
    free(sgr);
    /* The chrome is whole and the rule fills what is left. */
    char *gauge = span_bytes(nm_color_gutter(NM_COLOR_16), "-/- ");
    ASSERT_NOT_NULL(gauge);
    ASSERT_TRUE(strstr(row, gauge) != NULL);
    free(gauge);
    free(prompt);
    free(row);

    harness_free(h);
}

/* The composer owns the cursor's chrome offset: boba's textinput reports
 * its OWN rows (it no longer counts a status row), so the view declares
 * the input's row PLUS the chrome row it painted. The placement is
 * measured from the frame's END, and the chrome row sits above the
 * cursor, so the offset cancels in the emitted bytes — what this guards
 * is that the composed frame still places the cursor on the INPUT's row:
 * with a two-row input and the cursor moved back to the first, the frame
 * moves up exactly ONE row. Two would mean the chrome row was counted
 * twice; zero would mean the cursor was declared on the chrome row
 * (which is what forgetting the offset would do). */
static void test_status_row_cursor_lands_on_the_input_row(void)
{
    AppHarness *h = harness_new("ollama:local", "gpt-oss:20b", NULL);
    ASSERT_NOT_NULL(h);

    harness_type(h, "one");
    tui_runtime_send(h->rt, tui_msg_key(TUI_KEY_ENTER, 0, TUI_MOD_SHIFT));
    harness_type(h, "two");
    /* Up: the cursor leaves the second input row for the first. */
    tui_runtime_send(h->rt, tui_msg_key(TUI_KEY_UP, 0, 0));
    tui_runtime_drain(h->rt);
    tui_runtime_flush(h->rt);

    /* The cursor placement is the runtime's, not the frame buffer's: the
     * repaint brackets the frame with hide/show-cursor, so read the raw
     * output. */
    const char *out = harness_read(h);
    ASSERT_NOT_NULL(out);
    /* The placement is the runtime's, not the frame buffer's: the frame
     * is not bracketed by a hide/show pair (cursor visibility is
     * reconciled edge to edge, so a frame on a visible cursor emits no
     * visibility byte), so anchor on the LAST cursor-forward of a
     * visible-cursor frame — the placement's own column move. */
    const char *fwd = NULL;
    for (const char *p = strstr(out, "\x1b["); p;
         p = strstr(p + 2, "\x1b[")) {
        const char *q = p + 2;
        while (*q >= '0' && *q <= '9')
            q++;
        if (q > p + 2 && *q == 'C')
            fwd = p;
    }
    ASSERT_NOT_NULL(fwd);
    /* ...and the move that opens it is exactly ONE row up: two would
     * mean the chrome row was counted twice; zero would mean the cursor
     * was declared on the chrome row (which is what forgetting the
     * offset would do). */
    ASSERT_TRUE(fwd - out >= 5);
    ASSERT_TRUE(memcmp(fwd - 5, "\x1b[1A\r", 5) == 0);

    harness_free(h);
}

/* While a turn is in flight the input row is still there and still
 * gathers input: keys edit the buffer (R1), Enter is a silent no-op
 * (Q3), and Ctrl+C remains the interrupt. */
static void test_busy_input_gathers_type_ahead(void)
{
    struct ServerScript sc;
    memset(&sc, 0, sizeof(sc));
    sc.n_rounds = 1;
    sc.sse[0] = ""; /* stalling round: STREAMING with no payload */
    sc.stall_at_end = 1;
    sc.fd = server_bind(&sc.port);
    ASSERT_TRUE(sc.fd >= 0);
    pthread_t th;
    pthread_create(&th, NULL, chat_server_thread, &sc);

    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1", sc.port);
    AppHarness *h = harness_new("ollama:local", "test-model", base);
    ASSERT_NOT_NULL(h);

    harness_type(h, "first");
    harness_enter(h);
    harness_single_step(h);
    ASSERT_EQ(nm_chat_app_state(h->app), NM_AGENT_STREAMING);

    /* Keys edit the buffer mid-turn. */
    harness_type(h, "next");
    ASSERT_STR_EQ(tui_textinput_text(nm_chat_app_textinput(h->app)), "next");
    const char *frame = tui_runtime_render(h->rt);
    ASSERT_NOT_NULL(frame);
    ASSERT_TRUE(strstr(frame, "next") != NULL); /* painted in the input row */

    /* Enter is a silent no-op: no second turn starts, the text stays. */
    harness_enter(h);
    ASSERT_EQ(nm_chat_app_state(h->app), NM_AGENT_STREAMING);
    ASSERT_STR_EQ(tui_textinput_text(nm_chat_app_textinput(h->app)), "next");

    /* Ctrl+C stays the interrupt, and the typed-ahead text survives it. */
    tui_runtime_send(h->rt, tui_msg_interrupt());
    ASSERT_EQ(nm_chat_app_state(h->app), NM_AGENT_IDLE);
    ASSERT_STR_EQ(tui_textinput_text(nm_chat_app_textinput(h->app)), "next");

    harness_free(h);
    pthread_join(th, NULL);
    close(sc.fd);
}

static void test_quit_command_quits(void)
{

    AppHarness *h = harness_new("ollama:cloud", "gpt-oss:20b", NULL);
    ASSERT_NOT_NULL(h);

    harness_type(h, "/quit");
    harness_enter(h);

    ASSERT_TRUE(tui_runtime_should_quit(h->rt));
    /* The echoed command reached the scrollback. */
    ASSERT_TRUE(strstr(harness_read(h), "/quit") != NULL);

    harness_free(h);
}

static void test_model_command_sets_model(void)
{
    AppHarness *h = harness_new("ollama:cloud", "gpt-oss:20b", NULL);
    ASSERT_NOT_NULL(h);

    ASSERT_STR_EQ(nm_chat_app_model(h->app), "gpt-oss:20b");

    /* Arg form sets an exact catalog id. */
    harness_type(h, "/model gemma4:31b");
    harness_enter(h);
    ASSERT_STR_EQ(nm_chat_app_model(h->app), "gemma4:31b");
    ASSERT_TRUE(strstr(harness_read(h), "gemma4:31b") != NULL);

    /* A second identical submit does not wedge. */
    harness_type(h, "/model kimi-k3");
    harness_enter(h);
    ASSERT_STR_EQ(nm_chat_app_model(h->app), "kimi-k3");

    harness_free(h);
}

/* Bare /model opens the picker; the ACTIVE model is the first entry
 * and is pre-selected, so Enter-then-submit leaves it unchanged. */
static void test_model_picker_active_first(void)
{
    AppHarness *h = harness_new("ollama:cloud", "gemma4:31b", NULL);
    ASSERT_NOT_NULL(h);

    harness_type(h, "/model");
    harness_enter(h);
    const char *frame = tui_runtime_render(h->rt);
    ASSERT_TRUE(strstr(frame, "gemma4:31b") != NULL);
    ASSERT_TRUE(strstr(frame, "beam") != NULL);

    /* Enter COMPOSES the command, never applies it. */
    tui_runtime_send(h->rt, tui_msg_key(TUI_KEY_ENTER, 0, 0));
    const char *text = tui_textinput_text(nm_chat_app_textinput(h->app));
    ASSERT_NOT_NULL(text);
    ASSERT_STR_EQ(text, "/model gemma4:31b");
    /* The picker composes; the agent keeps the model until submit. */
    ASSERT_STR_EQ(nm_chat_app_model(h->app), "gemma4:31b");

    /* Submit re-applies the same model. */
    harness_enter(h);
    ASSERT_STR_EQ(nm_chat_app_model(h->app), "gemma4:31b");

    /* Escape during a later /model leaves the model alone. */
    harness_type(h, "/model");
    harness_enter(h);
    tui_runtime_send(h->rt, tui_msg_key(TUI_KEY_ESCAPE, 0, 0));
    ASSERT_STR_EQ(nm_chat_app_model(h->app), "gemma4:31b");
    ASSERT_EQ(nm_chat_app_state(h->app), NM_AGENT_IDLE);

    harness_free(h);
}

static void test_model_picker_selects_and_commits(void)
{
    AppHarness *h = harness_new("ollama:cloud", "gemma4:31b", NULL);
    ASSERT_NOT_NULL(h);

    /* Bare /model: active model first, catalog after. Down to the
     * next entry (the generated catalog's first row), Enter composes,
     * submit commits. */
    harness_type(h, "/model");
    harness_enter(h);
    tui_runtime_send(h->rt, tui_msg_key(TUI_KEY_DOWN, 0, 0));
    tui_runtime_send(h->rt, tui_msg_key(TUI_KEY_ENTER, 0, 0));
    ASSERT_STR_EQ(tui_textinput_text(nm_chat_app_textinput(h->app)),
                  "/model beam");
    ASSERT_STR_EQ(nm_chat_app_model(h->app), "gemma4:31b"); /* not yet */
    harness_enter(h);
    ASSERT_STR_EQ(nm_chat_app_model(h->app), "beam");
    ASSERT_TRUE(strstr(harness_read(h), "beam") != NULL);

    harness_free(h);
}

static void test_model_picker_pre_filters_by_query(void)
{
    AppHarness *h = harness_new("ollama:cloud", "gpt-oss:20b", NULL);
    ASSERT_NOT_NULL(h);

    /* /model with a query that is not an exact id: the popup opens
     * pre-filtered (boba's filter-as-view; the query is the stable
     * interface). "gpt" filters out gemma4:31b; the active model is
     * prepended and matches, so it stays first. */
    harness_type(h, "/model gpt-oss:120");
    harness_enter(h);
    const char *frame = tui_runtime_render(h->rt);
    ASSERT_TRUE(strstr(frame, "gpt-oss:120b") != NULL);
    ASSERT_TRUE(strstr(frame, "gemma4:31b") == NULL);      /* filtered out */
    ASSERT_TRUE(strstr(frame, "\"gpt-oss:120\"") != NULL); /* query in title */

    /* Escape dismisses; model untouched. */
    tui_runtime_send(h->rt, tui_msg_key(TUI_KEY_ESCAPE, 0, 0));
    ASSERT_STR_EQ(nm_chat_app_model(h->app), "gpt-oss:20b");

    harness_free(h);
}

static void test_model_picker_enter_composes_first_match(void)
{
    AppHarness *h = harness_new("ollama:cloud", "gpt-oss:20b", NULL);
    ASSERT_NOT_NULL(h);

    harness_type(h, "/model kimi");
    harness_enter(h);
    tui_runtime_send(h->rt, tui_msg_key(TUI_KEY_ENTER, 0, 0));
    ASSERT_STR_EQ(tui_textinput_text(nm_chat_app_textinput(h->app)),
                  "/model kimi-k2.6");
    ASSERT_STR_EQ(nm_chat_app_model(h->app), "gpt-oss:20b");
    harness_enter(h);
    ASSERT_STR_EQ(nm_chat_app_model(h->app), "kimi-k2.6");
    ASSERT_TRUE(strstr(harness_read(h), "kimi-k2.6") != NULL);

    harness_free(h);
}

static void test_model_picker_no_match_prints_nothing(void)
{
    AppHarness *h = harness_new("ollama:cloud", "gpt-oss:20b", NULL);
    ASSERT_NOT_NULL(h);

    /* No match: no popup; a note prints instead; model unchanged. The
     * status row's identity block carries the active model id, so the
     * absence is asserted on the frame's POPUP region. */
    harness_type(h, "/model zzz-no-such-model");
    harness_enter(h);
    const char *frame = tui_runtime_render(h->rt);
    char *prompt = span_bytes(nm_color_prompt(NM_COLOR_16), "\xe2\x9d\xaf ");
    ASSERT_NOT_NULL(prompt);
    ASSERT_TRUE(strstr(frame_popup(frame, prompt), "gpt-oss") == NULL);
    free(prompt);
    ASSERT_STR_EQ(nm_chat_app_model(h->app), "gpt-oss:20b");
    ASSERT_TRUE(strstr(harness_read(h), "no models") != NULL);

    harness_free(h);
}

static void test_provider_popup_composes_into_input(void)
{
    AppHarness *h = harness_new("ollama:cloud", "gpt-oss:20b", NULL);
    ASSERT_NOT_NULL(h);

    /* /provider with no arg opens the registry popup; Enter
     * COMPOSES "/provider <name>" into the input (selection =
     * composition, submit = commit: switching rebuilds the agent
     * and wipes the session, so a modal apply is a fat-finger
     * session killer). The ACTIVE provider is the first entry. */
    harness_type(h, "/provider");
    harness_enter(h);
    const char *frame = tui_runtime_render(h->rt);
    ASSERT_TRUE(strstr(frame, "ollama:cloud") != NULL);
    ASSERT_TRUE(strstr(frame, "ollama:local") != NULL);
    ASSERT_TRUE(strstr(frame, "openrouter") != NULL);
    ASSERT_TRUE(strstr(frame, "opencode:zen") != NULL);

    /* Enter on the pre-selected active entry composes it. */
    tui_runtime_send(h->rt, tui_msg_key(TUI_KEY_ENTER, 0, 0));
    const char *text = tui_textinput_text(nm_chat_app_textinput(h->app));
    ASSERT_NOT_NULL(text);
    ASSERT_STR_EQ(text, "/provider ollama:cloud");
    ASSERT_STR_EQ(nm_chat_app_provider(h->app), "ollama:cloud");

    /* Down to the second entry, Enter composes; submit commits the
     * switch to exactly the composed name. Clear the input first —
     * the previous compose left its text there. */
    tui_textinput_clear(nm_chat_app_textinput(h->app));
    harness_type(h, "/provider");
    harness_enter(h);
    tui_runtime_send(h->rt, tui_msg_key(TUI_KEY_DOWN, 0, 0));
    tui_runtime_send(h->rt, tui_msg_key(TUI_KEY_ENTER, 0, 0));
    text = tui_textinput_text(nm_chat_app_textinput(h->app));
    ASSERT_NOT_NULL(text);
    ASSERT_TRUE(strncmp(text, "/provider ", 10) == 0);
    char name[32] = { 0 };
    snprintf(name, sizeof(name), "%s", text + 10);
    /* Provider NOT switched yet (selection = composition). */
    ASSERT_STR_EQ(nm_chat_app_provider(h->app), "ollama:cloud");

    /* Submit commits the switch to exactly the composed name. */
    harness_enter(h);
    ASSERT_STR_EQ(nm_chat_app_provider(h->app), name);
    ASSERT_NOT_NULL(nm_provider_by_name(name));
    harness_free(h);
}

static void test_provider_popup_query_pre_filters(void)
{
    AppHarness *h = harness_new("ollama:cloud", "gpt-oss:20b", NULL);
    ASSERT_NOT_NULL(h);

    harness_type(h, "/provider openr");
    /* openr is an unknown provider NAME but a valid query: the popup
     * pre-filters the registry; Enter composes openrouter. */
    harness_enter(h);
    const char *frame = tui_runtime_render(h->rt);
    ASSERT_TRUE(strstr(frame, "openrouter") != NULL);
    ASSERT_TRUE(strstr(frame, "\"openr\"") != NULL);

    tui_runtime_send(h->rt, tui_msg_key(TUI_KEY_ENTER, 0, 0));
    harness_enter(h);
    ASSERT_STR_EQ(nm_chat_app_provider(h->app), "openrouter");

    harness_free(h);
}

static void test_model_validation_refuses_unknown_id(void)
{
    AppHarness *h = harness_new("ollama:cloud", "gpt-oss:20b", NULL);
    ASSERT_NOT_NULL(h);

    /* Not in the catalog: it is a QUERY, so the picker opens with no
     * match and prints a note; the model is untouched. */
    harness_type(h, "/model zzz-no-such");
    harness_enter(h);
    ASSERT_STR_EQ(nm_chat_app_model(h->app), "gpt-oss:20b");
    ASSERT_TRUE(strstr(harness_read(h), "no models match 'zzz-no-such'") != NULL);

    /* In the catalog: set. */
    harness_type(h, "/model kimi-k3");
    harness_enter(h);
    ASSERT_STR_EQ(nm_chat_app_model(h->app), "kimi-k3");

    harness_free(h);
}

static void test_model_exact_escape_hatch(void)
{
    AppHarness *h = harness_new("ollama:cloud", "gpt-oss:20b", NULL);
    ASSERT_NOT_NULL(h);

    /* Exact-set escape hatch: "! " prefix sets any id without
     * catalog validation (a local daemon may run private models
     * the static catalog doesn't know — offline homebrew rigs). */
    harness_type(h, "/model ! my-private-finetune");
    harness_enter(h);
    ASSERT_STR_EQ(nm_chat_app_model(h->app), "my-private-finetune");
    ASSERT_TRUE(strstr(harness_read(h), "my-private-finetune") != NULL);

    harness_free(h);
}

static void test_provider_command_switches_provider(void)
{
    AppHarness *h = harness_new("ollama:cloud", "gpt-oss:20b", NULL);
    ASSERT_NOT_NULL(h);

    ASSERT_STR_EQ(nm_chat_app_provider(h->app), "ollama:cloud");

    harness_type(h, "/provider ollama:local");
    harness_enter(h);

    ASSERT_STR_EQ(nm_chat_app_provider(h->app), "ollama:local");
    ASSERT_EQ(nm_chat_app_state(h->app), NM_AGENT_IDLE);
    ASSERT_TRUE(strstr(harness_read(h), "ollama:local") != NULL);

    /* Bare /provider opens the registry picker popup (the same
     * truth the router reads). */
    harness_type(h, "/provider");
    harness_enter(h);
    const char *frame = tui_runtime_render(h->rt);
    ASSERT_TRUE(strstr(frame, "ollama:cloud") != NULL);
    ASSERT_TRUE(strstr(frame, "openrouter") != NULL);
    ASSERT_TRUE(strstr(frame, "hyper") != NULL);
    ASSERT_STR_EQ(nm_chat_app_provider(h->app), "ollama:local");
    tui_runtime_send(h->rt, tui_msg_key(TUI_KEY_ESCAPE, 0, 0));

    /* A query that matches nothing: no popup, a printed note, and
     * the provider stays (typo can't silently switch anything). */
    harness_type(h, "/provider nope");
    harness_enter(h);
    ASSERT_STR_EQ(nm_chat_app_provider(h->app), "ollama:local");
    const char *out = harness_read(h);
    ASSERT_TRUE(strstr(out, "no providers match 'nope'") != NULL);

    harness_free(h);
}

static void test_help_command_lists_commands(void)
{
    AppHarness *h = harness_new("ollama:cloud", "gpt-oss:20b", NULL);
    ASSERT_NOT_NULL(h);

    harness_type(h, "/help");
    harness_enter(h);

    const char *out = harness_read(h);
    ASSERT_TRUE(strstr(out, "/model") != NULL);
    ASSERT_TRUE(strstr(out, "/provider") != NULL);
    ASSERT_TRUE(strstr(out, "/config") != NULL);
    ASSERT_TRUE(strstr(out, "/context") != NULL);
    ASSERT_TRUE(strstr(out, "/image") != NULL);
    ASSERT_TRUE(strstr(out, "/session") != NULL);
    ASSERT_TRUE(strstr(out, "/ps") != NULL);
    ASSERT_TRUE(strstr(out, "/kill") != NULL);
    ASSERT_TRUE(strstr(out, "/quit") != NULL);

    harness_free(h);
}

static void test_cancel_midstream_returns_to_idle(void)
{
    struct ServerScript sc;
    memset(&sc, 0, sizeof(sc));
    sc.n_rounds = 1;
    sc.sse[0] = ""; /* stalls: no bytes, so we can interrupt */
    sc.fd = server_bind(&sc.port);
    ASSERT_TRUE(sc.fd >= 0);
    pthread_t th;
    pthread_create(&th, NULL, chat_server_thread, &sc);

    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1", sc.port);
    AppHarness *h = harness_new("ollama:local", "test-model", base);
    ASSERT_NOT_NULL(h);

    harness_type(h, "stall please");
    harness_enter(h);
    ASSERT_EQ(nm_chat_app_state(h->app), NM_AGENT_STREAMING);
    ASSERT_TRUE(app_fd(h->app) >= 0);

    /* Ctrl+C arrives as an interrupt message. */
    tui_runtime_send(h->rt, tui_msg_interrupt());
    ASSERT_EQ(nm_chat_app_state(h->app), NM_AGENT_IDLE);
    ASSERT_EQ(app_fd(h->app), -1);
    const char *out = harness_read(h);
    ASSERT_TRUE(strstr(out, "stall please") != NULL);
    /* The interrupt marker: a full-width emoji in the tool role. */
    ASSERT_TRUE(strstr(out, sgr16(NM_SGR_TOOL, "🛑 interrupted")) != NULL);

    /* Interrupt on empty IDLE input quits. */
    tui_runtime_send(h->rt, tui_msg_interrupt());
    ASSERT_TRUE(tui_runtime_should_quit(h->rt));

    harness_free(h);
    pthread_join(th, NULL);
    close(sc.fd);
}

/* Escape interrupts the turn: the second interrupt key, next to
 * Ctrl+C. It cancels the round and NOTHING else — the stream closes,
 * the agent walks back to IDLE (so the next prompt can be typed), and
 * the interrupt marker is printed. The kitty tier reports Escape
 * unambiguously (CSI 27u), so it arrives as a plain key press. */
static void test_escape_cancels_the_streaming_turn(void)
{
    struct ServerScript sc;
    memset(&sc, 0, sizeof(sc));
    sc.n_rounds = 1;
    sc.sse[0] = ""; /* stalls: no bytes, so we can interrupt */
    sc.fd = server_bind(&sc.port);
    ASSERT_TRUE(sc.fd >= 0);
    pthread_t th;
    pthread_create(&th, NULL, chat_server_thread, &sc);

    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1", sc.port);
    AppHarness *h = harness_new("ollama:local", "test-model", base);
    ASSERT_NOT_NULL(h);

    harness_type(h, "stall please");
    harness_enter(h);
    ASSERT_EQ(nm_chat_app_state(h->app), NM_AGENT_STREAMING);
    ASSERT_TRUE(app_fd(h->app) >= 0);

    tui_runtime_send(h->rt, tui_msg_key(TUI_KEY_ESCAPE, 0, 0));
    ASSERT_EQ(nm_chat_app_state(h->app), NM_AGENT_IDLE);
    ASSERT_EQ(app_fd(h->app), -1);
    const char *out = harness_read(h);
    ASSERT_TRUE(strstr(out, sgr16(NM_SGR_TOOL, "🛑 interrupted")) != NULL);

    /* The cancel is a cancel of the TURN only: the app is still up and
     * the prompt is live again (the next message can be typed). */
    ASSERT_FALSE(tui_runtime_should_quit(h->rt));
    harness_type(h, "next one");
    ASSERT_STR_EQ(tui_textinput_text(nm_chat_app_textinput(h->app)),
                  "next one");

    harness_free(h);
    pthread_join(th, NULL);
    close(sc.fd);
}

/* Escape at the prompt is NOT an interrupt: it does not clear the
 * input and it does not quit — that ladder belongs to Ctrl+C alone.
 * The key still reaches the input (which spends it on clearing a
 * selection mark), so the text the user typed is intact. */
static void test_escape_at_the_prompt_keeps_the_text(void)
{
    AppHarness *h = harness_new("ollama:local", "test-model", NULL);
    ASSERT_NOT_NULL(h);

    harness_type(h, "half a thought");
    tui_runtime_send(h->rt, tui_msg_key(TUI_KEY_ESCAPE, 0, 0));
    ASSERT_STR_EQ(tui_textinput_text(nm_chat_app_textinput(h->app)),
                  "half a thought");
    ASSERT_FALSE(tui_runtime_should_quit(h->rt));

    /* And a second Escape: still the prompt, still no quit. */
    tui_runtime_send(h->rt, tui_msg_key(TUI_KEY_ESCAPE, 0, 0));
    ASSERT_STR_EQ(tui_textinput_text(nm_chat_app_textinput(h->app)),
                  "half a thought");
    ASSERT_FALSE(tui_runtime_should_quit(h->rt));

    harness_free(h);
}

/* One real turn driven to completion against a canned one-round server,
 * so a test can assert a PROMPT behavior in the state the user's next
 * prompt is actually in: a finished turn rests in NM_AGENT_DONE (only a
 * cancel walks back to IDLE). NULL = setup failed. The caller owns the
 * harness and joins/closes the server (one round: the thread ends by
 * itself). */
static AppHarness *turn_to_done(struct ServerScript *sc, pthread_t *th)
{
    memset(sc, 0, sizeof(*sc));
    sc->n_rounds = 1;
    sc->sse[0] =
        "data: {\"choices\":[{\"delta\":{\"content\":\"in one\"}}]}\n\n"
        "data: [DONE]\n\n";
    sc->fd = server_bind(&sc->port);
    if (sc->fd < 0)
        return NULL;
    pthread_create(th, NULL, chat_server_thread, sc);

    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1", sc->port);
    AppHarness *h = harness_new("ollama:local", "test-model", base);
    if (!h)
        return NULL;
    harness_type(h, "hi there");
    harness_enter(h);
    if (harness_drive(h, 500) != 0)
        return NULL;
    return h;
}

/* Ctrl+D quits at the prompt — and KEEPS quitting after a turn has run.
 * The EOF branch tested the state for NM_AGENT_IDLE, which a finished
 * turn is not: Ctrl+D quit exactly once, on the very first prompt,
 * before any turn had ever moved the agent off its initial state. */
static void test_ctrl_d_quits_after_a_turn(void)
{
    struct ServerScript sc;
    pthread_t th;
    AppHarness *h = turn_to_done(&sc, &th);
    ASSERT_NOT_NULL(h);
    ASSERT_EQ(nm_chat_app_state(h->app), NM_AGENT_DONE);

    /* The next prompt: empty input, finished turn. */
    tui_runtime_send(h->rt, tui_msg_eof());
    ASSERT_TRUE(tui_runtime_should_quit(h->rt));

    harness_free(h);
    pthread_join(th, NULL);
    close(sc.fd);

    /* And on a prompt that has never sent anything (the one case the
     * IDLE test did cover). */
    AppHarness *fresh = harness_new("ollama:cloud", "gpt-oss:20b", NULL);
    ASSERT_NOT_NULL(fresh);
    tui_runtime_send(fresh->rt, tui_msg_eof());
    ASSERT_TRUE(tui_runtime_should_quit(fresh->rt));
    harness_free(fresh);
}

/* At a NON-empty prompt Ctrl+D deletes the character under the cursor,
 * never quits — after a turn just as before one. Same stale predicate,
 * same branch: it did nothing at all once the turn had run, so the key
 * was silently dead (never reached the textinput either). */
static void test_ctrl_d_deletes_at_a_nonempty_prompt_after_a_turn(void)
{
    struct ServerScript sc;
    pthread_t th;
    AppHarness *h = turn_to_done(&sc, &th);
    ASSERT_NOT_NULL(h);
    ASSERT_EQ(nm_chat_app_state(h->app), NM_AGENT_DONE);

    harness_type(h, "ab");
    /* Ctrl+D deletes UNDER the cursor, and typing leaves it at the end:
     * Home puts it on 'a' so the edit is observable. */
    tui_runtime_send(h->rt, tui_msg_key(TUI_KEY_HOME, 0, 0));
    tui_runtime_send(h->rt, tui_msg_eof());

    ASSERT_FALSE(tui_runtime_should_quit(h->rt));
    ASSERT_STR_EQ(tui_textinput_text(nm_chat_app_textinput(h->app)), "b");

    harness_free(h);
    pthread_join(th, NULL);
    close(sc.fd);
}

/* P0 deadline seam in the TUI: a silent server never makes the stream
 * fd readable, so only the runtime's tick can fire the stream-
 * inactivity deadline. The tick interval is bounded by
 * nm_agent_next_timeout_ms and the tick drives the step when due. */
static void test_tick_fires_stream_inactivity_timeout(void)
{
    struct ServerScript sc;
    memset(&sc, 0, sizeof(sc));
    sc.n_rounds = 1;
    sc.sse[0] = ""; /* stalls: no bytes at all */
    sc.fd = server_bind(&sc.port);
    ASSERT_TRUE(sc.fd >= 0);
    pthread_t th;
    pthread_create(&th, NULL, chat_server_thread, &sc);

    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1", sc.port);
    AppHarness *h = harness_new("ollama:local", "test-model", base);
    ASSERT_NOT_NULL(h);

    /* A short budget for a fast test: the store's `timeout` key, which
     * the live agent resolves at the point of use (no setter to push). */
    scratch_store_begin();
    store_set(NM_CFG_KEY_TIMEOUT, "200");
    ASSERT_EQ(nm_agent_timeout_ms(nm_chat_app_agent(h->app)), 200);

    harness_type(h, "stall please");
    harness_enter(h);
    ASSERT_EQ(nm_chat_app_state(h->app), NM_AGENT_STREAMING);

    /* The tick interval is the spinner cadence, bounded by the
     * remaining deadline (never longer than 100 ms while busy). */
    int ms = nm_chat_app_tick_ms(h->app);
    ASSERT_TRUE(ms > 0 && ms <= 100);

    /* The fd is silent; only the timer advances. Drive VIRTUAL time:
     * the tick moves the clock by the app's own cadence, which is what
     * boba's loop does — except the deadline is crossed instantly
     * instead of in wall clock (a loaded runner can no longer decide
     * the outcome). */
    ASSERT_EQ(harness_drive_virtual(h, 200), 0);
    ASSERT_EQ(nm_chat_app_state(h->app), NM_AGENT_ERROR);
    tui_runtime_flush(h->rt);

    const char *out = harness_read(h);
    ASSERT_TRUE(strstr(out, "stall please") != NULL);
    ASSERT_NOT_NULL(strstr(out, "timed out"));

    /* Idle again: nothing to tick for. */
    ASSERT_EQ(nm_chat_app_tick_ms(h->app), -1);

    scratch_store_end();
    harness_free(h);
    pthread_join(th, NULL);
    close(sc.fd);
}

static void test_connect_error_prints_and_returns_to_idle(void)
{
    /* Bind then close: a port nothing listens on (connect refused). */
    int port = 0;
    int fd = server_bind(&port);
    ASSERT_TRUE(fd >= 0);
    close(fd);

    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1", port);
    AppHarness *h = harness_new("ollama:local", "test-model", base);
    ASSERT_NOT_NULL(h);

    harness_type(h, "anyone there");
    harness_enter(h);

    /* The async transport seam: submit returns while the connect is
     * still in flight (STREAMING, connect pending); the failure
     * surfaces through steps, exactly as boba's loop would. */
    ASSERT_EQ(harness_drive(h, 2000), 0);
    ASSERT_EQ(nm_chat_app_state(h->app), NM_AGENT_ERROR);
    const char *out = harness_read(h);
    ASSERT_TRUE(strstr(out, "anyone there") != NULL);
    /* An error line reached the scrollback. */
    ASSERT_TRUE(strstr(out, "nevermore") != NULL);

    /* A fresh turn still works (ERROR does not wedge the app). */
    ASSERT_EQ(app_fd(h->app), -1);

    harness_free(h);
}

/* The connect walk's notice reaches the transcript: a turn whose host
 * has more than one address — and no listener on ANY of them — prints a
 * system line naming the attempt it gave up on, instead of spinning
 * silently.
 *
 * No server here on purpose: the notice is the app's own line, and
 * whether the walk then LANDS on a live address is a host property
 * (which address answers, in what order the resolver returns them).
 * That half is pinned where the production clock lives, in test_wire's
 * test_connect_walk_notice_reports_the_next_address (a blocking walk
 * against a bound tail address). What this test owns is the app's half,
 * and it needs no I/O at all: the walk abandons the first address —
 * instantly on a refusal, or on its budget where a refusal is not
 * prompt — so the drive is virtual time and the outcome is the same on
 * every platform. */
static void test_connect_walk_notice_is_printed(void)
{
    store_clear(NM_CFG_KEY_SKIP_FAMILIES);
    /* A port nothing listens on, plus the walk's precondition: a
     * single-address resolver has no hop to make, so the notice cannot
     * be exercised there (a healthy box, not a failure). */
    int port = 0;
    int probe = test_bind_last_localhost_addr(&port);
    if (probe < 0) {
        fprintf(stderr, "  note: 'localhost' has no second address to "
                        "walk to; notice line not exercised\n");
        return;
    }
    close(probe); /* the port is free again: nothing must answer */

    char base[64];
    snprintf(base, sizeof(base), "http://localhost:%d/v1", port);
    AppHarness *h = harness_new("ollama:local", "test-model", base);
    ASSERT_NOT_NULL(h);

    harness_type(h, "hello");
    harness_enter(h);

    /* Drive the walk on virtual time to its end: move the clock past any
     * per-attempt budget, then STEP (a step drives the walk
     * unconditionally — the tick only does when the app reports a due
     * deadline, which is not something this test should depend on). With
     * no listener on any address the walk runs out and the turn fails. */
    for (int i = 0; i < 200; i++) {
        NmAgentState st = nm_chat_app_state(h->app);
        if (st == NM_AGENT_DONE || st == NM_AGENT_ERROR ||
            st == NM_AGENT_IDLE)
            break;
        nm_test_clock_advance_ms(300);
        nm_chat_app_step(h->app);
        tui_runtime_flush(h->rt);
    }

    /* The commit pass: a turn that errored inside the drive may have left
     * its notice in the transcript buffer with no flush after it. */
    tui_runtime_flush(h->rt);

    const char *out = harness_read(h);
    if (!strstr(out, "did not answer")) {
        size_t n = strlen(out);
        fprintf(stderr, "  note: no walk notice (state=%d): %s\n",
                (int)nm_chat_app_state(h->app),
                n > 400 ? out + (n - 400) : out);
    }
    /* The notice names the abandoned attempt and the walk length. */
    ASSERT_TRUE(strstr(out, "did not answer") != NULL);
    ASSERT_TRUE(strstr(out, "1/") != NULL);
    /* The walk ran out of addresses: the turn failed, loudly, rather
     * than hanging on the first one. */
    ASSERT_EQ(nm_chat_app_state(h->app), NM_AGENT_ERROR);

    harness_free(h);
}

/* The walk's budget needs a DRIVE on the event-driven path. A
 * black-holed address (RFC 5737's TEST-NET-1 range: reserved, routed
 * nowhere) produces NO socket event at all — never writable, never
 * exceptional, never readable — so an interest-only loop never steps
 * the walk, and the per-address budget, the walk's entire point, never
 * fires. nm_agent_next_timeout_ms must hand that budget to the tick.
 *
 * The endpoint is a bracketed IPv6 literal, the exact shape the
 * family-skip feature exists for: one address, unroutable here, so the
 * walk exhausts its single attempt and the turn reports within the
 * budget instead of hanging for the 300 s inactivity default. */
static void test_black_hole_connect_is_bounded_by_the_tick(void)
{
    scratch_store_begin();
    store_clear(NM_CFG_KEY_SKIP_FAMILIES);
    knobs_set_timeout(250);

    AppHarness *h = harness_new("ollama:local", "test-model",
                                "http://[2001:db8:dead::1]:9/v1");
    ASSERT_NOT_NULL(h);

    harness_type(h, "anyone there");
    harness_enter(h);
    ASSERT_EQ(nm_chat_app_state(h->app), NM_AGENT_STREAMING);

    /* The seam under test: while connecting, the agent must report the
     * budget as its next deadline. Without the fix this is the 300 s
     * inactivity value (or -1), and the tick below never fires the
     * walk's advance. */
    int to = nm_agent_next_timeout_ms(nm_chat_app_agent(h->app));
    ASSERT_TRUE(to >= 0 && to <= 250);

    /* Drive the loop the way boba does, on VIRTUAL time: the tick moves
     * the clock by the app's own reported cadence, so the per-address
     * budget is crossed by the deadline itself. That is the point of
     * the test — the deadline, not the pass count, ends the walk (a
     * pass costs microseconds, so an iteration bound could be spent
     * before the budget elapsed: the ~40 % flake this replaced). The
     * drive is bounded by the tick count, so a walk that never
     * advances still fails loudly. */
    ASSERT_EQ(harness_drive_virtual(h, 2000), 0);
    ASSERT_EQ(nm_chat_app_state(h->app), NM_AGENT_ERROR);

    tui_runtime_flush(h->rt);
    ASSERT_TRUE(strstr(harness_read(h), "nevermore") != NULL);

    harness_free(h);
    store_clear(NM_CFG_KEY_CONNECT_TIMEOUT);
    store_clear(NM_CFG_KEY_SKIP_FAMILIES);
    scratch_store_end();
}

/* Raw responder thread: drain the request, write bytes verbatim (no
 * chunked/SSE machinery). Used to script wire-truth error pages
 * whose bodies end with a trailing newline — raw-mode transcript
 * correctness (no staircasing) is the property under test. */
struct RawResponse
{
    int fd;   /* listen socket */
    int port; /* filled by the test */
    const char *response;
};

static void *raw_responder_thread(void *arg)
{
    struct RawResponse *rr = arg;
    struct timeval atv = { 2, 0 };
    fd_set arfds;
    FD_ZERO(&arfds);
    FD_SET(rr->fd, &arfds);
    if (select(rr->fd + 1, &arfds, NULL, NULL, &atv) <= 0)
        return NULL; /* cancelled before connecting: fine */
    int cfd = accept(rr->fd, NULL, NULL);
    if (cfd < 0)
        return NULL;
    char drain[REQ_CAP];
    size_t got = 0;
    while (got < sizeof(drain) - 1) {
        long n = recv(cfd, drain + got, sizeof(drain) - 1 - got, 0);
        if (n <= 0)
            break;
        got += (size_t)n;
        if (strstr(drain, "\r\n\r\n") && got > 4 && drain[got - 1] == '}')
            break;
    }
    size_t len = strlen(rr->response);
    size_t off = 0;
    while (off < len) {
        long n = send(cfd, rr->response + off, len - off, 0);
        if (n <= 0)
            break;
        off += (size_t)n;
    }
    close(cfd);
    return NULL;
}

/* Reasoning deltas ride stream 1, ahead of the answer, on their own
 * lines; the answer rides stream 0. Phase order is the deliverable
 * (the dim is step 4). Wire shape: reasoning-only chunks
 * (content:"") then content, phase-sequential. */
static void test_reasoning_prints_before_answer(void)
{
    struct RawResponse rr = {
        0, 0,
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/event-stream\r\n"
        "Connection: close\r\n\r\n"
        "data: {\"choices\":[{\"delta\":{\"content\":\"\","
        "\"reasoning_content\":\"weighing options\\n\"}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{\"content\":\"the answer\"}}]}\n\n"
        "data: [DONE]\n\n"
    };
    rr.fd = server_bind(&rr.port);
    ASSERT_TRUE(rr.fd >= 0);
    pthread_t th;
    pthread_create(&th, NULL, raw_responder_thread, &rr);

    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1", rr.port);
    AppHarness *h = harness_new("ollama:local", "test-model", base);
    ASSERT_NOT_NULL(h);

    harness_type(h, "think");
    harness_enter(h);
    ASSERT_EQ(harness_drive(h, 2000), 0);
    ASSERT_EQ(nm_chat_app_state(h->app), NM_AGENT_DONE);

    const char *out = harness_read(h);
    ASSERT_TRUE(strstr(out, "weighing options") != NULL);
    ASSERT_TRUE(strstr(out, "the answer") != NULL);
    /* Reasoning precedes the answer (phase-sequential). */
    const char *reason = strstr(out, "weighing options");
    const char *answer = strstr(out, "the answer");
    ASSERT_TRUE(reason < answer);
    /* Reasoning is dim (exact composed sequence; a bare ";2" needle
     * false-positives on every truecolor sequence). */
    const char *dim = strstr(out, "\x1b[0;2m");
    ASSERT_NOT_NULL(dim);
    ASSERT_TRUE(dim < reason);
    /* The answer itself carries no dim. */
    const char *answer_dim = strstr(answer, "\x1b[0;2m");
    ASSERT_TRUE(answer_dim == NULL || answer_dim > answer + 12);
    /* No staircasing. */
    for (const char *p = out; *p; p++)
        ASSERT_TRUE(*p != '\n' || (p > out && p[-1] == '\r'));

    harness_free(h);
}

/* Regression: a provider error body that ends with a trailing
 * newline (hyper's does — wire framing) must NOT staircase the
 * transcript. The error line — message + any follow-on text — must
 * land as whole \r\n-terminated lines; a bare \n inside the captured
 * output is the bug (raw mode: the terminal does not translate). */
static void test_error_line_endings_are_crnl(void)
{
    struct RawResponse rr = {
        0, 0,
        "HTTP/1.1 401 Unauthorized\r\n"
        "Content-Type: application/json\r\n"
        "Connection: close\r\n\r\n"
        "{\"error\":\"missing authorization\"}\n"
    };
    rr.fd = server_bind(&rr.port);
    ASSERT_TRUE(rr.fd >= 0);
    pthread_t th;
    pthread_create(&th, NULL, raw_responder_thread, &rr);

    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1", rr.port);
    AppHarness *h = harness_new("hyper", "test-model", base);
    ASSERT_NOT_NULL(h);
    /* No key: the agent's env-var hint rides the same error line. */

    harness_type(h, "hello");
    harness_enter(h);
    ASSERT_EQ(harness_drive(h, 2000), 0);
    ASSERT_EQ(nm_chat_app_state(h->app), NM_AGENT_ERROR);

    const char *out = harness_read(h);
    /* The whole error reached the scrollback. Long bodies wrap at the
     * terminal width (boba's explicit wrap), so assert the fragments,
     * not one contiguous run. */
    ASSERT_TRUE(strstr(out, "chat failed: auth rejected (HTTP 401)") != NULL);
    ASSERT_TRUE(strstr(out, "missing authorizatio") != NULL);
    ASSERT_TRUE(strstr(out, "export HYPER_API_KEY") != NULL);
    /* No bare LF anywhere the app wrote: every line break is \r\n
     * (the transcript is captured verbatim; a bare \n IS the
     * staircase in raw mode). */
    for (const char *p = out; *p; p++)
        ASSERT_TRUE(*p != '\n' || (p > out && p[-1] == '\r'));
    /* The hint follows the body in the error's own output block (both
     * come from the one sys_line), with no second error line between.
     * Searched FROM the body: the startup preflight note carries the
     * same env-var name earlier in the scrollback. */
    const char *msg = strstr(out, "chat failed:");
    ASSERT_NOT_NULL(msg);
    const char *hint = strstr(msg, "export HYPER_API_KEY");
    ASSERT_NOT_NULL(hint);
    ASSERT_TRUE(msg < hint);

    harness_free(h);
    pthread_join(th, NULL);
    close(rr.fd);
}

static void test_separator_between_reasoning_and_answer(void)
{
    struct RawResponse rr = {
        0, 0,
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/event-stream\r\n"
        "Connection: close\r\n\r\n"
        "data: {\"choices\":[{\"delta\":{\"content\":\"\","
        "\"reasoning_content\":\"weighing options\\n\"}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{\"content\":\"final words\"}}]}\n\n"
        "data: [DONE]\n\n"
    };
    rr.fd = server_bind(&rr.port);
    ASSERT_TRUE(rr.fd >= 0);
    pthread_t th;
    pthread_create(&th, NULL, raw_responder_thread, &rr);

    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1", rr.port);
    AppHarness *h = harness_new("ollama:local", "test-model", base);
    ASSERT_NOT_NULL(h);

    harness_type(h, "why");
    harness_enter(h);
    ASSERT_EQ(harness_drive(h, 500), 0);

    const char *out = harness_read(h);
    /* Reasoning, one blank, answer, one blank — never doubled. The
     * reasoning row is dim, so its SGR reset precedes the terminator
     * (D8); the answer row is plain. */
    ASSERT_TRUE(strstr(out, "weighing options\x1b[0m\r\n\r\n") != NULL);
    ASSERT_TRUE(strstr(out, "final words\r\n\r\n") != NULL);
    ASSERT_TRUE(strstr(out, "final words\r\n\r\n\r\n") == NULL);

    harness_free(h);
    pthread_join(th, NULL);
    close(rr.fd);
}

static void test_separator_collapses_trailing_blank_lines(void)
{
    struct RawResponse rr = {
        0, 0,
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/event-stream\r\n"
        "Connection: close\r\n\r\n"
        /* Unlabeled (byte-emitted) fence, never closed. The trailing
         * blank lines arrive as their OWN all-newline delta — the
         * pathological "too many" source, and the split (content, then
         * pure newlines) that the hold-back must still collapse. */
        "data: {\"choices\":[{\"delta\":{\"content\":"
        "\"```\\ncode\"}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{\"content\":\"\\n\\n\\n\"}}]}\n\n"
        "data: [DONE]\n\n"
    };
    rr.fd = server_bind(&rr.port);
    ASSERT_TRUE(rr.fd >= 0);
    pthread_t th;
    pthread_create(&th, NULL, raw_responder_thread, &rr);

    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1", rr.port);
    AppHarness *h = harness_new("ollama:local", "test-model", base);
    ASSERT_NOT_NULL(h);

    harness_type(h, "code please");
    harness_enter(h);
    ASSERT_EQ(harness_drive(h, 500), 0);
    ASSERT_EQ(nm_chat_app_state(h->app), NM_AGENT_DONE);

    const char *out = harness_read(h); /* The fence body committed, followed by exactly ONE blank row: the
                                        * two trailing blank lines the model emitted are collapsed, never
                                        * stacked onto the separator. */
    ASSERT_TRUE(strstr(out, "```\r\ncode\r\n\r\n") != NULL);
    ASSERT_TRUE(strstr(out, "code\r\n\r\n\r\n") == NULL);

    harness_free(h);
    pthread_join(th, NULL);
    close(rr.fd);
}

/* The "add one if missing" half: at the end of content streaming exactly
 * ONE blank line follows the answer. The model's own trailing newlines
 * (here two) collapse; the separator contributes the one. */
static void test_separator_blank_line_after_answer(void)
{
    struct RawResponse rr = {
        0, 0,
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/event-stream\r\n"
        "Connection: close\r\n\r\n"
        "data: {\"choices\":[{\"delta\":{\"content\":\"Hello world\\n\\n\"}}]}\n\n"
        "data: [DONE]\n\n"
    };
    rr.fd = server_bind(&rr.port);
    ASSERT_TRUE(rr.fd >= 0);
    pthread_t th;
    pthread_create(&th, NULL, raw_responder_thread, &rr);

    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1", rr.port);
    AppHarness *h = harness_new("ollama:local", "test-model", base);
    ASSERT_NOT_NULL(h);

    harness_type(h, "hi");
    harness_enter(h);
    ASSERT_EQ(harness_drive(h, 500), 0);
    ASSERT_EQ(nm_chat_app_state(h->app), NM_AGENT_DONE);

    const char *out = harness_read(h);
    ASSERT_TRUE(strstr(out, "Hello world\r\n\r\n") != NULL);
    ASSERT_TRUE(strstr(out, "Hello world\r\n\r\n\r\n") == NULL);

    harness_free(h);
    pthread_join(th, NULL);
    close(rr.fd);
}

/* A screen model for strip_frames: independent rows of bytes, no
 * bottom edge (a terminal would scroll rows off its screen; a
 * scrollback read keeps them). */
typedef struct Screen
{
    char **row;
    size_t *len;
    size_t *cap;
    size_t count;
} Screen;

static void screen_row(Screen *s, size_t r)
{
    while (s->count <= r) {
        size_t k = s->count++;
        s->row = realloc(s->row, s->count * sizeof(*s->row));
        s->len = realloc(s->len, s->count * sizeof(*s->len));
        s->cap = realloc(s->cap, s->count * sizeof(*s->cap));
        if (!s->row || !s->len || !s->cap)
            return;
        s->row[k] = NULL;
        s->len[k] = 0;
        s->cap[k] = 0;
    }
}

static void screen_put(Screen *s, size_t r, size_t col, char ch)
{
    screen_row(s, r);
    if (r >= s->count || !s->row)
        return;
    if (col >= s->cap[r]) {
        size_t cap = s->cap[r] ? s->cap[r] : 32;
        while (cap <= col)
            cap *= 2;
        char *grown = realloc(s->row[r], cap);
        if (!grown)
            return;
        s->row[r] = grown;
        s->cap[r] = cap;
    }
    while (s->len[r] < col)
        s->row[r][s->len[r]++] = ' ';
    if (col == s->len[r])
        s->len[r]++;
    s->row[r][col] = ch;
}

static void screen_free(Screen *s)
{
    for (size_t i = 0; i < s->count; i++)
        free(s->row[i]);
    free(s->row);
    free(s->len);
    free(s->cap);
    memset(s, 0, sizeof(*s));
}

/* Reduce a raw capture to the rows a user sees: the committed
 * transcript above the live region.
 *
 * The runtime paints the live region IN PLACE — cursor up to the
 * frame's top, the rows, then the placement — and it no longer
 * brackets a frame with a hide/show cursor pair (visibility is
 * edge-triggered: a change emits one mode byte, a steady cursor emits
 * none). Dropping the bytes between the old brackets therefore drops
 * nothing, and every repaint's rows would pile up as if they were new
 * scrollback. Replay the controls an inline paint actually
 * emits instead (CR, LF, cursor up/down/forward, erase-in-line, SGR);
 * the screen model then does the filtering by construction: a repaint
 * overwrites its own rows, and only the committed transcript stays
 * above them. The live region's own content is asserted on the frame
 * itself (tui_runtime_render), never on this text.
 *
 * Trailing blanks are trimmed and rows are joined with '\n', so
 * "\n\n" is one blank row. Heap-owned. */
static char *strip_frames(const char *in)
{
    Screen s = { 0 };
    size_t row = 0, col = 0, n = strlen(in);
    for (size_t i = 0; i < n;) {
        unsigned char c = (unsigned char)in[i];
        if (c == 0x1b) {
            char kind = in[i + 1];
            if (kind == '[') {
                /* CSI: params, then one final byte in @..~. Only the
                 * first parameter matters to the controls below. */
                size_t j = i + 2, arg = 0;
                int semi = 0;
                while (j < n && !(in[j] >= '@' && in[j] <= '~')) {
                    if (in[j] == ';')
                        semi = 1;
                    else if (!semi && in[j] >= '0' && in[j] <= '9')
                        arg = arg * 10 + (size_t)(in[j] - '0');
                    j++;
                }
                char final = j < n ? in[j] : '\0';
                size_t mv = arg ? arg : 1; /* cursor moves default to 1 */
                switch (final) {
                case 'A':
                    row = mv > row ? 0 : row - mv;
                    break;
                case 'B':
                    row += mv;
                    screen_row(&s, row);
                    break;
                case 'C':
                    col += mv;
                    break;
                case 'D':
                    col = mv > col ? 0 : col - mv;
                    break;
                case 'G':
                    col = arg > 0 ? arg - 1 : 0;
                    break;
                case 'K': /* EL: 0 to end (default), 1 to start, 2 all */
                    if (row < s.count) {
                        if (arg == 2)
                            s.len[row] = 0;
                        else if (arg == 1)
                            for (size_t k = 0; k < col && k < s.len[row]; k++)
                                s.row[row][k] = ' ';
                        else if (col < s.len[row])
                            s.len[row] = col;
                    }
                    break;
                default:
                    break; /* SGR and the rest: no text */
                }
                i = j < n ? j + 1 : n;
                continue;
            }
            if (kind == ']' || kind == '_' || kind == 'P') {
                /* OSC (BEL or ST terminated), APC and DCS (ST): no text. */
                size_t j = i + 2;
                while (j < n && !(in[j] == 0x07 ||
                                  (in[j] == 0x1b && in[j + 1] == '\\')))
                    j++;
                i = j < n ? j + (in[j] == 0x07 ? 1 : 2) : n;
                continue;
            }
            i += 2;
            continue;
        }
        if (c == '\r') {
            col = 0;
            i++;
            continue;
        }
        if (c == '\n') {
            row++;
            screen_row(&s, row);
            i++;
            continue;
        }
        screen_put(&s, row, col, (char)c);
        col++;
        i++;
    }

    size_t total = 0;
    for (size_t r = 0; r < s.count; r++) {
        while (s.len[r] > 0 && s.row[r][s.len[r] - 1] == ' ')
            s.len[r]--;
        total += s.len[r] + 1;
    }
    char *out = malloc(total + 1);
    if (out) {
        size_t o = 0;
        for (size_t r = 0; r < s.count; r++) {
            if (s.len[r] > 0) {
                memcpy(out + o, s.row[r], s.len[r]);
                o += s.len[r];
            }
            out[o++] = '\n';
        }
        out[o] = '\0';
    }
    screen_free(&s);
    return out;
}

/* True when the byte range contains a blank row (two consecutive
 * newlines) — the separator a tool block ends with. */
static int has_blank_row(const char *from, const char *to)
{
    for (const char *p = from; p + 1 < to; p++) {
        if (p[0] == '\n' && p[1] == '\n')
            return 1;
    }
    return 0;
}

/* A blank line the MODEL wrote INSIDE a run is part of that run's
 * markdown: the paragraph break reaches the scrollback as an empty row.
 * (boba draws no row for a separator, so the classifier continues the
 * block instead of reporting the break as BLANK.) Both agent streams are
 * covered: the reasoning trace's paragraphs and the answer's. */
static void test_blank_line_inside_a_run_renders(void)
{
    struct RawResponse rr = {
        0, 0,
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/event-stream\r\n"
        "Connection: close\r\n\r\n"
        "data: {\"choices\":[{\"delta\":{\"content\":\"\","
        "\"reasoning_content\":\"weighing options\\n\\nand another\\n\"}}]}"
        "\n\n"
        "data: {\"choices\":[{\"delta\":{\"content\":"
        "\"one paragraph\\n\\ntwo paragraph\\n\"}}]}\n\n"
        "data: [DONE]\n\n"
    };
    rr.fd = server_bind(&rr.port);
    ASSERT_TRUE(rr.fd >= 0);
    pthread_t th;
    pthread_create(&th, NULL, raw_responder_thread, &rr);

    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1", rr.port);
    AppHarness *h = harness_new("ollama:local", "test-model", base);
    ASSERT_NOT_NULL(h);

    harness_type(h, "think");
    harness_enter(h);
    ASSERT_EQ(harness_drive(h, 500), 0);
    ASSERT_EQ(nm_chat_app_state(h->app), NM_AGENT_DONE);

    /* The rows a user sees: the empty row between the two reasoning
     * paragraphs is the trace's OWN break, one row before the run
     * separator's. */
    char *seen = strip_frames(harness_read(h));
    ASSERT_NOT_NULL(seen);
    ASSERT_TRUE(strstr(seen, "weighing options\n\nand another\n") != NULL);
    ASSERT_TRUE(strstr(seen, "one paragraph\n\ntwo paragraph\n") != NULL);
    free(seen);

    harness_free(h);
    pthread_join(th, NULL);
    close(rr.fd);
}

#ifndef _WIN32
/* A run_command tool round runs asynchronously: while the child runs the
 * agent sits in RUNNING_TOOL with the child's output pipe as its fd, so
 * boba keeps ticking and the spinner paints the "executing" tier. */
static void test_tool_runs_async_and_spinner_ticks(void)
{
    struct ServerScript sc;
    memset(&sc, 0, sizeof(sc));
    sc.n_rounds = 2;
    sc.sse[0] =
        "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,"
        "\"id\":\"call_c\",\"type\":\"function\",\"function\":"
        "{\"name\":\"run_command\",\"arguments\":"
        "\"{\\\"cmd\\\":\\\"sleep 0.3; echo hi-cmd\\\"}\"}}]}}]}\n\n"
        "data: [DONE]\n\n";
    sc.sse[1] =
        "data: {\"choices\":[{\"delta\":{\"content\":\"all done\"}}]}\n\n"
        "data: [DONE]\n\n";
    sc.fd = server_bind(&sc.port);
    ASSERT_TRUE(sc.fd >= 0);
    pthread_t th;
    pthread_create(&th, NULL, chat_server_thread, &sc);

    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1", sc.port);
    AppHarness *h = harness_new("ollama:local", "test-model", base);
    ASSERT_NOT_NULL(h);

    harness_type(h, "run it");
    harness_enter(h);

    /* Step until the command is running (RUNNING_TOOL with a live fd). */
    int spins = 0;
    while (spins++ < 2000) {
        if (nm_chat_app_state(h->app) == NM_AGENT_RUNNING_TOOL &&
            app_fd(h->app) >= 0)
            break;
        int fd = app_fd(h->app);
        unsigned in = app_interest(h->app);
        if (fd >= 0 && in) {
            fd_set r, w;
            struct timeval tv = { 0, 10 * 1000 };
            FD_ZERO(&r);
            FD_ZERO(&w);
            if (in & NM_INTEREST_READ)
                FD_SET(fd, &r);
            if (in & NM_INTEREST_WRITE)
                FD_SET(fd, &w);
            select(fd + 1, &r, &w, NULL, &tv);
        }
        nm_chat_app_step(h->app);
        tui_runtime_flush(h->rt);
    }
    ASSERT_EQ(nm_chat_app_state(h->app), NM_AGENT_RUNNING_TOOL);
    ASSERT_TRUE(app_fd(h->app) >= 0);

    /* The status row while a child runs carries the STAR style's first
     * frame in the star set's yellow: the style is the whole "what is
     * it doing" readout (the dot streams, the stars execute), so no
     * label follows it. The gauge is still the row's landmark. */
    nm_chat_app_tick(h->app);
    const char *frame = tui_runtime_render(h->rt);
    ASSERT_NOT_NULL(frame);
    ASSERT_TRUE(strstr(frame, "executing") == NULL);
    char *glyph = span_bytes(tui_color_ansi(11), "\xc2\xb7 ");
    char *gauge = span_bytes(nm_color_gutter(NM_COLOR_16), "-/- ");
    ASSERT_NOT_NULL(glyph);
    ASSERT_NOT_NULL(gauge);
    ASSERT_TRUE(strstr(frame, glyph) != NULL);
    ASSERT_TRUE(strstr(frame, gauge) != NULL);
    free(glyph);
    free(gauge);

    /* And the turn completes, with the command output committed. */
    ASSERT_EQ(harness_drive(h, 2000), 0);
    ASSERT_EQ(nm_chat_app_state(h->app), NM_AGENT_DONE);
    char *clean = strip_frames(harness_read(h));
    ASSERT_NOT_NULL(clean);
    ASSERT_TRUE(strstr(clean, "hi-cmd") != NULL);
    /* Every row's text starts in the same column: the result's first row
     * opens with `  ╰─ ` (5 display columns) and carries the status line
     * — the command's exit status, which the transcript used to leave
     * out — then the body indents by exactly that much under "Output:". */
    ASSERT_TRUE(strstr(clean, "  ╰─ Process exited with code 0") != NULL);
    ASSERT_TRUE(strstr(clean, "     Output:") != NULL);
    ASSERT_TRUE(strstr(clean, "     hi-cmd") != NULL);
    free(clean);

    harness_free(h);
    pthread_join(th, NULL);
    close(sc.fd);
}
#endif /* !_WIN32 */

#ifndef _WIN32
/* Cancel while an async tool is mid-run: the announced plan never gets
 * its END, so the block never emitted its own blank line. The marker
 * must still start on a fresh row rather than gluing itself to the
 * plan (the open block is closed on the way out). The command sleeps
 * long enough that a blocking reap would be felt here. */
static void test_cancel_during_tool_closes_the_block(void)
{
    struct ServerScript sc;
    memset(&sc, 0, sizeof(sc));
    sc.n_rounds = 1;
    sc.sse[0] =
        "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,"
        "\"id\":\"call_c\",\"type\":\"function\",\"function\":"
        "{\"name\":\"run_command\",\"arguments\":"
        "\"{\\\"cmd\\\":\\\"sleep 5; echo never\\\"}\"}}]}}]}\n\n"
        "data: [DONE]\n\n";
    sc.fd = server_bind(&sc.port);
    ASSERT_TRUE(sc.fd >= 0);
    pthread_t th;
    pthread_create(&th, NULL, chat_server_thread, &sc);

    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1", sc.port);
    AppHarness *h = harness_new("ollama:local", "test-model", base);
    ASSERT_NOT_NULL(h);

    harness_type(h, "run it");
    harness_enter(h);

    /* Step until the command is running (RUNNING_TOOL with a live fd). */
    int spins = 0;
    while (spins++ < 2000) {
        if (nm_chat_app_state(h->app) == NM_AGENT_RUNNING_TOOL &&
            app_fd(h->app) >= 0)
            break;
        int fd = app_fd(h->app);
        unsigned in = app_interest(h->app);
        if (fd >= 0 && in) {
            fd_set r, w;
            struct timeval tv = { 0, 10 * 1000 };
            FD_ZERO(&r);
            FD_ZERO(&w);
            if (in & NM_INTEREST_READ)
                FD_SET(fd, &r);
            if (in & NM_INTEREST_WRITE)
                FD_SET(fd, &w);
            select(fd + 1, &r, &w, NULL, &tv);
        }
        nm_chat_app_step(h->app);
        tui_runtime_flush(h->rt);
    }
    ASSERT_EQ(nm_chat_app_state(h->app), NM_AGENT_RUNNING_TOOL);
    ASSERT_TRUE(app_fd(h->app) >= 0);

    tui_runtime_send(h->rt, tui_msg_interrupt());
    ASSERT_EQ(nm_chat_app_state(h->app), NM_AGENT_IDLE);

    const char *out = harness_read(h);
    char *clean = strip_frames(out);
    ASSERT_NOT_NULL(clean);
    const char *plan = strstr(clean, "cmd: sleep 5");
    const char *mark = strstr(clean, "🛑 interrupted");
    ASSERT_NOT_NULL(plan);
    ASSERT_NOT_NULL(mark);
    ASSERT_TRUE(plan < mark);
    ASSERT_TRUE(has_blank_row(plan, mark)); /* the open block was closed */
    free(clean);

    harness_free(h);
    pthread_join(th, NULL);
    close(sc.fd);
}

/* Escape interrupts a RUNNING TOOL, not just a stream: the child is
 * killed and the agent walks back to IDLE, so the user is not stuck
 * behind a long command. The marker is the same one Ctrl+C prints. */
static void test_escape_cancels_the_running_tool(void)
{
    struct ServerScript sc;
    memset(&sc, 0, sizeof(sc));
    sc.n_rounds = 1;
    sc.sse[0] =
        "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,"
        "\"id\":\"call_c\",\"type\":\"function\",\"function\":"
        "{\"name\":\"run_command\",\"arguments\":"
        "\"{\\\"cmd\\\":\\\"sleep 5; echo never\\\"}\"}}]}}]}\n\n"
        "data: [DONE]\n\n";
    sc.fd = server_bind(&sc.port);
    ASSERT_TRUE(sc.fd >= 0);
    pthread_t th;
    pthread_create(&th, NULL, chat_server_thread, &sc);

    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1", sc.port);
    AppHarness *h = harness_new("ollama:local", "test-model", base);
    ASSERT_NOT_NULL(h);

    harness_type(h, "run it");
    harness_enter(h);

    /* Step until the command is running (RUNNING_TOOL with a live fd). */
    int spins = 0;
    while (spins++ < 2000) {
        if (nm_chat_app_state(h->app) == NM_AGENT_RUNNING_TOOL &&
            app_fd(h->app) >= 0)
            break;
        int fd = app_fd(h->app);
        unsigned in = app_interest(h->app);
        if (fd >= 0 && in) {
            fd_set r, w;
            struct timeval tv = { 0, 10 * 1000 };
            FD_ZERO(&r);
            FD_ZERO(&w);
            if (in & NM_INTEREST_READ)
                FD_SET(fd, &r);
            if (in & NM_INTEREST_WRITE)
                FD_SET(fd, &w);
            select(fd + 1, &r, &w, NULL, &tv);
        }
        nm_chat_app_step(h->app);
        tui_runtime_flush(h->rt);
    }
    ASSERT_EQ(nm_chat_app_state(h->app), NM_AGENT_RUNNING_TOOL);
    ASSERT_TRUE(app_fd(h->app) >= 0);

    /* Escape, not the interrupt message: the key is the whole path. */
    tui_runtime_send(h->rt, tui_msg_key(TUI_KEY_ESCAPE, 0, 0));
    ASSERT_EQ(nm_chat_app_state(h->app), NM_AGENT_IDLE);
    ASSERT_FALSE(tui_runtime_should_quit(h->rt));

    const char *out = harness_read(h);
    char *clean = strip_frames(out);
    ASSERT_NOT_NULL(clean);
    ASSERT_TRUE(strstr(clean, "🛑 interrupted") != NULL);
    free(clean);

    harness_free(h);
    pthread_join(th, NULL);
    close(sc.fd);
}
#endif /* !_WIN32 */

static void test_tool_round_prints_panels(void)
{
    FILE *f = fopen(FIXTURE_PATH, "wb");
    ASSERT_NOT_NULL(f);
    fputs("the quick brown fox\n", f);
    fclose(f);

    struct ServerScript sc;
    memset(&sc, 0, sizeof(sc));
    sc.n_rounds = 2;
    sc.sse[0] =
        "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,"
        "\"id\":\"call_1\",\"type\":\"function\",\"function\":"
        "{\"name\":\"edit_file\",\"arguments\":\"{\\\"path\\\":\\\"" FIXTURE_PATH
        "\\\",\\\"old_string\\\":\\\"quick brown\\\",\\\"new_string\\\":"
        "\\\"slow red\\\"}\"}}]}}]}\n\n"
        "data: [DONE]\n\n";
    sc.sse[1] =
        "data: {\"choices\":[{\"delta\":{\"content\":\"edited the file\"}}]}\n\n"
        "data: [DONE]\n\n";
    sc.fd = server_bind(&sc.port);
    ASSERT_TRUE(sc.fd >= 0);
    pthread_t th;
    pthread_create(&th, NULL, chat_server_thread, &sc);

    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1", sc.port);
    AppHarness *h = harness_new("ollama:local", "test-model", base);
    ASSERT_NOT_NULL(h);

    harness_type(h, "change quick to slow");
    harness_enter(h);
    ASSERT_EQ(harness_drive(h, 500), 0);
    ASSERT_EQ(nm_chat_app_state(h->app), NM_AGENT_DONE);

    /* The tool plan and the result line are in the scrollback. The
     * plan shows the tool name + EVERY argument (not a one-line slug),
     * so the edit's old/new strings are visible before the result. */
    const char *out = harness_read(h);
    ASSERT_TRUE(strstr(out, "edit_file") != NULL);
    /* The plan row leads with the tool's own emoji - part of the tool
     * definition (NmTool.emoji), in the tool (Comment) role - rather
     * than a shared bullet glyph. */
    ASSERT_TRUE(strstr(out, sgr16(NM_SGR_TOOL, "✏️ edit_file")) != NULL);
    ASSERT_TRUE(strstr(out, "nm-test-chat.txt") != NULL);
    ASSERT_TRUE(strstr(out, "old_string: quick brown") != NULL);
    ASSERT_TRUE(strstr(out, "new_string: slow red") != NULL);
    ASSERT_TRUE(strstr(out, "edited the file") != NULL);

    /* Principle: the plan is committed BEFORE the tool runs (so it
     * precedes the result line), the whole result body follows, and
     * the tool block ends with one blank line before the answer. */
    char *clean = strip_frames(out);
    ASSERT_NOT_NULL(clean);
    const char *plan_at = strstr(clean, "new_string: slow red");
    const char *res_at = strstr(clean, "╰─");
    ASSERT_NOT_NULL(plan_at);
    ASSERT_NOT_NULL(res_at);
    ASSERT_TRUE(plan_at < res_at);
    /* The elbow sits in its own role (Cyan), not the panel's Comment
     * and not the body's Foreground; the raw bytes are the proof. */
    ASSERT_TRUE(strstr(out, sgr16(NM_SGR_TOOL_ELBOW, "  ╰─ ")) != NULL);
    /* The result body is the full tool output, not a one-line slug. */
    ASSERT_TRUE(strstr(res_at, "Edited") != NULL ||
                strstr(res_at, "Output") != NULL);
    /* A blank row separates the tool block from the answer. */
    ASSERT_TRUE(strstr(clean, "\n\nedited the file") != NULL);
    free(clean);

    /* The file edit actually happened. */
    char content[128];
    f = fopen(FIXTURE_PATH, "rb");
    ASSERT_NOT_NULL(f);
    size_t n = fread(content, 1, sizeof(content) - 1, f);
    content[n] = '\0';
    fclose(f);
    ASSERT_STR_EQ(content, "the slow red fox\n");
    remove(FIXTURE_PATH);

    harness_free(h);
    pthread_join(th, NULL);
    close(sc.fd);
}

/* ---------------------------------------------------------------- */
/* Reasoning echo-back: opt-in, OFF by default                       */
/* ---------------------------------------------------------------- */

/* Scripts one turn whose round 1 streams a reasoning trace and then a
 * tool call, so that a second request exists to inspect; round 2
 * answers. The canned server captures only the LAST request
 * (g_request) — round 2's, the one carrying round 1's assistant
 * message back. */
static void script_reasoning_tool_turn(struct ServerScript *sc)
{
    memset(sc, 0, sizeof(*sc));
    sc->n_rounds = 2;
    sc->sse[0] =
        "data: {\"choices\":[{\"delta\":{\"content\":\"\","
        "\"reasoning_content\":\"let me think about the edit\","
        "\"reasoning_details\":[{\"type\":\"reasoning.text\","
        "\"signature\":\"sig-chat\",\"format\":\"google-gemini-v1\"}]}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,"
        "\"id\":\"call_r\",\"type\":\"function\",\"function\":"
        "{\"name\":\"read_file\",\"arguments\":\"{\\\"path\\\":\\\"" FIXTURE_PATH
        "\\\"}\"}}]}}]}\n\n"
        "data: [DONE]\n\n";
    sc->sse[1] =
        "data: {\"choices\":[{\"delta\":{\"content\":\"all done\"}}]}\n\n"
        "data: [DONE]\n\n";
}

/* Write the read_file fixture and start the scripted server; the
 * caller owns the returned harness (and must join/close). */
static AppHarness *run_reasoning_tool_turn(struct ServerScript *sc,
                                           pthread_t *th)
{
    FILE *f = fopen(FIXTURE_PATH, "wb");
    if (!f)
        return NULL;
    fputs("the quick brown fox\n", f);
    fclose(f);

    script_reasoning_tool_turn(sc);
    sc->fd = server_bind(&sc->port);
    if (sc->fd < 0)
        return NULL;
    pthread_create(th, NULL, chat_server_thread, sc);

    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1", sc->port);
    return harness_new("ollama:local", "test-model", base);
}

/* The provider hears no reasoning by default: the trace is received,
 * printed (dimmed, ahead of the answer) and kept in the session, but
 * round 2's request carries the assistant tool-call message WITHOUT
 * reasoning_content. */
static void test_reasoning_not_echoed_by_default(void)
{
    struct ServerScript sc;
    pthread_t th;
    AppHarness *h = run_reasoning_tool_turn(&sc, &th);
    ASSERT_NOT_NULL(h);

    harness_type(h, "read it");
    harness_enter(h);
    ASSERT_EQ(harness_drive(h, 500), 0);
    ASSERT_EQ(nm_chat_app_state(h->app), NM_AGENT_DONE);

    /* Receiving and showing are untouched. */
    char *clean = strip_frames(harness_read(h));
    ASSERT_NOT_NULL(clean);
    ASSERT_TRUE(strstr(clean, "let me think about the edit") != NULL);
    ASSERT_TRUE(strstr(clean, "all done") != NULL);
    free(clean);

    /* Not fed back. */
    ASSERT_TRUE(strstr(g_request, "\"tool_calls\"") != NULL);
    ASSERT_TRUE(strstr(g_request, "reasoning_content") == NULL);

    harness_free(h);
    pthread_join(th, NULL);
    close(sc.fd);
    remove(FIXTURE_PATH);
}

/* Opted in, the same turn re-sends the trace as reasoning_content on
 * the request carrying the turn. `all` is the widest mode (every
 * assistant message with a trace); `tools` would cover this request
 * too, since the assistant message carries the tool calls. */
static void test_reasoning_echo_opt_in(void)
{
    scratch_store_begin();
    struct ServerScript sc;
    pthread_t th;
    AppHarness *h = run_reasoning_tool_turn(&sc, &th);
    ASSERT_NOT_NULL(h);

    store_set(NM_CFG_KEY_REASONING_ECHO, "all");

    harness_type(h, "read it");
    harness_enter(h);
    ASSERT_EQ(harness_drive(h, 500), 0);
    ASSERT_EQ(nm_chat_app_state(h->app), NM_AGENT_DONE);

    ASSERT_TRUE(strstr(g_request, "\"tool_calls\"") != NULL);
    ASSERT_TRUE(strstr(g_request,
                       "\"reasoning_content\":\"let me think about the edit\"") != NULL);

    harness_free(h);
    pthread_join(th, NULL);
    close(sc.fd);
    remove(FIXTURE_PATH);
    scratch_store_end();
}

/* The mode is FROZEN once a request has carried a trace: /config reports
 * the mode the NEXT request will use (the frozen one, not the store's),
 * and a change to the key says out loud that it applies to the next
 * chat — the prefix cannot change shape mid-conversation (prompt cache,
 * and the replay check the echo answers). */
static void test_config_reasoning_echo_freezes_for_the_chat(void)
{
    pin_cfg_paths("frozen-echo");
    struct ServerScript sc;
    pthread_t th;
    AppHarness *h = run_reasoning_tool_turn(&sc, &th);
    ASSERT_NOT_NULL(h);
    NmConfig *cfg = cfg_for(h);

    store_set(NM_CFG_KEY_REASONING_ECHO, "tools");
    harness_type(h, "read it");
    harness_enter(h);
    ASSERT_EQ(harness_drive(h, 500), 0);
    ASSERT_EQ(nm_chat_app_state(h->app), NM_AGENT_DONE);
    /* The tool round's trace rode back, which is what freezes it. */
    ASSERT_TRUE(strstr(g_request, "\"reasoning_content\"") != NULL);
    ASSERT_EQ(nm_agent_reasoning_echo_frozen(nm_chat_app_agent(h->app)), 1);

    /* The view answers "what will the next request send", not "what
     * does the store say". */
    harness_type(h, "/config");
    harness_enter(h);
    const char *out = harness_read(h);
    ASSERT_TRUE(strstr(out, "reasoning_echo") != NULL);
    ASSERT_TRUE(strstr(out, "frozen this chat") != NULL);

    /* The change is still accepted and persisted (it is the next
     * chat's setting), and the app says so instead of looking inert. */
    harness_type(h, "/config set reasoning_echo off");
    harness_enter(h);
    out = harness_read(h);
    ASSERT_TRUE(strstr(out, "config: reasoning_echo = off") != NULL);
    ASSERT_TRUE(strstr(out, "the reasoning echo is frozen at 'tools'") !=
                NULL);
    ASSERT_STR_EQ(cfg_read_shadow(), "reasoning_echo = off\n");

    nm_config_free(cfg);
    harness_free(h);
    pthread_join(th, NULL);
    close(sc.fd);
    remove(FIXTURE_PATH);
}

/* The `signature_echo` row and its freeze line. This harness runs
 * `ollama:local`, which declares nothing, so /config shows the store's
 * value until a request actually carries the field, then the frozen one
 * — and a change says it applies to the next chat. (The PROVIDER-DEFAULT
 * branch — openrouter's `tools` — is pinned by
 * test_config_shows_provider_default_signature below.) */
static void test_config_signature_echo_row_and_freeze(void)
{
    pin_cfg_paths("signature-echo");
    struct ServerScript sc;
    pthread_t th;
    AppHarness *h = run_reasoning_tool_turn(&sc, &th);
    ASSERT_NOT_NULL(h);
    NmConfig *cfg = cfg_for(h);

    /* Before anything has been sent: the built-in default. */
    harness_type(h, "/config");
    harness_enter(h);
    const char *out = harness_read(h);
    ASSERT_NOT_NULL(strstr(out, "signature_echo"));
    ASSERT_NOT_NULL(strstr(out, "signature_echo       off"));

    store_set(NM_CFG_KEY_SIGNATURE_ECHO, "tools");
    harness_type(h, "read it");
    harness_enter(h);
    ASSERT_EQ(harness_drive(h, 500), 0);
    ASSERT_EQ(nm_chat_app_state(h->app), NM_AGENT_DONE);
    /* Round 1's array rode back (the tool message carries the array, and
     * nothing else: the trace echo stayed at its own default). */
    ASSERT_TRUE(strstr(g_request, "reasoning_details") != NULL);
    ASSERT_TRUE(strstr(g_request, "reasoning_content") == NULL);
    ASSERT_EQ(nm_agent_signature_echo_frozen(nm_chat_app_agent(h->app)), 1);

    /* The row reports the FROZEN mode from here on. */
    harness_type(h, "/config");
    harness_enter(h);
    out = harness_read(h);
    ASSERT_NOT_NULL(strstr(out, "signature_echo       tools"));
    ASSERT_NOT_NULL(strstr(out, "frozen this chat"));

    /* A change is accepted (next chat's setting) and called out. */
    harness_type(h, "/config set signature_echo all");
    harness_enter(h);
    out = harness_read(h);
    ASSERT_NOT_NULL(strstr(out, "config: signature_echo = all"));
    ASSERT_NOT_NULL(strstr(out, "the signature echo is frozen at 'tools'"));
    ASSERT_STR_EQ(cfg_read_shadow(), "signature_echo = all\n");

    nm_config_free(cfg);
    harness_free(h);
    pthread_join(th, NULL);
    close(sc.fd);
    remove(FIXTURE_PATH);
}

/* The same row when the PROVIDER dictates the mode: openrouter declares
 * `tools` (Gemini's thought signature has to ride back with the tool
 * result), so with the store at its built-in default the row shows
 * `tools` + "(provider default)" — the declaration is what the next
 * request uses, not the `off` the bare key would report. A user's key
 * overrides it, and then the row shows the store's value instead. */
static void test_config_shows_provider_default_signature(void)
{
    pin_cfg_paths("sig-provdefault");
    AppHarness *h = harness_new("openrouter", "google/gemini-3.8-flash", NULL);
    ASSERT_NOT_NULL(h);
    NmConfig *cfg = cfg_for(h);

    harness_type(h, "/config");
    harness_enter(h);
    const char *out = harness_read(h);
    ASSERT_NOT_NULL(strstr(out, "signature_echo"));
    ASSERT_NOT_NULL(strstr(out, "signature_echo       tools"));
    ASSERT_NOT_NULL(strstr(out, "(provider default)"));

    /* The user overrides the declaration: the row goes back to the
     * store's value (no provider-default label any more). Only the
     * freshly printed block counts — harness_read hands back the whole
     * transcript, earlier /config rows included. */
    size_t seen = strlen(out);
    store_set(NM_CFG_KEY_SIGNATURE_ECHO, "off");
    harness_type(h, "/config");
    harness_enter(h);
    out = harness_read(h);
    const char *tail = out + (seen < strlen(out) ? seen : 0);
    ASSERT_NOT_NULL(strstr(tail, "signature_echo       off"));
    ASSERT_NULL(strstr(tail, "(provider default)"));
    store_clear(NM_CFG_KEY_SIGNATURE_ECHO);

    nm_config_free(cfg);
    harness_free(h);
}

/* Parallel tool calls arrive in ONE assistant message (several entries
 * in the wire tool_calls array) but nevermore runs them sequentially.
 * The transcript must say so: each call's plan is committed right
 * before that call's own result — plan1, result1, plan2, result2 —
 * instead of every plan in the round piling up first, which read as
 * "two tools started at once, results interleaved". */
static void test_parallel_tool_calls_pair_plan_with_result(void)
{
    FILE *f = fopen("nm-p1.txt", "wb");
    ASSERT_NOT_NULL(f);
    fputs("alpha body\n", f);
    fclose(f);
    f = fopen("nm-p2.txt", "wb");
    ASSERT_NOT_NULL(f);
    fputs("beta body\n", f);
    fclose(f);

    struct ServerScript sc;
    memset(&sc, 0, sizeof(sc));
    sc.n_rounds = 2;
    sc.sse[0] =
        "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,"
        "\"id\":\"call_a\",\"type\":\"function\",\"function\":"
        "{\"name\":\"read_file\",\"arguments\":"
        "\"{\\\"path\\\":\\\"nm-p1.txt\\\"}\"}}]}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":1,"
        "\"id\":\"call_b\",\"type\":\"function\",\"function\":"
        "{\"name\":\"read_file\",\"arguments\":"
        "\"{\\\"path\\\":\\\"nm-p2.txt\\\"}\"}}]}}]}\n\n"
        "data: [DONE]\n\n";
    sc.sse[1] =
        "data: {\"choices\":[{\"delta\":{\"content\":\"read both\"}}]}\n\n"
        "data: [DONE]\n\n";
    sc.fd = server_bind(&sc.port);
    ASSERT_TRUE(sc.fd >= 0);
    pthread_t th;
    pthread_create(&th, NULL, chat_server_thread, &sc);

    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1", sc.port);
    AppHarness *h = harness_new("ollama:local", "test-model", base);
    ASSERT_NOT_NULL(h);

    harness_type(h, "read both files");
    harness_enter(h);
    ASSERT_EQ(harness_drive(h, 500), 0);
    ASSERT_EQ(nm_chat_app_state(h->app), NM_AGENT_DONE);

    const char *out = harness_read(h);
    char *clean = strip_frames(out);
    ASSERT_NOT_NULL(clean);

    /* Distinct path + body per call, so each occurrence's position
     * identifies which call it belongs to. */
    const char *plan1 = strstr(clean, "path: nm-p1.txt");
    const char *res1 = strstr(clean, "alpha body");
    const char *plan2 = strstr(clean, "path: nm-p2.txt");
    const char *res2 = strstr(clean, "beta body");
    const char *plan2hdr = strstr(clean, "📖 read_file\n  path: nm-p2.txt");
    const char *ans = strstr(clean, "read both\n");
    ASSERT_NOT_NULL(plan1);
    ASSERT_NOT_NULL(res1);
    ASSERT_NOT_NULL(plan2);
    ASSERT_NOT_NULL(res2);
    ASSERT_NOT_NULL(plan2hdr);
    ASSERT_NOT_NULL(ans);
    ASSERT_TRUE(plan1 < res1); /* the plan precedes its own run */
    ASSERT_TRUE(res1 < plan2); /* and the next plan is not preloaded */
    ASSERT_TRUE(plan2 < res2);
    ASSERT_TRUE(plan2hdr < ans);

    /* One blank line closes EACH tool block (principle 4): block 1's
     * blank sits immediately before block 2's plan header, block 2's
     * immediately before the answer — and no blank splits a plan from
     * its own result (a plan + its result are one block). */
    ASSERT_TRUE(plan2hdr[-1] == '\n' && plan2hdr[-2] == '\n');
    ASSERT_TRUE(ans[-1] == '\n' && ans[-2] == '\n');
    ASSERT_TRUE(!has_blank_row(plan1, res1));
    ASSERT_TRUE(!has_blank_row(plan2, res2));
    free(clean);

    /* Both results reach the wire in the ONE follow-up request, each as
     * its own tool message, in call order (g_request holds the last
     * round's request body). */
    const char *tc_a = strstr(g_request, "\"tool_call_id\":\"call_a\"");
    const char *tc_b = strstr(g_request, "\"tool_call_id\":\"call_b\"");
    ASSERT_NOT_NULL(tc_a);
    ASSERT_NOT_NULL(tc_b);
    ASSERT_TRUE(tc_a < tc_b);
    ASSERT_TRUE(strstr(g_request, "alpha body") != NULL);
    ASSERT_TRUE(strstr(g_request, "beta body") != NULL);

    remove("nm-p1.txt");
    remove("nm-p2.txt");
    harness_free(h);
    pthread_join(th, NULL);
    close(sc.fd);
}

/* The real wire pattern behind the 2026-09-18 "eaten description"
 * report (dumps 094157/094235/094301): round 1 streams content whose
 * tail is an UNCLOSED ```json fence quoting the tool description, then
 * the tool call. The unclosed fence's body lives in the live region
 * until the tool round begins; the report read its transient frame
 * rows as "committed then deleted". The flash itself was a spinner OOB
 * read (test_spinner), not the transcript.
 *
 * This test pins the transcript half of that story so the two cannot be
 * confused again: the pending fence body DOES reach the scrollback,
 * committed in order before the tool plan, and the transient live frame
 * carries it meanwhile. */
static void test_open_json_fence_before_tool_call_commits(void)
{
    struct ServerScript sc;
    memset(&sc, 0, sizeof(sc));
    sc.n_rounds = 2;
    sc.sse[0] =
        "data: {\"choices\":[{\"delta\":{\"content\":\"Let me check.\\n\"}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{\"content\":\"```json\\n\"}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{\"content\":\"{\\n  \\\"description\\\": "
        "\\\"Run a shell command and capture its combined output and exit "
        "status\\\"\\n}\"}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,"
        "\"id\":\"call_d\",\"type\":\"function\",\"function\":"
        "{\"name\":\"run_command\",\"arguments\":"
        "\"{\\\"cmd\\\":\\\"sleep 0.2; echo done-check\\\"}\"}}]}}]}\n\n"
        "data: [DONE]\n\n";
    sc.sse[1] =
        "data: {\"choices\":[{\"delta\":{\"content\":\"Done. The tool said "
        "hello.\"}}]}\n\n"
        "data: [DONE]\n\n";
    /* Rendezvous: one delta per step (the unclosed fence's body must be
     * seen growing in the live region, not delivered in one burst). */
    server_paced_init(&sc);
    sc.fd = server_bind(&sc.port);
    ASSERT_TRUE(sc.fd >= 0);
    pthread_t th;
    pthread_create(&th, NULL, chat_server_thread, &sc);

    char base2[64];
    snprintf(base2, sizeof(base2), "http://127.0.0.1:%d/v1", sc.port);
    AppHarness *h = harness_new("ollama:local", "test-model", base2);
    ASSERT_NOT_NULL(h);
    h->pacer = &sc;

    harness_type(h, "check it");
    harness_enter(h);

    /* Mid-stream: step until the fence body is on the live frame.
     * (The FLASH itself was the spinner's tier-switch OOB read, pinned
     * deterministically in test_spinner.c; here we pin the other half
     * of the story — those bytes are real transcript data, not a stray
     * rodata leak.) */
    for (int i = 0; i < 300; i++) {
        if (nm_chat_app_state(h->app) != NM_AGENT_STREAMING)
            break;
        harness_step_once(h);
        const char *out = harness_read(h);
        if (strstr(out, "Run a shell command"))
            break;
    }
    printf("  mid-stream: description visible=%s\n",
           strstr(harness_read(h), "Run a shell command") ? "yes" : "no");

    /* Drive to completion the way the loop would. */
    ASSERT_EQ(harness_drive(h, 500), 0);
    ASSERT_EQ(nm_chat_app_state(h->app), NM_AGENT_DONE);

    const char *out = harness_read(h);
    char *clean = strip_frames(out);
    ASSERT_NOT_NULL(clean);
    /* ...and it must STAY: the unclosed fence's body commits at the
     * round boundary (stream_end finalizes it), before the tool plan. */
    const char *desc = strstr(clean, "Run a shell command");
    const char *plan = strstr(clean, "run_command");
    ASSERT_NOT_NULL(desc);
    ASSERT_NOT_NULL(plan);
    ASSERT_TRUE(desc < plan);
    free(clean);
    /* The turn's answer is also present. */
    ASSERT_TRUE(strstr(out, "Done. The tool said hello.") != NULL);

    server_release(&sc);
    harness_free(h);
    pthread_join(th, NULL);
    close(sc.fd);
}

static void test_tab_on_slash_prefix_opens_commands_popup(void)
{
    AppHarness *h = harness_new("ollama:cloud", "gpt-oss:20b", NULL);
    ASSERT_NOT_NULL(h);

    /* Regression (TUI crash): Tab after "/m" matches several slash
     * commands, so the commands popup opens with the filter applied.
     * The old loop strncmp'd the array's NULL sentinel here. With
     * the one-command-per-noun set, "/" is the multi-match prefix
     * (/help /model /provider /quit) — the same code path. */
    harness_type(h, "/");
    tui_runtime_send(h->rt, tui_msg_key(TUI_KEY_TAB, 0, 0));
    tui_runtime_flush(h->rt);

    const char *frame = tui_runtime_render(h->rt);
    ASSERT_TRUE(strstr(frame, "/model") != NULL);
    ASSERT_TRUE(strstr(frame, "/provider") != NULL);
    /* The process-job commands are in the completion set too. */
    ASSERT_TRUE(strstr(frame, "/ps") != NULL);
    ASSERT_TRUE(strstr(frame, "/kill") != NULL);
    /* And the context gauge's on-demand breakdown. */
    ASSERT_TRUE(strstr(frame, "/context") != NULL);
    /* The retired plurals are gone from the completion set. */
    ASSERT_TRUE(strstr(frame, "/models") == NULL);
    ASSERT_TRUE(strstr(frame, "/providers") == NULL);
    /* The input text is unchanged (several matches: popup, no insert). */
    ASSERT_STR_EQ(tui_textinput_text(nm_chat_app_textinput(h->app)), "/");

    /* Escape dismisses the popup. */
    tui_runtime_send(h->rt, tui_msg_key(TUI_KEY_ESCAPE, 0, 0));
    frame = tui_runtime_render(h->rt);
    ASSERT_TRUE(strstr(frame, "/model") == NULL);

    harness_free(h);
}

static void test_tab_single_match_inserts_completion(void)
{
    AppHarness *h = harness_new("ollama:cloud", "gpt-oss:20b", NULL);
    ASSERT_NOT_NULL(h);

    /* "/he" matches exactly one command: Tab completes to "/help". */
    harness_type(h, "/he");
    tui_runtime_send(h->rt, tui_msg_key(TUI_KEY_TAB, 0, 0));
    tui_runtime_flush(h->rt);

    ASSERT_STR_EQ(tui_textinput_text(nm_chat_app_textinput(h->app)), "/help");
    /* No popup for a unique match. */
    const char *frame = tui_runtime_render(h->rt);
    ASSERT_TRUE(strstr(frame, "/models") == NULL);

    harness_free(h);
}

static void test_tab_on_plain_word_is_a_noop(void)
{
    AppHarness *h = harness_new("ollama:cloud", "gpt-oss:20b", NULL);
    ASSERT_NOT_NULL(h);

    /* Regression (TUI crash): Tab after a NON-slash word emitted
     * TAB_COMPLETE and the app matched against garbage — ASan/UBSan:
     * "null pointer passed as argument 1" in strncmp. */
    harness_type(h, "he");
    tui_runtime_send(h->rt, tui_msg_key(TUI_KEY_TAB, 0, 0));
    tui_runtime_flush(h->rt);

    /* No popup, text untouched, app still alive and idle. */
    ASSERT_STR_EQ(tui_textinput_text(nm_chat_app_textinput(h->app)), "he");
    ASSERT_EQ(nm_chat_app_state(h->app), NM_AGENT_IDLE);

    /* Tab at empty input (prefix NULL): same no-op. */
    tui_textinput_clear(nm_chat_app_textinput(h->app));
    tui_runtime_send(h->rt, tui_msg_key(TUI_KEY_TAB, 0, 0));
    tui_runtime_flush(h->rt);
    ASSERT_EQ(nm_chat_app_state(h->app), NM_AGENT_IDLE);

    harness_free(h);
}

/* Count TRANSCRIPT occurrences of a completed line in the harness's
 * raw byte stream. The stream legitimately contains the same words as
 * live-region frame rows (the raw file keeps them); a transcript
 * line's signature is `text\r\n` NOT
 * followed by an EL (frame rows are `\r\n`-separated and each row
 * ends before an `\x1b[K`). This is the file-level stand-in for
 * "count on the rendered screen", per the bug report's review. */
/* Count transcript lines matching `line`, ignoring SGR styling around
 * the matched text (the renderer now styles rows). A match is a
 * transcript line when, after the needle, only SGR bytes remain before
 * the \r\n and the row is not a live frame row (the next bytes are not
 * an EL). */
static size_t count_transcript_line(const char *hay, const char *line)
{
    size_t n = 0;
    const char *p = hay;
    size_t ll = strlen(line);
    while ((p = strstr(p, line)) != NULL) {
        const char *eol = p + ll;
        while (*eol == '\x1b' && eol[1] == '[') {
            const char *q = eol + 2;
            while (*q && *q != 'm')
                q++;
            if (*q != 'm')
                break;
            eol = q + 1;
        }
        if (strncmp(eol, "\r\n", 2) == 0 &&
            strncmp(eol + 2, "\x1b[K", 3) != 0)
            n++;
        p = eol;
    }
    return n;
}

/* Regression (bugs/streaming-content-rendered-multiple-times.md):
 * a delta batch that completes a line while the partial tail spans
 * multiple wrapped rows. The transcript seam must print each
 * completed line exactly once — no stale partial prefixes stranded
 * in the scrollback. Two hard requirements from the review:
 *   - the width is FORCED (a multi-row live tail is the trigger; the
 *     app's default 80 would not wrap this fixture). Committed lines
 *     are no longer width-wrapped — the terminal owns wrapping — but
 *     the live region still lays out in explicit rows.
 *   - the assertion counts occurrences of the framed byte sequence
 *     (frame bytes legitimately contain the partial tail; only a
 *     count on the transcript's "\r\n"-framed form can see strays)
 * The interleaving hazard is exercised directly: two steps print
 * with no intervening runtime flush (the run loop dispatches the
 * external fd before its wakeup drain). */
static void test_streaming_multiline_no_duplicate_transcript(void)
{
    struct ServerScript sc;
    memset(&sc, 0, sizeof(sc));
    sc.n_rounds = 1;
    /* Long completed line (committed as one run), then a
     * newline-bearing delta whose REMAINDER must stay in the live
     * tail, then more partial growth on the same line. */
    sc.sse[0] =
        "data: {\"choices\":[{\"delta\":{\"content\":\""
        "First completed line long enough to wrap at the forced "
        "terminal width several times over for sure here\\n\\n\"}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{\"content\":\""
        "## The others\"}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{\"content\":\",\"}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{\"content\":\" for "
        "contrast\\n- `history.c` is next\"}}]}\n\n"
        "data: [DONE]\n\n";
    /* One delta per step (rendezvous), so the mid-stream loop below
     * sees the partial tail grow instead of the whole round at once. */
    server_paced_init(&sc);
    sc.fd = server_bind(&sc.port);
    ASSERT_TRUE(sc.fd >= 0);
    pthread_t th;
    pthread_create(&th, NULL, chat_server_thread, &sc);

    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1", sc.port);
    AppHarness *h = harness_new("ollama:local", "test-model", base);
    ASSERT_NOT_NULL(h);
    h->pacer = &sc;

    /* Force the geometry: narrow enough that the partial tail and the
     * completed line both wrap. */
    tui_runtime_send(h->rt, tui_msg_window_size(30, 8));
    tui_runtime_drain(h->rt);
    tui_runtime_flush(h->rt);

    harness_type(h, "review");
    harness_enter(h);

    /* Drive a few steps MID-STREAM with no interleaved runtime flush
     * (the ext-fd dispatch runs before the wakeup drain in run()):
     * the transcript print happens inside the step, so two steps can
     * print back-to-back before any flush. */
    for (int i = 0; i < 40; i++) {
        NmAgentState st = nm_chat_app_state(h->app);
        if (st != NM_AGENT_STREAMING && st != NM_AGENT_RUNNING_TOOL)
            break;
        int fd = app_fd(h->app);
        fd_set fds;
        struct timeval tv = { 0, 50 * 1000 };
        FD_ZERO(&fds);
        FD_SET(fd, &fds);
        select(fd + 1, &fds, NULL, NULL, &tv);
        harness_step_ack(h); /* prints internally, NO flush here */
    }
    tui_runtime_flush(h->rt);
    ASSERT_EQ(harness_drive(h, 500), 0);

    const char *out = harness_read(h);
    if (getenv("NM_DUMP_OUT")) {
        FILE *d = fopen(getenv("NM_DUMP_OUT"), "wb");
        if (d) {
            fwrite(out, 1, strlen(out), d);
            fclose(d);
        }
    }
    /* Each completed transcript line appears EXACTLY once in the
     * transcript sense (line-\r\n-terminated, not a frame row). The
     * long line is committed as ONE byte run: boba no longer bakes a
     * width break into committed bytes (the terminal owns wrapping and
     * reflow), so there are no wrap fragments to count. */
    ASSERT_EQ(count_transcript_line(out, "## The others, for contrast"), 1u);
    /* The list item now carries inline spans (`history.c` is a code
     * span), so match its unstyled suffix. */
    ASSERT_EQ(count_transcript_line(out, " is next"), 1u);
    /* The whole logical line, once, intact across the forced width. */
    ASSERT_EQ(count_transcript_line(
                  out,
                  "First completed line long enough to wrap at the forced "
                  "terminal width several times over for sure here"),
              1u);
    /* No wrap fragment: the line's head is never its own row (with the
     * old width wrap it was, and "First completed line long enou" ended
     * a committed fragment). */
    ASSERT_EQ(count_transcript_line(out, "First completed line long enou"), 0u);
    /* No stale partial prefixes stranded as transcript lines: only
     * the completed full line exists, never an intermediate prefix
     * as its own \r\n-terminated line. */
    ASSERT_EQ(count_transcript_line(out, "## The others,"), 0u);
    ASSERT_EQ(count_transcript_line(out, "## The others"), 0u);

    server_release(&sc);
    harness_free(h);
    pthread_join(th, NULL);
    close(sc.fd);
}

/* Regression (the 2026-09-16 report: "boba/nevermore is making
 * linebreaks at terminal width"): reasoning streamed one SSE delta per
 * token, a fence body line split across deltas. Every line inside the
 * fence landed on its own transcript row, so
 *   nevermore -p opencode:go -m deepseek-v4.1-flash
 * printed as never/more/ /-/p/ open/code/: /go...
 *
 * Root cause: nm_chat_app_on_delta decided "the reasoning stream is
 * still live" with tui_transcript_stream_raw_len(...) > 0. A buffer
 * length is not a liveness signal — boba's trim keeps the last
 * completed line (prev_off is part of the watermark), so the guard
 * stays true for the REST of the turn. Every content delta therefore
 * sent stream_end(reasoning), and stream_end on ANY stream runs
 * transcript_close_row — which closes the shared staging row. That is
 * invisible between rendered units (each unit already ends its own
 * row) but corrupts the byte-emitted path (an unlabeled fence's bytes
 * are staged incrementally and deliberately left row-open), so the
 * fence body broke at every delta:
 *   nevermore -p opencode:go -m deepseek-v4.1-flash
 * printed as never / more / " -" / p / open / code...
 *
 * The fix is an explicit per-turn flag (reasoning_open). The test
 * streams reasoning first (making the old guard true), then content
 * whose unlabeled fence body line is split across deltas, and asserts
 * the line survives as ONE row. */
/* Does `frag` reach the scrollback as an APPEND to the row it
 * continues? boba's extend path writes
 *   ESC [ 1 A  CR  ESC [ <digits> C  <text>
 * with the text IMMEDIATELY after the cursor-forward. The bug inserted
 * a row break between them (a spurious transcript_close_row), so the
 * fragment landed on the NEXT row as its own CRLF-framed line:
 *   ESC [ 1 A  CR  ESC [ <digits> C  CRLF  <text>
 * So: an extension is the cursor move directly followed by the
 * fragment; the row-break form is the bug. */
static int fragment_extends_row(const char *hay, const char *frag)
{
    const char *p = hay;
    while ((p = strstr(p, "\x1b[1A\r\x1b[")) != NULL) {
        const char *d = p + 7; /* past ESC[1A CR ESC[ */
        while (*d >= '0' && *d <= '9')
            d++;
        if (*d != 'C') {
            p++;
            continue;
        }
        if (strncmp(d + 1, frag, strlen(frag)) == 0)
            return 1;
        p++;
    }
    return 0;
}

static void test_fence_line_not_split_by_reasoning_stream_end(void)
{
    AppHarness *h = harness_new("ollama:local", "test-model", NULL);
    ASSERT_NOT_NULL(h);
    tui_runtime_send(h->rt, tui_msg_window_size(130, 30));
    tui_runtime_drain(h->rt);
    tui_runtime_flush(h->rt);

    /* Reasoning must leave bytes RETAINED in its raw buffer, which is
     * what the old guard keyed on: the reference dump's reasoning ends
     * mid-line (the content phase starts before the line's newline
     * arrives), so the partial tail keeps raw_len > 0. */
    nm_chat_app_on_delta(NM_STREAM_REASONING,
                         "thinking about the Go toolchain", NULL, 0, NULL);
    tui_runtime_flush(h->rt);

    /* Content: the fence opener, then the body line split token by
     * token exactly as the reference dump's SSE does, then the
     * closer. Every one of these deltas is CONTENT while (under the
     * old logic) the reasoning stream still read as live. */
    const char *c[] = { "```\nnever", "more", " -", "p", " open",
                        "code", ":go -m deepseek", "-v4.1-flash",
                        "\n```\n" };
    for (size_t i = 0; i < sizeof(c) / sizeof(c[0]); i++) {
        nm_chat_app_on_delta(NM_STREAM_CONTENT, c[i], NULL, 0, NULL);
        tui_runtime_flush(h->rt);
    }

    const char *out = harness_read(h);
    if (getenv("NM_DUMP_OUT")) {
        FILE *d = fopen(getenv("NM_DUMP_OUT"), "wb");
        if (d) {
            fwrite(out, 1, strlen(out), d);
            fclose(d);
        }
    }
    /* The bug's signature: each fragment became its own CRLF-framed
     * transcript row, so the fence body reads as "```\r\nnever\r\nmore
     * \r\n -\r\np...". A correct run keeps the row open across deltas:
     * the FIRST fragment ends the opener's row, and every later one is
     * appended to the growing row (the extend path). */
    ASSERT_TRUE(strstr(out, "```\r\nnever\r\nmore") == NULL);
    ASSERT_TRUE(fragment_extends_row(out, "more"));
    ASSERT_TRUE(fragment_extends_row(out, " -"));
    ASSERT_TRUE(fragment_extends_row(out, "p"));
    ASSERT_TRUE(fragment_extends_row(out, " open"));
    ASSERT_TRUE(fragment_extends_row(out, "code"));

    harness_free(h);
}

/* The D10 echo guard: submit finalizes LIVE blocks and does NOT echo
 * the user line through the transcript, and finish_inline is the one
 * echo (a frame persist — the rendered input row is not CRLF-framed,
 * so it is not a transcript line). A second committed copy would be
 * exactly the 2026-09-13 double-print; count it zero. */
static void test_submit_echoes_once(void)
{
    struct ServerScript sc;
    memset(&sc, 0, sizeof(sc));
    sc.n_rounds = 1;
    sc.sse[0] =
        "data: {\"choices\":[{\"delta\":{\"content\":\"ok\"}}]}\n\n"
        "data: [DONE]\n\n";
    sc.fd = server_bind(&sc.port);
    ASSERT_TRUE(sc.fd >= 0);
    pthread_t th;
    pthread_create(&th, NULL, chat_server_thread, &sc);

    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1", sc.port);
    AppHarness *h = harness_new("ollama:local", "test-model", base);
    ASSERT_NOT_NULL(h);

    harness_type(h, "unique-user-line");
    harness_enter(h);
    ASSERT_EQ(harness_drive(h, 500), 0);

    const char *out = harness_read(h);
    /* The line was echoed (frame-persisted) ... */
    ASSERT_TRUE(strstr(out, "unique-user-line") != NULL);
    /* ... but never as a committed transcript line (no double echo). */
    ASSERT_EQ(count_transcript_line(out, "unique-user-line"), 0u);

    harness_free(h);
    pthread_join(th, NULL);
    close(sc.fd);
}

/* The status line is live chrome (spinner + gauge + separator rule), not
 * history: it must never be finalized into the scrollback. The frame
 * finish_inline persists is the echoed prompt row ALONE — the chrome row
 * sits directly above it in the live frame, so finishing the whole frame
 * would strand a `--- -/- ---…` row in the scrollback above every
 * submitted line. The tell is the byte immediately before the echoed
 * row's prompt span: a CLEARED row (`\r\x1b[K`, the textinput's own
 * erase) with the fix, versus the status row's `\r\n\x1b[K` separator
 * without it (the rule would sit where the `\n` is). */
static void test_submit_does_not_finalize_status_line(void)
{
    struct ServerScript sc;
    memset(&sc, 0, sizeof(sc));
    sc.n_rounds = 1;
    sc.sse[0] =
        "data: {\"choices\":[{\"delta\":{\"content\":\"ok\"}}]}\n\n"
        "data: [DONE]\n\n";
    sc.fd = server_bind(&sc.port);
    ASSERT_TRUE(sc.fd >= 0);
    pthread_t th;
    pthread_create(&th, NULL, chat_server_thread, &sc);

    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1", sc.port);
    AppHarness *h = harness_new("ollama:local", "test-model", base);
    ASSERT_NOT_NULL(h);

    harness_type(h, "unique-user-line");
    harness_enter(h);
    ASSERT_EQ(harness_drive(h, 500), 0);

    const char *out = harness_read(h);
    /* The submit echo is the LAST render of the line (the input is
     * cleared on submit, so nothing later repaints it). */
    static const char line[] = "unique-user-line";
    const char *echo = NULL;
    for (const char *q = out + strlen(out); q > out; q--) {
        if (strncmp(q - 1, line, sizeof(line) - 1) == 0) {
            echo = q - 1;
            break;
        }
    }
    ASSERT_NOT_NULL(echo);
    char *p = span_bytes(nm_color_prompt(NM_COLOR_16), "\xe2\x9d\xaf ");
    ASSERT_NOT_NULL(p);
    size_t plen = strlen(p);
    /* The prompt span that opens the echoed row. */
    const char *pp = NULL;
    for (const char *q = echo; q > out; q--) {
        if (strncmp(q, p, plen) == 0) {
            pp = q;
            break;
        }
    }
    free(p);
    ASSERT_NOT_NULL(pp);
    ASSERT_TRUE(pp - out >= 4);
    /* The row opens on a plain erase (`\r\x1b[K`)... */
    ASSERT_TRUE(memcmp(pp - 3, "\x1b[K", 3) == 0);
    /* ...whose carriage return is a bare `\r`, not the `\n` of the
     * status row's `\r\n\x1b[K` separator — i.e. no chrome row above. */
    ASSERT_EQ(pp[-4], '\r');

    harness_free(h);
    pthread_join(th, NULL);
    close(sc.fd);
}

/* A provider switch resets the transcript (emits nothing) and prints a
 * separator line marking the boundary. */
static void test_provider_switch_clears_and_prints_separator(void)
{
    AppHarness *h = harness_new("ollama:cloud", "gpt-oss:20b", NULL);
    ASSERT_NOT_NULL(h);

    harness_type(h, "/provider ollama:local");
    harness_enter(h);
    ASSERT_STR_EQ(nm_chat_app_provider(h->app), "ollama:local");
    const char *out = harness_read(h);
    ASSERT_TRUE(strstr(out, "provider: ollama:local (fresh session)") != NULL);

    harness_free(h);
}

/* Regression: a /provider switch must resolve the NEW provider's key.
 * build_agent used to hand each agent app->api_key — the key resolved
 * once for the startup provider — so a switch carried the previous
 * provider's key (or, with no override, none at all) to the new
 * endpoint. */
static void test_provider_switch_resolves_new_provider_key(void)
{
    struct ServerScript sc;
    memset(&sc, 0, sizeof(sc));
    sc.n_rounds = 1;
    sc.sse[0] =
        "data: {\"choices\":[{\"delta\":{\"content\":\"ok\"}}]}\n\n"
        "data: [DONE]\n\n";
    sc.fd = server_bind(&sc.port);
    ASSERT_TRUE(sc.fd >= 0);
    pthread_t th;
    pthread_create(&th, NULL, chat_server_thread, &sc);

    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1", sc.port);

    /* A keyed startup provider with its own key. */
    test_setenv("OPENROUTER_API_KEY", "sk-old-openrouter");
    AppHarness *h = harness_new("openrouter", "test-model", base);
    ASSERT_NOT_NULL(h);

    /* Switch to a different keyed provider carrying a different key. */
    test_setenv("HYPER_API_KEY", "sk-new-hyper");
    g_request[0] = '\0';
    harness_type(h, "/provider hyper");
    harness_enter(h);
    ASSERT_STR_EQ(nm_chat_app_provider(h->app), "hyper");

    /* The next turn must authorize with the NEW provider's key. */
    harness_type(h, "hi");
    harness_enter(h);
    ASSERT_EQ(harness_drive(h, 500), 0);
    ASSERT_TRUE(strstr(g_request,
                       "Authorization: Bearer sk-new-hyper") != NULL);
    ASSERT_TRUE(strstr(g_request, "sk-old-openrouter") == NULL);

    harness_free(h);
    pthread_join(th, NULL);
    close(sc.fd);
    test_unsetenv("OPENROUTER_API_KEY");
    test_unsetenv("HYPER_API_KEY");
}

/* The app installs its per-stream highlighter state as the
 * transcript's user_data, so a labeled fence body streamed through the
 * app's own delta path carries token colors (keyword Pink, number
 * Orange) — in the TIER the app resolved. Both are driven here: the
 * truecolor harness gets the spec's hexes, the 16-color one the spec's
 * palette slots, which is the property the render state's depth exists
 * for. */
static void test_fence_body_tokens_highlighted_through_app(void)
{
    AppHarness *h = harness_new_rgb("ollama:local", "test-model", NULL);
    ASSERT_NOT_NULL(h);
    tui_runtime_send(h->rt, tui_msg_window_size(130, 30));
    tui_runtime_drain(h->rt);
    tui_runtime_flush(h->rt);

    nm_chat_app_on_delta(NM_STREAM_CONTENT, "```c\n", NULL, 0, NULL);
    tui_runtime_flush(h->rt);
    nm_chat_app_on_delta(NM_STREAM_CONTENT, "int x = 42;\n", NULL, 0, NULL);
    tui_runtime_flush(h->rt);
    nm_chat_app_on_delta(NM_STREAM_CONTENT, "```\n", NULL, 0, NULL);
    tui_runtime_flush(h->rt);

    const char *out = harness_read(h);
    /* keyword Pink #FF79C6 = 255;121;198 */
    ASSERT_TRUE(strstr(out, "\x1b[0;38;2;255;121;198mint") != NULL);
    /* number Orange #FFB86C = 255;184;108 */
    ASSERT_TRUE(strstr(out, "\x1b[0;38;2;255;184;108m42") != NULL);

    harness_free(h);

    /* The same stream on a 16-color terminal: the palette slots, and no
     * 24-bit sequence anywhere. */
    AppHarness *h16 = harness_new("ollama:local", "test-model", NULL);
    ASSERT_NOT_NULL(h16);
    tui_runtime_send(h16->rt, tui_msg_window_size(130, 30));
    tui_runtime_drain(h16->rt);
    tui_runtime_flush(h16->rt);

    nm_chat_app_on_delta(NM_STREAM_CONTENT, "```c\n", NULL, 0, NULL);
    tui_runtime_flush(h16->rt);
    nm_chat_app_on_delta(NM_STREAM_CONTENT, "int x = 42;\n", NULL, 0, NULL);
    tui_runtime_flush(h16->rt);
    nm_chat_app_on_delta(NM_STREAM_CONTENT, "```\n", NULL, 0, NULL);
    tui_runtime_flush(h16->rt);

    out = harness_read(h16);
    /* keyword Pink -> AnsiMagenta (5) -> SGR 35 */
    ASSERT_TRUE(strstr(out, "\x1b[0;35mint") != NULL);
    /* number Orange -> AnsiBrightYellow (11) -> SGR 93 */
    ASSERT_TRUE(strstr(out, "\x1b[0;93m42") != NULL);
    ASSERT_TRUE(strstr(out, "38;2;") == NULL);

    harness_free(h16);
}

/* Phase-sequential reasoning then content: both commit, in that order,
 * on their own streams. */
static void test_reasoning_and_content_commit_in_order(void)
{
    struct RawResponse rr = {
        0, 0,
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/event-stream\r\n"
        "Connection: close\r\n\r\n"
        "data: {\"choices\":[{\"delta\":{\"content\":\"\","
        "\"reasoning_content\":\"first thought\\n\"}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{\"content\":\"final words\"}}]}\n\n"
        "data: [DONE]\n\n"
    };
    rr.fd = server_bind(&rr.port);
    ASSERT_TRUE(rr.fd >= 0);
    pthread_t th;
    pthread_create(&th, NULL, raw_responder_thread, &rr);

    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1", rr.port);
    AppHarness *h = harness_new("ollama:local", "test-model", base);
    ASSERT_NOT_NULL(h);

    harness_type(h, "order");
    harness_enter(h);
    ASSERT_EQ(harness_drive(h, 500), 0);

    const char *out = harness_read(h);
    const char *r = strstr(out, "first thought");
    const char *c = strstr(out, "final words");
    ASSERT_NOT_NULL(r);
    ASSERT_NOT_NULL(c);
    ASSERT_TRUE(r < c);

    harness_free(h);
    pthread_join(th, NULL);
    close(rr.fd);
}

/* End-to-end: the blank line the model wrote after a table reaches the
 * scrollback as its own row — the block carries the line and the box
 * renderer draws it (the separator is content, not a dropped verdict). */
static void test_table_blank_line_renders(void)
{
    struct RawResponse rr = {
        0, 0,
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/event-stream\r\n"
        "Connection: close\r\n\r\n"
        "data: {\"choices\":[{\"delta\":{\"content\":"
        "\"| Region | 2025 |\\n| ------ | ---: |\\n| North | 1234 |\\n\\n"
        "After the table.\\n\\n\"}}]}\n\n"
        "data: [DONE]\n\n"
    };
    rr.fd = server_bind(&rr.port);
    ASSERT_TRUE(rr.fd >= 0);
    pthread_t th;
    pthread_create(&th, NULL, raw_responder_thread, &rr);

    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1", rr.port);
    AppHarness *h = harness_new("ollama:local", "test-model", base);
    ASSERT_NOT_NULL(h);

    harness_type(h, "table please");
    harness_enter(h);
    ASSERT_EQ(harness_drive(h, 500), 0);
    ASSERT_EQ(nm_chat_app_state(h->app), NM_AGENT_DONE);

    char *seen = strip_frames(harness_read(h));
    ASSERT_NOT_NULL(seen);
    const char *corner = NULL;
    for (const char *q = seen; (q = strstr(q, "\xe2\x94\x98")) != NULL; q++)
        corner = q; /* the box's bottom-right corner */
    ASSERT_NOT_NULL(corner);
    const char *after = strstr(seen, "After the table.");
    ASSERT_NOT_NULL(after);
    ASSERT_TRUE(corner < after);
    ASSERT_TRUE(has_blank_row(corner, after)); /* the box's own blank */
    free(seen);

    harness_free(h);
    pthread_join(th, NULL);
    close(rr.fd);
}

/* End-to-end: a standalone image line followed by a BLANK line and more
 * prose (the blank reaches the classifier — it is interior, only a
 * run's trailing newlines are held back). The blank rides the image
 * block (so it is a row) and the reference still parses out of the
 * block's whole text: the marker keeps its dims, never "no source". */
static void test_image_followed_by_a_blank_still_parses(void)
{
    struct RawResponse rr = {
        0, 0,
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/event-stream\r\n"
        "Connection: close\r\n\r\n"
        "data: {\"choices\":[{\"delta\":{\"content\":"
        "\"Before it.\\n\\n"
        "![pic](data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAEAAAAAg)"
        "\\n\\nAfter it.\\n\\n\"}}]}\n\n"
        "data: [DONE]\n\n"
    };
    rr.fd = server_bind(&rr.port);
    ASSERT_TRUE(rr.fd >= 0);
    pthread_t th;
    pthread_create(&th, NULL, raw_responder_thread, &rr);

    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1", rr.port);
    AppHarness *h = harness_new("ollama:local", "test-model", base);
    ASSERT_NOT_NULL(h);

    harness_type(h, "draw something");
    harness_enter(h);
    ASSERT_EQ(harness_drive(h, 500), 0);

    h->rt->probe_state = 3;
    h->rt->profile.resolved = 1;
    tui_runtime_flush(h->rt);

    const char *out = harness_read(h);
    ASSERT_TRUE(strstr(out, "Before it.") != NULL);
    ASSERT_TRUE(strstr(out, "After it.") != NULL);
    /* the reference parsed: the marker names the alt, the format and
     * the size (had the block's blank line confused the parse, the
     * marker would read "no source") */
    ASSERT_TRUE(strstr(out, "pic") != NULL);
    ASSERT_TRUE(strstr(out, "PNG 64x32") != NULL);
    ASSERT_TRUE(strstr(out, "no source") == NULL);
    ASSERT_TRUE(strstr(out, "iVBORw0KGgo") == NULL);
    /* and the block's own blank line is a row: the marker's row is
     * followed by an empty one (strip_frames drops the escapes and the
     * live frames, so a blank row is two consecutive newlines) */
    char *seen = strip_frames(out);
    ASSERT_NOT_NULL(seen);
    const char *marker = strstr(seen, "pic");
    ASSERT_NOT_NULL(marker);
    const char *eol = strchr(marker, '\n');
    ASSERT_NOT_NULL(eol);
    ASSERT_TRUE(eol[1] == '\n');
    free(seen);

    harness_free(h);
    pthread_join(th, NULL);
    close(rr.fd);
}

/* End-to-end: a table row immediately followed by a fence line (no blank
 * between — legal markdown). The container's open verdict is a block
 * boundary, so the box commits BEFORE the fence's lines: the scrollback
 * order is the wire's order. (This pins the boba revision too: an older
 * boba froze only the pending line there, and the fence landed first.) */
static void test_table_then_fence_keeps_wire_order(void)
{
    struct RawResponse rr = {
        0, 0,
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/event-stream\r\n"
        "Connection: close\r\n\r\n"
        "data: {\"choices\":[{\"delta\":{\"content\":"
        "\"| a | b |\\n| - | - |\\n| c | d |\\n```c\\ncode here\\n```\\n\\n"
        "after the fence\\n\\n\"}}]}\n\n"
        "data: [DONE]\n\n"
    };
    rr.fd = server_bind(&rr.port);
    ASSERT_TRUE(rr.fd >= 0);
    pthread_t th;
    pthread_create(&th, NULL, raw_responder_thread, &rr);

    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1", rr.port);
    AppHarness *h = harness_new("ollama:local", "test-model", base);
    ASSERT_NOT_NULL(h);

    harness_type(h, "table then code");
    harness_enter(h);
    ASSERT_EQ(harness_drive(h, 500), 0);
    ASSERT_EQ(nm_chat_app_state(h->app), NM_AGENT_DONE);

    char *seen = strip_frames(harness_read(h));
    ASSERT_NOT_NULL(seen);
    const char *box = strstr(seen, "\xe2\x94\x98"); /* the box's corner */
    const char *fence = strstr(seen, "```c");
    const char *after = strstr(seen, "after the fence");
    ASSERT_NOT_NULL(box);
    ASSERT_NOT_NULL(fence);
    ASSERT_NOT_NULL(after);
    /* the table's box, then the fence, then the prose — never inverted,
     * and the fence's lines are not table cells */
    ASSERT_TRUE(box < fence);
    ASSERT_TRUE(fence < after);
    free(seen);

    harness_free(h);
    pthread_join(th, NULL);
    close(rr.fd);
}

/* End-to-end: a table streamed from a canned SSE round reaches the
 * scrollback aligned (the markdown classifier + renderer through the
 * full chat_app stack). */
static void test_markdown_table_reaches_scrollback_aligned(void)
{
    struct RawResponse rr = {
        0, 0,
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/event-stream\r\n"
        "Connection: close\r\n\r\n"
        "data: {\"choices\":[{\"delta\":{\"content\":"
        "\"| Region | 2025 |\\n| ------ | ---: |\\n"
        "| North | 1234 |\\n| South | 56 |\\n\"}}]}\n\n"
        "data: [DONE]\n\n"
    };
    rr.fd = server_bind(&rr.port);
    ASSERT_TRUE(rr.fd >= 0);
    pthread_t th;
    pthread_create(&th, NULL, raw_responder_thread, &rr);

    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1", rr.port);
    AppHarness *h = harness_new("ollama:local", "test-model", base);
    ASSERT_NOT_NULL(h);

    harness_type(h, "table please");
    harness_enter(h);
    ASSERT_EQ(harness_drive(h, 500), 0);

    const char *out = harness_read(h);
    ASSERT_TRUE(strstr(out, "Region") != NULL);
    ASSERT_TRUE(strstr(out, "North") != NULL);
    ASSERT_TRUE(strstr(out, "South") != NULL);
    /* Box borders (U+250C / U+2514) reached the scrollback. */
    ASSERT_TRUE(strstr(out, "\xe2\x94\x8c") != NULL);
    ASSERT_TRUE(strstr(out, "\xe2\x94\x94") != NULL);
    /* No bare LF. */
    for (const char *p = out; *p; p++)
        ASSERT_TRUE(*p != '\n' || (p > out && p[-1] == '\r'));

    harness_free(h);
    pthread_join(th, NULL);
    close(rr.fd);
}

/* End-to-end: an image data URI streamed from a canned SSE round
 * degrades to the marker through the full chat_app stack (the test
 * terminal answers no probe, so the profile resolves conservative) —
 * and the payload NEVER lands in the captured output. */
static void test_image_data_uri_degrades_to_marker(void)
{
    struct RawResponse rr = {
        0, 0,
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/event-stream\r\n"
        "Connection: close\r\n\r\n"
        "data: {\"choices\":[{\"delta\":{\"content\":"
        "\"![pic](data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAEAAAAAg)"
        "\\n\\n\"}}]}\n\n"
        "data: [DONE]\n\n"
    };
    rr.fd = server_bind(&rr.port);
    ASSERT_TRUE(rr.fd >= 0);
    pthread_t th;
    pthread_create(&th, NULL, raw_responder_thread, &rr);

    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1", rr.port);
    AppHarness *h = harness_new("ollama:local", "test-model", base);
    ASSERT_NOT_NULL(h);

    harness_type(h, "draw something");
    harness_enter(h);
    ASSERT_EQ(harness_drive(h, 500), 0);

    /* The tmpfile terminal never answers the probe; the real loop's
     * tick would resolve it at the 250 ms deadline — do the verdict
     * by hand and flush once more, which is exactly what that tick
     * does (the held image batch then commits as the marker). */
    h->rt->probe_state = 3;
    h->rt->profile.resolved = 1;
    tui_runtime_flush(h->rt);

    const char *out = harness_read(h);
    /* the marker: alt, format, dims, reason */
    ASSERT_TRUE(strstr(out, "pic") != NULL);
    ASSERT_TRUE(strstr(out, "PNG 64x32") != NULL);
    ASSERT_TRUE(strstr(out, "no graphics support") != NULL);
    /* the payload and the transport never appear (the probe's own
     * query is _Gi=..., not the transmit form) */
    ASSERT_TRUE(strstr(out, "iVBORw0KGgo") == NULL);
    ASSERT_TRUE(strstr(out, "\x1b_Ga=T") == NULL);

    harness_free(h);
    pthread_join(th, NULL);
    close(rr.fd);
}

/* ---------------------------------------------------------------- */
/* Config write-back: the runtime shadow                          */
/* ---------------------------------------------------------------- */

/* A runtime change lands in the shadow file and reports where it went.
 * The user config file's bytes are never touched — the app writes
 * exactly one file. */
static void test_config_runtime_change_writes_shadow(void)
{
    pin_cfg_paths("write");
    write_file_at(g_cfg_user, "model = from-user\n");

    AppHarness *h = harness_new("ollama:local", "from-user", NULL);
    ASSERT_NOT_NULL(h);
    NmConfig *cfg = cfg_for(h);

    harness_type(h, "/config set rounds 7");
    harness_enter(h);
    ASSERT_TRUE(strstr(harness_read(h), "config: rounds = 7") != NULL);
    ASSERT_TRUE(strstr(harness_read(h), "saved to the session shadow") !=
                NULL);

    harness_type(h, "/config set reasoning_echo on");
    harness_enter(h);
    /* `on` is the pre-granularity spelling of `all`, and the value is
     * reported (and persisted) canonically. */
    ASSERT_TRUE(strstr(harness_read(h), "config: reasoning_echo = all") != NULL);

    /* The shadow holds the normalized value (one canonical spelling per
     * key: `on` -> `all` here, `TRUE` -> `on` for a bool key). */
    ASSERT_STR_EQ(cfg_read_shadow(), "rounds = 7\nreasoning_echo = all\n");
    /* The user file is byte-identical: the app wrote exactly one file. */
    ASSERT_TRUE(cfg_file_present(g_cfg_user));
    ASSERT_STR_EQ(cfg_user_bytes(), "model = from-user\n");

    nm_config_free(cfg);
    harness_free(h);
}

/* Environment pins the run: the change still lands in the shadow (that
 * is the file's meaning), and the report says it is inert. */
static void test_config_env_pin_is_reported(void)
{
    pin_cfg_paths("envpin");
    test_setenv("NEVERMORE_MAX_ROUNDS", "9");

    AppHarness *h = harness_new("ollama:local", "m", NULL);
    ASSERT_NOT_NULL(h);
    NmConfig *cfg = cfg_for(h);
    ASSERT_EQ(nm_agent_max_rounds(nm_chat_app_agent(h->app)), 9);

    harness_type(h, "/config set rounds 3");
    harness_enter(h);
    const char *out = harness_read(h);
    ASSERT_TRUE(strstr(out, "NEVERMORE_MAX_ROUNDS pins this run") != NULL);
    /* Persisted anyway: the shadow is what the user typed. */
    ASSERT_STR_EQ(cfg_read_shadow(), "rounds = 3\n");
    /* The live agent still honors the environment (the env layer is
     * above the shadow, so the command's value is inert this run). */
    ASSERT_EQ(nm_agent_max_rounds(nm_chat_app_agent(h->app)), 9);

    nm_config_free(cfg);
    harness_free(h);
    test_unsetenv("NEVERMORE_MAX_ROUNDS");
}

/* /config reports provenance; /config reset drops shadow lines. */
static void test_config_command_reports_and_resets(void)
{
    pin_cfg_paths("report");
    write_file_at(g_cfg_user, "model = from-user\n");

    AppHarness *h = harness_new("ollama:local", "from-user", NULL);
    ASSERT_NOT_NULL(h);
    NmConfig *cfg = cfg_for(h);

    /* "! id" is the exact-set path (the id need not be in the catalog).
     * The model is persisted per provider: `model.<provider>`. */
    harness_type(h, "/model ! from-user");
    harness_enter(h);
    ASSERT_STR_EQ(cfg_read_shadow(), "model.ollama:local = from-user\n");

    harness_type(h, "/config");
    harness_enter(h);
    const char *out = harness_read(h);
    ASSERT_TRUE(strstr(out, g_cfg_user) != NULL);
    ASSERT_TRUE(strstr(out, g_cfg_shadow) != NULL);
    /* model resolves to the shadow (just set); rounds has no layer. */
    ASSERT_TRUE(strstr(out, "(session shadow)") != NULL);
    ASSERT_TRUE(strstr(out, "(built-in default)") != NULL);
    /* The duration keys show their built-in defaults too (the stream
     * inactivity and run_command budgets are ordinary keys now). The
     * leading spaces pin the `timeout` ROW (not a *_timeout key). */
    ASSERT_NOT_NULL(strstr(out, "  timeout "));
    ASSERT_NOT_NULL(strstr(out, "run_command_timeout"));

    harness_type(h, "/config reset model");
    harness_enter(h);
    ASSERT_TRUE(strstr(harness_read(h), "config: model reset") != NULL);
    ASSERT_FALSE(cfg_file_present(g_cfg_shadow));

    /* Resetting all removes the file. */
    harness_type(h, "/model ! from-user");
    harness_enter(h);
    ASSERT_TRUE(cfg_file_present(g_cfg_shadow));
    harness_type(h, "/config reset all");
    harness_enter(h);
    ASSERT_TRUE(strstr(harness_read(h), "config: all keys reset") != NULL);
    ASSERT_FALSE(cfg_file_present(g_cfg_shadow));

    nm_config_free(cfg);
    harness_free(h);
}

/* The /config row for a key the PROVIDER dictates: on opencode:go the
 * reasoning_echo row shows `tools` + "(provider default)", not the
 * `off` the bare key would report — the wire requirement is what the
 * next request uses. (The override path — a set value winning — is
 * covered by test_agent_reasoning_echo_user_overrides_provider.) */
static void test_config_shows_provider_default_echo(void)
{
    pin_cfg_paths("provdefault");
    AppHarness *h = harness_new("opencode:go", "deepseek-v4.1-flash", NULL);
    ASSERT_NOT_NULL(h);
    NmConfig *cfg = cfg_for(h);

    harness_type(h, "/config");
    harness_enter(h);
    const char *out = harness_read(h);
    ASSERT_NOT_NULL(strstr(out, "reasoning_echo"));
    ASSERT_NOT_NULL(strstr(out, "(provider default)"));
    /* The row reports the provider's mode, not the built-in `off`. */
    ASSERT_NOT_NULL(strstr(out, "reasoning_echo       tools"));

    nm_config_free(cfg);
    harness_free(h);
}

/* A model pick is remembered PER PROVIDER: /model writes
 * `model.<provider>`, a switch re-resolves for the new provider (no
 * memory = the ask, never the previous provider's id), and switching
 * back restores it. */
static void test_model_memory_is_per_provider(void)
{
    pin_cfg_paths("scopedmodel");
    write_file_at(g_cfg_user, "provider = ollama:local\n");

    AppHarness *h = harness_new("ollama:local", "m1", NULL);
    ASSERT_NOT_NULL(h);
    NmConfig *cfg = cfg_for(h);
    ASSERT_STR_EQ(nm_chat_app_model(h->app), "m1");

    /* The pick lands under the ACTIVE provider's scoped spelling. */
    harness_type(h, "/model ! m2");
    harness_enter(h);
    ASSERT_STR_EQ(nm_chat_app_model(h->app), "m2");
    ASSERT_STR_EQ(cfg_read_shadow(), "model.ollama:local = m2\n");

    /* A provider with no memory for this model: the switch lands on the
     * ask (no model), not the previous provider's id. */
    harness_type(h, "/provider hyper");
    harness_enter(h);
    ASSERT_STR_EQ(nm_chat_app_provider(h->app), "hyper");
    ASSERT_NULL(nm_chat_app_model(h->app));
    ASSERT_TRUE(strstr(harness_read(h),
                       "no model for provider 'hyper'") != NULL);

    /* Back to ollama:local: the memory restores the id. */
    harness_type(h, "/provider ollama:local");
    harness_enter(h);
    ASSERT_STR_EQ(nm_chat_app_provider(h->app), "ollama:local");
    ASSERT_STR_EQ(nm_chat_app_model(h->app), "m2");

    nm_config_free(cfg);
    harness_free(h);
}

/* A send with no model for the provider is refused BEFORE the echo: the
 * line never looks delivered and the input is left for a retry after
 * /model. */
static void test_send_without_model_is_refused(void)
{
    AppHarness *h = harness_new("ollama:local", NULL, NULL);
    ASSERT_NOT_NULL(h);
    ASSERT_NULL(nm_chat_app_model(h->app));

    /* The startup notice says how to set one. */
    ASSERT_TRUE(strstr(harness_read(h),
                       "no model for provider 'ollama:local'") != NULL);

    /* A plain send is refused: the agent stays idle and the input is
     * NOT cleared (submit's echo/clear never ran). */
    harness_type(h, "hello");
    harness_enter(h);
    ASSERT_EQ(nm_chat_app_state(h->app), NM_AGENT_IDLE);
    ASSERT_STR_EQ(tui_textinput_text(nm_chat_app_textinput(h->app)), "hello");

    /* Slash commands still run: /model sets one (clear the refused
     * buffer first — the point of leaving it was the retry). */
    tui_textinput_clear(nm_chat_app_textinput(h->app));
    harness_type(h, "/model ! m1");
    harness_enter(h);
    ASSERT_STR_EQ(nm_chat_app_model(h->app), "m1");

    harness_free(h);
}

/* A provider that needs a key and has none configured warns BEFORE the
 * turn — the post-401 hint's preflight twin. The test is the key seam
 * (nm_provider_api_key), so a ~/.authinfo entry counts as configured. */
static void test_missing_key_preflight_warns(void)
{
    test_unsetenv("OPENROUTER_API_KEY");
    test_unsetenv("OLLAMA_API_KEY");
    test_unsetenv("HYPER_API_KEY");

    /* A keyless provider (the local daemon) never warns. */
    AppHarness *h0 = harness_new("ollama:local", "m", NULL);
    ASSERT_NOT_NULL(h0);
    ASSERT_TRUE(strstr(harness_read(h0), "needs an API key") == NULL);

    /* Switching to a keyed provider with no key: the note names the env
     * var and the authinfo machine. */
    harness_type(h0, "/provider openrouter");
    harness_enter(h0);
    const char *out = harness_read(h0);
    ASSERT_TRUE(strstr(out, "needs an API key") != NULL);
    ASSERT_TRUE(strstr(out, "OPENROUTER_API_KEY") != NULL);
    ASSERT_TRUE(strstr(out, "openrouter.ai") != NULL);
    harness_free(h0);

    /* With the key configured, the same provider is quiet. */
    test_setenv("OPENROUTER_API_KEY", "sk-x");
    AppHarness *h1 = harness_new("openrouter", "m", NULL);
    ASSERT_NOT_NULL(h1);
    ASSERT_TRUE(strstr(harness_read(h1), "needs an API key") == NULL);
    harness_free(h1);
    test_unsetenv("OPENROUTER_API_KEY");
}

/* The popup no longer blocks on a wire catalog: /model returns at once
 * with a note, the event loop drives the fetch, and the popup opens
 * when it lands — here with the static fallback, because the peer never
 * answers and the fetch's own deadline ends it. The point is the ORDER:
 * the note (and a live render) happen before any round trip. */
static void test_model_popup_does_not_block_on_a_wire_catalog(void)
{
    /* A listening socket that is never accepted: the connect completes
     * (the kernel's backlog), the request goes out, nothing comes back
     * — a guaranteed PENDING. */
    int port;
    int lfd = server_bind(&port);
    ASSERT_TRUE(lfd >= 0);

    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1", port);
    AppHarness *h = harness_new("openrouter", "~openai/gpt-astra-latest", base);
    ASSERT_NOT_NULL(h);

    harness_type(h, "/model");
    harness_enter(h);
    /* /model returned WITHOUT the popup: the fetch is in flight and the
     * UI is live (the note, not a frozen frame). */
    ASSERT_TRUE(strstr(harness_read(h), "loading the openrouter") != NULL);
    ASSERT_TRUE(strstr(tui_runtime_render(h->rt), "GPT Astra") == NULL);

    /* A few event-loop turns: the connect lands and the request goes
     * out. Still nothing to show. */
    for (int i = 0; i < 5; i++) {
        NmSource s = nm_chat_app_source(h->app);
        if (s.handle < 0 || !s.flags)
            break;
        app_wait_src(&s, 10);
        nm_chat_app_external_ready(h->app, s.handle, s.flags);
    }
    ASSERT_TRUE(strstr(tui_runtime_render(h->rt), "GPT Astra") == NULL);

    /* The peer never answers: the fetch's own INACTIVITY budget (5 s
     * of silence on the fake clock) ends it, and the popup opens with
     * the built-in list. */
    nm_test_clock_advance_ms(5100);
    nm_chat_app_tick(h->app);
    tui_runtime_flush(h->rt);
    ASSERT_EQ(harness_pump_catalog(h, 50), 0);
    ASSERT_TRUE(strstr(harness_read(h),
                       "could not load the live openrouter catalog") != NULL);
    const char *frame = tui_runtime_render(h->rt);
    ASSERT_TRUE(strstr(frame, "aion-labs/aion-2.0") != NULL);

    harness_free(h);
    close(lfd);
}

/* A one-shot canned catalog: answers the next connection with ONE id
 * the shipped static table does not carry. The listener is bounded so a
 * test that never drives the fetch cannot wedge this thread. */
static void *wire_only_catalog_server_thread(void *arg)
{
    int lfd = *(int *)arg;
    fd_set r;
    struct timeval tv = { 2, 0 };
    FD_ZERO(&r);
    FD_SET(lfd, &r);
    if (select(lfd + 1, &r, NULL, NULL, &tv) <= 0)
        return NULL;
    int cfd = accept(lfd, NULL, NULL);
    if (cfd < 0)
        return NULL;
    /* Drain the GET: headers only, to the blank line (the canned-server
     * shape this file uses). A server that closes with its request still
     * unread sends an RST, and an RST can discard the reply the client
     * has not read yet — the macOS/BSD behaviour that failed the warm
     * fixture below in CI. */
    char req[2048];
    size_t got = 0;
    while (got < sizeof(req) - 1) {
        long n = recv(cfd, req + got, sizeof(req) - 1 - got, 0);
        if (n <= 0)
            break;
        got += (size_t)n;
        req[got] = '\0';
        if (strstr(req, "\r\n\r\n"))
            break;
    }
    static const char body[] = "{\"data\":[{\"id\":\"wire-only-model\"}]}";
    char head[160];
    int hl = snprintf(head, sizeof(head),
                      "HTTP/1.1 200 OK\r\n"
                      "Content-Type: application/json\r\n"
                      "Content-Length: %zu\r\n\r\n",
                      strlen(body));
    /* Hold the reply briefly: the test's point is the ASYNC gap (the
     * command must return with the fetch still in flight), and loopback
     * is fast enough that the reply can already be sitting in the
     * client's socket when its first step looks — under ASan (a slower,
     * instrumented client; this thread is not instrumented) that is the
     * COMMON case, which made the "loading the …" assertion flake in CI.
     * The hold makes the pending state deterministic instead of a race
     * (the same hold the big-catalog fixture below documents). */
    usleep(200 * 1000);
    send(cfd, head, (size_t)hl, 0);
    send(cfd, body, strlen(body), 0);
    close(cfd);
    return NULL;
}

/* /model <exact id> is a key handler too — the same UI thread the
 * /provider switch froze on (BUG 1, in the /model dress). The id is
 * validated against the CACHED catalog, never the blocking drive: an
 * id only a wire catalog carries is not refused, it opens the picker
 * with the text as the query and the fetch lands async. So the command
 * returns BEFORE any round trip and the active model is untouched —
 * while the blocking drive would have found the id (the canned server
 * answers with it) and set it inline. */
static void test_model_exact_id_never_fetches_on_the_ui_thread(void)
{
    int port;
    int lfd = server_bind(&port);
    ASSERT_TRUE(lfd >= 0);
    pthread_t th;
    pthread_create(&th, NULL, wire_only_catalog_server_thread, &lfd);

    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1", port);
    AppHarness *h = harness_new("ollama:local", "test-model", base);
    ASSERT_NOT_NULL(h);

    harness_type(h, "/model wire-only-model");
    harness_enter(h);

    /* Returned with the fetch still in flight: no inline set, and the
     * note says the picker is on its way. */
    ASSERT_STR_EQ(nm_chat_app_model(h->app), "test-model");
    ASSERT_TRUE(strstr(harness_read(h),
                       "loading the ollama:local model catalog") != NULL);

    harness_free(h);
    close(lfd);
    pthread_join(th, NULL);
}

/* A canned hyper catalog: ONE id the shipped static table does not carry,
 * with the wire's context_window (hyper's parse reads that key). The
 * request is drained before the reply: the client must READ this body,
 * and a close over an unread request is an RST — which on macOS/BSD can
 * discard the reply (the gauge then stayed cold and CI went red there). */
static void *warm_catalog_server_thread(void *arg)
{
    int lfd = *(int *)arg;
    fd_set r;
    struct timeval tv = { 2, 0 };
    FD_ZERO(&r);
    FD_SET(lfd, &r);
    if (select(lfd + 1, &r, NULL, NULL, &tv) <= 0)
        return NULL;
    int cfd = accept(lfd, NULL, NULL);
    if (cfd < 0)
        return NULL;
    char req[2048];
    size_t got = 0;
    while (got < sizeof(req) - 1) {
        long n = recv(cfd, req + got, sizeof(req) - 1 - got, 0);
        if (n <= 0)
            break;
        got += (size_t)n;
        req[got] = '\0';
        if (strstr(req, "\r\n\r\n"))
            break;
    }
    static const char body[] =
        "{\"object\":\"list\",\"data\":["
        "{\"id\":\"wire-only-model\",\"display_name\":\"Wire Only\","
        "\"context_window\":1048576,\"capabilities\":{\"vision\":false}}]}";
    char head[160];
    int hl = snprintf(head, sizeof(head),
                      "HTTP/1.1 200 OK\r\n"
                      "Content-Type: application/json\r\n"
                      "Content-Length: %zu\r\n\r\n",
                      strlen(body));
    send(cfd, head, (size_t)hl, 0);
    send(cfd, body, strlen(body), 0);
    close(cfd);
    return NULL;
}

/* The TUI WARMS the active provider's catalog in the BACKGROUND
 * (main.c -> nm_chat_app_warm_catalog): the gauge's denominator is
 * catalog metadata resolved at the point of use from `models_cached`
 * (agent.c's model_entry), so it needs a warm — and the /model picker
 * cannot be the only warm site, because the model id is PERSISTED (the
 * config shadow), so a session that never opens the picker reads the
 * catalog cold. That was the reported bug's remaining half: after the
 * point-of-use fix, `11k/-` against `hyper · deepseek-v4.1-flash`
 * still lasted the whole session — hyper's static fallback is one row,
 * and nothing else had warmed the cache.
 *
 * The warm is silent and shows nothing (it is the gauge's, not the
 * user's): it fills the provider cache and the frame moves with it, no
 * push and no rebuild. The prompt's vision clause is NOT resolved here
 * — it is settled at the first turn, from the warm cache, before the
 * first request exists (agent.c's apply_vision_clause); this test's
 * subject is the gauge.
 *
 * REGISTERED LAST (see main): provider catalogs are process-global, so
 * the live commit this test makes is visible to every test after it —
 * it would replace the static fallback those tests pin. */
static void test_startup_warm_fills_the_gauge_without_a_popup(void)
{
    /* The warm is gated on the live-catalog knob (it is live-catalog
     * traffic the user did not ask for), so lift the suite's pin for
     * this one test — the production default. LAST test, so nothing
     * after it can read the unpinned state, and the pin's own tripwire
     * (test_offline_catalog_is_pinned) already ran first. */
    test_unsetenv("NM_NO_LIVE_CATALOG");

    int port;
    int lfd = server_bind(&port);
    ASSERT_TRUE(lfd >= 0);
    pthread_t th;
    pthread_create(&th, NULL, warm_catalog_server_thread, &lfd);

    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1", port);
    AppHarness *h = harness_new("hyper", "wire-only-model", base);
    ASSERT_NOT_NULL(h);

    /* Cold: the static fallback does not carry the id, so the limit is
     * honestly unknown. */
    char *cold = span_bytes(nm_color_gutter(NM_COLOR_16), "-/- ");
    ASSERT_NOT_NULL(cold);
    ASSERT_TRUE(strstr(tui_runtime_render(h->rt), cold) != NULL);
    free(cold);

    /* main.c's call, verbatim (the harness sets the endpoint after the
     * runtime, so the warm is explicit here as it is there). */
    nm_chat_app_warm_catalog(h->app);

    /* In flight and still silent: no popup, no line. */
    ASSERT_TRUE(strstr(tui_runtime_render(h->rt), "Wire Only") == NULL);
    ASSERT_TRUE(strstr(harness_read(h), "catalog") == NULL);

    /* It lands and fills the cache: the gauge resolves the new row on
     * the spot. */
    ASSERT_EQ(harness_pump_catalog(h, 50), 0);
    char *warm = span_bytes(nm_color_gutter(NM_COLOR_16), "-/1M ");
    ASSERT_NOT_NULL(warm);
    ASSERT_TRUE(strstr(tui_runtime_render(h->rt), warm) != NULL);
    free(warm);

    /* Still nothing said, and still no popup: the fetch is the gauge's. */
    ASSERT_TRUE(strstr(tui_runtime_render(h->rt), "Wire Only") == NULL);
    ASSERT_TRUE(strstr(harness_read(h), "catalog") == NULL);

    /* A second warm is a no-op (the catalog is cached; nothing refetches
     * — the canned server accepts exactly one connection). */
    nm_chat_app_warm_catalog(h->app);
    ASSERT_EQ(harness_pump_catalog(h, 50), 0);

    harness_free(h);
    close(lfd);
    pthread_join(th, NULL);
    /* Restore the suite's pin (harmless — this is the last test — but
     * the process should leave the world as it found it). */
    test_pin_offline_catalog();
}

/* ---------------------------------------------------------------- */
/* The tool phase vs a catalog fetch in flight                        */
/* ---------------------------------------------------------------- */

/* A catalog fetch borrows the app's ONE primary source slot (the
 * agent's source when it has one, else the catalog's wait target), and
 * the TOOL PHASE is exactly the moment the agent has no source of its
 * own: the stream is closed and the tool runs inline. A step loop that
 * asked the APP for its source therefore read the catalog's socket,
 * took it for "the agent is waiting on I/O" and ended the loop before
 * the tool ran.
 *
 * The stall was TOTAL, not a delay: the tool phase has no deadline, so
 * nothing else ever stepped the agent again — the turn waited for the
 * catalog to land, and the landing did not resume it. The reported
 * shape ("a tool cannot run unless the catalog has been loaded") is
 * this: the warm's fetch is in flight for the first turns of a
 * session, and the turn that calls a tool in that window dies.
 *
 * The server below answers the CHAT and HOLDS the catalog request, so
 * the fetch stays in flight for the whole turn — the state the stall
 * needs (the fetch's own deadline is the fake clock's, which does not
 * move here). */
struct HeldCatalogServer
{
    int lfd;
    volatile int stop; /* the test is done: leave the accept loop */
    int chat_rounds;   /* the chat requests served */
    int held;          /* the catalog request is being held */
    pthread_t holder;  /* the thread holding the catalog connection */
};

/* Hold one connection open until the peer closes it. A closed peer is
 * READABLE (EOF), so poll with a timeout and read: 0 = gone. */
static void *hold_conn_thread(void *arg)
{
    int cfd = (int)(intptr_t)arg;
    for (;;) {
        struct timeval tv = { 0, 200 * 1000 };
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(cfd, &rfds);
        if (select(cfd + 1, &rfds, NULL, NULL, &tv) <= 0)
            continue;
        char sink[256];
        long n = recv(cfd, sink, sizeof(sink), 0);
        if (n <= 0)
            break; /* the peer closed */
    }
    close(cfd);
    return NULL;
}

/* The chat's two canned rounds: the tool call, then the answer. */
static const char *const g_held_chat_sse[2] = {
    "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,"
    "\"id\":\"call_1\",\"type\":\"function\",\"function\":"
    "{\"name\":\"read_file\",\"arguments\":\"{\\\"path\\\":\\\"" FIXTURE_PATH
    "\\\"}\"}}]}}]}\n\n"
    "data: [DONE]\n\n",
    "data: {\"choices\":[{\"delta\":{\"content\":\"all done\"}}]}\n\n"
    "data: [DONE]\n\n"
};

/* The server: the catalog's GET is held (in its own thread, so the
 * accept loop keeps taking the chat's connections), the chat's POST is
 * answered with the scripted round. */
static void *held_catalog_server_thread(void *arg)
{
    struct HeldCatalogServer *hs = arg;
    for (;;) {
        if (hs->stop)
            return NULL;
        struct timeval atv = { 0, 100 * 1000 };
        fd_set arfds;
        FD_ZERO(&arfds);
        FD_SET(hs->lfd, &arfds);
        if (select(hs->lfd + 1, &arfds, NULL, NULL, &atv) <= 0)
            continue; /* the timeout re-checks `stop` */
        int cfd = accept(hs->lfd, NULL, NULL);
        if (cfd < 0)
            continue;
        /* Read the request head AND its body: an unread request + close
         * sends an RST, which on macOS/BSD can discard the client's
         * unread reply. The body length is the head's Content-Length. */
        char req[REQ_CAP];
        size_t got = 0;
        size_t want = 0;
        for (;;) {
            struct timeval tv = { 0, 200 * 1000 };
            fd_set rfds;
            FD_ZERO(&rfds);
            FD_SET(cfd, &rfds);
            if (select(cfd + 1, &rfds, NULL, NULL, &tv) <= 0)
                break;
            long n = recv(cfd, req + got, sizeof(req) - 1 - got, 0);
            if (n <= 0)
                break;
            got += (size_t)n;
            req[got] = '\0';
            const char *end = strstr(req, "\r\n\r\n");
            if (!end)
                continue;
            if (!want) {
                const char *cl = strstr(req, "Content-Length:");
                want = cl ? (size_t)strtoul(cl + 15, NULL, 10) : 0;
                if (!want)
                    break; /* no body: the head is all there is */
            }
            if (got >= (size_t)(end - req) + 4 + want)
                break;
        }
        req[got] = '\0';

        if (strncmp(req, "GET ", 4) == 0) {
            /* The catalog's request: HOLD it. The fetch stays in flight
             * for the whole turn, so the app's primary slot is the
             * catalog's socket while the agent is in its tool phase. */
            pthread_create(&hs->holder, NULL, hold_conn_thread,
                           (void *)(intptr_t)cfd);
            hs->held = 1;
            continue;
        }
        /* The chat: the scripted round (the last one repeats, so a
         * third request would still be answered). */
        int round = hs->chat_rounds++;
        sse_send_chunked(cfd, g_held_chat_sse[round < 2 ? round : 1], NULL);
        send(cfd, "0\r\n\r\n", 5, 0);
        close(cfd);
    }
}

/* The tool runs while the catalog fetch is in flight. */
static void test_tool_runs_while_a_catalog_fetch_is_in_flight(void)
{
    /* The fixture the tool reads (removed first: a stale file from an
     * earlier run would be read instead). */
    remove(FIXTURE_PATH);
    FILE *f = fopen(FIXTURE_PATH, "wb");
    ASSERT_NOT_NULL(f);
    fputs("the quick brown fox\n", f);
    fclose(f);

    int port;
    int lfd = server_bind(&port);
    ASSERT_TRUE(lfd >= 0);
    struct HeldCatalogServer hs;
    memset(&hs, 0, sizeof(hs));
    hs.lfd = lfd;
    pthread_t th;
    pthread_create(&th, NULL, held_catalog_server_thread, &hs);

    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1", port);
    AppHarness *h = harness_new("ollama:local", "test-model", base);
    ASSERT_NOT_NULL(h);

    /* main.c's warm, verbatim: the fetch is in flight while the turn
     * runs. The warm is gated on the live-catalog knob, so lift the
     * suite's pin for it — and put the pin back BEFORE the first
     * assertion, so a failed assert cannot leave it off for the tests
     * after this one (they would then dial the real provider). */
    test_unsetenv("NM_NO_LIVE_CATALOG");
    nm_chat_app_warm_catalog(h->app);
    /* The fetch is in flight: the app's primary slot is the catalog's
     * socket (the agent is idle). */
    int in_flight = nm_chat_app_source(h->app).handle >= 0;

    harness_type(h, "read it");
    harness_enter(h);
    /* The EVENT-LOOP drive, not the plain one: the plain drive steps
     * on every iteration, and a step is the only thing that can move
     * the tool phase (it has no deadline) — so the plain drive walks
     * straight past the stall this test is about. */
    int drove = harness_drive_events(h, 10000);
    test_pin_offline_catalog();

    ASSERT_TRUE(in_flight);
    /* The turn ran to its end: the tool ran, and the answer landed. */
    ASSERT_EQ(drove, 0);
    ASSERT_EQ(nm_chat_app_state(h->app), NM_AGENT_DONE);
    char *clean = strip_frames(harness_read(h));
    ASSERT_NOT_NULL(clean);
    ASSERT_TRUE(strstr(clean, "all done") != NULL);
    free(clean);

    harness_free(h); /* frees the fetch: the held connection closes */
    hs.stop = 1;
    pthread_join(th, NULL);
    if (hs.held)
        pthread_join(hs.holder, NULL);
    close(lfd);
    remove(FIXTURE_PATH);
}

/* ---------------------------------------------------------------- */
/* The fetch's deadline bounds the RESPONSE, not the connect         */
/* ---------------------------------------------------------------- */

/* A BLACK-HOLED address (RFC 5737's TEST-NET-1: reserved, routed
 * nowhere): the connect is sent and never answered, so the fetch stays
 * in its CONNECT phase for as long as the transport's own budget says.
 * It is the one way to hold a fetch in that phase on EVERY platform —
 * a listener with a full accept queue is a Linux-shaped trick (the
 * queue's depth is the kernel's, and macOS accepts the next connect
 * instead of dropping it, which let the fetch's request go out and
 * ended the phase early).
 *
 * The address is unroutable, so nothing answers it and nothing is
 * left behind: no listener, no filler, no child. */
#define NM_BLACK_HOLE_HOST "192.0.2.1"

/* The catalog fetch's per-request deadline (openai_client.c) is the
 * RESPONSE bound — "a peer that answers the connection and then never
 * sends the response" — and it is armed the moment the request goes on
 * the wire. It used to be armed at nm_fetch_begin, which silently
 * overrode the transport's own budgets for the connect walk
 * (`connect_timeout`, per address) and the TLS handshake
 * (`handshake_timeout`): a 2 s constant over a 10 s handshake budget.
 * A cold connect — an address budget burned on a black-holed address,
 * a slow TLS handshake — then failed the fetch BEFORE the peer had
 * been asked anything, and the background warm has no retry, so the
 * gauge's denominator read `-` for the whole session (the reported
 * `11k/-`).
 *
 * Here the fetch is still CONNECTING when the fake clock crosses the
 * deadline: the connect is the walk's to bound (its budget is raised
 * so the walk itself does not abandon the address), so the fetch must
 * still be in flight — no failure note, no popup.
 *
 * The endpoint is an unroutable address (RFC 5737's TEST-NET-1), so
 * the connect is sent and never answered: the fetch's CONNECT phase
 * lasts as long as the transport's budget says. The earlier shape — a
 * listener with a full accept queue — was Linux-specific: macOS
 * accepts the next connect instead of dropping its SYN, so the
 * request went out, the fetch left its CONNECT phase, and the fetch's
 * own response deadline then fired — the test measured the walk's
 * queue, not the deadline. */
static void test_catalog_fetch_deadline_does_not_bound_the_connect(void)
{
    pin_cfg_paths("catdeadline");
    char base[64];
    snprintf(base, sizeof(base), "http://%s:9/v1", NM_BLACK_HOLE_HOST);
    /* hyper: the ONLY provider whose live cache is still clean here
     * (the picker tests pin it offline, and the warm test that commits
     * a live one runs LAST) — an already-cached catalog would answer
     * READY and never start the fetch this test is about. */
    AppHarness *h = harness_new("hyper", "gpt-oss-120b", base);
    ASSERT_NOT_NULL(h);
    NmConfig *cfg = cfg_for(h);
    /* The walk's own budget must outlast the fetch deadline, or the
     * walk abandons the address and the test measures the walk. */
    store_set(NM_CFG_KEY_CONNECT_TIMEOUT, "30000");

    /* /model starts the fetch. The connect is armed against an
     * unroutable address, so nothing is on the wire yet. */
    harness_type(h, "/model");
    harness_enter(h);
    ASSERT_TRUE(strstr(harness_read(h), "loading the hyper") != NULL);

    /* The clock crosses the fetch's own INACTIVITY budget (5 s) while
     * the fetch is still CONNECTING. */
    nm_test_clock_advance_ms(5100);
    nm_chat_app_tick(h->app);
    tui_runtime_flush(h->rt);

    /* Still in flight: the deadline did not fire (no note, no popup —
     * the note and the built-in list are what a FAILED fetch opens). */
    ASSERT_TRUE(strstr(harness_read(h),
                       "could not load the live hyper catalog") == NULL);
    ASSERT_TRUE(strstr(tui_runtime_render(h->rt), "GPT OSS 120b") == NULL);

    nm_config_free(cfg);
    harness_free(h);
    store_clear(NM_CFG_KEY_CONNECT_TIMEOUT);
}

/* ---------------------------------------------------------------- */
/* The fetch's budget is an IDLE one, not a total one                */
/* ---------------------------------------------------------------- */

/* The catalog's body arrives in TWO chunks with a virtual-clock gap
 * between them: the fetch's budget is the SILENCE between bytes, so a
 * body that keeps arriving is never cut, however long the transfer
 * takes.
 *
 * The budget used to be a TOTAL one (2 s from the request to the last
 * byte), which made a peer that was merely SLOW indistinguishable from
 * a peer that was HUNG: the OpenRouter catalog (~790 KB, 2.0-2.7 s on
 * the wire) landed exactly on that line, and the live catalog
 * degraded to the three-row static table in 3 of 20 live runs —
 * silently. The gap here is 3 s of virtual time: over the old total
 * budget, under the idle one. */
static const char slow_catalog_body[] =
    "{\"object\":\"list\",\"data\":[{\"id\":\"glm-5.3\","
    "\"object\":\"model\",\"created\":0,\"owned_by\":\"opencode\"}]}";

struct SlowCatalogServer
{
    int lfd;
    volatile int first_sent; /* the head + the first chunk are out */
    volatile int go;         /* the test released the second chunk */
};

static void *slow_catalog_server_thread(void *arg)
{
    struct SlowCatalogServer *ss = arg;
    int cfd = accept(ss->lfd, NULL, NULL);
    if (cfd < 0)
        return NULL;
    /* Drain the request (a GET's head is all it has). */
    char req[2048];
    size_t got = 0;
    while (got < sizeof(req) - 1) {
        long n = recv(cfd, req + got, sizeof(req) - 1 - got, 0);
        if (n <= 0)
            break;
        got += (size_t)n;
        req[got] = '\0';
        if (strstr(req, "\r\n\r\n"))
            break;
    }
    char head[160];
    int hl = snprintf(head, sizeof(head),
                      "HTTP/1.1 200 OK\r\n"
                      "Content-Type: application/json\r\n"
                      "Content-Length: %zu\r\n\r\n",
                      strlen(slow_catalog_body));
    size_t half = strlen(slow_catalog_body) / 2;
    send(cfd, head, (size_t)hl, 0);
    send(cfd, slow_catalog_body, half, 0);
    ss->first_sent = 1;
    /* The gap: the test moves the virtual clock while the body waits. */
    while (!ss->go)
        usleep(1000);
    send(cfd, slow_catalog_body + half, strlen(slow_catalog_body) - half, 0);
    close(cfd);
    return NULL;
}

static void test_catalog_fetch_budget_is_idle_not_total(void)
{
    pin_cfg_paths("catidle");
    int port;
    int lfd = server_bind(&port);
    ASSERT_TRUE(lfd >= 0);
    struct SlowCatalogServer ss;
    memset(&ss, 0, sizeof(ss));
    ss.lfd = lfd;
    pthread_t th;
    pthread_create(&th, NULL, slow_catalog_server_thread, &ss);

    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1", port);
    /* opencode:go: its live catalog slot is still clean here (the
     * tests before it read the shipped table, and the warm test that
     * commits a live one runs LAST). The active model is a STATIC row:
     * the live row ("glm-5.3") is the only thing that can put it
     * on the screen, so the assertion cannot pass on the static
     * fallback. */
    AppHarness *h = harness_new("opencode:go", "deepseek-v4.1-flash", base);
    ASSERT_NOT_NULL(h);

    harness_type(h, "/model");
    harness_enter(h);
    ASSERT_TRUE(strstr(harness_read(h), "loading the opencode:go") != NULL);

    /* Pump until the server has the first half of the body out (the
     * connect and the send need the event loop too). */
    for (int i = 0; i < 500 && !ss.first_sent; i++) {
        NmSource s = nm_chat_app_source(h->app);
        if (s.handle >= 0 && s.flags) {
            app_wait_src(&s, 10);
            nm_chat_app_external_ready(h->app, s.handle, s.flags);
        }
        tui_runtime_flush(h->rt);
        usleep(1000);
    }
    ASSERT_TRUE(ss.first_sent);

    /* The clock jumps PAST the old total budget while the body is
     * still arriving: a slow peer is not a hung peer. */
    nm_test_clock_advance_ms(3000);
    ss.go = 1;

    /* The fetch lands: the LIVE row, and no degradation note. */
    ASSERT_EQ(harness_pump_catalog(h, 2000), 0);
    ASSERT_TRUE(strstr(harness_read(h),
                       "could not load the live opencode:go") == NULL);
    ASSERT_TRUE(strstr(tui_runtime_render(h->rt), "glm-5.3") != NULL);

    harness_free(h);
    pthread_join(th, NULL);
    close(lfd);
}

/* /config lists the provider-scoped keys that are set, after the plain
 * ones — the per-provider model memory. */
static void test_config_lists_scoped_model_keys(void)
{
    pin_cfg_paths("scopedlist");
    write_file_at(g_cfg_user, "provider = ollama:local\nmodel.ollama:local = gpt-x\n");

    AppHarness *h = harness_new("ollama:local", "gpt-x", NULL);
    ASSERT_NOT_NULL(h);
    NmConfig *cfg = cfg_for(h);

    harness_type(h, "/config");
    harness_enter(h);
    const char *out = harness_read(h);
    ASSERT_TRUE(strstr(out, "model.ollama:local") != NULL);
    ASSERT_TRUE(strstr(out, "gpt-x") != NULL);

    nm_config_free(cfg);
    harness_free(h);
}

/* /config set: one plain key — validated, normalized, persisted — and
 * the machinery reads it back from the store at the point of use (no
 * proxied copy). /config shows a runtime (machinery-written) value's
 * layer, and reset clears that runtime value too. */
static void test_config_set_and_runtime_layer(void)
{
    pin_cfg_paths("cfgset");
    AppHarness *h = harness_new("ollama:local", "m", NULL);
    ASSERT_NOT_NULL(h);
    NmConfig *cfg = cfg_for(h);

    harness_type(h, "/config set rounds 4");
    harness_enter(h);
    ASSERT_TRUE(strstr(harness_read(h), "config: rounds = 4") != NULL);
    ASSERT_STR_EQ(cfg_read_shadow(), "rounds = 4\n");
    /* The agent resolves the store: the cap applies with no push. */
    ASSERT_EQ(nm_agent_max_rounds(nm_chat_app_agent(h->app)), 4);

    /* An invalid value is refused and leaves the shadow untouched. */
    harness_type(h, "/config set rounds nope");
    harness_enter(h);
    ASSERT_TRUE(strstr(harness_read(h), "invalid value") != NULL);
    ASSERT_STR_EQ(cfg_read_shadow(), "rounds = 4\n");

    /* provider/model go through their own commands, never here. */
    harness_type(h, "/config set model from-user");
    harness_enter(h);
    ASSERT_TRUE(strstr(harness_read(h), "use /provider or /model") != NULL);

    /* The image window is a plain key like any other: validated,
     * normalized, persisted, and read by the agent at the point of use
     * (no push). */
    harness_type(h, "/config set image_live 3");
    harness_enter(h);
    ASSERT_TRUE(strstr(harness_read(h), "config: image_live = 3") != NULL);
    ASSERT_EQ(nm_agent_image_live(nm_chat_app_agent(h->app)), 3ul);
    harness_type(h, "/config set image_live off");
    harness_enter(h);
    ASSERT_TRUE(strstr(harness_read(h), "config: image_live = off") != NULL);
    ASSERT_EQ(nm_agent_image_live(nm_chat_app_agent(h->app)), 0ul);
    /* `0` is refused: an image that never rides is a broken image. */
    harness_type(h, "/config set image_live 0");
    harness_enter(h);
    ASSERT_TRUE(strstr(harness_read(h), "invalid value") != NULL);
    ASSERT_EQ(nm_agent_image_live(nm_chat_app_agent(h->app)), 0ul);
    /* The refused write left the accepted one in place. */
    ASSERT_TRUE(strstr(cfg_read_shadow(), "image_live = off") != NULL);
    harness_type(h, "/config reset image_live");
    harness_enter(h);
    ASSERT_EQ(nm_agent_image_live(nm_chat_app_agent(h->app)), 1ul);

    /* A machinery-written runtime value shows its own layer. */
    nm_config_runtime_set(nm_config_store(), NM_CFG_KEY_SEARXNG_ENABLED,
                          "off");
    harness_type(h, "/config");
    harness_enter(h);
    const char *out = harness_read(h);
    ASSERT_TRUE(strstr(out, "searxng_enabled") != NULL);
    ASSERT_TRUE(strstr(out, "(runtime)") != NULL);

    /* reset clears the runtime layer as well as the shadow line. */
    harness_type(h, "/config reset searxng_enabled");
    harness_enter(h);
    ASSERT_TRUE(strstr(harness_read(h), "config: searxng_enabled reset") !=
                NULL);
    ASSERT_EQ(nm_config_resolve_bool(nm_config_store(),
                                     NM_CFG_KEY_SEARXNG_ENABLED, 1),
              1);

    nm_config_free(cfg);
    harness_free(h);
}

/* With no config handle the commands still work and nothing is
 * written anywhere (the hermetic default the other tests rely on). */
static void test_config_absent_is_no_persistence(void)
{
    pin_cfg_paths("absent");
    AppHarness *h = harness_new("ollama:local", "m", NULL);
    ASSERT_NOT_NULL(h);

    harness_type(h, "/config set rounds 5");
    harness_enter(h);
    const char *out = harness_read(h);
    ASSERT_TRUE(strstr(out, "no config") != NULL);
    ASSERT_FALSE(cfg_file_present(g_cfg_shadow));

    harness_type(h, "/config");
    harness_enter(h);
    ASSERT_TRUE(strstr(harness_read(h), "no config") != NULL);

    harness_free(h);
}

/* The offline-catalog tripwire: the model tests below assert against
 * the STATIC ollama catalog, so a live fetch would replace it (see
 * test_net_helpers.h). */
TEST_OFFLINE_CATALOG_PIN_CHECK()

/* ---------------------------------------------------------------- */
/* The connect knobs through /config                              */
/* ---------------------------------------------------------------- */

/* /config set writes both knobs to the shadow; the transport resolves
 * them from the store at the point of use (no push, no app-side copy). */
static void test_connect_knobs_via_config_command(void)
{
    pin_cfg_paths("connectcmd");
    AppHarness *h = harness_new("ollama:local", "m", NULL);
    ASSERT_NOT_NULL(h);
    NmConfig *cfg = cfg_for(h);

    harness_type(h, "/config set connect_timeout 1200");
    harness_enter(h);
    ASSERT_TRUE(strstr(harness_read(h),
                       "config: connect_timeout = 1200") != NULL);
    ASSERT_EQ(nm_connection_connect_timeout_ms(), 1200);

    harness_type(h, "/config set family_skip on");
    harness_enter(h);
    ASSERT_TRUE(strstr(harness_read(h), "config: family_skip = on") != NULL);
    ASSERT_STR_EQ(cfg_read_shadow(),
                  "connect_timeout = 1200\nfamily_skip = on\n");

    /* The table view reports both effective values. */
    harness_type(h, "/config");
    harness_enter(h);
    ASSERT_TRUE(strstr(harness_read(h), "1200") != NULL);
    ASSERT_TRUE(strstr(harness_read(h), "family_skip") != NULL);

    /* Garbage is refused; the current value stands. */
    harness_type(h, "/config set connect_timeout nope");
    harness_enter(h);
    ASSERT_TRUE(strstr(harness_read(h), "invalid value") != NULL);
    ASSERT_EQ(nm_connection_connect_timeout_ms(), 1200);

    /* reset clears the shadow line and the live value (budget back to
     * the transport's default). */
    harness_type(h, "/config reset connect_timeout");
    harness_enter(h);
    ASSERT_TRUE(strstr(harness_read(h), "config: connect_timeout reset") !=
                NULL);
    ASSERT_EQ(nm_connection_connect_timeout_ms(), NM_CONNECT_ATTEMPT_MS);
    ASSERT_STR_EQ(cfg_read_shadow(), "family_skip = on\n");

    /* The latch (skip_families) is its own key, and the POLICY gates the
     * EFFECT, not the stored value: with family_skip off the value is
     * still there (and /config says what it is doing — nothing), which
     * is what /config reset clears. */
    knobs_set_skip_families(NM_FAMILY_V6);
    ASSERT_TRUE(nm_connection_skipped_families() != 0);
    harness_type(h, "/config set family_skip off");
    harness_enter(h);
    ASSERT_TRUE(strstr(harness_read(h), "config: family_skip = off") != NULL);
    ASSERT_TRUE(nm_connection_skipped_families() != 0);
    harness_type(h, "/config");
    harness_enter(h);
    ASSERT_TRUE(strstr(harness_read(h), "inert: family_skip off") != NULL);
    harness_type(h, "/config reset skip_families");
    harness_enter(h);
    ASSERT_EQ(nm_connection_skipped_families(), 0);

    nm_config_free(cfg);
    harness_free(h);
    store_clear(NM_CFG_KEY_SKIP_FAMILIES);
    store_clear(NM_CFG_KEY_CONNECT_TIMEOUT);
}

/* A config file's connect knobs reach the transport when main.c's
 * wiring is reproduced (cfg_for does it): both land on the transport's
 * process-global slots. */
static void test_connect_knobs_from_config_reach_transport(void)
{
    pin_cfg_paths("connectcfg");
    write_file_at(g_cfg_user, "connect_timeout = 850\nfamily_skip = on\n");

    AppHarness *h = harness_new("ollama:local", "m", NULL);
    ASSERT_NOT_NULL(h);
    NmConfig *cfg = cfg_for(h);

    ASSERT_EQ(nm_connection_connect_timeout_ms(), 850);
    ASSERT_EQ(nm_connection_family_skip(), 1);

    /* family_skip is the ONE switch: with the POLICY off the latch
     * (skip_families) is INERT — the walk neither earns nor honours it
     * — so the value is a preference the user clears, not a veto. */
    knobs_set_skip_families(NM_FAMILY_V6);
    store_set(NM_CFG_KEY_FAMILY_SKIP, "off");
    ASSERT_EQ(nm_connection_family_skip(), 0);
    ASSERT_TRUE(nm_connection_skipped_families() != 0);
    store_clear(NM_CFG_KEY_SKIP_FAMILIES);
    ASSERT_EQ(nm_connection_skipped_families(), 0);

    nm_config_free(cfg);
    harness_free(h);
    store_clear(NM_CFG_KEY_SKIP_FAMILIES);
    store_clear(NM_CFG_KEY_CONNECT_TIMEOUT);
    store_clear(NM_CFG_KEY_FAMILY_SKIP);
}

/* Count non-overlapping occurrences of `needle` in `hay` (no regex —
 * a literal scan; the tests' own small helper). */
static int count_substr(const char *hay, const char *needle)
{
    int n = 0;
    size_t nl = strlen(needle);
    if (!nl)
        return 0;
    for (const char *p = hay; (p = strstr(p, needle)) != NULL; p += nl)
        n++;
    return n;
}

/* A latched family prints one line (the app notices the fresh bit) —
 * and only one: a later step with the same latch stays silent. */
static void test_family_skip_notice_prints_once(void)
{
    pin_cfg_paths("connectskip");
    AppHarness *h = harness_new("ollama:local", "m", NULL);
    ASSERT_NOT_NULL(h);
    NmConfig *cfg = cfg_for(h);
    harness_type(h, "/config set family_skip on");
    harness_enter(h);

    /* Drive the app's step: the latch lands at connect completion, and
     * the step is where the app reports a fresh one. */
    knobs_set_skip_families(NM_FAMILY_V6);
    nm_chat_app_step(h->app);
    tui_runtime_flush(h->rt);
    const char *out = harness_read(h);
    ASSERT_EQ(count_substr(out, "skipping it for this session"), 1);
    ASSERT_TRUE(strstr(out, "IPv6") != NULL);

    /* The latch did not change: a second step adds no skip line. */
    nm_chat_app_step(h->app);
    tui_runtime_flush(h->rt);
    ASSERT_EQ(count_substr(harness_read(h), "skipping it for this session"),
              1);

    nm_config_free(cfg);
    harness_free(h);
    store_clear(NM_CFG_KEY_SKIP_FAMILIES);
    store_clear(NM_CFG_KEY_FAMILY_SKIP);
}

/* ---------------------------------------------------------------- */
/* P3: the multi-fd wait set + job teardown                      */
/*                                                                    */
/* Jobs cannot be started on Windows yet (proc_w32.c stub,   */
/* P5), so the tests that need a live job are POSIX-only; the     */
/* fd-budget arithmetic below is checked on every platform.           */
/* ---------------------------------------------------------------- */

/* The fd budget: boba's pool must hold every job plus the agent's
 * own fd, or a job goes unsubscribed (and then undrained). The
 * compile-time typedef in chat_app.c pins it; this pins the arithmetic
 * at runtime too. */
static void test_job_cap_fits_the_fd_budget(void)
{
    ASSERT_TRUE((size_t)NM_PROC_MAX_JOBS + 1 <= TUI_IO_SOURCE_MAX);
#ifndef _WIN32
    AppHarness *h = harness_new("ollama:local", "test-model", NULL);
    ASSERT_NOT_NULL(h);
    nm_proc_reset(); /* a clean cap + registry, whatever ran before */

    nm_proc_set_max_jobs(4);
    char err[128];
    for (int i = 0; i < 4; i++) {
        int id = -1;
        ASSERT_NOT_NULL(
            nm_proc_start("sleep 30", NULL, NULL, &id, err, sizeof(err)));
    }
    /* One past the cap fails loudly instead of spawning a child nobody
     * will ever drain. */
    int id = -1;
    ASSERT_NULL(nm_proc_start("sleep 30", NULL, NULL, &id, err, sizeof(err)));
    ASSERT_TRUE(strstr(err, "cap") != NULL);

    NmSource set[TUI_IO_SOURCE_MAX];
    ASSERT_EQ(nm_chat_app_interest(h->app, set, TUI_IO_SOURCE_MAX), 4);
    for (size_t i = 0; i < 4; i++) {
        ASSERT_TRUE(set[i].handle >= 0);
        for (size_t j = i + 1; j < 4; j++)
            ASSERT_TRUE(set[i].handle != set[j].handle);
    }

    harness_free(h);
    nm_proc_reset(); /* later tests get the default cap back */
    ASSERT_EQ(nm_proc_count(), 0);
#endif
}

#ifdef _WIN32
/* The Windows job kind.  A registered job's wait entry must declare
 * NM_SRC_HANDLE — a waitable event — and not a descriptor/socket: boba
 * would otherwise hand a HANDLE to WSAEventSelect and fail to
 * subscribe it at all, so the job would never be drained and its child
 * would wedge on a full pipe.  The loop's dispatch must also resolve
 * the handle back to the job. */
static void test_windows_job_source_kind(void)
{
    AppHarness *h = harness_new("ollama:local", "test-model", NULL);
    ASSERT_NOT_NULL(h);

    NmSource set[TUI_IO_SOURCE_MAX];
    ASSERT_EQ(nm_chat_app_interest(h->app, set, TUI_IO_SOURCE_MAX), 0);

    char err[128];
    int id = -1;
    NmProc *p = nm_proc_start("ping -n 31 127.0.0.1 >nul", NULL, NULL, &id, err,
                              sizeof(err));
    ASSERT_NOT_NULL(p);
    ASSERT_TRUE(nm_proc_live(p) == 1); /* still running: a real job */

    size_t n = nm_chat_app_interest(h->app, set, TUI_IO_SOURCE_MAX);
    ASSERT_EQ(n, 1);
    ASSERT_EQ(set[0].handle, nm_proc_handle(p));
    ASSERT_EQ(set[0].flags, NM_INTEREST_READ);
    ASSERT_EQ(set[0].kind, NM_SRC_HANDLE);
    ASSERT_TRUE(nm_proc_by_handle(set[0].handle) == p);

    /* Routing: the handle resolves to the background job and drains it
     * (nothing is echoed, so this must not crash or step the agent). */
    nm_chat_app_external_ready(h->app, set[0].handle, TUI_IO_READ);

    nm_proc_close(p);
    ASSERT_EQ(nm_chat_app_interest(h->app, set, TUI_IO_SOURCE_MAX), 0);
    harness_free(h);
}
#endif

#ifndef _WIN32
/* Every registered job rides the wait set (one READ entry each),
 * because a job left out of the set is never drained and its child
 * stalls on a full PTY. Order is agent-first, then registry order; a
 * closed job's fd drops out. */
static void test_interest_lists_every_job(void)
{
    AppHarness *h = harness_new("ollama:local", "test-model", NULL);
    ASSERT_NOT_NULL(h);

    NmSource set[TUI_IO_SOURCE_MAX];
    /* Idle app, no jobs: nothing to wait on. */
    ASSERT_EQ(nm_chat_app_interest(h->app, set, TUI_IO_SOURCE_MAX), 0);
    ASSERT_EQ(nm_chat_app_interest(h->app, set, 0), 0);

    char err[128];
    int id1 = -1, id2 = -1;
    NmProc *p1 = nm_proc_start("sleep 30", NULL, NULL, &id1, err, sizeof(err));
    ASSERT_NOT_NULL(p1);
    NmProc *p2 = nm_proc_start("sleep 30", NULL, NULL, &id2, err, sizeof(err));
    ASSERT_NOT_NULL(p2);

    size_t n = nm_chat_app_interest(h->app, set, TUI_IO_SOURCE_MAX);
    ASSERT_EQ(n, 2);
    ASSERT_EQ(set[0].handle, nm_proc_handle(p1));
    ASSERT_EQ(set[1].handle, nm_proc_handle(p2));
    ASSERT_EQ(set[0].flags, NM_INTEREST_READ);
    ASSERT_EQ(set[1].flags, NM_INTEREST_READ);
    ASSERT_TRUE(set[0].handle != set[1].handle);
    /* nm_proc_by_handle resolves what the loop is handed. */
    ASSERT_TRUE(nm_proc_by_handle(set[1].handle) == p2);

    /* A closed job stops being declared. */
    nm_proc_close(p1);
    ASSERT_EQ(nm_chat_app_interest(h->app, set, TUI_IO_SOURCE_MAX), 1);
    ASSERT_EQ(set[0].handle, nm_proc_handle(p2));

    /* A cap smaller than the set truncates (the caller's pool bound),
     * and the API never writes past it. */
    ASSERT_EQ(nm_chat_app_interest(h->app, set, 1), 1);
    ASSERT_EQ(set[0].handle, nm_proc_handle(p2));

    harness_free(h);
    ASSERT_EQ(nm_proc_count(), 0); /* teardown killed the survivor */
}

/* App teardown is where jobs die: they are process-global (a tool's
 * userdata is a workdir path string, so it cannot carry a manager) and
 * they outlive the turn that started them. Without the close-all a dev
 * server the model started keeps running after the user quits. */
static void test_app_teardown_kills_jobs(void)
{
    AppHarness *h = harness_new("ollama:local", "test-model", NULL);
    ASSERT_NOT_NULL(h);

    char err[128];
    int id = -1;
    ASSERT_NOT_NULL(nm_proc_start("sleep 30", NULL, NULL, &id, err, sizeof(err)));
    ASSERT_EQ(nm_proc_count(), 1);

    harness_free(h); /* runtime -> component free -> nm_chat_app_free */
    ASSERT_EQ(nm_proc_count(), 0);
}

/* A background job's output is drained by the loop (via
 * nm_chat_app_external_ready) into its bounded buffer, and NOT echoed
 * to the transcript: the model reads it later with write_stdin, and
 * /ps is the human's window. */
static void test_external_ready_drains_background_job(void)
{
    AppHarness *h = harness_new("ollama:local", "test-model", NULL);
    ASSERT_NOT_NULL(h);

    char err[128];
    int id = -1;
    NmProc *p = nm_proc_start("printf 'background output\\n'; sleep 30",
                              NULL, NULL, &id, err, sizeof(err));
    ASSERT_NOT_NULL(p);
    int fd = nm_proc_handle(p);
    ASSERT_TRUE(fd >= 0);

    /* An unknown fd (and the -1 "nothing" case) is a safe no-op. */
    nm_chat_app_external_ready(h->app, -1, TUI_IO_READ);
    nm_chat_app_external_ready(h->app, fd + 1000, TUI_IO_READ);
    ASSERT_EQ(nm_proc_buffered(p), 0u);

    int drained = 0;
    for (int i = 0; i < 100 && !drained; i++) {
        fd_set fds;
        struct timeval tv = { 0, 20 * 1000 };
        FD_ZERO(&fds);
        FD_SET(fd, &fds);
        select(fd + 1, &fds, NULL, NULL, &tv);
        nm_chat_app_external_ready(h->app, fd, TUI_IO_READ);
        drained = nm_proc_buffered(p) > 0;
    }
    ASSERT_TRUE(drained);
    ASSERT_NOT_NULL(strstr(nm_proc_take_output(p), "background output"));

    /* Nothing reached the transcript (no echo of background output). */
    ASSERT_EQ(nm_chat_app_tail_len(h->app), 0u);

    harness_free(h);
    ASSERT_EQ(nm_proc_count(), 0);
}

/* An active exec_command's fd IS its job's master, so the wait set
 * carries it ONCE (boba treats a duplicated fd as undefined). After the
 * yield window closes and the call ends, the job is still live and
 * the set holds it on its own — which is the whole P3 point: the
 * job keeps draining after its tool call returned. */
static void test_interest_dedupes_active_exec_job(void)
{
    struct ServerScript sc;
    memset(&sc, 0, sizeof(sc));
    sc.n_rounds = 2;
    sc.sse[0] =
        "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,"
        "\"id\":\"call_x\",\"type\":\"function\",\"function\":"
        "{\"name\":\"exec_command\",\"arguments\":"
        "\"{\\\"cmd\\\":\\\"echo starting; sleep 30\\\","
        "\\\"yield_time_ms\\\":250}\"}}]}}]}\n\n"
        "data: [DONE]\n\n";
    sc.sse[1] =
        "data: {\"choices\":[{\"delta\":{\"content\":\"job up\"}}]}\n\n"
        "data: [DONE]\n\n";
    sc.fd = server_bind(&sc.port);
    ASSERT_TRUE(sc.fd >= 0);
    pthread_t th;
    pthread_create(&th, NULL, chat_server_thread, &sc);

    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1", sc.port);
    AppHarness *h = harness_new("ollama:local", "test-model", base);
    ASSERT_NOT_NULL(h);

    harness_type(h, "start it");
    harness_enter(h);

    /* Step until the job is up (RUNNING_TOOL with a live fd). */
    int spins = 0;
    while (spins++ < 2000) {
        if (nm_chat_app_state(h->app) == NM_AGENT_RUNNING_TOOL &&
            app_fd(h->app) >= 0)
            break;
        int fd = app_fd(h->app);
        unsigned in = app_interest(h->app);
        if (fd >= 0 && in) {
            fd_set r, w;
            struct timeval tv = { 0, 10 * 1000 };
            FD_ZERO(&r);
            FD_ZERO(&w);
            if (in & NM_INTEREST_READ)
                FD_SET(fd, &r);
            if (in & NM_INTEREST_WRITE)
                FD_SET(fd, &w);
            select(fd + 1, &r, &w, NULL, &tv);
        }
        nm_chat_app_step(h->app);
        tui_runtime_flush(h->rt);
    }
    ASSERT_EQ(nm_chat_app_state(h->app), NM_AGENT_RUNNING_TOOL);
    int job_handle = app_fd(h->app);
    ASSERT_TRUE(job_handle >= 0);
    ASSERT_EQ(nm_proc_count(), 1);
    ASSERT_EQ(job_handle, nm_proc_handle(nm_proc_at(0)));

    NmSource set[TUI_IO_SOURCE_MAX];
    ASSERT_EQ(nm_chat_app_interest(h->app, set, TUI_IO_SOURCE_MAX), 1);
    ASSERT_EQ(set[0].handle, job_handle);
    ASSERT_EQ(set[0].flags, NM_INTEREST_READ);

    /* Close the yield window — virtual time, so the exec's 250 ms
     * window costs nothing — then the call ends, the round finishes,
     * and the job survives on its own in the wait set. */
    nm_test_clock_advance_ms(1000);
    ASSERT_EQ(harness_drive(h, 2000), 0);
    ASSERT_EQ(nm_chat_app_state(h->app), NM_AGENT_DONE);
    ASSERT_EQ(app_fd(h->app), -1);
    /* The job is still REGISTERED — it outlives the call that started
     * it, and the model polls it with write_stdin later.
     *
     * The count is the ONE thing here that the platform can decide: a
     * job is retired when it EXITS, and `echo starting; sleep 30` does
     * not exit — but on macOS the shell's exit can be observed before
     * the sleep is gone (the PTY's session leader is the shell; the
     * `sleep` is its child, and the shell's own exit is what the reap
     * sees once the master is drained). So the registry may hold a
     * job whose child has gone: the assertion is that the JOB (the
     * handle the model polls) is still there, not that a process is
     * still running. What the test really pins is the DEDUPE: the
     * wait set carries the job's handle ONCE, and it is the same
     * handle the tool phase was waiting on. */
    ASSERT_TRUE(nm_proc_count() >= 1);
    NmProc *survivor = nm_proc_at(0);
    ASSERT_NOT_NULL(survivor);
    ASSERT_EQ(nm_proc_handle(survivor), job_handle);
    ASSERT_TRUE(harness_read(h) != NULL);
    ASSERT_EQ(nm_chat_app_interest(h->app, set, TUI_IO_SOURCE_MAX), 1);
    ASSERT_EQ(set[0].handle, job_handle);
    ASSERT_EQ(set[0].flags, NM_INTEREST_READ);

    harness_free(h);
    ASSERT_EQ(nm_proc_count(), 0);
    pthread_join(th, NULL);
    close(sc.fd);
}
#endif /* !_WIN32 */

/* ---------------------------------------------------------------- */
/* P4: /ps, /kill, and the exec spinner tier                         */
/* ---------------------------------------------------------------- */

#ifndef _WIN32
/* 1 when every non-ASCII sequence in `s` is well-formed. A byte-count
 * cut through a multi-byte character leaves a lone continuation byte;
 * this is how the /ps command column's cluster-safe elision is checked
 * (a cut codepoint would be invalid UTF-8 on the terminal). */
static int utf8_well_formed(const char *s)
{
    for (size_t i = 0; s[i];) {
        unsigned char c = (unsigned char)s[i];
        int len = c < 0x80             ? 1
                  : (c & 0xE0) == 0xC0 ? 2
                  : (c & 0xF0) == 0xE0 ? 3
                  : (c & 0xF8) == 0xF0 ? 4
                                       : 0;
        if (len == 0)
            return 0; /* a stray continuation byte or invalid lead */
        for (int k = 1; k < len; k++) {
            if (((unsigned char)s[i + k] & 0xC0) != 0x80)
                return 0;
        }
        i += (size_t)len;
    }
    return 1;
}
#endif

/* /ps with no jobs: a note, not an empty table (and no crash on an
 * empty registry). Runs on every platform. */
static void test_ps_without_jobs(void)
{
    AppHarness *h = harness_new("ollama:local", "test-model", NULL);
    ASSERT_NOT_NULL(h);

    harness_type(h, "/ps");
    harness_enter(h);
    char *clean = strip_frames(harness_read(h));
    ASSERT_NOT_NULL(clean);
    ASSERT_TRUE(strstr(clean, "no process jobs") != NULL);
    free(clean);

    harness_free(h);
}

/* /skills with nothing discovered: the honest "none found" line — the
 * tier main() pins is an empty directory, so this is exactly what a
 * user on a machine without skills sees. */
static void test_skills_reports_none_found(void)
{
    AppHarness *h = harness_new("ollama:local", "test-model", NULL);
    ASSERT_NOT_NULL(h);

    harness_type(h, "/skills");
    harness_enter(h);
    char *clean = strip_frames(harness_read(h));
    ASSERT_NOT_NULL(clean);
    ASSERT_TRUE(strstr(clean, "skills: none found") != NULL);
    free(clean);

    harness_free(h);
}

/* /skills lists what discovery found, by name and description. The
 * user tier is re-pointed at a fixture directory THIS test creates,
 * then restored to the empty one main() pinned (the seam is
 * process-global, like every other path seam). */
static void test_skills_lists_found_skills(void)
{
    char fixture[512];
    snprintf(fixture, sizeof(fixture), "%s/skills-fixture",
             test_scratch_dir());
    char skill[700], md[800];
    snprintf(skill, sizeof(skill), "%s/pdf-processing", fixture);
    chat_mkdir(fixture);
    chat_mkdir(skill);
    snprintf(md, sizeof(md), "%s/SKILL.md", skill);
    FILE *f = fopen(md, "wb");
    ASSERT_NOT_NULL(f);
    if (f) {
        fputs("---\nname: pdf-processing\n"
              "description: Extract text from PDF files.\n---\n\n"
              "# PDF\n\nUse pypdf.\n",
              f);
        fclose(f);
    }
    nm_skills_set_global_dir(fixture);

    AppHarness *h = harness_new("ollama:local", "test-model", NULL);
    ASSERT_NOT_NULL(h);

    harness_type(h, "/skills");
    harness_enter(h);
    char *clean = strip_frames(harness_read(h));
    ASSERT_NOT_NULL(clean);
    ASSERT_TRUE(strstr(clean, "skills: 1 found") != NULL);
    ASSERT_TRUE(strstr(clean, "pdf-processing") != NULL);
    ASSERT_TRUE(strstr(clean, "Extract text from PDF files.") != NULL);
    free(clean);

    harness_free(h);
    char none[512];
    snprintf(none, sizeof(none), "%s/no-skills", test_scratch_dir());
    nm_skills_set_global_dir(none);
}

#ifndef _WIN32
/* /ps lists every registered job with its id, state, command and
 * how much output is waiting; /kill <id> then removes exactly one. */
static void test_ps_lists_and_kill_removes(void)
{
    AppHarness *h = harness_new("ollama:local", "test-model", NULL);
    ASSERT_NOT_NULL(h);
    nm_proc_reset();

    char err[128];
    int id_run = -1, id_done = -1;
    NmProc *pr = nm_proc_start("echo hello; sleep 30", NULL, NULL, &id_run, err,
                               sizeof(err));
    ASSERT_NOT_NULL(pr);
    /* This one exits on its own: /ps must show BOTH states. */
    ASSERT_NOT_NULL(
        nm_proc_start("exit 3", NULL, NULL, &id_done, err, sizeof(err)));
    ASSERT_EQ(nm_proc_count(), 2);
    /* Wait for the second one to actually leave — on its own handle
     * (its exit is what makes the handle readable/EOF), not a sleep. */
    NmProc *d = NULL;
    for (int i = 0; i < 3000; i++) {
        d = nm_proc_find(id_done);
        if (d && nm_proc_exit(d) >= 0)
            break;
        if (d) {
            NmSource s = { nm_proc_handle(d), NM_INTEREST_READ,
                           nm_proc_source_kind() };
            app_wait_src(&s, 10);
        } else {
            usleep(1000);
        }
    }
    ASSERT_NOT_NULL(d);
    ASSERT_TRUE(nm_proc_exit(d) >= 0);

    harness_type(h, "/ps");
    harness_enter(h);
    char *clean = strip_frames(harness_read(h));
    ASSERT_NOT_NULL(clean);
    ASSERT_TRUE(strstr(clean, "2 process jobs:") != NULL);
    char idbuf[16];
    snprintf(idbuf, sizeof(idbuf), "%2d", id_run);
    ASSERT_NOT_NULL(strstr(clean, idbuf));
    snprintf(idbuf, sizeof(idbuf), "%2d", id_done);
    ASSERT_NOT_NULL(strstr(clean, idbuf));
    ASSERT_TRUE(strstr(clean, "running") != NULL);
    ASSERT_TRUE(strstr(clean, "exited 3") != NULL);
    ASSERT_TRUE(strstr(clean, "echo hello; sleep 30") != NULL);
    ASSERT_TRUE(strstr(clean, "buffered)") != NULL);
    free(clean);

    /* /kill <running id>: group-kill + report. */
    char cmd[32];
    snprintf(cmd, sizeof(cmd), "/kill %d", id_run);
    harness_type(h, cmd);
    harness_enter(h);
    clean = strip_frames(harness_read(h));
    ASSERT_NOT_NULL(clean);
    ASSERT_TRUE(strstr(clean, "killed job") != NULL);
    ASSERT_TRUE(strstr(clean, "echo hello; sleep 30") != NULL);
    free(clean);
    ASSERT_NULL(nm_proc_find(id_run));
    ASSERT_EQ(nm_proc_count(), 1);

    /* /kill on the already-exited one: unregistered, reported as such. */
    snprintf(cmd, sizeof(cmd), "/kill %d", id_done);
    harness_type(h, cmd);
    harness_enter(h);
    clean = strip_frames(harness_read(h));
    ASSERT_NOT_NULL(clean);
    ASSERT_TRUE(strstr(clean, "already exited") != NULL);
    free(clean);
    ASSERT_EQ(nm_proc_count(), 0);

    /* A HIDDEN job (nevermore's own machinery — the context <env> git
     * stage, a Windows run_command) is drained like any other but never
     * listed: /ps is the window on jobs the USER started. */
    int id_hidden = -1;
    ASSERT_NOT_NULL(nm_proc_start("echo HIDDEN-MARKER; sleep 30", NULL, NULL,
                                  &id_hidden, err, sizeof(err)));
    ASSERT_EQ(nm_proc_count(), 1);
    NmProc *hid = nm_proc_find(id_hidden);
    ASSERT_NOT_NULL(hid);
    nm_proc_set_hidden(hid, 1);
    harness_type(h, "/ps");
    harness_enter(h);
    clean = strip_frames(harness_read(h));
    ASSERT_NOT_NULL(clean);
    ASSERT_TRUE(strstr(clean, "no process jobs") != NULL);
    ASSERT_NULL(strstr(clean, "HIDDEN-MARKER"));
    free(clean);
    nm_proc_close(hid);
    ASSERT_EQ(nm_proc_count(), 0);

    /* Back to the empty note. */
    harness_type(h, "/ps");
    harness_enter(h);
    clean = strip_frames(harness_read(h));
    ASSERT_NOT_NULL(clean);
    ASSERT_TRUE(strstr(clean, "no process jobs") != NULL);
    free(clean);

    harness_free(h);
    nm_proc_reset();
}

/* /kill argument handling: no id, a non-numeric id, an over-long id and
 * an unknown id all refuse without touching the registry. */
static void test_kill_rejects_bad_ids(void)
{
    AppHarness *h = harness_new("ollama:local", "test-model", NULL);
    ASSERT_NOT_NULL(h);
    nm_proc_reset();

    char err[128];
    int id = -1;
    ASSERT_NOT_NULL(nm_proc_start("sleep 30", NULL, NULL, &id, err, sizeof(err)));

    harness_type(h, "/kill");
    harness_enter(h);
    harness_type(h, "/kill abc");
    harness_enter(h);
    harness_type(h, "/kill 0");
    harness_enter(h);
    harness_type(h, "/kill 12abc");
    harness_enter(h);
    harness_type(h, "/kill 999999999999");
    harness_enter(h);
    harness_type(h, "/kill 999");
    harness_enter(h);

    char *clean = strip_frames(harness_read(h));
    ASSERT_NOT_NULL(clean);
    ASSERT_NOT_NULL(strstr(clean, "expected a job id"));
    ASSERT_NOT_NULL(strstr(clean, "expected a positive job id"));
    ASSERT_NOT_NULL(strstr(clean, "no job 999"));
    free(clean);

    /* None of it touched the real job. */
    ASSERT_EQ(nm_proc_count(), 1);
    ASSERT_NOT_NULL(nm_proc_find(id));

    harness_free(h);
    nm_proc_reset();
    ASSERT_EQ(nm_proc_count(), 0);
}

/* The /ps command column: whitespace runs collapse, an over-long command
 * elides with "…", and the cut never splits a multi-byte character. */
static void test_ps_command_column_elides_safely(void)
{
    AppHarness *h = harness_new("ollama:local", "test-model", NULL);
    ASSERT_NOT_NULL(h);
    nm_proc_reset();

    /* Runs of spaces/tabs and a newline collapse to single spaces. */
    char err[128];
    int id_a = -1, id_b = -1;
    ASSERT_NOT_NULL(nm_proc_start("sleep\t\t 30; echo   a\nb", NULL, NULL, &id_a,
                                  err, sizeof(err)));
    ASSERT_NOT_NULL(
        nm_proc_start("sleep 30", NULL, NULL, &id_b, err, sizeof(err)));

    harness_type(h, "/ps");
    harness_enter(h);
    char *clean = strip_frames(harness_read(h));
    ASSERT_NOT_NULL(clean);
    ASSERT_NOT_NULL(strstr(clean, "sleep 30; echo a b"));
    /* No collapsed run survives as a double space. */
    ASSERT_TRUE(strstr(clean, "sleep\t") == NULL);
    ASSERT_TRUE(strstr(clean, "echo  a") == NULL);
    free(clean);

    /* An over-long ASCII command elides at the column budget. */
    char long_cmd[256];
    size_t o = 0;
    o += (size_t)snprintf(long_cmd + o, sizeof(long_cmd) - o, "echo");
    for (int i = 0; i < 40 && o + 5 < sizeof(long_cmd); i++)
        o += (size_t)snprintf(long_cmd + o, sizeof(long_cmd) - o, " word%d",
                              i);
    int id_c = -1;
    ASSERT_NOT_NULL(nm_proc_start(long_cmd, NULL, NULL, &id_c, err, sizeof(err)));
    harness_type(h, "/ps");
    harness_enter(h);
    clean = strip_frames(harness_read(h));
    ASSERT_NOT_NULL(clean);
    ASSERT_NOT_NULL(strstr(clean, "\xE2\x80\xA6")); /* elided with "…" */
    ASSERT_TRUE(utf8_well_formed(clean));
    free(clean);

    /* A wide character straddling the cut must not be halved: the
     * summary stays valid UTF-8 (each CJK glyph is 3 columns, so the
     * 46-column budget lands mid-run). */
    char wide[512];
    o = (size_t)snprintf(wide, sizeof(wide), "echo");
    for (int i = 0; i < 60; i++)
        o += (size_t)snprintf(wide + o, sizeof(wide) - o, " \xE6\xBC\xA2");
    int id_d = -1;
    ASSERT_NOT_NULL(nm_proc_start(wide, NULL, NULL, &id_d, err, sizeof(err)));
    harness_type(h, "/ps");
    harness_enter(h);
    clean = strip_frames(harness_read(h));
    ASSERT_NOT_NULL(clean);
    ASSERT_TRUE(utf8_well_formed(clean));
    ASSERT_NOT_NULL(strstr(clean, "\xE2\x80\xA6"));
    free(clean);

    harness_free(h);
    nm_proc_reset();
}

/* The exec spinner tier: while exec_command's yield window is open the
 * status row animates the STAR style — the same style run_command gets
 * (P4's spinner item), and now the only readout of it. */
static void test_exec_command_spinner_tier(void)
{
    struct ServerScript sc;
    memset(&sc, 0, sizeof(sc));
    sc.n_rounds = 1;
    sc.sse[0] =
        "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,"
        "\"id\":\"call_s\",\"type\":\"function\",\"function\":"
        "{\"name\":\"exec_command\",\"arguments\":"
        "\"{\\\"cmd\\\":\\\"sleep 30\\\",\\\"yield_time_ms\\\":5000}\"}}]}}]}\n\n"
        "data: [DONE]\n\n";
    sc.fd = server_bind(&sc.port);
    ASSERT_TRUE(sc.fd >= 0);
    pthread_t th;
    pthread_create(&th, NULL, chat_server_thread, &sc);

    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1", sc.port);
    AppHarness *h = harness_new("ollama:local", "test-model", base);
    ASSERT_NOT_NULL(h);
    nm_proc_reset(); /* this test owns the registry */

    harness_type(h, "start the server");
    harness_enter(h);

    int spins = 0;
    while (spins++ < 2000) {
        if (nm_chat_app_state(h->app) == NM_AGENT_RUNNING_TOOL &&
            app_fd(h->app) >= 0)
            break;
        int fd = app_fd(h->app);
        unsigned in = app_interest(h->app);
        if (fd >= 0 && in) {
            fd_set r, w;
            struct timeval tv = { 0, 10 * 1000 };
            FD_ZERO(&r);
            FD_ZERO(&w);
            if (in & NM_INTEREST_READ)
                FD_SET(fd, &r);
            if (in & NM_INTEREST_WRITE)
                FD_SET(fd, &w);
            select(fd + 1, &r, &w, NULL, &tv);
        }
        nm_chat_app_step(h->app);
        tui_runtime_flush(h->rt);
    }
    ASSERT_EQ(nm_chat_app_state(h->app), NM_AGENT_RUNNING_TOOL);
    ASSERT_TRUE(app_fd(h->app) >= 0);

    /* The yield window is open (a silent child): the spinner still
     * animates, in the star style — a rotating glyph in the star set's
     * yellow, and no label beside it. */
    nm_chat_app_tick(h->app);
    const char *frame = tui_runtime_render(h->rt);
    ASSERT_NOT_NULL(frame);
    char *glyph = span_bytes(tui_color_ansi(11), "\xc2\xb7 ");
    ASSERT_NOT_NULL(glyph);
    ASSERT_TRUE(strstr(frame, glyph) != NULL);
    free(glyph);
    ASSERT_TRUE(strstr(frame, "executing") == NULL);

    /* The window stays open on a silent child; interrupting the turn
     * returns the UI to idle and LEAVES THE JOB ALIVE (P3's whole
     * point: a call's end frees its state, not the job). */
    tui_runtime_send(h->rt, tui_msg_interrupt());
    ASSERT_EQ(nm_chat_app_state(h->app), NM_AGENT_IDLE);
    ASSERT_EQ(nm_proc_count(), 1);

    harness_free(h);
    ASSERT_EQ(nm_proc_count(), 0); /* teardown reaps the survivor */
    pthread_join(th, NULL);
    close(sc.fd);
}
#endif /* !_WIN32 */

/* ---------------------------------------------------------------- */
/* Images: /image, the pending set, the echo                         */
/* ---------------------------------------------------------------- */

/* A 64x32 PNG header (the sniffer reads headers only, no decoder). */
static const unsigned char CHAT_PNG[] = {
    0x89,
    'P',
    'N',
    'G',
    0x0d,
    0x0a,
    0x1a,
    0x0a, /* signature */
    0x00,
    0x00,
    0x00,
    0x0d,
    'I',
    'H',
    'D',
    'R', /* IHDR      */
    0x00,
    0x00,
    0x00,
    0x40, /* width 64  */
    0x00,
    0x00,
    0x00,
    0x20, /* height 32 */
};

/* Write the PNG header into the scratch cwd and hand back its path
 * (a static buffer: one fixture path per test). */
static const char *chat_png_fixture(const char *name)
{
    static char path[300];
    snprintf(path, sizeof(path), "%s/%s", test_scratch_dir(), name);
    FILE *f = fopen(path, "wb");
    if (f) {
        fwrite(CHAT_PNG, 1, sizeof(CHAT_PNG), f);
        fclose(f);
    }
    return path;
}

/* Submit one /image command for a path (the command takes the whole
 * remainder verbatim, spaces included). */
static void chat_img_cmd(AppHarness *h, const char *path)
{
    char cmd[320];
    snprintf(cmd, sizeof(cmd), "/image %s", path);
    harness_type(h, cmd);
    harness_enter(h);
}

static void test_image_command_attaches_lists_and_drops(void)
{
    AppHarness *h = harness_new("ollama:local", "test-model", NULL);
    ASSERT_NOT_NULL(h);

    const char *path = chat_png_fixture("chat-img.png");
    chat_img_cmd(h, path);
    const char *out = harness_read(h);
    /* the attach line: alt, format, dims, size */
    ASSERT_TRUE(strstr(out, "image: chat-img.png — PNG 64x32, 24 B") != NULL);

    /* bare /image lists the pending set, numbered (the -n argument) */
    harness_type(h, "/image");
    harness_enter(h);
    out = harness_read(h);
    ASSERT_TRUE(strstr(out, "images: 1 pending") != NULL);
    ASSERT_TRUE(strstr(out, "1  chat-img.png — PNG 64x32, 24 B") != NULL);

    /* /image -1 drops it (and says what went) */
    harness_type(h, "/image -1");
    harness_enter(h);
    ASSERT_TRUE(strstr(harness_read(h),
                       "image: chat-img.png — dropped (0 pending)") != NULL);
    harness_type(h, "/image");
    harness_enter(h);
    ASSERT_TRUE(strstr(harness_read(h), "images: none pending") != NULL);

    harness_free(h);
}

static void test_image_command_refusals(void)
{
    AppHarness *h = harness_new("ollama:local", "test-model", NULL);
    ASSERT_NOT_NULL(h);

    /* unreadable: named, not attached */
    harness_type(h, "/image /nonexistent-dir/nm-chat-img.png");
    harness_enter(h);
    ASSERT_TRUE(strstr(harness_read(h),
                       "not attached: source unreadable") != NULL);

    /* not a container we know */
    char txt[300];
    snprintf(txt, sizeof(txt), "%s/nm-chat-notimage.txt", test_scratch_dir());
    write_file_at(txt, "plain text, no image here");
    chat_img_cmd(h, txt);
    ASSERT_TRUE(strstr(harness_read(h),
                       "not attached: unknown container") != NULL);

    /* over the WIRE cap: refused locally, because those bytes would
     * ride every request */
    char big[300];
    snprintf(big, sizeof(big), "%s/nm-chat-big.png", test_scratch_dir());
    FILE *f = fopen(big, "wb");
    ASSERT_NOT_NULL(f);
    fwrite(CHAT_PNG, 1, sizeof(CHAT_PNG), f);
    ASSERT_EQ(fseek(f, (long)NM_IMAGE_MAX_WIRE_BYTES, SEEK_SET), 0);
    fputc('x', f);
    fclose(f);
    chat_img_cmd(h, big);
    ASSERT_TRUE(strstr(harness_read(h), "not attached: too large") != NULL);

    /* dropping from an empty set is an error, not a crash */
    harness_type(h, "/image -3");
    harness_enter(h);
    ASSERT_TRUE(strstr(harness_read(h), "is not a pending image") != NULL);

    /* nothing was ever attached */
    harness_type(h, "/image");
    harness_enter(h);
    ASSERT_TRUE(strstr(harness_read(h), "images: none pending") != NULL);

    harness_free(h);
}

/* The subcommands are matched as WHOLE words, so a path that merely
 * starts with one is still a path: /image save.png attaches a file
 * named save.png, it does not save anything. */
static void test_image_subcommands_are_whole_words(void)
{
    AppHarness *h = harness_new("ollama:local", "test-model", NULL);
    ASSERT_NOT_NULL(h);

    const char *path = chat_png_fixture("save.png");
    chat_img_cmd(h, path);
    const char *out = harness_read(h);
    ASSERT_TRUE(strstr(out, "image: save.png — PNG 64x32, 24 B") != NULL);
    /* It is PENDING (the attach side), not a save. */
    harness_type(h, "/image");
    harness_enter(h);
    out = harness_read(h);
    ASSERT_TRUE(strstr(out, "images: 1 pending") != NULL);
    ASSERT_TRUE(strstr(out, "1  save.png — PNG 64x32, 24 B") != NULL);
    ASSERT_TRUE(strstr(out, "saved image") == NULL);

    harness_free(h);
}

/* The pending set is consumed by the next message, the wire carries the
 * parts array, and the transcript shows the CAPTURED bytes (the echo is
 * the same IMAGE markdown the model's own images arrive as). */
static void test_img_submit_sends_parts_and_echoes(void)
{
    struct ServerScript sc;
    memset(&sc, 0, sizeof(sc));
    sc.n_rounds = 1;
    sc.sse[0] = "data: {\"choices\":[{\"delta\":{\"content\":\"a test "
                "image\"}}]}\n\n"
                "data: [DONE]\n\n";
    sc.fd = server_bind(&sc.port);
    ASSERT_TRUE(sc.fd >= 0);
    pthread_t th;
    pthread_create(&th, NULL, chat_server_thread, &sc);

    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1", sc.port);
    AppHarness *h = harness_new("ollama:local", "test-model", base);
    ASSERT_NOT_NULL(h);

    const char *path = chat_png_fixture("chat-img-send.png");
    chat_img_cmd(h, path);

    harness_type(h, "what is this?");
    harness_enter(h);
    ASSERT_EQ(nm_chat_app_state(h->app), NM_AGENT_STREAMING);
    ASSERT_EQ(harness_drive(h, 500), 0);

    /* the wire: content is a parts array — the text part first, then
     * the frozen image part */
    ASSERT_TRUE(strstr(g_request,
                       "\"content\":[{\"type\":\"text\",\"text\":\"what is "
                       "this?\"},{\"type\":\"image_url\",") != NULL);
    ASSERT_TRUE(strstr(g_request, "\"type\":\"image_url\"") != NULL);
    ASSERT_TRUE(strstr(g_request, "\"detail\"") == NULL);

    /* The tmpfile terminal never answers the profile probe; do the
     * verdict by hand and flush once more (exactly what the real
     * loop's tick does), so the held image batch commits as its
     * marker. The marker carries the alt, the format and the dims —
     * and never the payload. */
    h->rt->probe_state = 3;
    h->rt->profile.resolved = 1;
    tui_runtime_flush(h->rt);
    const char *out = harness_read(h);
    ASSERT_TRUE(strstr(out, "chat-img-send.png") != NULL);
    ASSERT_TRUE(strstr(out, "PNG 64x32") != NULL);
    ASSERT_TRUE(strstr(out, "base64,") == NULL);

    /* the pending set was consumed by the turn */
    harness_type(h, "/image");
    harness_enter(h);
    ASSERT_TRUE(strstr(harness_read(h), "images: none pending") != NULL);

    harness_free(h);
    close(sc.fd);
}

/* Vision gating is a warning, never a refusal (the wire takes the
 * image and the model answers blind — live-probed on hyper), and the
 * catalog flag is the only authority. opencode:go's static catalog
 * carries text-only ids; the offline pin keeps the lookup static. */
static void test_img_text_only_model_warns(void)
{
    /* glm-5.2: the shipped (models.dev) table says text-only. */
    AppHarness *h = harness_new("opencode:go", "glm-5.2", NULL);
    ASSERT_NOT_NULL(h);

    const char *path = chat_png_fixture("chat-img-warn.png");
    chat_img_cmd(h, path);
    const char *out = harness_read(h);
    /* attached all the same — the warning is a note, not a refusal */
    ASSERT_TRUE(strstr(out, "image: chat-img-warn.png — PNG 64x32") != NULL);
    ASSERT_TRUE(strstr(out, "note: glm-5.2 is text-only — the "
                            "provider strips image content") != NULL);

    /* the /model twin: the conversation already carries an image, and
     * the new model is text-only too */
    harness_type(h, "/model deepseek-v4-pro");
    harness_enter(h);
    ASSERT_TRUE(strstr(harness_read(h),
                       "note: deepseek-v4-pro is text-only — the provider "
                       "strips the 1 image in this conversation") != NULL);

    harness_free(h);
}

/* A provider switch rebuilds the agent — and the session owns the
 * attached images, so the pending set dies with it. That is reported,
 * never silent (an id into a dead store would be worse). */
static void test_img_pending_dropped_on_provider_switch(void)
{
    AppHarness *h = harness_new("ollama:local", "test-model", NULL);
    ASSERT_NOT_NULL(h);

    const char *path = chat_png_fixture("chat-img-switch.png");
    chat_img_cmd(h, path);
    ASSERT_TRUE(strstr(harness_read(h), "1 pending") == NULL ||
                strstr(harness_read(h), "image: chat-img-switch.png") != NULL);

    harness_type(h, "/provider ollama:local");
    harness_enter(h);
    const char *out = harness_read(h);
    ASSERT_TRUE(strstr(out, "— provider: ollama:local (fresh session) —") != NULL);
    ASSERT_TRUE(strstr(out,
                       "image: 1 pending attachment dropped with the "
                       "session") != NULL);
    /* the set is empty afterwards */
    harness_type(h, "/image");
    harness_enter(h);
    ASSERT_TRUE(strstr(harness_read(h), "images: none pending") != NULL);

    harness_free(h);
}

/* Count the kitty APC transmissions in the harness output (one per
 * rendered image). */
static size_t count_image_apc(const char *out)
{
    size_t n = 0;
    for (const char *p = out; (p = strstr(p, "\x1b_Ga=T,f=100")) != NULL; p++)
        n++;
    return n;
}

/* A terminal that renders images shows the attached image AT THE ATTACH
 * ("if supported"), and the turn that carries it does not show it a
 * second time: one image, one transmission. The profile is the
 * runtime's — resolved by hand here, since the tmpfile terminal never
 * answers the probe. */
static void test_img_attach_shows_the_image_when_supported(void)
{
    struct ServerScript sc;
    memset(&sc, 0, sizeof(sc));
    sc.n_rounds = 1;
    sc.sse[0] = "data: {\"choices\":[{\"delta\":{\"content\":\"a test "
                "image\"}}]}\n\n"
                "data: [DONE]\n\n";
    sc.fd = server_bind(&sc.port);
    ASSERT_TRUE(sc.fd >= 0);
    pthread_t th;
    pthread_create(&th, NULL, chat_server_thread, &sc);

    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1", sc.port);
    AppHarness *h = harness_new("ollama:local", "test-model", base);
    ASSERT_NOT_NULL(h);

    /* The terminal answered the kitty query: images are supported. */
    h->rt->probe_state = 3;
    h->rt->profile.resolved = 1;
    h->rt->profile.kitty_graphics = 1;

    const char *path = chat_png_fixture("chat-img-attach.png");
    chat_img_cmd(h, path);
    const char *out = harness_read(h);
    /* the attach line, then the image itself — not its marker */
    const char *named = strstr(out, "image: chat-img-attach.png — PNG 64x32");
    const char *apc = strstr(out, "\x1b_Ga=T,f=100,s=64,v=32");
    ASSERT_TRUE(named != NULL);
    ASSERT_TRUE(apc != NULL);
    ASSERT_TRUE(named < apc); /* the image lands BELOW the line naming it */
    ASSERT_TRUE(strstr(out, "no graphics support") == NULL);
    ASSERT_EQ(count_image_apc(out), 1);

    /* A second /image opens its OWN block: the attach finalizes the first
     * (its blank is the classifier's prev line), so the second image
     * renders too instead of continuing the first as a paragraph. */
    const char *path2 = chat_png_fixture("chat-img-attach2.png");
    chat_img_cmd(h, path2);
    out = harness_read(h);
    ASSERT_TRUE(strstr(out, "image: chat-img-attach2.png — PNG 64x32") != NULL);
    ASSERT_EQ(count_image_apc(out), 2);

    /* Submit: the wire carries the parts, and neither image is re-shown
     * under the user's line (the attaches' transmissions are the only
     * ones). */
    harness_type(h, "what is this?");
    harness_enter(h);
    ASSERT_EQ(nm_chat_app_state(h->app), NM_AGENT_STREAMING);
    ASSERT_EQ(harness_drive(h, 500), 0);
    ASSERT_TRUE(strstr(g_request,
                       "\"content\":[{\"type\":\"text\",\"text\":\"what is "
                       "this?\"},{\"type\":\"image_url\",") != NULL);
    out = harness_read(h);
    ASSERT_TRUE(strstr(out, "a test image") != NULL); /* the answer */
    ASSERT_EQ(count_image_apc(out), 2);

    harness_free(h);
    close(sc.fd);
}

/* read_file on an image: the tool panel names it,
 * and the image renders under that panel through the ONE pipeline —
 * the same markdown block /image posts, the same profile ladder. */
static void test_tool_read_file_image_renders_under_the_panel(void)
{
    const char *png = chat_png_fixture("nm-chat-tool-img.png");

    char sse0[1200];
    snprintf(sse0, sizeof(sse0),
             "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,"
             "\"id\":\"call_img\",\"type\":\"function\",\"function\":"
             "{\"name\":\"read_file\",\"arguments\":\"{\\\"path\\\":\\\"%s"
             "\\\"}\"}}]}}]}\n\n"
             "data: [DONE]\n\n",
             png);

    struct ServerScript sc;
    memset(&sc, 0, sizeof(sc));
    sc.n_rounds = 2;
    sc.sse[0] = sse0;
    sc.sse[1] =
        "data: {\"choices\":[{\"delta\":{\"content\":\"i see it\"}}]}\n\n"
        "data: [DONE]\n\n";
    sc.fd = server_bind(&sc.port);
    ASSERT_TRUE(sc.fd >= 0);
    pthread_t th;
    pthread_create(&th, NULL, chat_server_thread, &sc);

    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1", sc.port);
    AppHarness *h = harness_new("ollama:local", "test-model", base);
    ASSERT_NOT_NULL(h);

    /* A graphics terminal: the image renders (kitty APC). */
    h->rt->probe_state = 3;
    h->rt->profile.resolved = 1;
    h->rt->profile.kitty_graphics = 1;

    harness_type(h, "look at this");
    harness_enter(h);
    ASSERT_EQ(harness_drive(h, 500), 0);
    ASSERT_EQ(nm_chat_app_state(h->app), NM_AGENT_DONE);
    tui_runtime_flush(h->rt);

    const char *out = harness_read(h);
    /* the panel names the image (alt · format · dims · size) ... */
    const char *named = strstr(out, "[image] nm-chat-tool-img.png — PNG 64x32");
    const char *apc = strstr(out, "\x1b_Ga=T,f=100,s=64,v=32");
    ASSERT_NOT_NULL(named);
    ASSERT_NOT_NULL(apc);
    ASSERT_TRUE(named < apc); /* the image lands under its own panel */
    ASSERT_EQ(count_image_apc(out), 1);
    /* the fan-out rode the wire as a parts array (the synthetic user
     * message), and the answer follows the block */
    ASSERT_TRUE(strstr(g_request, "\"type\":\"image_url\"") != NULL);
    ASSERT_TRUE(strstr(out, "i see it") != NULL);

    harness_free(h);
    pthread_join(th, NULL);
    close(sc.fd);
}

/* The draft §4 repro: the model streams content text, THEN calls
 * read_file on a PNG. The image must reach the terminal as its own
 * explicit unit. Before the image-unit seam the image line was posted
 * as TEXT after a non-blank content line: the classifier saw a
 * paragraph in progress and committed ~the whole base64 payload into
 * the scrollback as literal text. */
static void test_tool_image_after_streamed_text_never_commits_payload(void)
{
    const char *png = chat_png_fixture("nm-chat-tool-img-after-text.png");

    char sse0[1400];
    snprintf(sse0, sizeof(sse0),
             "data: {\"choices\":[{\"delta\":{\"content\":\"Let me read "
             "that.\"}}]}\n\n"
             "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,"
             "\"id\":\"call_img\",\"type\":\"function\",\"function\":"
             "{\"name\":\"read_file\",\"arguments\":\"{\\\"path\\\":\\\"%s"
             "\\\"}\"}}]}}]}\n\n"
             "data: [DONE]\n\n",
             png);

    struct ServerScript sc;
    memset(&sc, 0, sizeof(sc));
    sc.n_rounds = 2;
    sc.sse[0] = sse0;
    sc.sse[1] =
        "data: {\"choices\":[{\"delta\":{\"content\":\"i see it\"}}]}\n\n"
        "data: [DONE]\n\n";
    sc.fd = server_bind(&sc.port);
    ASSERT_TRUE(sc.fd >= 0);
    pthread_t th;
    pthread_create(&th, NULL, chat_server_thread, &sc);

    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1", sc.port);
    AppHarness *h = harness_new("ollama:local", "test-model", base);
    ASSERT_NOT_NULL(h);

    /* A graphics terminal: the image renders (kitty APC). */
    h->rt->probe_state = 3;
    h->rt->profile.resolved = 1;
    h->rt->profile.kitty_graphics = 1;

    harness_type(h, "look at this");
    harness_enter(h);
    ASSERT_EQ(harness_drive(h, 500), 0);
    ASSERT_EQ(nm_chat_app_state(h->app), NM_AGENT_DONE);
    tui_runtime_flush(h->rt);

    const char *out = harness_read(h);
    /* the model's text committed as its own block ... */
    const char *text = strstr(out, "Let me read that.");
    ASSERT_NOT_NULL(text);
    /* ... the image rendered exactly once, BELOW that text ... */
    const char *apc = strstr(out, "\x1b_Ga=T,f=100,s=64,v=32");
    ASSERT_NOT_NULL(apc);
    ASSERT_TRUE(text < apc);
    ASSERT_EQ(count_image_apc(out), 1);
    /* ... and the payload NEVER reached the transcript as the markdown
     * data URL (the old failure committed it as paragraph text). The
     * kitty APC legitimately carries base64, so the check is the
     * data-URL text form, not a bare base64 substring. */
    ASSERT_TRUE(strstr(out, "data:image/png;base64,") == NULL);
    ASSERT_TRUE(strstr(out, "![") == NULL);
    ASSERT_TRUE(strstr(out, "i see it") != NULL);

    harness_free(h);
    pthread_join(th, NULL);
    close(sc.fd);
}

/* The same round on a terminal that cannot take the image: nothing is
 * posted, the panel's result line IS the record, and there is no marker
 * duplication (unlike /image, there is no later submit echo to carry
 * one). */
static void test_tool_read_file_image_degrades_to_the_panel_line(void)
{
    const char *png = chat_png_fixture("nm-chat-tool-img-dumb.png");

    char sse0[1200];
    snprintf(sse0, sizeof(sse0),
             "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,"
             "\"id\":\"call_img\",\"type\":\"function\",\"function\":"
             "{\"name\":\"read_file\",\"arguments\":\"{\\\"path\\\":\\\"%s"
             "\\\"}\"}}]}}]}\n\n"
             "data: [DONE]\n\n",
             png);

    struct ServerScript sc;
    memset(&sc, 0, sizeof(sc));
    sc.n_rounds = 2;
    sc.sse[0] = sse0;
    sc.sse[1] =
        "data: {\"choices\":[{\"delta\":{\"content\":\"i see it\"}}]}\n\n"
        "data: [DONE]\n\n";
    sc.fd = server_bind(&sc.port);
    ASSERT_TRUE(sc.fd >= 0);
    pthread_t th;
    pthread_create(&th, NULL, chat_server_thread, &sc);

    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1", sc.port);
    AppHarness *h = harness_new("ollama:local", "test-model", base);
    ASSERT_NOT_NULL(h);

    /* A resolved profile with NO graphics support: the gate says no. */
    h->rt->probe_state = 3;
    h->rt->profile.resolved = 1;
    h->rt->profile.kitty_graphics = 0;
    h->rt->profile.iterm2_images = 0;

    harness_type(h, "look at this");
    harness_enter(h);
    ASSERT_EQ(harness_drive(h, 500), 0);
    ASSERT_EQ(nm_chat_app_state(h->app), NM_AGENT_DONE);
    tui_runtime_flush(h->rt);

    const char *out = harness_read(h);
    /* the panel line is the record ... */
    ASSERT_TRUE(strstr(out, "[image] nm-chat-tool-img-dumb.png — PNG 64x32") !=
                NULL);
    /* ... nothing rendered, and no marker duplicated under it */
    ASSERT_EQ(count_image_apc(out), 0);
    ASSERT_TRUE(strstr(out, "no graphics support") == NULL);
    ASSERT_TRUE(strstr(out, "i see it") != NULL);

    harness_free(h);
    pthread_join(th, NULL);
    close(sc.fd);
}

/* A text-only model: the image is still attached and fanned out (the
 * wire takes it; the provider strips it), and the app says so. */
static void test_tool_read_file_image_text_only_model_warns(void)
{
    const char *png = chat_png_fixture("nm-chat-tool-img-warn.png");

    char sse0[1200];
    snprintf(sse0, sizeof(sse0),
             "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,"
             "\"id\":\"call_img\",\"type\":\"function\",\"function\":"
             "{\"name\":\"read_file\",\"arguments\":\"{\\\"path\\\":\\\"%s"
             "\\\"}\"}}]}}]}\n\n"
             "data: [DONE]\n\n",
             png);

    struct ServerScript sc;
    memset(&sc, 0, sizeof(sc));
    sc.n_rounds = 2;
    sc.sse[0] = sse0;
    sc.sse[1] =
        "data: {\"choices\":[{\"delta\":{\"content\":\"blind answer\"}}]}\n\n"
        "data: [DONE]\n\n";
    sc.fd = server_bind(&sc.port);
    ASSERT_TRUE(sc.fd >= 0);
    pthread_t th;
    pthread_create(&th, NULL, chat_server_thread, &sc);

    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1", sc.port);
    /* deepseek-v4-pro: the offline catalog says text-only (the
     * shipped table is models.dev's snapshot — the model is picked
     * from the ones it carries today). */
    AppHarness *h = harness_new("opencode:go", "deepseek-v4-pro", base);
    ASSERT_NOT_NULL(h);

    harness_type(h, "look at this");
    harness_enter(h);
    ASSERT_EQ(harness_drive(h, 500), 0);
    ASSERT_EQ(nm_chat_app_state(h->app), NM_AGENT_DONE);
    tui_runtime_flush(h->rt);

    const char *out = harness_read(h);
    ASSERT_TRUE(strstr(out, "note: deepseek-v4-pro is text-only — the "
                            "provider strips the image read_file attached") !=
                NULL);

    harness_free(h);
    pthread_join(th, NULL);
    close(sc.fd);
}

/* ---------------------------------------------------------------- */
/* Imagegen: a model-generated image renders                           */
/* through the one IMAGE-block pipeline                                */
/* ---------------------------------------------------------------- */

/* The received image's data URL (the 64x32 PNG fixture, base64'd). */
static void chat_recv_image_url(char *out, size_t cap)
{
    size_t b64_len = 0;
    char *b64 = nm_image_b64_encode(CHAT_PNG, sizeof(CHAT_PNG), &b64_len);
    snprintf(out, cap, "data:image/png;base64,%s", b64 ? b64 : "");
    free(b64);
}

/* The whole image arrives as ONE delta.images event, posts the same
 * markdown block /image posts, and renders through the one profile
 * ladder (kitty APC here). The editing turn replays it message-level. */
static void test_recv_image_renders_through_image_block(void)
{
    char url[256];
    chat_recv_image_url(url, sizeof(url));
    char sse0[1200];
    snprintf(sse0, sizeof(sse0),
             "data: {\"choices\":[{\"delta\":{\"content\":\"\",\"images\":"
             "[{\"type\":\"image_url\",\"image_url\":{\"url\":\"%s\"}}]}}]}"
             "\n\n"
             "data: {\"choices\":[{\"delta\":{\"content\":\"made it\"}}]}"
             "\n\n"
             "data: [DONE]\n\n",
             url);

    struct ServerScript sc;
    memset(&sc, 0, sizeof(sc));
    sc.n_rounds = 2;
    sc.sse[0] = sse0;
    sc.sse[1] =
        "data: {\"choices\":[{\"delta\":{\"content\":\"now blue\"}}]}\n\n"
        "data: [DONE]\n\n";
    sc.fd = server_bind(&sc.port);
    ASSERT_TRUE(sc.fd >= 0);
    pthread_t th;
    pthread_create(&th, NULL, chat_server_thread, &sc);

    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1", sc.port);
    AppHarness *h = harness_new("ollama:local", "test-model", base);
    ASSERT_NOT_NULL(h);

    /* A graphics terminal: the image renders (kitty APC). Resolve the
     * probe by hand (the tmpfile terminal never answers it). */
    h->rt->probe_state = 3;
    h->rt->profile.resolved = 1;
    h->rt->profile.kitty_graphics = 1;

    harness_type(h, "draw one");
    harness_enter(h);
    ASSERT_EQ(harness_drive(h, 500), 0);
    ASSERT_EQ(nm_chat_app_state(h->app), NM_AGENT_DONE);
    tui_runtime_flush(h->rt);

    const char *out = harness_read(h);
    /* ONE transmission (the block commits once), before the answer. */
    const char *apc = strstr(out, "\x1b_Ga=T,f=100,s=64,v=32");
    ASSERT_NOT_NULL(apc);
    ASSERT_EQ(count_image_apc(out), 1);
    ASSERT_TRUE(apc < strstr(out, "made it"));
    /* The caption a rendered picture needs (a picture carries no text
     * of its own): the id /image save takes, and the facts. */
    ASSERT_TRUE(strstr(out, "image #1 — PNG 64x32, 24 B") != NULL);
    /* the payload is never the transcript's text */
    ASSERT_TRUE(strstr(out, "base64,") == NULL);

    /* The editing turn: the assistant message replays the image
     * message-level (the probed shape), verbatim. */
    harness_type(h, "make it blue");
    harness_enter(h);
    ASSERT_EQ(harness_drive(h, 500), 0);
    ASSERT_TRUE(strstr(g_request,
                       "\"role\":\"assistant\",\"content\":\"made it\","
                       "\"images\":[{\"type\":\"image_url\"") != NULL);
    ASSERT_TRUE(strstr(g_request, url) != NULL);

    harness_free(h);
    pthread_join(th, NULL);
    close(sc.fd);
}

/* The same round on a terminal without graphics: the block commits as
 * the one-line MARKER (alt · format · dims · size), never the
 * payload — there is no second showing to dedupe against, so the
 * marker IS the record. */
static void test_recv_image_marker_on_dumb_terminal(void)
{
    char url[256];
    chat_recv_image_url(url, sizeof(url));
    char sse0[1200];
    snprintf(sse0, sizeof(sse0),
             "data: {\"choices\":[{\"delta\":{\"content\":\"\",\"images\":"
             "[{\"type\":\"image_url\",\"image_url\":{\"url\":\"%s\"}}]}}]}"
             "\n\n"
             "data: {\"choices\":[{\"delta\":{\"content\":\"made it\"}}]}"
             "\n\n"
             "data: [DONE]\n\n",
             url);

    struct ServerScript sc;
    memset(&sc, 0, sizeof(sc));
    sc.n_rounds = 1;
    sc.sse[0] = sse0;
    sc.fd = server_bind(&sc.port);
    ASSERT_TRUE(sc.fd >= 0);
    pthread_t th;
    pthread_create(&th, NULL, chat_server_thread, &sc);

    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1", sc.port);
    AppHarness *h = harness_new("ollama:local", "test-model", base);
    ASSERT_NOT_NULL(h);

    /* Resolved, no graphics. */
    h->rt->probe_state = 3;
    h->rt->profile.resolved = 1;
    h->rt->profile.kitty_graphics = 0;
    h->rt->profile.iterm2_images = 0;

    harness_type(h, "draw one");
    harness_enter(h);
    ASSERT_EQ(harness_drive(h, 500), 0);
    tui_runtime_flush(h->rt);

    const char *out = harness_read(h);
    /* The caption names the image — the id /image save takes — and the
     * first
     * image of a chat spells the command out once. */
    ASSERT_TRUE(strstr(out, "image #1") != NULL);
    ASSERT_TRUE(strstr(out, "/image save") != NULL);
    /* The block's own marker: the alt, the format, the dims. */
    ASSERT_TRUE(strstr(out, "\xe2\x96\x92 image") != NULL);
    ASSERT_TRUE(strstr(out, "PNG 64x32") != NULL);
    ASSERT_TRUE(strstr(out, "base64,") == NULL); /* never the payload */
    ASSERT_TRUE(strstr(out, "made it") != NULL);

    harness_free(h);
    pthread_join(th, NULL);
    close(sc.fd);
}

/* ---------------------------------------------------------------- */
/* /image save — the image that has no file of its own               */
/* ---------------------------------------------------------------- */

/* Read a whole file; its byte count, or -1. */
static long chat_slurp(const char *path, unsigned char *buf, size_t cap)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return -1;
    size_t n = fread(buf, 1, cap, f);
    fclose(f);
    return (long)n;
}

/* A received image is persisted by the APP, byte for byte: /image save
 * takes the id its caption printed, writes the conversation's own bytes
 * (no re-encode), and /image save list reads a chat back by the same
 * number. */
static void test_image_save_writes_the_received_image(void)
{
    char url[256];
    chat_recv_image_url(url, sizeof(url));
    char sse0[1200];
    snprintf(sse0, sizeof(sse0),
             "data: {\"choices\":[{\"delta\":{\"content\":\"\",\"images\":"
             "[{\"type\":\"image_url\",\"image_url\":{\"url\":\"%s\"}}]}}]}"
             "\n\n"
             "data: {\"choices\":[{\"delta\":{\"content\":\"made it\"}}]}"
             "\n\n"
             "data: [DONE]\n\n",
             url);

    struct ServerScript sc;
    memset(&sc, 0, sizeof(sc));
    sc.n_rounds = 1;
    sc.sse[0] = sse0;
    sc.fd = server_bind(&sc.port);
    ASSERT_TRUE(sc.fd >= 0);
    pthread_t th;
    pthread_create(&th, NULL, chat_server_thread, &sc);

    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1", sc.port);
    AppHarness *h = harness_new("ollama:local", "test-model", base);
    ASSERT_NOT_NULL(h);

    /* A terminal without graphics: the caption is the image's only
     * name — exactly the case /image save exists for. */
    h->rt->probe_state = 3;
    h->rt->profile.resolved = 1;
    h->rt->profile.kitty_graphics = 0;
    h->rt->profile.iterm2_images = 0;

    harness_type(h, "draw one");
    harness_enter(h);
    ASSERT_EQ(harness_drive(h, 500), 0);

    /* /image list: the chat read back, by the number the caption
     * gave. */
    harness_type(h, "/image list");
    harness_enter(h);
    const char *out = harness_read(h);
    ASSERT_TRUE(strstr(out, "images: 1 in this conversation") != NULL);
    ASSERT_TRUE(strstr(out, "#1  image — PNG 64x32, 24 B") != NULL);

    /* Bare /image save writes the newest image under the deterministic
     * name (ask mode's shape) and says what it wrote. */
    harness_type(h, "/image save");
    harness_enter(h);
    out = harness_read(h);
    ASSERT_TRUE(strstr(out, "saved image #1 → nevermore-image-1.png") != NULL);
    unsigned char got[512];
    long n = chat_slurp("nevermore-image-1.png", got, sizeof(got));
    ASSERT_EQ(n, (long)sizeof(CHAT_PNG));
    ASSERT_TRUE(memcmp(got, CHAT_PNG, sizeof(CHAT_PNG)) == 0);

    /* An explicit path is taken verbatim (spaces included). */
    harness_type(h, "/image save 1 nm-chat-saved.png");
    harness_enter(h);
    ASSERT_TRUE(strstr(harness_read(h),
                       "saved image #1 → nm-chat-saved.png") != NULL);
    n = chat_slurp("nm-chat-saved.png", got, sizeof(got));
    ASSERT_EQ(n, (long)sizeof(CHAT_PNG));
    ASSERT_TRUE(memcmp(got, CHAT_PNG, sizeof(CHAT_PNG)) == 0);

    /* An id that is not in this chat is refused by name, never by a
     * file. */
    harness_type(h, "/image save 2");
    harness_enter(h);
    ASSERT_TRUE(strstr(harness_read(h),
                       "image: '2' is not an image in this chat") != NULL);

    harness_free(h);
    pthread_join(th, NULL);
    close(sc.fd);
}

/* With nothing received (or attached), the save says so rather than
 * invent a file. */
static void test_image_save_without_images(void)
{
    AppHarness *h = harness_new("ollama:local", "test-model", NULL);
    ASSERT_NOT_NULL(h);
    harness_type(h, "/image save");
    harness_enter(h);
    ASSERT_TRUE(strstr(harness_read(h),
                       "image: no images in this conversation") != NULL);
    harness_type(h, "/image list");
    harness_enter(h);
    ASSERT_TRUE(strstr(harness_read(h), "none in this conversation") != NULL);
    harness_free(h);
}

/* ---------------------------------------------------------------- */
/* /session — the transcript itself                                  */
/* ---------------------------------------------------------------- */

/* A chat that has run a turn: /session reports the transcript's shape,
 * /session list numbers every message, and /session save writes the
 * markdown the agent would replay (session.c's writer). */
static void test_session_inspects_and_saves(void)
{
    struct ServerScript sc;
    memset(&sc, 0, sizeof(sc));
    sc.n_rounds = 1;
    /* The round streams a reasoning trace before its answer, so the
     * save's trace passage is pinned through the WHOLE path: the canned
     * wire -> the agent -> the session -> session.c's writer. */
    sc.sse[0] =
        "data: {\"choices\":[{\"delta\":{\"reasoning_content\":\"let me "
        "think\"}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{\"content\":\"hi there\"}}]}\n\n"
        "data: [DONE]\n\n";
    sc.fd = server_bind(&sc.port);
    ASSERT_TRUE(sc.fd >= 0);
    pthread_t th;
    pthread_create(&th, NULL, chat_server_thread, &sc);

    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1", sc.port);
    AppHarness *h = harness_new("ollama:local", "test-model", base);
    ASSERT_NOT_NULL(h);

    harness_type(h, "hello agent");
    harness_enter(h);
    ASSERT_EQ(harness_drive(h, 500), 0);
    ASSERT_EQ(nm_chat_app_state(h->app), NM_AGENT_DONE);
    tui_runtime_flush(h->rt);

    /* Bare /session: the shape at a glance. Message 0 is the assembled
     * system prompt; the turn added one user and one assistant message.
     * No image line: this chat has none (the line is omitted, never a
     * zero count). */
    harness_type(h, "/session");
    harness_enter(h);
    const char *out = harness_read(h);
    ASSERT_TRUE(strstr(out, "session: 3 messages — 1 user, 1 assistant, "
                            "0 tool, 1 system") != NULL);
    ASSERT_TRUE(strstr(out, "/session save [path] writes markdown") != NULL);
    ASSERT_TRUE(strstr(out, "in the store") == NULL);

    /* /session list: one numbered line per message, in transcript
     * order, with the content size. */
    harness_type(h, "/session list");
    harness_enter(h);
    out = harness_read(h);
    ASSERT_TRUE(strstr(out, "  #0   system") != NULL);
    ASSERT_TRUE(strstr(out, "  #1   user") != NULL);
    ASSERT_TRUE(strstr(out, "  #2   assistant") != NULL);
    ASSERT_TRUE(strstr(out, "11 B") != NULL); /* "hello agent" */
    /* The note the listing shows for a message the save now carries a
     * trace for: the two halves agree about what is in the transcript. */
    ASSERT_TRUE(strstr(out, "reasoning") != NULL);

    /* The save: an explicit path is taken verbatim (spaces included). */
    char path[300];
    snprintf(path, sizeof(path), "%s/nm-chat-session.md", test_scratch_dir());
    char cmd[340];
    snprintf(cmd, sizeof(cmd), "/session save %s", path);
    harness_type(h, cmd);
    harness_enter(h);
    out = harness_read(h);
    ASSERT_TRUE(strstr(out, "saved session → ") != NULL);
    ASSERT_TRUE(strstr(out, "3 messages, markdown") != NULL);

    unsigned char got[8192];
    long n = chat_slurp(path, got, sizeof(got) - 1);
    ASSERT_TRUE(n > 0);
    got[n] = '\0';
    const char *md = (const char *)got;
    /* The metadata header, then one `## <role>` section per message —
     * the transcript the agent replays, roles and text intact. Prose
     * (user/assistant) is live markdown, the trace included; the system
     * prompt is ours, so it rides a fence. Each message carries the meta
     * line the loader reads. */
    ASSERT_TRUE(strstr(md, "<!-- nevermore session: 3 messages -->") != NULL);
    ASSERT_TRUE(strstr(md, "<!-- nevermore session-format: 3 -->") != NULL);
    ASSERT_TRUE(strstr(md, "## system\n<!-- meta: {\"role\":\"system\","
                           "\"parts\":[[\"content\",") != NULL);
    ASSERT_TRUE(strstr(md, "## system\n<!-- meta:") != NULL);
    ASSERT_TRUE(strstr(md, "```text\n") != NULL);
    ASSERT_TRUE(strstr(md, "## user\n<!-- meta: {\"role\":\"user\","
                           "\"parts\":[[\"content\",11]]} -->\n"
                           "\nhello agent") != NULL);
    /* The round's trace, persisted: the labelled passage and the answer
     * after it, both live markdown ("let me think" is 12 bytes, "hi
     * there" 8). */
    ASSERT_TRUE(strstr(md, "## assistant\n<!-- meta: {\"role\":\"assistant\","
                           "\"parts\":[[\"reasoning\",12],[\"content\",8]]}"
                           " -->\n"
                           "\n### reasoning\n"
                           "\nlet me think\n"
                           "\n### content\n"
                           "\nhi there\n") != NULL);
    remove(path);

    /* Bare: the deterministic name, the shape /image save's default
     * has. */
    harness_type(h, "/session save");
    harness_enter(h);
    ASSERT_TRUE(strstr(harness_read(h),
                       "saved session → nevermore-session.md") != NULL);
    n = chat_slurp("nevermore-session.md", got, sizeof(got) - 1);
    ASSERT_TRUE(n > 0);
    remove("nevermore-session.md");

    /* An unknown subcommand is refused by name, never guessed at. */
    harness_type(h, "/session bogus");
    harness_enter(h);
    ASSERT_TRUE(strstr(harness_read(h),
                       "session: expected nothing, 'list', or 'save [path]'") != NULL);

    harness_free(h);
    pthread_join(th, NULL);
    close(sc.fd);
}

/* Nothing has been said yet: the commands say so rather than invent a
 * transcript or write an empty file. */
static void test_session_without_a_transcript(void)
{
    AppHarness *h = harness_new("ollama:local", "test-model", NULL);
    ASSERT_NOT_NULL(h);
    harness_type(h, "/session");
    harness_enter(h);
    ASSERT_TRUE(strstr(harness_read(h),
                       "session: nothing yet — no turn has run") != NULL);
    harness_type(h, "/session list");
    harness_enter(h);
    ASSERT_TRUE(strstr(harness_read(h),
                       "session: nothing yet — no turn has run") != NULL);
    harness_type(h, "/session save");
    harness_enter(h);
    ASSERT_TRUE(strstr(harness_read(h),
                       "session: nothing to save — no turn has run") != NULL);
    /* And nothing was written: the refusal is not a file. */
    unsigned char probe[64];
    ASSERT_TRUE(chat_slurp("nevermore-session.md", probe, sizeof(probe)) < 0);
    harness_free(h);
}

/* The picker row carries a right-aligned metadata column: the context
 * window and the capability badges (vision 👀, imagegen 🖼). Vision
 * was previously invisible; both badges are POSITIVE claims only. The
 * item VALUE stays the bare id (the metadata is a separate column), so
 * compose yields the wire id unchanged. */
static void test_model_picker_shows_capability_metadata(void)
{
    /* openrouter's GENERATED catalog: the active row is an image
     * generator that also claims vision and tools (the three badges at
     * once). The active row is PREPENDED, so it is always visible. */
    AppHarness *h = harness_new("openrouter", "google/gemini-3-pro-image", NULL);
    ASSERT_NOT_NULL(h);

    harness_type(h, "/model");
    harness_enter(h);
    const char *frame = tui_runtime_render(h->rt);
    ASSERT_TRUE(strstr(frame, "👀") != NULL); /* vision badge */
    /* imagegen badge, VS16 included: the selector is what the width
     * table reads as two cells (see format_model_meta). */
    ASSERT_TRUE(strstr(frame, "🖼️") != NULL);
    /* tool badge (tools == 1, VS16 included). */
    ASSERT_TRUE(strstr(frame, "🔧️") != NULL);
    ASSERT_TRUE(strstr(frame, "131k") != NULL); /* context window */

    /* Down to the next row, Enter composes the BARE id (the meta is a
     * separate column, never part of the value). */
    tui_runtime_send(h->rt, tui_msg_key(TUI_KEY_DOWN, 0, 0));
    tui_runtime_send(h->rt, tui_msg_key(TUI_KEY_ENTER, 0, 0));
    ASSERT_STR_EQ(tui_textinput_text(nm_chat_app_textinput(h->app)),
                  "/model aion-labs/aion-2.0");
    harness_enter(h);
    ASSERT_STR_EQ(nm_chat_app_model(h->app), "aion-labs/aion-2.0");

    harness_free(h);
}

/* "/model @image" opens the picker filtered to image generators. The
 * active vision model does NOT carry the capability, so it is not
 * prepended — the answer is only the models that claim it. */
static void test_model_picker_capability_query_image(void)
{
    AppHarness *h = harness_new("openrouter", "~openai/gpt-astra-latest", NULL);
    ASSERT_NOT_NULL(h);

    harness_type(h, "/model @image");
    harness_enter(h);
    const char *frame = tui_runtime_render(h->rt);
    char *prompt = span_bytes(nm_color_prompt(NM_COLOR_16), "\xe2\x9d\xaf ");
    ASSERT_NOT_NULL(prompt);
    const char *popup = frame_popup(frame, prompt);
    ASSERT_TRUE(strstr(frame, "google/gemini-2.5-flash-image") != NULL);
    /* Not in the LIST: the identity block names the active model, so the
     * absence is asserted on the popup region. */
    ASSERT_TRUE(strstr(popup, "~openai/gpt-astra-latest") == NULL);
    free(prompt);

    /* The first match: Enter composes it. */
    tui_runtime_send(h->rt, tui_msg_key(TUI_KEY_ENTER, 0, 0));
    ASSERT_STR_EQ(tui_textinput_text(nm_chat_app_textinput(h->app)),
                  "/model google/gemini-2.5-flash-image");

    harness_free(h);
}

/* "/model @tools" opens the picker filtered to the models whose catalog
 * CLAIMS tool use (tools == 1) — the same claim the 🔧 badge shows. The
 * vision model (tools == 0, "the catalog says nothing") and the image
 * generator (tools == -1, "listed without tools") both drop out, and
 * neither is a "no tools" answer either (a query answers with claims). */
static void test_model_picker_capability_query_tools(void)
{
    /* The active model claims NO tools (the catalog lists its
     * parameters without "tools"), so the filter drops it and the
     * popup lists only the tool-claiming rows. */
    AppHarness *h = harness_new("openrouter", "aion-labs/aion-rp-llama-3.1-8b", NULL);
    ASSERT_NOT_NULL(h);

    harness_type(h, "/model @tools aion-labs");
    harness_enter(h);
    const char *frame = tui_runtime_render(h->rt);
    char *prompt = span_bytes(nm_color_prompt(NM_COLOR_16), "\xe2\x9d\xaf ");
    ASSERT_NOT_NULL(prompt);
    const char *popup = frame_popup(frame, prompt);
    ASSERT_TRUE(strstr(frame, "aion-labs/aion-2.0") != NULL);
    ASSERT_TRUE(strstr(frame, "🔧️") != NULL);
    /* The identity block names the active model: assert the absences on
     * the popup region (what the picker LISTS). */
    ASSERT_TRUE(strstr(popup, "aion-labs/aion-rp-llama-3.1-8b") == NULL);
    free(prompt);

    /* The first match: Enter composes the bare id (no meta in the
     * value). */
    tui_runtime_send(h->rt, tui_msg_key(TUI_KEY_ENTER, 0, 0));
    ASSERT_STR_EQ(tui_textinput_text(nm_chat_app_textinput(h->app)),
                  "/model aion-labs/aion-2.0");

    harness_free(h);
}

/* A catalog with no tool claim at all answers the `@tools` filter with
 * the empty-catalog note, never a silent empty modal. */
static void test_model_picker_capability_query_tools_empty(void)
{
    /* ollama's static catalog claims no tools (all 0 = "says
     * nothing"), so the tool filter finds nothing. */
    AppHarness *h = harness_new("ollama:cloud", "gemma4:31b", NULL);
    ASSERT_NOT_NULL(h);

    harness_type(h, "/model @tools");
    harness_enter(h);
    ASSERT_TRUE(strstr(harness_read(h), "no tool-capable models") != NULL);
    ASSERT_STR_EQ(nm_chat_app_model(h->app), "gemma4:31b");

    harness_free(h);
}

/* The tool claim reaches the picker for the two providers whose whole
 * catalog claims it: hyper by a PROVIDER-level rule (its wire carries
 * no per-model tool field; every model takes tools) and opencode by
 * models.dev's `tool_call` (true for every row). So both badge every
 * row with the 🔧, not just OpenRouter's `supported_parameters` rows. */
static void test_model_picker_tool_badge_for_hyper_and_opencode(void)
{
    /* hyper: the offline pin keeps the lookup on the shipped one-row
     * static catalog, whose row carries the provider rule. */
    AppHarness *h = harness_new("hyper", "gpt-oss-120b", NULL);
    ASSERT_NOT_NULL(h);
    harness_type(h, "/model");
    harness_enter(h);
    ASSERT_TRUE(strstr(tui_runtime_render(h->rt), "🔧️") != NULL);
    harness_free(h);

    /* opencode:go: the generated table claims tools on every row. */
    h = harness_new("opencode:go", "deepseek-v4.1-flash", NULL);
    ASSERT_NOT_NULL(h);
    harness_type(h, "/model");
    harness_enter(h);
    ASSERT_TRUE(strstr(tui_runtime_render(h->rt), "🔧️") != NULL);
    harness_free(h);
}

/* "/model @vision" opens the picker filtered to vision models: the
 * image generator is filtered out, the active vision model remains. */
static void test_model_picker_capability_query_vision(void)
{
    AppHarness *h = harness_new("openrouter", "~openai/gpt-astra-latest", NULL);
    ASSERT_NOT_NULL(h);

    harness_type(h, "/model @vision");
    harness_enter(h);
    const char *frame = tui_runtime_render(h->rt);
    ASSERT_TRUE(strstr(frame, "~openai/gpt-astra-latest") != NULL);
    ASSERT_TRUE(strstr(frame, "google/gemini-3.1-flash-lite-image") == NULL);

    harness_free(h);
}

/* An unknown capability token is refused with the vocabulary, never a
 * silent id query. */
static void test_model_picker_capability_query_unknown(void)
{
    AppHarness *h = harness_new("openrouter", "~openai/gpt-astra-latest", NULL);
    ASSERT_NOT_NULL(h);

    harness_type(h, "/model @bogus");
    harness_enter(h);
    ASSERT_TRUE(strstr(harness_read(h), "unknown capability") != NULL);
    ASSERT_STR_EQ(nm_chat_app_model(h->app), "~openai/gpt-astra-latest");

    harness_free(h);
}

/* Grammar v2: `tag:NAME` filters by the entry's tags (NmEntry.tags),
 * the same vocabulary `@vision` reads. The openrouter static catalog
 * tags only the vision model. */
static void test_model_picker_grammar_tag(void)
{
    AppHarness *h = harness_new("openrouter", "~openai/gpt-astra-latest", NULL);
    ASSERT_NOT_NULL(h);

    /* Case-insensitive: the tag is stored lowercase. */
    harness_type(h, "/model tag:VISION");
    harness_enter(h);
    const char *frame = tui_runtime_render(h->rt);
    ASSERT_TRUE(strstr(frame, "~openai/gpt-astra-latest") != NULL);
    ASSERT_TRUE(strstr(frame, "google/gemini-3.1-flash-lite-image") == NULL);
    ASSERT_TRUE(strstr(frame, "meta-llama/llama-3.3-70b-instruct") == NULL);

    tui_runtime_send(h->rt, tui_msg_key(TUI_KEY_ENTER, 0, 0));
    ASSERT_STR_EQ(tui_textinput_text(nm_chat_app_textinput(h->app)),
                  "/model ~openai/gpt-astra-latest");
    harness_free(h);
}

/* Grammar v2: `ctx:<op>N` bounds the context window (k/M suffix,
 * 1000-based like the column). `ctx:>1M` keeps only the 1050000-ctx
 * model; the 131072 one is under the bound and the -1 (unknown) one
 * never satisfies it. */
static void test_model_picker_grammar_ctx(void)
{
    AppHarness *h = harness_new("openrouter", "~openai/gpt-astra-latest", NULL);
    ASSERT_NOT_NULL(h);

    harness_type(h, "/model ctx:>1M");
    harness_enter(h);
    const char *frame = tui_runtime_render(h->rt);
    ASSERT_TRUE(strstr(frame, "~openai/gpt-astra-latest") != NULL);
    ASSERT_TRUE(strstr(frame, "meta-llama/llama-3.3-70b-instruct") == NULL);
    ASSERT_TRUE(strstr(frame, "google/gemini-3.1-flash-lite-image") == NULL);
    harness_free(h);
}

/* Grammar v2: repeated ctx: tokens intersect — `ctx:>64k ctx:<1M` is
 * a range, so only the 131072 model lands (1050000 exceeds the upper
 * bound; -1 has no window). */
static void test_model_picker_grammar_ctx_range(void)
{
    AppHarness *h = harness_new("openrouter", "~openai/gpt-astra-latest", NULL);
    ASSERT_NOT_NULL(h);

    harness_type(h, "/model ctx:>64k ctx:<1M meta-llama");
    harness_enter(h);
    const char *frame = tui_runtime_render(h->rt);
    char *prompt = span_bytes(nm_color_prompt(NM_COLOR_16), "\xe2\x9d\xaf ");
    ASSERT_NOT_NULL(prompt);
    const char *popup = frame_popup(frame, prompt);
    ASSERT_TRUE(strstr(frame, "meta-llama/llama-3.3-70b-instruct") != NULL);
    /* The active model is out of the RANGE — absent from the list, though
     * the identity block still names it. */
    ASSERT_TRUE(strstr(popup, "~openai/gpt-astra-latest") == NULL);
    ASSERT_TRUE(strstr(frame, "google/gemini-3.1-flash-lite-image") == NULL);
    free(prompt);
    harness_free(h);
}

/* Grammar v2: tokens combine — a capability, a tag and a ctx bound
 * AND together, and free text rides along. `tag:vision ctx:>128k`
 * leaves the one model that claims both; adding `@image` (which that
 * model does not claim) empties the view with the generic note. */
static void test_model_picker_grammar_combines(void)
{
    AppHarness *h = harness_new("openrouter", "~openai/gpt-astra-latest", NULL);
    ASSERT_NOT_NULL(h);

    harness_type(h, "/model tag:vision ctx:>128k");
    harness_enter(h);
    const char *frame = tui_runtime_render(h->rt);
    ASSERT_TRUE(strstr(frame, "~openai/gpt-astra-latest") != NULL);
    ASSERT_TRUE(strstr(frame, "meta-llama/llama-3.3-70b-instruct") == NULL);
    tui_runtime_send(h->rt, tui_msg_key(TUI_KEY_ESCAPE, 0, 0));

    /* Three filters, no match: the image generators are all small. */
    harness_type(h, "/model tag:vision @image ctx:>2000000");
    harness_enter(h);
    ASSERT_TRUE(strstr(harness_read(h), "no models match the filter") != NULL);
    ASSERT_STR_EQ(nm_chat_app_model(h->app), "~openai/gpt-astra-latest");

    harness_free(h);
}

/* Grammar v2: a malformed token is a refusal (a named error), never a
 * silent id query — and the model is untouched. */
static void test_model_picker_grammar_errors(void)
{
    AppHarness *h = harness_new("openrouter", "~openai/gpt-astra-latest", NULL);
    ASSERT_NOT_NULL(h);

    harness_type(h, "/model ctx:bogus");
    harness_enter(h);
    ASSERT_TRUE(strstr(harness_read(h), "ctx: expects") != NULL);
    ASSERT_STR_EQ(nm_chat_app_model(h->app), "~openai/gpt-astra-latest");

    harness_type(h, "/model tag:");
    harness_enter(h);
    ASSERT_TRUE(strstr(harness_read(h), "tag: needs a name") != NULL);
    ASSERT_STR_EQ(nm_chat_app_model(h->app), "~openai/gpt-astra-latest");

    harness_free(h);
}

/* Regression: the picker once capped its rows at 128, silently
 * truncating a live catalog — with OpenRouter's (464 entries,
 * Sep 2026) that surfaced as "only one image_gen model", the sole
 * one before the cut; the other ten sat past it. Serve a 200-entry
 * catalog whose ONLY image generator is the last entry: the picker
 * must see past the old cap, filtered or not. Registered AFTER the
 * static-fallback openrouter picker tests above: a canned fetch
 * populates the provider's process-global live cache. */
#define BIG_CATALOG_FILLERS 199
static char big_catalog_body[64 * 1024];

static void *big_catalog_server_thread(void *arg)
{
    int lfd = *(int *)arg;
    int cfd = accept(lfd, NULL, NULL);
    if (cfd < 0)
        return NULL;
    /* Drain the GET: headers only, to the blank line. */
    char req[2048];
    size_t got = 0;
    while (got < sizeof(req) - 1) {
        long n = recv(cfd, req + got, sizeof(req) - 1 - got, 0);
        if (n <= 0)
            break;
        got += (size_t)n;
        req[got] = '\0';
        if (strstr(req, "\r\n\r\n"))
            break;
    }
    char head[160];
    int hl = snprintf(head, sizeof(head),
                      "HTTP/1.1 200 OK\r\n"
                      "Content-Type: application/json\r\n"
                      "Content-Length: %zu\r\n\r\n",
                      strlen(big_catalog_body));
    /* Hold the reply briefly. The picker's point is the ASYNC gap: /model
     * must return with the fetch still in flight. Loopback is fast enough
     * that the reply can already be sitting in the client's socket when
     * its first step looks — and under ASan (a slower, instrumented
     * client; this thread is not instrumented) that is the COMMON case,
     * which made the "loading the …" assertion flake. The hold makes the
     * pending state deterministic instead of a race. */
    usleep(200 * 1000);
    send(cfd, head, (size_t)hl, 0);
    size_t off = 0, bl = strlen(big_catalog_body);
    while (off < bl) {
        long n = send(cfd, big_catalog_body + off, bl - off, 0);
        if (n <= 0)
            break;
        off += (size_t)n;
    }
    close(cfd);
    close(lfd);
    return NULL;
}

static void test_model_picker_sees_past_the_old_row_cap(void)
{
    /* The openrouter wire shape (OPENROUTER-API.md §2/§5.1): text-only
     * fillers, then ONE image generator beyond the old 128-row cap. */
    size_t off = 0;
    off += (size_t)snprintf(big_catalog_body + off,
                            sizeof(big_catalog_body) - off, "{\"data\":[");
    for (int i = 0; i < BIG_CATALOG_FILLERS; i++) {
        off += (size_t)snprintf(
            big_catalog_body + off, sizeof(big_catalog_body) - off,
            "{\"id\":\"filler-%03d\",\"name\":\"Filler %d\","
            "\"context_length\":8192,"
            "\"architecture\":{\"input_modalities\":[\"text\"],"
            "\"output_modalities\":[\"text\"]}},",
            i, i);
    }
    off += (size_t)snprintf(
        big_catalog_body + off, sizeof(big_catalog_body) - off,
        "{\"id\":\"vendor/deep-image\",\"name\":\"Deep Image\","
        "\"context_length\":65536,"
        "\"architecture\":{\"input_modalities\":[\"text\"],"
        "\"output_modalities\":[\"image\",\"text\"]}}]}");
    ASSERT_TRUE(off < sizeof(big_catalog_body));

    int port;
    int lfd = server_bind(&port);
    ASSERT_TRUE(lfd >= 0);
    pthread_t th;
    pthread_create(&th, NULL, big_catalog_server_thread, &lfd);

    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1", port);
    AppHarness *h = harness_new("openrouter", "filler-000", base);
    ASSERT_NOT_NULL(h);

    /* /model @image: the deep generator is the ONLY match — past the
     * cap it was invisible and the answer was "none in the catalog".
     * The active filler carries no image_gen, so nothing prepends.
     * The catalog is a WIRE catalog here (an explicit base): /model
     * starts the fetch and returns, so pump the event loop until the
     * popup lands. */
    harness_type(h, "/model @image");
    harness_enter(h);
    ASSERT_TRUE(strstr(harness_read(h), "loading the openrouter") != NULL);
    ASSERT_EQ(harness_pump_catalog(h, 2000), 0);
    const char *frame = tui_runtime_render(h->rt);
    char *prompt = span_bytes(nm_color_prompt(NM_COLOR_16), "\xe2\x9d\xaf ");
    ASSERT_NOT_NULL(prompt);
    const char *popup = frame_popup(frame, prompt);
    ASSERT_TRUE(strstr(frame, "vendor/deep-image") != NULL);
    ASSERT_TRUE(strstr(frame, "🖼️") != NULL);
    /* The active filler is named by the identity block, not listed: the
     * absence is asserted on the popup region. */
    ASSERT_TRUE(strstr(popup, "filler-000") == NULL);
    free(prompt);

    /* The unfiltered cut: a plain query must find a deep row too. */
    tui_runtime_send(h->rt, tui_msg_key(TUI_KEY_ESCAPE, 0, 0));
    harness_type(h, "/model deep");
    harness_enter(h);
    ASSERT_EQ(harness_pump_catalog(h, 2000), 0);
    frame = tui_runtime_render(h->rt);
    ASSERT_TRUE(strstr(frame, "vendor/deep-image") != NULL);

    harness_free(h);
    pthread_join(th, NULL);
}

/* The image event may arrive with text already streaming (unobserved
 * but wire-possible): the content run must END first, or the posted
 * image line would continue the open paragraph and commit the data URL
 * as literal text. */
static void test_recv_image_after_text_opens_its_own_block(void)
{
    char url[256];
    chat_recv_image_url(url, sizeof(url));
    char sse0[1200];
    snprintf(sse0, sizeof(sse0),
             "data: {\"choices\":[{\"delta\":{\"content\":\"the "
             "picture:\"}}]}\n\n"
             "data: {\"choices\":[{\"delta\":{\"content\":\"\",\"images\":"
             "[{\"type\":\"image_url\",\"image_url\":{\"url\":\"%s\"}}]}}]}"
             "\n\n"
             "data: [DONE]\n\n",
             url);

    struct ServerScript sc;
    memset(&sc, 0, sizeof(sc));
    sc.n_rounds = 1;
    sc.sse[0] = sse0;
    sc.fd = server_bind(&sc.port);
    ASSERT_TRUE(sc.fd >= 0);
    pthread_t th;
    pthread_create(&th, NULL, chat_server_thread, &sc);

    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1", sc.port);
    AppHarness *h = harness_new("ollama:local", "test-model", base);
    ASSERT_NOT_NULL(h);

    h->rt->probe_state = 3;
    h->rt->profile.resolved = 1; /* no graphics: the marker is the record */

    harness_type(h, "draw");
    harness_enter(h);
    ASSERT_EQ(harness_drive(h, 500), 0);
    tui_runtime_flush(h->rt);

    const char *out = harness_read(h);
    ASSERT_TRUE(strstr(out, "the picture:") != NULL); /* the text committed */
    ASSERT_TRUE(strstr(out, "PNG 64x32") != NULL);    /* as its own block */
    ASSERT_TRUE(strstr(out, "base64,") == NULL);      /* never literal text */

    harness_free(h);
    pthread_join(th, NULL);
    close(sc.fd);
}

/* ---------------------------------------------------------------- */
/* Reminders in the UI (transparency)                                 */
/* ---------------------------------------------------------------- */

/* A nested reminder renders in the harness's own role, and the role
 * ENDS at the closing tag (the rest of the body is tool output again).
 * The bytes are the proof: a reminder must never read as tool output,
 * as the model's words, or as an error. */
static void test_reminder_panel_renders_in_its_own_role(void)
{
    AppHarness *h =
        harness_new("ollama:local", "test-model", "http://127.0.0.1:1/v1");
    ASSERT_NOT_NULL(h);

    char body[512];
    snprintf(body, sizeof(body),
             "STATUS\nOutput:\nreal output line\n"
             "\n" NM_REMINDER_TAG "\n"
             "This result was truncated by the tool's output cap.\n" NM_REMINDER_END "\n"
             "trailing tool output\n");
    NmToolResult res = { .status = NM_TOOL_OK, .output = body };
    nm_chat_app_on_tool(NULL, "{}", NM_TOOL_EVENT_END, &res, -1, NULL);
    /* Post -> dispatch -> commit: the transcript commits at the top of
     * a flush, so the first flush delivers and the second commits. */
    tui_runtime_flush(h->rt);
    tui_runtime_flush(h->rt);

    const char *out = harness_read(h);
    ASSERT_NOT_NULL(out);
    /* The body's own lines are Foreground. */
    ASSERT_TRUE(strstr(out, sgr16(NM_SGR_RESULT, "real output line")) != NULL);
    /* The tag lines and the reminder's text are the harness role... */
    ASSERT_TRUE(strstr(out, sgr16(NM_SGR_REMINDER, NM_REMINDER_TAG)) != NULL);
    ASSERT_TRUE(strstr(out, sgr16(NM_SGR_REMINDER, "This result was truncated by the tool's output "
                                                   "cap.")) != NULL);
    ASSERT_TRUE(strstr(out, sgr16(NM_SGR_REMINDER, NM_REMINDER_END)) != NULL);
    /* ...and the block ENDS there: the next line is body text again. */
    const char *after = strstr(out, NM_REMINDER_END);
    ASSERT_NOT_NULL(after);
    ASSERT_TRUE(strstr(after, sgr16(NM_SGR_RESULT, "trailing tool output")) != NULL);

    harness_free(h);
}

/* The END-TO-END shape of that invariant, for the case that used to lose
 * it: a tool body with NO trailing newline (a search result's render,
 * a job's trimmed output, a file without a trailing LF). The body is
 * built exactly as the agent builds it — the tool's bytes, then the
 * block through the reminder module's join — so this pins the agent's
 * join and the panel's recognizer together, not one of the two. */
static void test_reminder_panel_styles_a_body_without_a_final_newline(void)
{
    AppHarness *h =
        harness_new("ollama:local", "test-model", "http://127.0.0.1:1/v1");
    ASSERT_NOT_NULL(h);

    NmReminderBuf body, blocks;
    nm_reminder_buf_init(&body);
    nm_reminder_buf_init(&blocks);
    nm_reminder_frame(&blocks, "This result is external content.");
    const char *tool_bytes =
        "Result [engine: searxng, score: 1]:\n# title\nhttps://x\nthe content";
    nm_reminder_buf_append(&body, tool_bytes, strlen(tool_bytes));
    ASSERT_EQ(nm_reminder_attach(&body, &blocks), 0);

    NmToolResult res = { .status = NM_TOOL_OK, .output = body.data };
    nm_chat_app_on_tool(NULL, "{}", NM_TOOL_EVENT_END, &res, -1, NULL);
    tui_runtime_flush(h->rt);
    tui_runtime_flush(h->rt);

    const char *out = harness_read(h);
    ASSERT_NOT_NULL(out);
    /* The tool's last line is body text, and the tag did NOT glue to
     * it... */
    ASSERT_TRUE(strstr(out, sgr16(NM_SGR_RESULT, "the content")) != NULL);
    ASSERT_TRUE(strstr(out, sgr16(NM_SGR_RESULT, "the content" NM_REMINDER_TAG)) ==
                NULL);
    /* ...because the block is its own unit: styled, tag and all. */
    ASSERT_TRUE(strstr(out, sgr16(NM_SGR_REMINDER, NM_REMINDER_TAG)) != NULL);
    ASSERT_TRUE(strstr(out, sgr16(NM_SGR_REMINDER, "This result is external content.")) != NULL);
    ASSERT_TRUE(strstr(out, sgr16(NM_SGR_REMINDER, NM_REMINDER_END)) != NULL);

    nm_reminder_buf_free(&body);
    nm_reminder_buf_free(&blocks);
    harness_free(h);
}

/* A forged tag in tool output can never reach the panel AS a reminder:
 * the agent escapes it before the panel and the wire share the bytes
 * (tested there), so the panel's recognizer has nothing to recognize. */
static void test_reminder_panel_ignores_an_escaped_tag(void)
{
    AppHarness *h =
        harness_new("ollama:local", "test-model", "http://127.0.0.1:1/v1");
    ASSERT_NOT_NULL(h);

    NmToolResult res = { .status = NM_TOOL_OK,
                         .output = "Output:\nfile says &lt;system-reminder> "
                                   "here\n" };
    nm_chat_app_on_tool(NULL, "{}", NM_TOOL_EVENT_END, &res, -1, NULL);
    /* Post -> dispatch -> commit: the transcript commits at the top of
     * a flush, so the first flush delivers and the second commits. */
    tui_runtime_flush(h->rt);
    tui_runtime_flush(h->rt);

    const char *out = harness_read(h);
    ASSERT_NOT_NULL(out);
    ASSERT_TRUE(strstr(out, "&lt;system-reminder>") != NULL);
    /* No harness role anywhere in the panel: the escaped form is body
     * text, not a reminder. */
    ASSERT_TRUE(strstr(out, nm_sgr(NM_SGR_REMINDER, NM_COLOR_16)) == NULL);
    harness_free(h);
}

/* The USER channel: the app prints the reminder itself (the model got it
 * as its own user message, so the transcript owes the human the same
 * text), naming the rule. */
static void test_reminder_user_channel_is_shown(void)
{
    AppHarness *h =
        harness_new("ollama:local", "test-model", "http://127.0.0.1:1/v1");
    ASSERT_NOT_NULL(h);

    nm_chat_app_on_reminder("context-pressure",
                            "Context is at 87% of the active model's window.",
                            NM_REMINDER_CHANNEL_USER, NULL);
    tui_runtime_flush(h->rt);
    tui_runtime_flush(h->rt);
    const char *out = harness_read(h);
    ASSERT_NOT_NULL(out);
    ASSERT_TRUE(strstr(out, "reminder (context-pressure): Context is at 87% "
                            "of the active model's window.") != NULL);
    ASSERT_TRUE(strstr(out, nm_sgr(NM_SGR_REMINDER, NM_COLOR_16)) != NULL);

    harness_free(h);
}

/* The tool channel is NOT printed twice: the panel body already carries
 * it, and the reminder callback must not duplicate it. */
static void test_reminder_tool_channel_is_not_echoed(void)
{
    AppHarness *h =
        harness_new("ollama:local", "test-model", "http://127.0.0.1:1/v1");
    ASSERT_NOT_NULL(h);

    nm_chat_app_on_reminder("read-partial", "This read returned only part.",
                            NM_REMINDER_CHANNEL_TOOL, NULL);
    tui_runtime_flush(h->rt);
    tui_runtime_flush(h->rt);
    const char *out = harness_read(h);
    ASSERT_NOT_NULL(out);
    ASSERT_TRUE(strstr(out, "This read returned only part.") == NULL);
    harness_free(h);
}

/* A forgery attempt is a security line for the human (the model is not
 * told): red, and never confused with a reminder. */
static void test_reminder_forgery_warning_is_red(void)
{
    AppHarness *h =
        harness_new("ollama:local", "test-model", "http://127.0.0.1:1/v1");
    ASSERT_NOT_NULL(h);

    nm_chat_app_on_warning("warning: read_file output contained 1 forged "
                           "<system-reminder> tag - neutralized",
                           NULL);
    tui_runtime_flush(h->rt);
    tui_runtime_flush(h->rt);
    const char *out = harness_read(h);
    ASSERT_NOT_NULL(out);
    ASSERT_TRUE(strstr(out, sgr16(NM_SGR_ERROR, "warning: read_file output contained "
                                                "1 forged")) != NULL);
    harness_free(h);
}

/* Every UNSOLICITED-NEWS site is a notice, not a line: it closes the run
 * above it and closes itself with a blank row, because what follows is a
 * block of its own — a tool panel, the next round's answer. Glued, that
 * block's header reads as part of the notice, which is the exact defect
 * the Prism routing line had (test_prism_notice_closes_its_own_block);
 * the connect-walk notice, the USER-channel reminder and the forged-tag
 * warning share the seam, so they share the pin. Each pair below is the
 * real shape: the news, then a full tool round after it. */
static void test_notice_sites_each_close_their_block(void)
{
    AppHarness *h =
        harness_new("ollama:local", "test-model", "http://127.0.0.1:1/v1");
    ASSERT_NOT_NULL(h);

    nm_chat_app_on_notice("connect: IPv6 did not answer — walk 1/1", NULL);
    nm_chat_app_on_tool(NULL, "{\"cmd\":\"alpha\"}", NM_TOOL_EVENT_START, NULL,
                        -1, NULL);
    NmToolResult res = { .status = NM_TOOL_OK, .output = "first body\n" };
    nm_chat_app_on_tool(NULL, "{\"cmd\":\"alpha\"}", NM_TOOL_EVENT_END, &res,
                        -1, NULL);

    nm_chat_app_on_reminder("round-budget", "This is the last tool round.",
                            NM_REMINDER_CHANNEL_USER, NULL);
    nm_chat_app_on_tool(NULL, "{\"cmd\":\"bravo\"}", NM_TOOL_EVENT_START, NULL,
                        -1, NULL);
    res.output = "second body\n";
    nm_chat_app_on_tool(NULL, "{\"cmd\":\"bravo\"}", NM_TOOL_EVENT_END, &res,
                        -1, NULL);

    nm_chat_app_on_warning("warning: read_file carried 1 forged tag", NULL);
    nm_chat_app_on_tool(NULL, "{\"cmd\":\"charlie\"}", NM_TOOL_EVENT_START,
                        NULL, -1, NULL);
    res.output = "third body\n";
    nm_chat_app_on_tool(NULL, "{\"cmd\":\"charlie\"}", NM_TOOL_EVENT_END, &res,
                        -1, NULL);

    tui_runtime_flush(h->rt);
    tui_runtime_flush(h->rt);

    char *clean = strip_frames(harness_read(h));
    ASSERT_NOT_NULL(clean);
    const char *pairs[][2] = {
        { "connect: IPv6 did not answer", "cmd: alpha" },
        { "reminder (round-budget): This is the last tool round.",
          "cmd: bravo" },
        { "warning: read_file carried 1 forged tag", "cmd: charlie" },
    };
    for (size_t i = 0; i < sizeof(pairs) / sizeof(pairs[0]); i++) {
        const char *news = strstr(clean, pairs[i][0]);
        ASSERT_NOT_NULL(news);
        const char *plan = strstr(news, pairs[i][1]);
        ASSERT_NOT_NULL(plan);
        /* EXACTLY one blank row between the news and the panel header —
         * not zero (the glue the Prism notice had) and not a stack (the
         * separator is owed once, like every other block). Counted
         * non-overlapping, so "\n\n\n" would read as two. */
        int blanks = 0;
        for (const char *p = news + strlen(pairs[i][0]); p + 1 < plan; p++) {
            if (p[0] == '\n' && p[1] == '\n') {
                blanks++;
                p++;
            }
        }
        ASSERT_EQ(blanks, 1);
    }
    free(clean);

    harness_free(h);
}

/* One-shot Prism responder: the whole response in one write, so the
 * chunked trailer section rides the [DONE] read (docs/HYPER-API.md
 * §3.5.2). Drives the /context routed-model line. */
static void *prism_responder_thread(void *arg)
{
    int lfd = (int)(intptr_t)arg;
    int cfd = accept(lfd, NULL, NULL);
    if (cfd < 0)
        return NULL;
    char req[REQ_CAP];
    size_t got = 0;
    while (got < sizeof(req) - 1) {
        long n = recv(cfd, req + got, sizeof(req) - 1 - got, 0);
        if (n <= 0)
            break;
        got += (size_t)n;
        if (strstr(req, "\r\n\r\n") && got > 4 && req[got - 1] == '}')
            break;
    }
    static const char body[] =
        "data: {\"choices\":[{\"delta\":{\"content\":\"hi\"},"
        "\"finish_reason\":\"stop\"}]}\n\ndata: [DONE]\n\n";
    char resp[1024];
    size_t n = (size_t)snprintf(resp, sizeof(resp),
                                "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
                                "X-Prism-Model-Id: glm-5.3-flash\r\n"
                                "X-Prism-Model-Name: GLM 5.3 Flash\r\n"
                                "Transfer-Encoding: chunked\r\n\r\n");
    n += (size_t)snprintf(resp + n, sizeof(resp) - n, "%zx\r\n",
                          strlen(body));
    memcpy(resp + n, body, strlen(body));
    n += strlen(body);
    n += (size_t)snprintf(resp + n, sizeof(resp) - n, "\r\n");
    n += (size_t)snprintf(resp + n, sizeof(resp) - n,
                          "0\r\nX-Prism-Hypercredit-Savings: 12.5\r\n"
                          "X-Prism-Dollar-Savings: 0.0021\r\n\r\n");
    send(cfd, resp, n, 0);
    for (;;) {
        struct timeval tv = { 0, 50 * 1000 };
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(cfd, &rfds);
        if (select(cfd + 1, &rfds, NULL, NULL, &tv) <= 0)
            break;
        char sink[64];
        if (recv(cfd, sink, sizeof(sink), 0) <= 0)
            break; /* peer gone */
    }
    close(cfd);
    close(lfd);
    return NULL;
}

/* /context spells out the last completed round's Prism routing: which
 * model actually answered, and the savings (docs/HYPER-API.md §3.5.2).
 * A turn with no routing headers shows neither line (the "direct"
 * case, which is what the gateway does today). */
static void test_context_reports_the_prism_routed_model(void)
{
    int port;
    int lfd = server_bind(&port);
    ASSERT_TRUE(lfd >= 0);
    pthread_t th;
    pthread_create(&th, NULL, prism_responder_thread, (void *)(intptr_t)lfd);

    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1", port);
    AppHarness *h = harness_new("hyper", "glm-5.3-flash", base);
    ASSERT_NOT_NULL(h);

    harness_type(h, "hi");
    harness_enter(h);
    ASSERT_EQ(harness_drive(h, 500), 0);
    ASSERT_EQ(nm_chat_app_state(h->app), NM_AGENT_DONE);

    harness_type(h, "/context");
    harness_enter(h);
    const char *out = harness_read(h);
    ASSERT_NOT_NULL(out);
    ASSERT_TRUE(strstr(out, "context: last round routed to glm-5.3-flash "
                            "(GLM 5.3 Flash)") != NULL);
    ASSERT_TRUE(strstr(out, "context: routing saved 12.5 hypercredits / "
                            "$0.0021") != NULL);

    harness_free(h);
    pthread_join(th, NULL);
    close(lfd);
}

/* The notice the `prism_notice` key gates (default on): a routed round
 * says which model actually answered, on the SYSTEM stream, under the
 * round it describes. The line is terminal-only — the conversation the
 * model receives never carries it, so /session's counts stay what a
 * direct round's are and no request prefix can change. The savings
 * figure rides the same line when the gateway reported one (the canned
 * trailers here). */
static void test_prism_notice_prints_under_a_routed_round(void)
{
    int port;
    int lfd = server_bind(&port);
    ASSERT_TRUE(lfd >= 0);
    pthread_t th;
    pthread_create(&th, NULL, prism_responder_thread, (void *)(intptr_t)lfd);

    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1", port);
    AppHarness *h = harness_new("hyper", "glm-5.3-flash", base);
    ASSERT_NOT_NULL(h);

    harness_type(h, "hi");
    harness_enter(h);
    ASSERT_EQ(harness_drive(h, 500), 0);
    ASSERT_EQ(nm_chat_app_state(h->app), NM_AGENT_DONE);

    const char *out = harness_read(h);
    ASSERT_NOT_NULL(out);
    ASSERT_TRUE(strstr(out, "prism: routed to glm-5.3-flash "
                            "(GLM 5.3 Flash) · saved 12.5 hypercredits / "
                            "$0.0021") != NULL);
    /* The conversation is untouched: /session counts the same three
     * messages a direct round leaves (user, assistant, the system
     * prompt) — a notice is UI output, never transcript material. */
    harness_type(h, "/session");
    harness_enter(h);
    out = harness_read(h);
    ASSERT_TRUE(strstr(out, "session: 3 messages — 1 user, 1 assistant, "
                            "0 tool, 1 system") != NULL);

    harness_free(h);
    pthread_join(th, NULL);
    close(lfd);
}

/* The notice is a BLOCK, not a prefix: one blank line closes it, the
 * same separator every other system-stream block ends with. A routed
 * TOOL round is the shape that shows the difference — the round streams
 * its text, the notice lands at the round's finish, and that SAME
 * round's tool panel is flushed right after it, inside one step. With no
 * closing blank the panel header starts on the notice's heels and reads
 * as part of it. Both canned rounds carry the routing headers in the
 * response head (the streaming case, docs/HYPER-API.md §3.5.2). */
static void test_prism_notice_closes_its_own_block(void)
{
    FILE *f = fopen(FIXTURE_PATH, "wb");
    ASSERT_NOT_NULL(f);
    fputs("the quick brown fox\n", f);
    fclose(f);

    struct ServerScript sc;
    memset(&sc, 0, sizeof(sc));
    sc.n_rounds = 2;
    sc.prism_head = "X-Prism-Model-Id: glm-5.3-flash\r\n"
                    "X-Prism-Model-Name: GLM 5.3 Flash\r\n";
    sc.sse[0] =
        "data: {\"choices\":[{\"delta\":{\"content\":\"let me edit\","
        "\"tool_calls\":[{\"index\":0,\"id\":\"call_1\","
        "\"type\":\"function\",\"function\":{\"name\":\"edit_file\","
        "\"arguments\":\"{\\\"path\\\":\\\"" FIXTURE_PATH
        "\\\",\\\"old_string\\\":\\\"quick brown\\\",\\\"new_string\\\":"
        "\\\"slow red\\\"}\"}}]}}]}\n\n"
        "data: [DONE]\n\n";
    sc.sse[1] =
        "data: {\"choices\":[{\"delta\":{\"content\":\"edited the file\"}}]}\n\n"
        "data: [DONE]\n\n";
    sc.fd = server_bind(&sc.port);
    ASSERT_TRUE(sc.fd >= 0);
    pthread_t th;
    pthread_create(&th, NULL, chat_server_thread, &sc);

    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1", sc.port);
    AppHarness *h = harness_new("hyper", "glm-5.3-flash", base);
    ASSERT_NOT_NULL(h);

    harness_type(h, "change quick to slow");
    harness_enter(h);
    ASSERT_EQ(harness_drive(h, 500), 0);
    ASSERT_EQ(nm_chat_app_state(h->app), NM_AGENT_DONE);

    char *clean = strip_frames(harness_read(h));
    ASSERT_NOT_NULL(clean);
    const char *answer = strstr(clean, "let me edit");
    const char *notice =
        strstr(clean, "prism: routed to glm-5.3-flash (GLM 5.3 Flash)");
    const char *plan = strstr(clean, "edit_file");
    ASSERT_NOT_NULL(answer);
    ASSERT_NOT_NULL(notice);
    ASSERT_NOT_NULL(plan);
    /* The round's own bookends: the notice sits UNDER the text the round
     * streamed and ABOVE the tool call that same round made — it
     * describes the round that just answered, never the next one. */
    ASSERT_TRUE(answer < notice);
    ASSERT_TRUE(notice < plan);
    /* And it stands alone at both ends: no neighbour shares its rows. */
    ASSERT_TRUE(has_blank_row(answer, notice));
    ASSERT_TRUE(has_blank_row(notice, plan));
    free(clean);

    remove(FIXTURE_PATH);
    harness_free(h);
    pthread_join(th, NULL);
    close(sc.fd);
}

/* The gate: `prism_notice off` silences the LINE and nothing else —
 * /context still reports the routing, because the key owns the notice,
 * not the capture (conf.h). */
static void test_prism_notice_off_silences_only_the_line(void)
{
    pin_cfg_paths("prism-notice-off");
    int port;
    int lfd = server_bind(&port);
    ASSERT_TRUE(lfd >= 0);
    pthread_t th;
    pthread_create(&th, NULL, prism_responder_thread, (void *)(intptr_t)lfd);

    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1", port);
    AppHarness *h = harness_new("hyper", "glm-5.3-flash", base);
    ASSERT_NOT_NULL(h);
    NmConfig *cfg = cfg_for(h);
    ASSERT_NOT_NULL(cfg);
    /* The runtime layer, the way /config set does — takes effect this
     * chat, and /config reports it. */
    ASSERT_EQ(nm_config_runtime_set(cfg, NM_CFG_KEY_PRISM_NOTICE, "off"), 0);

    harness_type(h, "hi");
    harness_enter(h);
    ASSERT_EQ(harness_drive(h, 500), 0);
    ASSERT_EQ(nm_chat_app_state(h->app), NM_AGENT_DONE);

    const char *out = harness_read(h);
    ASSERT_NOT_NULL(out);
    ASSERT_TRUE(strstr(out, "prism: routed") == NULL);

    /* The capture is untouched: /context still answers. */
    harness_type(h, "/context");
    harness_enter(h);
    out = harness_read(h);
    ASSERT_TRUE(strstr(out, "context: last round routed to glm-5.3-flash "
                            "(GLM 5.3 Flash)") != NULL);

    nm_config_free(cfg);
    harness_free(h);
    pthread_join(th, NULL);
    close(lfd);
}

/* The framing tail, full stack: a routed round whose trailer section
 * (the savings) arrives AFTER the [DONE] read — the shape of a gateway
 * that computes the figure once the stream ends (docs/HYPER-API.md
 * §3.5.2). The round must stay PENDING through the tail and still
 * produce the complete notice: the line is composed from the round
 * meta at the framing end, so it carries the figure, not a partial. */
static void test_prism_notice_prints_under_a_late_trailer_round(void)
{
    int port;
    int lfd = server_bind(&port);
    ASSERT_TRUE(lfd >= 0);
    struct ServerScript sc;
    memset(&sc, 0, sizeof(sc));
    sc.n_rounds = 1;
    sc.sse[0] =
        "data: {\"choices\":[{\"delta\":{\"content\":\"hi\"},"
        "\"finish_reason\":\"stop\"}]}\n\ndata: [DONE]\n\n";
    sc.prism_head =
        "X-Prism-Model-Id: glm-5.3-flash\r\n"
        "X-Prism-Model-Name: GLM 5.3 Flash\r\n";
    sc.prism_trailer =
        "X-Prism-Hypercredit-Savings: 12.5\r\n"
        "X-Prism-Dollar-Savings: 0.0021\r\n";
    sc.terminator_delay_ms = 40; /* well inside the tail budget */
    sc.fd = server_bind(&sc.port);
    ASSERT_TRUE(sc.fd >= 0);
    pthread_t th;
    pthread_create(&th, NULL, chat_server_thread, &sc);

    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1", sc.port);
    AppHarness *h = harness_new("hyper", "glm-5.3-flash", base);
    ASSERT_NOT_NULL(h);

    harness_type(h, "hi");
    harness_enter(h);
    ASSERT_EQ(harness_drive(h, 500), 0);
    ASSERT_EQ(nm_chat_app_state(h->app), NM_AGENT_DONE);

    const char *out = harness_read(h);
    ASSERT_NOT_NULL(out);
    ASSERT_TRUE(strstr(out, "prism: routed to glm-5.3-flash "
                            "(GLM 5.3 Flash) · saved 12.5 hypercredits / "
                            "$0.0021") != NULL);

    harness_free(h);
    pthread_join(th, NULL);
    close(sc.fd);
}

/* The tail budget's expiry, on the fake clock: [DONE] arrives, the
 * connection is held WITHOUT the terminating chunk, and no trailer
 * ever comes. The round still ends DONE — routing, no figure, no
 * error — because the tail's own budget (the stream's deadline seam,
 * 250 ms) is what ends it, and a no-figure answer beats a stalled or
 * failed one. Virtual time is the only honest way to spend a
 * deadline; real readiness consumes the events first. */
static void test_prism_tail_budget_ends_the_round_without_a_figure(void)
{
    int port;
    int lfd = server_bind(&port);
    ASSERT_TRUE(lfd >= 0);
    struct ServerScript sc;
    memset(&sc, 0, sizeof(sc));
    sc.n_rounds = 1;
    sc.sse[0] =
        "data: {\"choices\":[{\"delta\":{\"content\":\"hi\"},"
        "\"finish_reason\":\"stop\"}]}\n\ndata: [DONE]\n\n";
    sc.prism_head =
        "X-Prism-Model-Id: glm-5.3-flash\r\n"
        "X-Prism-Model-Name: GLM 5.3 Flash\r\n";
    sc.stall_at_end = 1; /* no terminator, no trailer: the tail hangs */
    sc.fd = server_bind(&sc.port);
    ASSERT_TRUE(sc.fd >= 0);
    pthread_t th;
    pthread_create(&th, NULL, chat_server_thread, &sc);

    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1", sc.port);
    AppHarness *h = harness_new("hyper", "glm-5.3-flash", base);
    ASSERT_NOT_NULL(h);

    harness_type(h, "hi");
    harness_enter(h);
    for (int i = 0; i < 400; i++) {
        NmAgentState st = nm_chat_app_state(h->app);
        if (st == NM_AGENT_DONE || st == NM_AGENT_ERROR)
            break;
        NmSource s = nm_chat_app_source(h->app);
        if (s.handle >= 0 && s.flags)
            app_wait(h, 2); /* real readiness: the events, then quiet */
        harness_step_ack(h);
        nm_test_clock_advance_ms(25); /* the 250 ms budget: due by spin 10 */
        nm_chat_app_tick(h->app);     /* fires only when it is due NOW */
        tui_runtime_flush(h->rt);
    }
    ASSERT_EQ(nm_chat_app_state(h->app), NM_AGENT_DONE);

    const char *out = harness_read(h);
    ASSERT_NOT_NULL(out);
    /* The routing rode the head, so it is named; the figure never
     * arrived, so no savings — and no failure anywhere. */
    ASSERT_TRUE(strstr(out, "prism: routed to glm-5.3-flash "
                            "(GLM 5.3 Flash)") != NULL);
    ASSERT_NULL(strstr(out, "saved"));
    ASSERT_NULL(strstr(out, "timed out"));
    ASSERT_NULL(strstr(out, "chat failed"));

    harness_free(h);
    pthread_join(th, NULL);
    close(sc.fd);
}

/* The Agent Skills round trip on the real transcript: the menu is what
 * the model read, load_skill's body is what it got, and BOTH are
 * visible — the menu in the request the wire carried, the instructions
 * in the transcript the human watched. This is the transparency claim
 * pinned end to end: a skill's instructions arrive as a TOOL RESULT, so
 * they show like any other tool output, and the skill's own directory
 * rides the same result (the body's relative file references resolve
 * from there). The request side pins the other half of progressive
 * disclosure: the menu names the description and never carries a body. */
static void test_load_skill_round_shows_the_body(void)
{
    char fixture[512];
    snprintf(fixture, sizeof(fixture), "%s/skills-roundtrip",
             test_scratch_dir());
    char skill[700], md[800];
    snprintf(skill, sizeof(skill), "%s/reader", fixture);
    chat_mkdir(fixture);
    chat_mkdir(skill);
    snprintf(md, sizeof(md), "%s/SKILL.md", skill);
    FILE *f = fopen(md, "wb");
    ASSERT_NOT_NULL(f);
    if (f) {
        fputs("---\nname: reader\ndescription: Reads the fixture.\n---\n\n"
              "# Reader\n\nOpen the fixture file.\n",
              f);
        fclose(f);
    }
    nm_skills_set_global_dir(fixture);

    struct ServerScript sc;
    memset(&sc, 0, sizeof(sc));
    sc.n_rounds = 2;
    sc.sse[0] =
        "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,"
        "\"id\":\"call_s\",\"type\":\"function\",\"function\":"
        "{\"name\":\"load_skill\",\"arguments\":"
        "\"{\\\"name\\\":\\\"reader\\\"}\"}}]}}]}\n\n"
        "data: [DONE]\n\n";
    sc.sse[1] =
        "data: {\"choices\":[{\"delta\":{\"content\":\"all done\"}}]}\n\n"
        "data: [DONE]\n\n";
    sc.fd = server_bind(&sc.port);
    ASSERT_TRUE(sc.fd >= 0);
    pthread_t th;
    pthread_create(&th, NULL, chat_server_thread, &sc);

    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1", sc.port);
    AppHarness *h = harness_new("ollama:local", "test-model", base);
    ASSERT_NOT_NULL(h);

    harness_type(h, "use the reader skill");
    harness_enter(h);
    ASSERT_EQ(harness_drive(h, 500), 0);
    ASSERT_EQ(nm_chat_app_state(h->app), NM_AGENT_DONE);

    char *clean = strip_frames(harness_read(h));
    ASSERT_NOT_NULL(clean);
    /* The plan row, then the result in the one shape every tool uses. */
    ASSERT_TRUE(strstr(clean, "load_skill") != NULL);
    ASSERT_TRUE(strstr(clean, "Loaded skill 'reader'") != NULL);
    ASSERT_TRUE(strstr(clean, "Open the fixture file.") != NULL);
    ASSERT_TRUE(strstr(clean, "Skill directory:") != NULL);
    /* The frontmatter was stripped: the model reads instructions, and
     * the description it already saw in the menu is not repeated. */
    ASSERT_NULL(strstr(clean, "description: Reads the fixture."));
    ASSERT_TRUE(strstr(clean, "all done") != NULL);
    free(clean);

    /* Progressive disclosure, pinned where it lives: the MENU carries
     * metadata only — a body never rides the prompt — while the body
     * rides the CONVERSATION from the round that loaded it (round 2's
     * request is what the canned server captured, and it holds the tool
     * result). */
    const char *menu = strstr(g_request, "<skills>");
    ASSERT_NOT_NULL(menu);
    const char *menu_end = strstr(menu, "</skills>");
    ASSERT_NOT_NULL(menu_end);
    size_t mlen = (size_t)(menu_end - menu);
    char *block = malloc(mlen + 1);
    ASSERT_NOT_NULL(block);
    memcpy(block, menu, mlen);
    block[mlen] = '\0';
    ASSERT_NOT_NULL(strstr(block, "reader"));
    ASSERT_NULL(strstr(block, "Open the fixture file."));
    free(block);
    ASSERT_NOT_NULL(strstr(g_request, "Open the fixture file."));
    ASSERT_NOT_NULL(strstr(g_request, "\"load_skill\""));

    harness_free(h);
    pthread_join(th, NULL);
    close(sc.fd);
    remove(md);
    {
        char none[512];
        snprintf(none, sizeof(none), "%s/no-skills", test_scratch_dir());
        nm_skills_set_global_dir(none);
    }
}

int main(void)
{
#ifndef _WIN32
    signal(SIGPIPE, SIG_IGN); /* writes to closed sockets: EPIPE, not a signal */
#endif
    if (test_wsa_init() != 0) {
        fprintf(stderr, "  FAIL: WSAStartup\n");
        return 1;
    }
    /* No default-base catalog probes: the model tests assert the
     * static catalog, and a live fetch would block and answer with
     * whatever the service serves today. */
    if (test_pin_offline_catalog() != 0) {
        fprintf(stderr, "  FAIL: offline catalog pin\n");
        return 1;
    }
    /* No context files in the test's cwd: agent construction reads
     * AGENTS.md from the working directory. */
    if (test_chdir_to_scratch() != 0) {
        fprintf(stderr, "  FAIL: scratch cwd\n");
        return 1;
    }
    /* The Agent Skills half of the same pin: the tier must not be the
     * dev box's real ~/.agents/skills, or its menu rides every captured
     * request this file asserts on. */
    {
        char noskills[512];
        snprintf(noskills, sizeof(noskills), "%s/no-skills",
                 test_scratch_dir());
        nm_skills_set_global_dir(noskills);
    }
    /* Pin ~/.authinfo away from the real file: the app resolves each
     * provider's key (env then authinfo) when it builds an agent, so a
     * dev box's real keys would otherwise leak into the canned-server
     * tests. Env keys the tests set still win. */
    nm_authinfo_set_path("/nonexistent/nm-chat-app-authinfo");
    /* Provider names in config files are validated through the registry
     * hook main.c installs (config.c links no registry of its own). */
    nm_config_set_provider_validator(chat_valid_provider);
    /* No config paths leak in from the real home directory: every
     * config test pins both, and the others never set one. */
    nm_config_set_paths("/nonexistent/nm-chat-app-config",
                        "/nonexistent/nm-chat-app-shadow");
    printf("test_chat_app:\n");
    RUN_TEST(test_offline_catalog_is_pinned);
    RUN_TEST(test_submit_echoes_and_prints_answer);
    RUN_TEST(test_separator_blank_line_after_answer);
    RUN_TEST(test_separator_collapses_trailing_blank_lines);
    RUN_TEST(test_blank_line_inside_a_run_renders);
    RUN_TEST(test_context_reports_the_prism_routed_model);
    RUN_TEST(test_prism_notice_prints_under_a_routed_round);
    RUN_TEST(test_prism_notice_closes_its_own_block);
    RUN_TEST(test_prism_notice_off_silences_only_the_line);
    RUN_TEST(test_prism_notice_prints_under_a_late_trailer_round);
    RUN_TEST(test_prism_tail_budget_ends_the_round_without_a_figure);
    RUN_TEST(test_separator_between_reasoning_and_answer);
    RUN_TEST(test_delta_line_continuation_is_preserved);
    RUN_TEST(test_busy_frame_with_empty_tail_has_no_phantom_row);
    RUN_TEST(test_dot_pulses_on_a_truecolor_terminal);
    RUN_TEST(test_submit_paints_the_handoff_frame);
    RUN_TEST(test_dot_blinks_and_holds_its_column_on_16_colors);
    RUN_TEST(test_spinner_clears_when_round_completes);
    RUN_TEST(test_spinner_glyphs_are_one_cell);
    RUN_TEST(test_streaming_frame_shows_tail_and_spinner);
    RUN_TEST(test_output_marker_is_one_cell_under_every_model);
    RUN_TEST(test_context_gauge_unknown_reads_as_dash);
    RUN_TEST(test_multiline_input_continuation_aligns_under_prompt);
    RUN_TEST(test_kbd_tier_follows_the_terminal);
    RUN_TEST(test_kbd_full_tier_gives_shift_enter);
    RUN_TEST(test_kbd_wire_shift_press_types_nothing);
    RUN_TEST(test_kbd_wire_lock_modifiers_keep_the_modifiers);
    RUN_TEST(test_kbd_mode_overrides_the_probe);
    RUN_TEST(test_config_reports_the_kbd_tier);
    RUN_TEST(test_paste_lands_as_text_with_newlines);
    RUN_TEST(test_context_gauge_reports_usage_and_limit);
    RUN_TEST(test_context_gauge_cache_rate_is_cumulative);
    RUN_TEST(test_context_gauge_output_marker_waits_for_a_report);
    RUN_TEST(test_output_marker_color_follows_the_terminal_tier);
    RUN_TEST(test_context_gauge_survives_the_worst_case_counts);
    RUN_TEST(test_context_gauge_warns_near_the_limit);
    RUN_TEST(test_status_row_identity_follows_model);
    RUN_TEST(test_status_row_identity_follows_provider);
    RUN_TEST(test_status_row_identity_no_model);
    RUN_TEST(test_status_row_identity_elides_on_narrow_terminal);
    RUN_TEST(test_status_row_identity_dropped_when_short);
    RUN_TEST(test_status_row_cursor_lands_on_the_input_row);
    RUN_TEST(test_busy_input_gathers_type_ahead);
    RUN_TEST(test_quit_command_quits);
    RUN_TEST(test_model_command_sets_model);
    RUN_TEST(test_model_picker_active_first);
    RUN_TEST(test_model_picker_selects_and_commits);
    RUN_TEST(test_model_picker_pre_filters_by_query);
    RUN_TEST(test_model_picker_enter_composes_first_match);
    RUN_TEST(test_model_picker_no_match_prints_nothing);
    RUN_TEST(test_provider_command_switches_provider);
    RUN_TEST(test_provider_popup_composes_into_input);
    RUN_TEST(test_provider_popup_query_pre_filters);
    RUN_TEST(test_model_validation_refuses_unknown_id);
    RUN_TEST(test_model_exact_escape_hatch);
    RUN_TEST(test_help_command_lists_commands);
    RUN_TEST(test_config_runtime_change_writes_shadow);
    RUN_TEST(test_config_reasoning_echo_freezes_for_the_chat);
    RUN_TEST(test_config_signature_echo_row_and_freeze);
    RUN_TEST(test_config_shows_provider_default_signature);
    RUN_TEST(test_config_env_pin_is_reported);
    RUN_TEST(test_config_command_reports_and_resets);
    RUN_TEST(test_config_shows_provider_default_echo);
    RUN_TEST(test_config_set_and_runtime_layer);
    RUN_TEST(test_model_memory_is_per_provider);
    RUN_TEST(test_send_without_model_is_refused);
    RUN_TEST(test_missing_key_preflight_warns);
    RUN_TEST(test_model_popup_does_not_block_on_a_wire_catalog);
    RUN_TEST(test_model_exact_id_never_fetches_on_the_ui_thread);
    RUN_TEST(test_config_lists_scoped_model_keys);
    RUN_TEST(test_config_absent_is_no_persistence);
    RUN_TEST(test_connect_knobs_via_config_command);
    RUN_TEST(test_connect_knobs_from_config_reach_transport);
    RUN_TEST(test_family_skip_notice_prints_once);
    RUN_TEST(test_tab_on_slash_prefix_opens_commands_popup);
    RUN_TEST(test_tab_single_match_inserts_completion);
    RUN_TEST(test_tab_on_plain_word_is_a_noop);
    RUN_TEST(test_cancel_midstream_returns_to_idle);
    RUN_TEST(test_escape_cancels_the_streaming_turn);
    RUN_TEST(test_escape_at_the_prompt_keeps_the_text);
    RUN_TEST(test_ctrl_d_quits_after_a_turn);
    RUN_TEST(test_ctrl_d_deletes_at_a_nonempty_prompt_after_a_turn);
    RUN_TEST(test_tick_fires_stream_inactivity_timeout);
    RUN_TEST(test_connect_error_prints_and_returns_to_idle);
    RUN_TEST(test_connect_walk_notice_is_printed);
    RUN_TEST(test_black_hole_connect_is_bounded_by_the_tick);
    RUN_TEST(test_error_line_endings_are_crnl);
    RUN_TEST(test_reasoning_prints_before_answer);
    RUN_TEST(test_reminder_panel_renders_in_its_own_role);
    RUN_TEST(test_reminder_panel_styles_a_body_without_a_final_newline);
    RUN_TEST(test_reminder_panel_ignores_an_escaped_tag);
    RUN_TEST(test_reminder_user_channel_is_shown);
    RUN_TEST(test_reminder_tool_channel_is_not_echoed);
    RUN_TEST(test_reminder_forgery_warning_is_red);
    RUN_TEST(test_notice_sites_each_close_their_block);
    RUN_TEST(test_tool_round_prints_panels);
    RUN_TEST(test_reasoning_not_echoed_by_default);
    RUN_TEST(test_reasoning_echo_opt_in);
    RUN_TEST(test_parallel_tool_calls_pair_plan_with_result);
    RUN_TEST(test_open_json_fence_before_tool_call_commits);
#ifndef _WIN32
    RUN_TEST(test_cancel_during_tool_closes_the_block);
    RUN_TEST(test_escape_cancels_the_running_tool);
    RUN_TEST(test_tool_runs_async_and_spinner_ticks);
#endif
    RUN_TEST(test_streaming_multiline_no_duplicate_transcript);
    RUN_TEST(test_fence_line_not_split_by_reasoning_stream_end);
    RUN_TEST(test_submit_echoes_once);
    RUN_TEST(test_submit_does_not_finalize_status_line);
    RUN_TEST(test_provider_switch_clears_and_prints_separator);
    RUN_TEST(test_provider_switch_resolves_new_provider_key);
    RUN_TEST(test_fence_body_tokens_highlighted_through_app);
    RUN_TEST(test_reasoning_and_content_commit_in_order);
    RUN_TEST(test_markdown_table_reaches_scrollback_aligned);
    RUN_TEST(test_image_data_uri_degrades_to_marker);
    RUN_TEST(test_image_followed_by_a_blank_still_parses);
    RUN_TEST(test_table_blank_line_renders);
    RUN_TEST(test_table_then_fence_keeps_wire_order);
    RUN_TEST(test_image_command_attaches_lists_and_drops);
    RUN_TEST(test_image_command_refusals);
    RUN_TEST(test_image_subcommands_are_whole_words);
    RUN_TEST(test_img_submit_sends_parts_and_echoes);
    RUN_TEST(test_img_attach_shows_the_image_when_supported);
    RUN_TEST(test_recv_image_renders_through_image_block);
    RUN_TEST(test_recv_image_marker_on_dumb_terminal);
    RUN_TEST(test_image_save_writes_the_received_image);
    RUN_TEST(test_image_save_without_images);
    RUN_TEST(test_session_inspects_and_saves);
    RUN_TEST(test_session_without_a_transcript);
    RUN_TEST(test_model_picker_shows_capability_metadata);
    RUN_TEST(test_model_picker_capability_query_image);
    RUN_TEST(test_model_picker_capability_query_tools);
    RUN_TEST(test_model_picker_capability_query_tools_empty);
    RUN_TEST(test_model_picker_tool_badge_for_hyper_and_opencode);
    RUN_TEST(test_model_picker_capability_query_vision);
    RUN_TEST(test_model_picker_capability_query_unknown);
    RUN_TEST(test_model_picker_grammar_tag);
    RUN_TEST(test_model_picker_grammar_ctx);
    RUN_TEST(test_model_picker_grammar_ctx_range);
    RUN_TEST(test_model_picker_grammar_combines);
    RUN_TEST(test_model_picker_grammar_errors);
    RUN_TEST(test_model_picker_sees_past_the_old_row_cap);
    RUN_TEST(test_recv_image_after_text_opens_its_own_block);
    RUN_TEST(test_tool_read_file_image_renders_under_the_panel);
    RUN_TEST(test_tool_image_after_streamed_text_never_commits_payload);
    RUN_TEST(test_tool_read_file_image_degrades_to_the_panel_line);
    RUN_TEST(test_tool_read_file_image_text_only_model_warns);
    RUN_TEST(test_img_text_only_model_warns);
    RUN_TEST(test_img_pending_dropped_on_provider_switch);
    RUN_TEST(test_job_cap_fits_the_fd_budget);
    RUN_TEST(test_ps_without_jobs);
    RUN_TEST(test_skills_reports_none_found);
    RUN_TEST(test_skills_lists_found_skills);
    RUN_TEST(test_load_skill_round_shows_the_body);
#ifdef _WIN32
    RUN_TEST(test_windows_job_source_kind);
#else
    RUN_TEST(test_interest_lists_every_job);
    RUN_TEST(test_app_teardown_kills_jobs);
    RUN_TEST(test_external_ready_drains_background_job);
    RUN_TEST(test_interest_dedupes_active_exec_job);
    RUN_TEST(test_ps_lists_and_kill_removes);
    RUN_TEST(test_kill_rejects_bad_ids);
    RUN_TEST(test_ps_command_column_elides_safely);
    RUN_TEST(test_exec_command_spinner_tier);
#endif
    RUN_TEST(test_catalog_fetch_deadline_does_not_bound_the_connect);
    RUN_TEST(test_catalog_fetch_budget_is_idle_not_total);
    RUN_TEST(test_tool_runs_while_a_catalog_fetch_is_in_flight);
    /* LAST on purpose: this one commits a live hyper catalog, and the
     * provider catalogs are process-global — every test after it would
     * read the canned catalog instead of the static fallback it pins. */
    RUN_TEST(test_startup_warm_fills_the_gauge_without_a_popup);
    TEST_SUMMARY();
}
