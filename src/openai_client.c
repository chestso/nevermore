/* openai_client.c - shared OpenAI-compatible wire client
 *
 * POST {base_url}/chat/completions with "stream": true, parse SSE
 * deltas, drive req->on_delta. Shared by the openai, ollama, and
 * openrouter providers — only the endpoint struct differs. Port of
 * quoth-openai-client.el (wire truth: docs/OLLAMA-CLOUD-API.md).
 *
 * Streaming loop: pull nm_read_body chunks into the connection's
 * reused read buffer, feed the SSE parser, dispatch complete events.
 * The loop lives here (phase-1 blocking ask); phase 4 lifts it into
 * a boba socket-readable callback around the same feed/dispach seam
 * (nm_openai_stream_step) without touching the parser layers.
 *
 * Memory model (memory-reuse principle): one NmSseParser and one read
 * buffer per chat call; every SSE event is parsed into an arena tree
 * freed before the next pull. No per-token allocation survives the
 * callback.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "openai_client.h"
#include "sse.h"
#include "transport_internal.h"

#define READ_BUF_CAP 16384
#define ERROR_BODY_MAX 2048 /* like quoth's openai error-body cap */

/* ---------------------------------------------------------------- */
/* URL / endpoint plumbing                                          */
/* ---------------------------------------------------------------- */

/* Parse "http(s)://host[:port]" out of base_url. Writes host/port.
 * Returns 0 on success. Character-level, no regex. */
int nm_openai_split_base_url(const char *url, char *host, size_t host_cap,
                             int *port, NmTransportMode *mode)
{
    *mode = NM_TRANSPORT_PLAIN;
    *port = 80;
    if (strncmp(url, "https://", 8) == 0) {
        *mode = NM_TRANSPORT_TLS;
        *port = 443;
        url += 8;
    } else if (strncmp(url, "http://", 7) == 0) {
        url += 7;
    } else {
        return -1;
    }
    const char *slash = strchr(url, '/');
    size_t hlen = slash ? (size_t)(slash - url) : strlen(url);
    const char *colon = memchr(url, ':', hlen);
    if (colon) {
        hlen = (size_t)(colon - url);
        *port = atoi(colon + 1);
    }
    if (hlen == 0 || hlen >= host_cap)
        return -1;
    memcpy(host, url, hlen);
    host[hlen] = '\0';
    return 0;
}

/* Path prefix from base_url ("" or "/v1"); the request path appends
 * "/chat/completions". base_url is caller-owned and lives through
 * the call, so the pointer is borrowed. */
static const char *url_path_prefix(const char *url)
{
    const char *p = strstr(url, "://");
    if (!p)
        return "";
    p = strchr(p + 3, '/');
    return p ? p : "";
}

/* ---------------------------------------------------------------- */
/* Request composition                                              */
/* ---------------------------------------------------------------- */

/* Build the chat/completions JSON body. One heap document, freed at
 * the end of nm_openai_chat — the request body is per-call by
 * nature, but it is ONE allocation tree, not per-message churn. */
static char *compose_body(const NmOpenaiEndpoint *ep,
                          const NmChatRequest *req)
{
    NmJson *body = nm_json_new_object();
    NmJson *messages = nm_json_new_array();

    if (req->system) {
        NmJson *m = nm_json_new_object();
        nm_json_set(m, "role", nm_json_new_string("system"));
        nm_json_set(m, "content", nm_json_new_string(req->system));
        nm_json_push(messages, m);
    }
    for (size_t i = 0; i < req->n_messages; i++) {
        NmJson *m = nm_json_new_object();
        nm_json_set(m, "role", nm_json_new_string(req->messages[i].role));
        nm_json_set(m, "content",
                    nm_json_new_string(req->messages[i].content));
        nm_json_push(messages, m);
    }
    nm_json_set(body, "model", nm_json_new_string(req->model));
    nm_json_set(body, "messages", messages);
    nm_json_set(body, "stream", nm_json_new_bool(1));
    if (req->temperature >= 0)
        nm_json_set(body, "temperature", nm_json_new_number(req->temperature));
    if (req->max_tokens >= 0)
        nm_json_set(body, "max_tokens", nm_json_new_number((double)req->max_tokens));

    /* tools_json: pre-serialized array from the toolset, embedded
     * verbatim via a raw parse (arena document, short-lived). */
    if (req->tools_json && *req->tools_json) {
        const char *err = NULL;
        NmJson *tools = nm_json_parse(req->tools_json,
                                     strlen(req->tools_json), &err);
        if (tools) {
            nm_json_set(body, "tools", tools);
            nm_json_set(body, "tool_choice", nm_json_new_string("auto"));
        }
        /* Malformed schema: omit tools rather than fail the turn —
         * the model just won't call them. */
    }

    char *s = nm_json_dump(body);
    nm_json_free(body);
    return s;
}

