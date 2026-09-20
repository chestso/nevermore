/* test_chat_app.c - the inline chat component against canned SSE
 * servers, driven through boba without a terminal.
 *
 * The component's contract under test:
 *   - submit -> nm_agent_start; step-driven turn prints the user
 *     echo and the answer into the runtime's output FILE* (captured
 *     via tmpfile), never through view()
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
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <boba/dynamic_buffer.h>
#include <boba/msg.h>
#include <boba/runtime.h>
#include <boba/stream.h>

#include "chat_app.h"
#include "agent.h"
#include "nm_config.h"
#include "authinfo.h"
#include "colors.h"
#include "nm_process.h"
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
    int delay_us;     /* between events of each round (0 = none) */
    int stall_at_end; /* keep the last round's connection open (no
                       * terminating chunk) until the peer closes */
};

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

        char head[128];
        int hl = snprintf(head, sizeof(head),
                          "HTTP/1.1 200 OK\r\n"
                          "Content-Type: text/event-stream\r\n"
                          "Transfer-Encoding: chunked\r\n\r\n");
        send(cfd, head, (size_t)hl, 0);
        size_t bl = strlen(body);
        size_t off = 0;
        while (off < bl) {
            const char *ev_end = strstr(body + off, "\n\n");
            size_t ev_len =
                ev_end ? (size_t)(ev_end - (body + off)) + 2 : bl - off;
            char chunk[REQ_CAP];
            int cl = snprintf(chunk, sizeof(chunk), "%zx\r\n", ev_len);
            memcpy(chunk + cl, body + off, ev_len);
            cl += (int)ev_len;
            cl += sprintf(chunk + cl, "\r\n");
            size_t cs = 0;
            while (cs < (size_t)cl) {
                long n = send(cfd, chunk + cs, (size_t)cl - cs, 0);
                if (n <= 0)
                    break;
                cs += (size_t)n;
            }
            if (sc->delay_us)
                usleep((unsigned)sc->delay_us);
            off += ev_len;
        }
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
        send(cfd, "0\r\n\r\n", 5, 0);
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

/* Create the app + runtime pair wired like main.c does it. Provider
 * defaults to "openai" (static catalog + openai_client wire). */
static AppHarness *harness_new(const char *provider, const char *model,
                               const char *base_url)
{
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
    return h;
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
 * real ~/.config or ~/.local/state. */
static void pin_cfg_paths(const char *sub)
{
    snprintf(g_cfg_root, sizeof(g_cfg_root), "%s/cfg-%s",
             test_scratch_dir(), sub);
    chat_mkdir(g_cfg_root);
    snprintf(g_cfg_user, sizeof(g_cfg_user), "%s/config", g_cfg_root);
    snprintf(g_cfg_shadow, sizeof(g_cfg_shadow), "%s/shadow", g_cfg_root);
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
 * it on the app, then apply the resolved rounds/echo values. */
static NmConfig *cfg_for(AppHarness *h)
{
    NmConfig *cfg = nm_config_load();
    nm_config_set_env(cfg);
    nm_chat_app_set_config(h->app, cfg);
    nm_chat_app_set_max_rounds(h->app,
                               nm_config_get_int(cfg, NM_CFG_KEY_ROUNDS, 0));
    nm_chat_app_set_echo_reasoning(
        h->app, nm_config_get_bool(cfg, NM_CFG_KEY_REASONING, 0));
    return cfg;
}

static void harness_enter(AppHarness *h)
{
    tui_runtime_send(h->rt, tui_msg_key(TUI_KEY_ENTER, 0, 0));
    tui_runtime_flush(h->rt);
}

/* The agent's own entry in the app's wait set. The set is an array now
 * (agent stream/exec fd first, then one READ entry per process
 * session), so a test that wants "the agent's interest" looks up its
 * fd rather than assuming slot 0 is a single connection. */
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
/* Wait up to `timeout_ms` for the app's agent source, the way boba's loop
 * does. A Windows process job's readiness object is a waitable event, so
 * select() cannot be used on it — it would fail at once and every caller
 * here would spin instead of waiting (run_command and the job tools both
 * ride that mechanism). */
static void app_wait(AppHarness *h, int timeout_ms)
{
    NmSource s = nm_chat_app_source(h->app);
    if (s.handle < 0 || !s.flags)
        return;
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
            nm_chat_app_step(h->app);
        } else if (s.handle < 0) {
            /* Tool phase (announce/execute): no source, step makes
             * progress immediately. */
            nm_chat_app_step(h->app);
        } else {
            usleep(10 * 1000);
        }
        tui_runtime_flush(h->rt);
    }
    return -1;
}

/* One step + frame repaint (for mid-stream assertions). Steps until
 * the round's text collector has content (bounded) — the server's
 * accept+send races the poll, and readability alone can fire before
 * any payload bytes have landed. */
/* One step, plus the flush the event loop would do. No payload wait:
 * for tests that intentionally STALL (no SSE payload at all), where
 * harness_step_once would burn its whole budget waiting for a tail
 * that never comes (5 s of the watchdog's 10 s per binary). */
/* One step, plus the flush the event loop would do. No payload wait:
 * for tests that intentionally STALL (no SSE payload at all), where
 * harness_step_once would burn its whole budget waiting for a tail
 * that never comes (5 s of the watchdog's 10 s per binary). */
