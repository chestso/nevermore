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

#include "chat_app.h"
#include "test_helpers.h"
#include "test_net_helpers.h"

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

static void harness_enter(AppHarness *h)
{
    tui_runtime_send(h->rt, tui_msg_key(TUI_KEY_ENTER, 0, 0));
    tui_runtime_flush(h->rt);
}

/* Drive the agent to completion the way the runtime's external-fd
 * loop would: poll fd -> step. Bounded. */
static int harness_drive(AppHarness *h, int max_spins)
{
    for (int i = 0; i < max_spins; i++) {
        NmAgentState st = nm_chat_app_state(h->app);
        if (st == NM_AGENT_DONE || st == NM_AGENT_ERROR ||
            st == NM_AGENT_IDLE)
            return 0;
        int fd = nm_chat_app_fd(h->app);
        if (fd >= 0) {
            fd_set fds;
            struct timeval tv = { 0, 10 * 1000 };
            FD_ZERO(&fds);
            FD_SET(fd, &fds);
            select(fd + 1, &fds, NULL, NULL, &tv);
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
static void harness_step_once(AppHarness *h)
{
    for (int i = 0; i < 100; i++) {
        int fd = nm_chat_app_fd(h->app);
        if (fd < 0)
            break;
        fd_set fds;
        struct timeval tv = { 0, 50 * 1000 };
        FD_ZERO(&fds);
        FD_SET(fd, &fds);
        select(fd + 1, &fds, NULL, NULL, &tv);
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
    /* A braille spinner glyph is on the frame. */
    ASSERT_TRUE(strstr(frame, "\xe2\xa0\x8b") != NULL); /* "⠋" */

    /* The tail is not in the SCROLLBACK (it is live-region content):
     * every whole-line print to the output file is framed by
     * clear_inline's erase sequence, never a bare tail line. */
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
    AppHarness *h = harness_new("ollama", "gpt-oss:20b", NULL);
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
    AppHarness *h = harness_new("ollama", "gpt-oss:20b", NULL);
    ASSERT_NOT_NULL(h);

    ASSERT_STR_EQ(nm_chat_app_model(h->app), "gpt-oss:20b");

    /* No-arg form prints the current model. */
    harness_type(h, "/model");
    harness_enter(h);
    ASSERT_TRUE(strstr(harness_read(h), "gpt-oss:20b") != NULL);
    ASSERT_EQ(nm_chat_app_state(h->app), NM_AGENT_IDLE);

    /* Arg form sets it. */
    harness_type(h, "/model llama3.2");
    harness_enter(h);
    ASSERT_STR_EQ(nm_chat_app_model(h->app), "llama3.2");
    ASSERT_TRUE(strstr(harness_read(h), "llama3.2") != NULL);

    harness_free(h);
}

static void test_models_popup_selects_model(void)
{
    AppHarness *h = harness_new("ollama", "gpt-oss:20b", NULL);
    ASSERT_NOT_NULL(h);

    harness_type(h, "/models");
    harness_enter(h);

    /* The catalog popped up as a frame popup (view-side). */
    const char *frame = tui_runtime_render(h->rt);
    ASSERT_TRUE(strstr(frame, "gpt-oss:20b") != NULL);
    ASSERT_TRUE(strstr(frame, "llama3.2") != NULL);

    /* Down + Enter selects the second entry. */
    tui_runtime_send(h->rt, tui_msg_key(TUI_KEY_DOWN, 0, 0));
    tui_runtime_send(h->rt, tui_msg_key(TUI_KEY_ENTER, 0, 0));
    ASSERT_STR_EQ(nm_chat_app_model(h->app), "gpt-oss:120b");

    /* Escape during a later /models dismisses without changing. */
    harness_type(h, "/models");
    harness_enter(h);
    tui_runtime_send(h->rt, tui_msg_key(TUI_KEY_ESCAPE, 0, 0));
    ASSERT_STR_EQ(nm_chat_app_model(h->app), "gpt-oss:120b");
    ASSERT_EQ(nm_chat_app_state(h->app), NM_AGENT_IDLE);

    harness_free(h);
}

static void test_provider_command_switches_provider(void)
{
    AppHarness *h = harness_new("ollama", "gpt-oss:20b", NULL);
    ASSERT_NOT_NULL(h);

    ASSERT_STR_EQ(nm_chat_app_provider(h->app), "ollama");

    harness_type(h, "/provider openai");
    harness_enter(h);

    ASSERT_STR_EQ(nm_chat_app_provider(h->app), "openai");
    ASSERT_EQ(nm_chat_app_state(h->app), NM_AGENT_IDLE);
    ASSERT_TRUE(strstr(harness_read(h), "openai") != NULL);

    /* Bare /provider lists every registered provider (the names
     * /provider accepts), current first with a marker. */
    harness_type(h, "/provider");
    harness_enter(h);
    const char *out = harness_read(h);
    ASSERT_TRUE(strstr(out, "providers:") != NULL);
    ASSERT_TRUE(strstr(out, "openai") != NULL);
    ASSERT_TRUE(strstr(out, "openrouter") != NULL);
    ASSERT_TRUE(strstr(out, "hyper") != NULL);
    ASSERT_TRUE(strstr(out, "ollama") != NULL);
    ASSERT_STR_EQ(nm_chat_app_provider(h->app), "openai");

    /* Unknown provider is refused, current stays; the error lists
     * the valid names. */
    harness_type(h, "/provider nope");
    harness_enter(h);
    ASSERT_STR_EQ(nm_chat_app_provider(h->app), "openai");
    out = harness_read(h);
    ASSERT_TRUE(strstr(out, "unknown provider 'nope'") != NULL);
    ASSERT_TRUE(strstr(out, "openrouter") != NULL);

    harness_free(h);
}

static void test_help_command_lists_commands(void)
{
    AppHarness *h = harness_new("ollama", "gpt-oss:20b", NULL);
    ASSERT_NOT_NULL(h);

    harness_type(h, "/help");
    harness_enter(h);

    const char *out = harness_read(h);
    ASSERT_TRUE(strstr(out, "/model") != NULL);
    ASSERT_TRUE(strstr(out, "/provider") != NULL);
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
    AppHarness *h = harness_new("openai", "test-model", base);
    ASSERT_NOT_NULL(h);

    harness_type(h, "stall please");
    harness_enter(h);
    ASSERT_EQ(nm_chat_app_state(h->app), NM_AGENT_STREAMING);
    ASSERT_TRUE(nm_chat_app_fd(h->app) >= 0);

    /* Ctrl+C arrives as an interrupt message. */
    tui_runtime_send(h->rt, tui_msg_interrupt());
    ASSERT_EQ(nm_chat_app_state(h->app), NM_AGENT_IDLE);
    ASSERT_EQ(nm_chat_app_fd(h->app), -1);
    ASSERT_TRUE(strstr(harness_read(h), "stall please") != NULL);

    /* Interrupt on empty IDLE input quits. */
    tui_runtime_send(h->rt, tui_msg_interrupt());
    ASSERT_TRUE(tui_runtime_should_quit(h->rt));

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

    ASSERT_EQ(nm_chat_app_state(h->app), NM_AGENT_ERROR);
    const char *out = harness_read(h);
    ASSERT_TRUE(strstr(out, "anyone there") != NULL);
    /* An error line reached the scrollback. */
    ASSERT_TRUE(strstr(out, "nevermore") != NULL);

    /* A fresh turn still works (ERROR does not wedge the app). */
    ASSERT_EQ(nm_chat_app_fd(h->app), -1);

    harness_free(h);
}

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

    /* The tool start panel and the result line are in the scrollback.
     * The panel shows the tool name + its path summary (the args'
     * identifying value), not the edit text. */
    const char *out = harness_read(h);
    ASSERT_TRUE(strstr(out, "edit_file") != NULL);
    ASSERT_TRUE(strstr(out, "nm-test-chat.txt") != NULL);
    ASSERT_TRUE(strstr(out, "edited the file") != NULL);

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

static void test_tab_on_slash_prefix_opens_commands_popup(void)
{
    AppHarness *h = harness_new("ollama", "gpt-oss:20b", NULL);
    ASSERT_NOT_NULL(h);

    /* Regression (TUI crash): Tab after "/m" matches several slash
     * commands (/model, /models), so the commands popup opens with
     * the filter applied. The old loop strncmp'd the array's NULL
     * sentinel here. */
    harness_type(h, "/m");
    tui_runtime_send(h->rt, tui_msg_key(TUI_KEY_TAB, 0, 0));
    tui_runtime_flush(h->rt);

    const char *frame = tui_runtime_render(h->rt);
    ASSERT_TRUE(strstr(frame, "/model") != NULL);
    ASSERT_TRUE(strstr(frame, "/models") != NULL);
    /* The input text is unchanged (several matches: popup, no insert). */
    ASSERT_STR_EQ(tui_textinput_text(nm_chat_app_textinput(h->app)), "/m");

    /* Escape dismisses the popup. */
    tui_runtime_send(h->rt, tui_msg_key(TUI_KEY_ESCAPE, 0, 0));
    frame = tui_runtime_render(h->rt);
    ASSERT_TRUE(strstr(frame, "/model") == NULL);

    harness_free(h);
}

static void test_tab_single_match_inserts_completion(void)
{
    AppHarness *h = harness_new("ollama", "gpt-oss:20b", NULL);
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
    AppHarness *h = harness_new("ollama", "gpt-oss:20b", NULL);
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

int main(void)
{
#ifndef _WIN32
    signal(SIGPIPE, SIG_IGN); /* writes to closed sockets: EPIPE, not a signal */
#endif
    if (test_wsa_init() != 0) {
        fprintf(stderr, "  FAIL: WSAStartup\n");
        return 1;
    }
    printf("test_chat_app:\n");
    RUN_TEST(test_submit_echoes_and_prints_answer);
    RUN_TEST(test_delta_line_continuation_is_preserved);
    RUN_TEST(test_streaming_frame_shows_tail_and_spinner);
    RUN_TEST(test_quit_command_quits);
    RUN_TEST(test_model_command_sets_model);
    RUN_TEST(test_models_popup_selects_model);
    RUN_TEST(test_provider_command_switches_provider);
    RUN_TEST(test_help_command_lists_commands);
    RUN_TEST(test_tab_on_slash_prefix_opens_commands_popup);
    RUN_TEST(test_tab_single_match_inserts_completion);
    RUN_TEST(test_tab_on_plain_word_is_a_noop);
    RUN_TEST(test_cancel_midstream_returns_to_idle);
    RUN_TEST(test_connect_error_prints_and_returns_to_idle);
    RUN_TEST(test_tool_round_prints_panels);
    TEST_SUMMARY();
}