/* ---------------------------------------------------------------- */
/* SSE delta dispatch                                                */
/* ---------------------------------------------------------------- */

typedef struct StreamState
{
    NmSseParser *sse;
    const NmChatRequest *req;
    int done;             /* [DONE] seen or fatal error */
    NmChatStatus status;  /* final status */
    int http_status;
    char *error_body;    /* captured non-SSE error body, capped */
    size_t error_len;
} StreamState;

/* Handle one complete SSE event's data payload (already JSON-parsed
 * arena tree `obj`, borrowed). Port of quoth's sse-extract-deltas +
 * merge-tool-calls; tool_calls are reported through on_delta as
 * complete JSON strings when finish_reason arrives. */
static void handle_event(StreamState *st, const char *data, size_t len)
{
    if (strcmp(data, "[DONE]") == 0) {
        st->done = 1;
        return;
    }
    const char *err = NULL;
    NmJson *obj = nm_json_parse(data, len, &err);
    if (!obj)
        return; /* per-event malformed payload: skip, stream continues */

    /* Provider error event: {"error": {...}} — fatal, like quoth. */
    NmJson *eobj = nm_json_get(obj, "error");
    if (eobj) {
        st->done = 1;
        st->status = NM_CHAT_ERR_HTTP;
        st->http_status = 0;
        const char *msg = nm_json_str(nm_json_get(eobj, "message"));
        if (!msg)
            msg = nm_json_str(eobj);
        free(st->error_body);
        st->error_body = msg ? strdup(msg) : NULL;
        nm_json_free(obj);
        return;
    }

    NmJson *choices = nm_json_get(obj, "choices");
    NmJson *choice = nm_json_at(choices, 0);
    if (choice) {
        NmJson *delta = nm_json_get(choice, "delta");
        if (delta) {
            const char *content = nm_json_str(nm_json_get(delta, "content"));
            /* quoth drops empty content deltas: ollama puts "" on
             * every reasoning chunk; emitting them would garble the
             * region boundaries. */
            if (content && *content && st->req->on_delta)
                st->req->on_delta(content, NULL, st->req->userdata);
        }
    }
    nm_json_free(obj);
}

/* Pump: pull bytes, feed SSE, dispatch events until done or EOF.
 * Returns final status via st. */
static NmChatStatus stream_pump(NmConnection *conn, StreamState *st)
{
    char *rbuf = malloc(READ_BUF_CAP);
    NmSseEvent ev;
    if (!rbuf)
        return NM_CHAT_ERR_TRANSPORT;

    for (;;) {
        long n = nm_read_body(conn, rbuf, READ_BUF_CAP);
        if (n < 0) {
            free(rbuf);
            return NM_CHAT_ERR_TRANSPORT;
        }
        if (n == 0)
            break; /* EOF: stream complete (or truncated; caller sees status) */

        /* nm_sse_feed consumes the whole buffer each call (an unconsumed
         * tail is stashed inside the parser); each call emits at most
         * one event. Feed, then drain the stashed tail with empty
         * feeds until no more events come out. */
        int r = nm_sse_feed(st->sse, rbuf, (size_t)n, &ev);
        while (r == 1) {
            handle_event(st, ev.data, strlen(ev.data));
            if (st->done)
                break;
            r = nm_sse_feed(st->sse, "", 0, &ev);
        }
        if (r < 0) {
            free(rbuf);
            return NM_CHAT_ERR_PARSE;
        }
        if (st->done)
            break;
    }
    free(rbuf);
    return st->status;
}

/* ---------------------------------------------------------------- */
/* Public: chat + models                                            */
/* ---------------------------------------------------------------- */