static void harness_single_step(AppHarness *h)
{
    app_wait(h, 50);
    nm_chat_app_step(h->app);
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
        nm_chat_app_step(h->app);
        if (nm_chat_app_tail_len(h->app) > 0)
            break;
    }
    tui_runtime_flush(h->rt);
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
    AppHarness *h = harness_new("openai", "test-model", base);
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
     * server delays between events so the app sees them as separate
     * steps. */
    struct ServerScript sc;
    memset(&sc, 0, sizeof(sc));
    sc.n_rounds = 1;
    sc.sse[0] =
        "data: {\"choices\":[{\"delta\":{\"content\":\"Hello \"}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{\"content\":\"world\"}}]}\n\n"
        "data: [DONE]\n\n";
    sc.delay_us = 120 * 1000; /* force separate readable events */
    sc.fd = server_bind(&sc.port);
    ASSERT_TRUE(sc.fd >= 0);
    pthread_t th;
    pthread_create(&th, NULL, chat_server_thread, &sc);

    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1", sc.port);
    AppHarness *h = harness_new("openai", "test-model", base);
    ASSERT_NOT_NULL(h);

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
    AppHarness *h = harness_new("openai", "test-model", base);
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
    /* A braille spinner glyph is on the frame, in its own Yellow role
     * (the live "activity" pixel) - the muted Comment label follows. */
    ASSERT_TRUE(strstr(frame, NM_SGR_SPINNER "\xe2\xa0\x8b") != NULL);
    ASSERT_TRUE(strstr(frame, NM_SGR_SPINNER "\xe2\xa0\x8b" NM_SGR_TOOL
                                             " thinking…") != NULL);
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
    AppHarness *h = harness_new("openai", "test-model", base);
    ASSERT_NOT_NULL(h);

    harness_type(h, "go");
    harness_enter(h);
    harness_step_once(h);
    nm_chat_app_tick(h->app); /* advance the spinner (the run loop's tick) */
    tui_runtime_flush(h->rt);

    /* Mid-stream: state STREAMING, the tail is LIVE-REGION content
     * (frame), the input prompt is not rendered. */
    ASSERT_EQ(nm_chat_app_state(h->app), NM_AGENT_STREAMING);
    const char *frame = tui_runtime_render(h->rt);
    ASSERT_NOT_NULL(frame);
    ASSERT_TRUE(strstr(frame, "streaming tail") != NULL);
    ASSERT_TRUE(strstr(frame, "go") == NULL); /* input hidden */
    /* A braille spinner glyph is on the frame, painted in its own
     * (Yellow) role. */
    ASSERT_TRUE(strstr(frame, NM_SGR_SPINNER "\xe2\xa0\x8b") != NULL);

    /* The tail is live-region content: nothing has committed to the
     * scrollback mid-stream. This is a real state assertion (the
     * transcript's own commit count), not a byte-scan proxy. */
    ASSERT_EQ(tui_transcript_commit_count(nm_chat_app_transcript(h->app)),
              0u);
    const char *out = harness_read(h);
    ASSERT_TRUE(strstr(out, "\r\nstreaming tail without newline yet\r\n") == NULL);

    harness_free(h);
    /* The app was cancelled by teardown: the runtime free tears the
     * socket down, which releases the server's stall loop. Join so
     * the thread never outlives this frame (its `sc` is on it). */
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
    harness_type(h, "/model llama3.2");
    harness_enter(h);
    ASSERT_STR_EQ(nm_chat_app_model(h->app), "llama3.2");
    ASSERT_TRUE(strstr(harness_read(h), "llama3.2") != NULL);

    /* A second identical submit does not wedge. */
    harness_type(h, "/model qwen3-coder");
    harness_enter(h);
    ASSERT_STR_EQ(nm_chat_app_model(h->app), "qwen3-coder");

    harness_free(h);
}

/* Bare /model opens the picker; the ACTIVE model is the first entry
 * and is pre-selected, so Enter-then-submit leaves it unchanged. */
static void test_model_picker_active_first(void)
{
    AppHarness *h = harness_new("ollama:cloud", "llama3.2", NULL);
    ASSERT_NOT_NULL(h);

    harness_type(h, "/model");
    harness_enter(h);
    const char *frame = tui_runtime_render(h->rt);
    ASSERT_TRUE(strstr(frame, "llama3.2") != NULL);
    ASSERT_TRUE(strstr(frame, "gpt-oss:20b") != NULL);

    /* Enter COMPOSES the command, never applies it. */
    tui_runtime_send(h->rt, tui_msg_key(TUI_KEY_ENTER, 0, 0));
    const char *text = tui_textinput_text(nm_chat_app_textinput(h->app));
    ASSERT_NOT_NULL(text);
    ASSERT_STR_EQ(text, "/model llama3.2");
    /* The picker composes; the agent keeps the model until submit. */
    ASSERT_STR_EQ(nm_chat_app_model(h->app), "llama3.2");

    /* Submit re-applies the same model. */
    harness_enter(h);
    ASSERT_STR_EQ(nm_chat_app_model(h->app), "llama3.2");

    /* Escape during a later /model leaves the model alone. */
    harness_type(h, "/model");
    harness_enter(h);
    tui_runtime_send(h->rt, tui_msg_key(TUI_KEY_ESCAPE, 0, 0));
    ASSERT_STR_EQ(nm_chat_app_model(h->app), "llama3.2");
    ASSERT_EQ(nm_chat_app_state(h->app), NM_AGENT_IDLE);

    harness_free(h);
}

static void test_model_picker_selects_and_commits(void)
{
    AppHarness *h = harness_new("ollama:cloud", "llama3.2", NULL);
    ASSERT_NOT_NULL(h);

    /* Bare /model: active model first, catalog after. Down to the
     * next entry (gpt-oss:20b), Enter composes, submit commits. */
    harness_type(h, "/model");
    harness_enter(h);
    tui_runtime_send(h->rt, tui_msg_key(TUI_KEY_DOWN, 0, 0));
    tui_runtime_send(h->rt, tui_msg_key(TUI_KEY_ENTER, 0, 0));
    ASSERT_STR_EQ(tui_textinput_text(nm_chat_app_textinput(h->app)),
                  "/model gpt-oss:20b");
    ASSERT_STR_EQ(nm_chat_app_model(h->app), "llama3.2"); /* not yet */
    harness_enter(h);
    ASSERT_STR_EQ(nm_chat_app_model(h->app), "gpt-oss:20b");
    ASSERT_TRUE(strstr(harness_read(h), "gpt-oss:20b") != NULL);

    harness_free(h);
}

static void test_model_picker_pre_filters_by_query(void)
{
    AppHarness *h = harness_new("ollama:cloud", "gpt-oss:20b", NULL);
    ASSERT_NOT_NULL(h);

    /* /model with a query that is not an exact id: the popup opens
     * pre-filtered (boba's filter-as-view; the query is the stable
     * interface). "gpt" filters out llama3.2; the active model is
     * prepended and matches, so it stays first. */
    harness_type(h, "/model gpt-oss:120");
    harness_enter(h);
    const char *frame = tui_runtime_render(h->rt);
    ASSERT_TRUE(strstr(frame, "gpt-oss:120b") != NULL);
    ASSERT_TRUE(strstr(frame, "llama3.2") == NULL);        /* filtered out */
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

    harness_type(h, "/model llama");
    harness_enter(h);
    tui_runtime_send(h->rt, tui_msg_key(TUI_KEY_ENTER, 0, 0));
    ASSERT_STR_EQ(tui_textinput_text(nm_chat_app_textinput(h->app)),
                  "/model llama3.2");
    ASSERT_STR_EQ(nm_chat_app_model(h->app), "gpt-oss:20b");
    harness_enter(h);
    ASSERT_STR_EQ(nm_chat_app_model(h->app), "llama3.2");
    ASSERT_TRUE(strstr(harness_read(h), "llama3.2") != NULL);

    harness_free(h);
}