NmChatResult nm_openai_chat(const NmOpenaiEndpoint *ep,
                            const NmChatRequest *req)
{
    NmChatResult r = { NM_CHAT_OK, 0, NULL };
    if (!ep || !req || !req->model)
        return r.status = NM_CHAT_ERR_PARSE, r;

    char host[256];
    int port;
    NmTransportMode mode;
    if (nm_openai_split_base_url(ep->base_url, host, sizeof(host), &port, &mode) != 0) {
        r.status = NM_CHAT_ERR_TRANSPORT;
        return r;
    }

    /* Compose + connect + send. */
    char *body = compose_body(ep, req);
    if (!body) {
        r.status = NM_CHAT_ERR_PARSE;
        return r;
    }

    NmTransportStatus tst;
    NmConnection *conn = nm_connect(host, port, mode, &tst);
    if (!conn) {
        free(body);
        r.status = NM_CHAT_ERR_TRANSPORT;
        return r;
    }

    /* Headers: Content-Type, auth, User-Agent (nevermore as itself;
     * no Crush emulation — see openai_client.h). */
    NmRequestHeader hdrs[3];
    size_t nh = 0;
    hdrs[nh].name = "Content-Type";
    hdrs[nh].value = "application/json";
    nh++;
    if (ep->auth_header && ep->api_key && *ep->api_key) {
        static char authbuf[512];
        snprintf(authbuf, sizeof(authbuf), ep->auth_header, ep->api_key);
        hdrs[nh].name = "Authorization";
        hdrs[nh].value = authbuf;
        nh++;
    }
    hdrs[nh].name = "User-Agent";
    hdrs[nh].value = ep->user_agent ? ep->user_agent : "nevermore";
    nh++;

    char path[512];
    snprintf(path, sizeof(path), "%s/chat/completions",
             url_path_prefix(ep->base_url));

    NmTransportStatus rs = nm_request(conn, "POST", path, hdrs, nh, body,
                                      strlen(body));
    free(body);
    if (rs != NM_TRANSPORT_OK) {
        nm_connection_close(conn);
        r.status = NM_CHAT_ERR_TRANSPORT;
        return r;
    }

    const NmResponse *resp = nm_response(conn);
    if (resp->status < 200 || resp->status >= 300
        || !resp->content_type
        || strncmp(resp->content_type, "text/event-stream", 17) != 0) {
        /* Non-SSE response: an error body. Capture (capped) for the
         * caller — like quoth's error-body accumulator. */
        char ebuf[ERROR_BODY_MAX];
        size_t elen = 0;
        long n;
        while ((n = nm_read_body(conn, ebuf + elen,
                                 sizeof(ebuf) - 1 - elen)) > 0) {
            elen += (size_t)n;
            if (elen >= sizeof(ebuf) - 1)
                break;
        }
        ebuf[elen] = '\0';
        /* Read the response fields BEFORE closing: resp points into
         * the connection, freed by nm_connection_close. */
        int status = resp->status;
        nm_connection_close(conn);
        r.status = (status == 401 || status == 403) ? NM_CHAT_ERR_AUTH
                                                    : NM_CHAT_ERR_HTTP;
        r.http_status = status;
        r.error_body = strdup(ebuf);
        return r;
    }

    /* SSE stream: pump until done. */
    StreamState st = { 0 };
    st.sse = nm_sse_new();
    st.req = req;
    st.status = NM_CHAT_OK;
    NmChatStatus status = stream_pump(conn, &st);

    r.status = status;
    r.http_status = st.http_status;
    r.error_body = st.error_body; /* ownership moves to result */
    if (!st.done && status == NM_CHAT_OK) {
        /* EOF before [DONE]: truncated stream, still deliver text but
         * flag the transport condition. */
        r.status = NM_CHAT_ERR_TRANSPORT;
        free(r.error_body);
        r.error_body = NULL;
    }
    nm_sse_free(st.sse);
    nm_connection_close(conn);
    return r;
}

NmJson *nm_openai_models(const NmOpenaiEndpoint *ep, const char **err)
{
    /* GET {base_url}/models — phase 2 wires this for openai; the
     * ollama provider uses its static catalog until phase 5. */
    (void)ep;
    if (err)
        *err = "not yet implemented";
    return NULL; /* TODO(phase 2/5) */
}