static void test_model_picker_no_match_prints_nothing(void)
{
    AppHarness *h = harness_new("ollama:cloud", "gpt-oss:20b", NULL);
    ASSERT_NOT_NULL(h);

    /* No match: no popup; a note prints instead; model unchanged. */
    harness_type(h, "/model zzz-no-such-model");
    harness_enter(h);
    ASSERT_TRUE(strstr(tui_runtime_render(h->rt), "gpt-oss") == NULL);
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
    ASSERT_TRUE(strstr(frame, "openai") != NULL);
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

static void test_providers_alias_is_unknown_command(void)
{
    AppHarness *h = harness_new("ollama:cloud", "gpt-oss:20b", NULL);
    ASSERT_NOT_NULL(h);

    /* /providers is retired: one command per noun. It is now an
     * unknown command, naming the survivor. */
    harness_type(h, "/providers");
    harness_enter(h);
    ASSERT_STR_EQ(nm_chat_app_provider(h->app), "ollama:cloud");
    ASSERT_TRUE(strstr(harness_read(h), "unknown command 'providers'") != NULL);

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
    harness_type(h, "/model qwen3-coder");
    harness_enter(h);
    ASSERT_STR_EQ(nm_chat_app_model(h->app), "qwen3-coder");

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

    harness_type(h, "/provider openai");
    harness_enter(h);

    ASSERT_STR_EQ(nm_chat_app_provider(h->app), "openai");
    ASSERT_EQ(nm_chat_app_state(h->app), NM_AGENT_IDLE);
    ASSERT_TRUE(strstr(harness_read(h), "openai") != NULL);

    /* Bare /provider opens the registry picker popup (the same
     * truth the router reads). */
    harness_type(h, "/provider");
    harness_enter(h);
    const char *frame = tui_runtime_render(h->rt);
    ASSERT_TRUE(strstr(frame, "ollama:cloud") != NULL);
    ASSERT_TRUE(strstr(frame, "openrouter") != NULL);
    ASSERT_TRUE(strstr(frame, "hyper") != NULL);
    ASSERT_STR_EQ(nm_chat_app_provider(h->app), "openai");
    tui_runtime_send(h->rt, tui_msg_key(TUI_KEY_ESCAPE, 0, 0));

    /* A query that matches nothing: no popup, a printed note, and
     * the provider stays (typo can't silently switch anything). */
    harness_type(h, "/provider nope");
    harness_enter(h);
    ASSERT_STR_EQ(nm_chat_app_provider(h->app), "openai");
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
    ASSERT_TRUE(strstr(out, "/rounds") != NULL);
    ASSERT_TRUE(strstr(out, "/ps") != NULL);
    ASSERT_TRUE(strstr(out, "/kill") != NULL);
    ASSERT_TRUE(strstr(out, "/quit") != NULL);
    /* The retired plurals are no longer advertised. */
    ASSERT_TRUE(strstr(out, "/models") == NULL);
    ASSERT_TRUE(strstr(out, "/providers") == NULL);

    harness_free(h);
}

/* /rounds shows and sets the tool-round cap on the live agent;
 * "reset" drops the shadow line and reveals the layer below. */
static void test_rounds_command_shows_and_sets_cap(void)
{
    AppHarness *h = harness_new("ollama:cloud", "gpt-oss:20b", NULL);
    ASSERT_NOT_NULL(h);

    /* Bare: the active cap and the built-in default. */
    harness_type(h, "/rounds");
    harness_enter(h);
    const char *out = harness_read(h);
    char want[64];
    snprintf(want, sizeof(want), "tool rounds: %d",
             NM_AGENT_DEFAULT_MAX_ROUNDS);
    ASSERT_TRUE(strstr(out, want) != NULL);

    /* Set a small cap; it lands on the live agent. */
    harness_type(h, "/rounds 3");
    harness_enter(h);
    ASSERT_TRUE(strstr(harness_read(h), "tool rounds: 3") != NULL);

    /* Garbage is refused; the cap is untouched. */
    harness_type(h, "/rounds nope");
    harness_enter(h);
    ASSERT_TRUE(strstr(harness_read(h), "expected a positive count") != NULL);
    ASSERT_EQ(nm_agent_max_rounds(nm_chat_app_agent(h->app)), 3);

    /* "reset" drops the shadow line and reveals the layer below (no
     * config here: the built-in default). */
    harness_type(h, "/rounds reset");
    harness_enter(h);
    snprintf(want, sizeof(want), "tool rounds: %d (shadow reset)",
             NM_AGENT_DEFAULT_MAX_ROUNDS);
    ASSERT_TRUE(strstr(harness_read(h), want) != NULL);
    ASSERT_EQ(nm_agent_max_rounds(nm_chat_app_agent(h->app)),
              NM_AGENT_DEFAULT_MAX_ROUNDS);

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
    AppHarness *h = harness_new("openai", "test-model", base);
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
    ASSERT_TRUE(strstr(out, NM_SGR_TOOL "🛑 interrupted") != NULL);

    /* Interrupt on empty IDLE input quits. */
    tui_runtime_send(h->rt, tui_msg_interrupt());
    ASSERT_TRUE(tui_runtime_should_quit(h->rt));

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
    AppHarness *h = harness_new("openai", "test-model", base);
    ASSERT_NOT_NULL(h);

    /* A short budget for a fast test (applied to the live agent too). */
    nm_chat_app_set_timeout_ms(h->app, 200);
    ASSERT_EQ(nm_agent_timeout_ms(nm_chat_app_agent(h->app)), 200);

    harness_type(h, "stall please");
    harness_enter(h);
    ASSERT_EQ(nm_chat_app_state(h->app), NM_AGENT_STREAMING);

    /* The tick interval is the spinner cadence, bounded by the
     * remaining deadline (never longer than 100 ms while busy). */
    int ms = nm_chat_app_tick_ms(h->app);
    ASSERT_TRUE(ms > 0 && ms <= 100);

    /* The fd is silent; only ticks advance the clock. Pump until the
     * tick drives the step that fires the timeout. Bounded. */
    for (int i = 0;
         i < 200 && nm_chat_app_state(h->app) == NM_AGENT_STREAMING; i++) {
        usleep(10 * 1000);
        nm_chat_app_tick(h->app);
    }
    ASSERT_EQ(nm_chat_app_state(h->app), NM_AGENT_ERROR);
    tui_runtime_flush(h->rt);

    const char *out = harness_read(h);
    ASSERT_TRUE(strstr(out, "stall please") != NULL);
    ASSERT_NOT_NULL(strstr(out, "timed out"));

    /* Idle again: nothing to tick for. */
    ASSERT_EQ(nm_chat_app_tick_ms(h->app), -1);

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
    AppHarness *h = harness_new("openai", "test-model", base);
    ASSERT_NOT_NULL(h);

    harness_type(h, "anyone there");
    harness_enter(h);

    /* The async transport seam: submit returns while the connect is
     * still in flight (STREAMING, connect pending); the failure
     * surfaces through steps, exactly as boba's loop would. */
    for (int i = 0; i < 200; i++) {
        NmAgentState st = nm_chat_app_state(h->app);
        if (st == NM_AGENT_DONE || st == NM_AGENT_ERROR || st == NM_AGENT_IDLE)
            break;
        nm_chat_app_step(h->app);
        usleep(5 * 1000);
    }
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
 * has a black-holed address prints a system line about it while the
 * connect is still in flight, instead of spinning silently. The
 * listener lives on the LAST address "localhost" resolves to, so the
 * walk's first attempt is guaranteed to be abandoned (refused) before
 * the live one is dialled. */
static void test_connect_walk_notice_is_printed(void)
{
    int port = 0;
    int lfd = test_bind_last_localhost_addr(&port);
    if (lfd < 0) {
        fprintf(stderr, "  note: 'localhost' has no second address to "
                        "walk to; notice line not exercised\n");
        return;
    }

    struct ServerScript sc;
    memset(&sc, 0, sizeof(sc));
    sc.n_rounds = 1;
    sc.sse[0] = "data: {\"choices\":[{\"delta\":{\"content\":\"hi\"}}]}\n\n"
                "data: [DONE]\n\n";
    sc.fd = lfd;
    sc.port = port;
    pthread_t th;
    pthread_create(&th, NULL, chat_server_thread, &sc);

    char base[64];
    snprintf(base, sizeof(base), "http://localhost:%d/v1", port);
    AppHarness *h = harness_new("openai", "test-model", base);
    ASSERT_NOT_NULL(h);

    harness_type(h, "hello");
    harness_enter(h);
    ASSERT_EQ(harness_drive(h, 500), 0);
    ASSERT_EQ(nm_chat_app_state(h->app), NM_AGENT_DONE);

    const char *out = harness_read(h);
    /* The notice names the abandoned attempt and the walk length. */
    ASSERT_TRUE(strstr(out, "did not answer") != NULL);
    ASSERT_TRUE(strstr(out, "1/") != NULL);
    /* And the turn still completed on the live address. */
    ASSERT_TRUE(strstr(out, "hi") != NULL);

    harness_free(h);
    pthread_join(th, NULL);
    close(sc.fd);
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
    AppHarness *h = harness_new("openai", "test-model", base);
    ASSERT_NOT_NULL(h);

    harness_type(h, "think");
    harness_enter(h);
    for (int i = 0; i < 200; i++) {
        NmAgentState st = nm_chat_app_state(h->app);
        if (st == NM_AGENT_DONE || st == NM_AGENT_ERROR || st == NM_AGENT_IDLE)
            break;
        nm_chat_app_step(h->app);
        usleep(5 * 1000);
    }
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
    for (int i = 0; i < 200; i++) {
        NmAgentState st = nm_chat_app_state(h->app);
        if (st == NM_AGENT_DONE || st == NM_AGENT_ERROR ||
            st == NM_AGENT_IDLE)
            break;
        nm_chat_app_step(h->app);
        usleep(5 * 1000);
    }
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
     * come from the one sys_line), with no second error line between. */
    const char *msg = strstr(out, "chat failed:");
    ASSERT_NOT_NULL(msg);
    const char *hint = strstr(out, "export HYPER_API_KEY");
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
    AppHarness *h = harness_new("openai", "test-model", base);
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
    AppHarness *h = harness_new("openai", "test-model", base);
    ASSERT_NOT_NULL(h);

    harness_type(h, "code please");
    harness_enter(h);
    ASSERT_EQ(harness_drive(h, 500), 0);
    ASSERT_EQ(nm_chat_app_state(h->app), NM_AGENT_DONE);

    const char *out = harness_read(h);
    /* The fence body committed, followed by exactly ONE blank row: the
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
    AppHarness *h = harness_new("openai", "test-model", base);
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

/* Strip CSI/OSC escape sequences and carriage returns, leaving the
 * logical text rows a user would see (used to assert commit ORDER
 * without live-region framing bytes in the way). Heap-owned. */
static char *strip_frames(const char *in)
{
    size_t n = strlen(in), o = 0;
    char *out = malloc(n + 1);
    if (!out)
        return NULL;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)in[i];
        if (c == 0x1b) {
            if (in[i + 1] == '[') {
                i += 2;
                while (in[i] && !(in[i] >= '@' && in[i] <= '~'))
                    i++;
            } else if (in[i + 1] == ']') {
                i += 2;
                while (in[i] && in[i] != 0x07)
                    i++;
            } else if (in[i + 1]) {
                i++;
            }
            continue;
        }
        if (c == '\r')
            continue;
        out[o++] = (char)c;
    }
    out[o] = '\0';
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
    AppHarness *h = harness_new("openai", "test-model", base);
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

    /* The spinner tier paints "executing" while the child runs: the
     * charset-tier glyph in the Yellow activity role, the label muted
     * Comment. */
    nm_chat_app_tick(h->app);
    const char *frame = tui_runtime_render(h->rt);
    ASSERT_NOT_NULL(frame);
    ASSERT_TRUE(strstr(frame, "executing") != NULL);
    ASSERT_TRUE(strstr(frame, NM_SGR_SPINNER "\xc2\xb7" NM_SGR_TOOL
                                             " executing run_command…") != NULL);

    /* And the turn completes, with the command output committed. */
    ASSERT_EQ(harness_drive(h, 2000), 0);
    ASSERT_EQ(nm_chat_app_state(h->app), NM_AGENT_DONE);
    char *clean = strip_frames(harness_read(h));
    ASSERT_NOT_NULL(clean);
    ASSERT_TRUE(strstr(clean, "hi-cmd") != NULL);
    /* Every row's text starts in the same column: the label row opens
     * with `  ╰─ ` (5 display columns), later rows indent by exactly
     * that much. */
    ASSERT_TRUE(strstr(clean, "  ╰─ Output:") != NULL);
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
    AppHarness *h = harness_new("openai", "test-model", base);
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
    AppHarness *h = harness_new("openai", "test-model", base);
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
    ASSERT_TRUE(strstr(out, NM_SGR_TOOL "✏️ edit_file") != NULL);
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
    ASSERT_TRUE(strstr(out, NM_SGR_TOOL_ELBOW "  ╰─ " NM_SGR_RESULT) != NULL);
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
        "\"reasoning_content\":\"let me think about the edit\"}}]}\n\n"
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
    return harness_new("openai", "test-model", base);
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
 * the request carrying the turn (the shape hyper requires). */
static void test_reasoning_echo_opt_in(void)
{
    struct ServerScript sc;
    pthread_t th;
    AppHarness *h = run_reasoning_tool_turn(&sc, &th);
    ASSERT_NOT_NULL(h);

    nm_chat_app_set_echo_reasoning(h->app, 1);

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
    AppHarness *h = harness_new("openai", "test-model", base);
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
    sc.delay_us = 60 * 1000; /* separate readable events per delta */
    sc.fd = server_bind(&sc.port);
    ASSERT_TRUE(sc.fd >= 0);
    pthread_t th;
    pthread_create(&th, NULL, chat_server_thread, &sc);

    char base2[64];
    snprintf(base2, sizeof(base2), "http://127.0.0.1:%d/v1", sc.port);
    AppHarness *h = harness_new("openai", "test-model", base2);
    ASSERT_NOT_NULL(h);

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
    sc.delay_us = 120 * 1000; /* separate readable events */
    sc.fd = server_bind(&sc.port);
    ASSERT_TRUE(sc.fd >= 0);
    pthread_t th;
    pthread_create(&th, NULL, chat_server_thread, &sc);

    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1", sc.port);
    AppHarness *h = harness_new("openai", "test-model", base);
    ASSERT_NOT_NULL(h);

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
        nm_chat_app_step(h->app); /* prints internally, NO flush here */
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
    AppHarness *h = harness_new("openai", "test-model", NULL);
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
    AppHarness *h = harness_new("openai", "test-model", base);
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

/* A provider switch resets the transcript (emits nothing) and prints a
 * separator line marking the boundary. */
static void test_provider_switch_clears_and_prints_separator(void)
{
    AppHarness *h = harness_new("ollama:cloud", "gpt-oss:20b", NULL);
    ASSERT_NOT_NULL(h);

    harness_type(h, "/provider openai");
    harness_enter(h);
    ASSERT_STR_EQ(nm_chat_app_provider(h->app), "openai");
    const char *out = harness_read(h);
    ASSERT_TRUE(strstr(out, "provider: openai (fresh session)") != NULL);

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
    test_setenv("OPENAI_API_KEY", "sk-old-openai");
    AppHarness *h = harness_new("openai", "test-model", base);
    ASSERT_NOT_NULL(h);

    /* Switch to a different keyed provider carrying a different key. */
    test_setenv("OPENROUTER_API_KEY", "sk-new-openrouter");
    g_request[0] = '\0';
    harness_type(h, "/provider openrouter");
    harness_enter(h);
    ASSERT_STR_EQ(nm_chat_app_provider(h->app), "openrouter");

    /* The next turn must authorize with the NEW provider's key. */
    harness_type(h, "hi");
    harness_enter(h);
    ASSERT_EQ(harness_drive(h, 500), 0);
    ASSERT_TRUE(strstr(g_request,
                       "Authorization: Bearer sk-new-openrouter") != NULL);
    ASSERT_TRUE(strstr(g_request, "sk-old-openai") == NULL);

    harness_free(h);
    pthread_join(th, NULL);
    close(sc.fd);
    test_unsetenv("OPENAI_API_KEY");
    test_unsetenv("OPENROUTER_API_KEY");
}

/* The app installs its per-stream highlighter state as the
 * transcript's user_data, so a labeled fence body streamed through the
 * app's own delta path carries token colors (keyword Pink, number
 * Orange). */
static void test_fence_body_tokens_highlighted_through_app(void)
{
    AppHarness *h = harness_new("openai", "test-model", NULL);
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
    AppHarness *h = harness_new("openai", "test-model", base);
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
    AppHarness *h = harness_new("openai", "test-model", base);
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

    AppHarness *h = harness_new("openai", "from-user", NULL);
    ASSERT_NOT_NULL(h);
    NmConfig *cfg = cfg_for(h);

    harness_type(h, "/rounds 7");
    harness_enter(h);
    ASSERT_TRUE(strstr(harness_read(h), "tool rounds: 7") != NULL);
    ASSERT_TRUE(strstr(harness_read(h), "saved to the session shadow") !=
                NULL);

    harness_type(h, "/reasoning on");
    harness_enter(h);
    ASSERT_TRUE(strstr(harness_read(h), "reasoning echo: on") != NULL);

    /* The shadow holds exactly what the user typed. */
    ASSERT_STR_EQ(cfg_read_shadow(), "rounds = 7\nreasoning = on\n");
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

    AppHarness *h = harness_new("openai", "m", NULL);
    ASSERT_NOT_NULL(h);
    NmConfig *cfg = cfg_for(h);
    ASSERT_EQ(nm_agent_max_rounds(nm_chat_app_agent(h->app)), 9);

    harness_type(h, "/rounds 3");
    harness_enter(h);
    const char *out = harness_read(h);
    ASSERT_TRUE(strstr(out, "NEVERMORE_MAX_ROUNDS pins this run") != NULL);
    /* Persisted anyway: the shadow is what the user typed. */
    ASSERT_STR_EQ(cfg_read_shadow(), "rounds = 3\n");
    /* The live agent still honors the environment. */
    ASSERT_EQ(nm_agent_max_rounds(nm_chat_app_agent(h->app)), 3);

    nm_config_free(cfg);
    harness_free(h);
    test_unsetenv("NEVERMORE_MAX_ROUNDS");
}

/* /config reports provenance; /config reset drops shadow lines. */
static void test_config_command_reports_and_resets(void)
{
    pin_cfg_paths("report");
    write_file_at(g_cfg_user, "model = from-user\n");

    AppHarness *h = harness_new("openai", "from-user", NULL);
    ASSERT_NOT_NULL(h);
    NmConfig *cfg = cfg_for(h);

    /* "! id" is the exact-set path (the id need not be in the catalog). */
    harness_type(h, "/model ! from-user");
    harness_enter(h);
    ASSERT_STR_EQ(cfg_read_shadow(), "model = from-user\n");

    harness_type(h, "/config");
    harness_enter(h);
    const char *out = harness_read(h);
    ASSERT_TRUE(strstr(out, g_cfg_user) != NULL);
    ASSERT_TRUE(strstr(out, g_cfg_shadow) != NULL);
    /* model resolves to the shadow (just set); rounds has no layer. */
    ASSERT_TRUE(strstr(out, "(session shadow)") != NULL);
    ASSERT_TRUE(strstr(out, "(built-in default)") != NULL);

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

/* With no config handle the commands still work and nothing is
 * written anywhere (the hermetic default the other tests rely on). */
static void test_config_absent_is_no_persistence(void)
{
    pin_cfg_paths("absent");
    AppHarness *h = harness_new("openai", "m", NULL);
    ASSERT_NOT_NULL(h);

    harness_type(h, "/rounds 5");
    harness_enter(h);
    const char *out = harness_read(h);
    ASSERT_TRUE(strstr(out, "tool rounds: 5") != NULL);
    ASSERT_FALSE(strstr(out, "shadow") != NULL);
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
/* P3: the multi-fd wait set + job teardown                      */
/*                                                                    */
/* Jobs cannot be started on Windows yet (nm_process_win.c stub,   */
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
    AppHarness *h = harness_new("openai", "test-model", NULL);
    ASSERT_NOT_NULL(h);
    nm_proc_reset(); /* a clean cap + registry, whatever ran before */

    nm_proc_set_max_jobs(4);
    char err[128];
    for (int i = 0; i < 4; i++) {
        int id = -1;
        ASSERT_NOT_NULL(
            nm_proc_start("sleep 30", NULL, &id, err, sizeof(err)));
    }
    /* One past the cap fails loudly instead of spawning a child nobody
     * will ever drain. */
    int id = -1;
    ASSERT_NULL(nm_proc_start("sleep 30", NULL, &id, err, sizeof(err)));
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
    AppHarness *h = harness_new("openai", "test-model", NULL);
    ASSERT_NOT_NULL(h);

    NmSource set[TUI_IO_SOURCE_MAX];
    ASSERT_EQ(nm_chat_app_interest(h->app, set, TUI_IO_SOURCE_MAX), 0);

    char err[128];
    int id = -1;
    NmProc *p = nm_proc_start("ping -n 31 127.0.0.1 >nul", NULL, &id, err,
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
    AppHarness *h = harness_new("openai", "test-model", NULL);
    ASSERT_NOT_NULL(h);

    NmSource set[TUI_IO_SOURCE_MAX];
    /* Idle app, no jobs: nothing to wait on. */
    ASSERT_EQ(nm_chat_app_interest(h->app, set, TUI_IO_SOURCE_MAX), 0);
    ASSERT_EQ(nm_chat_app_interest(h->app, set, 0), 0);

    char err[128];
    int id1 = -1, id2 = -1;
    NmProc *p1 = nm_proc_start("sleep 30", NULL, &id1, err, sizeof(err));
    ASSERT_NOT_NULL(p1);
    NmProc *p2 = nm_proc_start("sleep 30", NULL, &id2, err, sizeof(err));
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
    AppHarness *h = harness_new("openai", "test-model", NULL);
    ASSERT_NOT_NULL(h);

    char err[128];
    int id = -1;
    ASSERT_NOT_NULL(nm_proc_start("sleep 30", NULL, &id, err, sizeof(err)));
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
    AppHarness *h = harness_new("openai", "test-model", NULL);
    ASSERT_NOT_NULL(h);

    char err[128];
    int id = -1;
    NmProc *p = nm_proc_start("printf 'background output\\n'; sleep 30",
                              NULL, &id, err, sizeof(err));
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
    AppHarness *h = harness_new("openai", "test-model", base);
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

    /* The yield window closes, the call ends, the round finishes — and
     * the job survives on its own in the wait set. */
    ASSERT_EQ(harness_drive(h, 2000), 0);
    ASSERT_EQ(nm_chat_app_state(h->app), NM_AGENT_DONE);
    ASSERT_EQ(app_fd(h->app), -1);
    ASSERT_EQ(nm_proc_count(), 1);
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
    AppHarness *h = harness_new("openai", "test-model", NULL);
    ASSERT_NOT_NULL(h);

    harness_type(h, "/ps");
    harness_enter(h);
    char *clean = strip_frames(harness_read(h));
    ASSERT_NOT_NULL(clean);
    ASSERT_TRUE(strstr(clean, "no process jobs") != NULL);
    free(clean);

    harness_free(h);
}

#ifndef _WIN32
/* /ps lists every registered job with its id, state, command and
 * how much output is waiting; /kill <id> then removes exactly one. */
static void test_ps_lists_and_kill_removes(void)
{
    AppHarness *h = harness_new("openai", "test-model", NULL);
    ASSERT_NOT_NULL(h);
    nm_proc_reset();

    char err[128];
    int id_run = -1, id_done = -1;
    NmProc *pr = nm_proc_start("echo hello; sleep 30", NULL, &id_run, err,
                               sizeof(err));
    ASSERT_NOT_NULL(pr);
    /* This one exits on its own: /ps must show BOTH states. */
    ASSERT_NOT_NULL(
        nm_proc_start("exit 3", NULL, &id_done, err, sizeof(err)));
    for (int i = 0; i < 300 && nm_proc_count() < 2; i++)
        usleep(5 * 1000);
    /* Let the second one actually leave. */
    int reaped = 0;
    for (int i = 0; i < 300 && !reaped; i++) {
        NmProc *d = nm_proc_find(id_done);
        reaped = d && nm_proc_exit(d) >= 0;
        if (!reaped)
            usleep(5 * 1000);
    }
    ASSERT_TRUE(reaped);

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
    AppHarness *h = harness_new("openai", "test-model", NULL);
    ASSERT_NOT_NULL(h);
    nm_proc_reset();

    char err[128];
    int id = -1;
    ASSERT_NOT_NULL(nm_proc_start("sleep 30", NULL, &id, err, sizeof(err)));

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
    AppHarness *h = harness_new("openai", "test-model", NULL);
    ASSERT_NOT_NULL(h);
    nm_proc_reset();

    /* Runs of spaces/tabs and a newline collapse to single spaces. */
    char err[128];
    int id_a = -1, id_b = -1;
    ASSERT_NOT_NULL(nm_proc_start("sleep\t\t 30; echo   a\nb", NULL, &id_a,
                                  err, sizeof(err)));
    ASSERT_NOT_NULL(
        nm_proc_start("sleep 30", NULL, &id_b, err, sizeof(err)));

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
    ASSERT_NOT_NULL(nm_proc_start(long_cmd, NULL, &id_c, err, sizeof(err)));
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
    ASSERT_NOT_NULL(nm_proc_start(wide, NULL, &id_d, err, sizeof(err)));
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
 * status row reads "executing exec_command…" in the activity role — the
 * same tier run_command gets (P4's spinner item). */
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
    AppHarness *h = harness_new("openai", "test-model", base);
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
     * animates, tier "executing exec_command". */
    nm_chat_app_tick(h->app);
    const char *frame = tui_runtime_render(h->rt);
    ASSERT_NOT_NULL(frame);
    ASSERT_TRUE(strstr(frame, NM_SGR_SPINNER) != NULL);
    ASSERT_TRUE(strstr(frame, "executing exec_command") != NULL);

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
    RUN_TEST(test_separator_between_reasoning_and_answer);
    RUN_TEST(test_delta_line_continuation_is_preserved);
    RUN_TEST(test_busy_frame_with_empty_tail_has_no_phantom_row);
    RUN_TEST(test_streaming_frame_shows_tail_and_spinner);
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
    RUN_TEST(test_providers_alias_is_unknown_command);
    RUN_TEST(test_model_validation_refuses_unknown_id);
    RUN_TEST(test_model_exact_escape_hatch);
    RUN_TEST(test_help_command_lists_commands);
    RUN_TEST(test_rounds_command_shows_and_sets_cap);
    RUN_TEST(test_config_runtime_change_writes_shadow);
    RUN_TEST(test_config_env_pin_is_reported);
    RUN_TEST(test_config_command_reports_and_resets);
    RUN_TEST(test_config_absent_is_no_persistence);
    RUN_TEST(test_tab_on_slash_prefix_opens_commands_popup);
    RUN_TEST(test_tab_single_match_inserts_completion);
    RUN_TEST(test_tab_on_plain_word_is_a_noop);
    RUN_TEST(test_cancel_midstream_returns_to_idle);
    RUN_TEST(test_tick_fires_stream_inactivity_timeout);
    RUN_TEST(test_connect_error_prints_and_returns_to_idle);
    RUN_TEST(test_connect_walk_notice_is_printed);
    RUN_TEST(test_error_line_endings_are_crnl);
    RUN_TEST(test_reasoning_prints_before_answer);
    RUN_TEST(test_tool_round_prints_panels);
    RUN_TEST(test_reasoning_not_echoed_by_default);
    RUN_TEST(test_reasoning_echo_opt_in);
    RUN_TEST(test_parallel_tool_calls_pair_plan_with_result);
    RUN_TEST(test_open_json_fence_before_tool_call_commits);
#ifndef _WIN32
    RUN_TEST(test_cancel_during_tool_closes_the_block);
    RUN_TEST(test_tool_runs_async_and_spinner_ticks);
#endif
    RUN_TEST(test_streaming_multiline_no_duplicate_transcript);
    RUN_TEST(test_fence_line_not_split_by_reasoning_stream_end);
    RUN_TEST(test_submit_echoes_once);
    RUN_TEST(test_provider_switch_clears_and_prints_separator);
    RUN_TEST(test_provider_switch_resolves_new_provider_key);
    RUN_TEST(test_fence_body_tokens_highlighted_through_app);
    RUN_TEST(test_reasoning_and_content_commit_in_order);
    RUN_TEST(test_markdown_table_reaches_scrollback_aligned);
    RUN_TEST(test_job_cap_fits_the_fd_budget);
    RUN_TEST(test_ps_without_jobs);
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
    TEST_SUMMARY();
}
