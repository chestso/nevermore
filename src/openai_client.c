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

/* Blocking-pump wait (nm_openai_chat): one 10ms readiness poll. */
#ifdef _WIN32
#include <winsock2.h>
#else
#include <sys/select.h>
#include <sys/time.h>
#endif

#define READ_BUF_CAP 16384
/* Non-SSE error-body capture cap: larger than NM_CHAT_MSG_MAX so
 * the "body too long" marker can note what was dropped. */
#define ERROR_BODY_MAX (NM_CHAT_MSG_MAX + 32)

/* Silences -Wunused-parameter on params kept for vtable symmetry. */
#if defined(__GNUC__)
#define NM_UNUSED __attribute__((unused))
#else
#define NM_UNUSED
#endif

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
static char *compose_body(const NmOpenaiEndpoint *ep NM_UNUSED,
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
        /* Tool-call round-trip: an assistant message may carry its
         * tool_calls array (pre-serialized JSON, embedded verbatim);
         * a tool message names the call it answers. */
        if (req->messages[i].tool_calls_json && *req->messages[i].tool_calls_json) {
            const char *jerr = NULL;
            NmJson *tcs = nm_json_parse(req->messages[i].tool_calls_json,
                                        strlen(req->messages[i].tool_calls_json),
                                        &jerr);
            if (tcs) {
                nm_json_set(m, "tool_calls", tcs);
                nm_json_free(tcs); /* set() deep-copied it */
            }
            /* Malformed array: omit rather than fail the turn. */
        }
        if (req->messages[i].tool_call_id && *req->messages[i].tool_call_id)
            nm_json_set(m, "tool_call_id",
                        nm_json_new_string(req->messages[i].tool_call_id));
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
            nm_json_free(tools); /* set() deep-copied it */
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

/* Stream handle: the StreamState + connection + reused read buffer.
 * One allocation per chat call (memory-reuse principle: the SSE
 * parser, read buffer, and tool-call array live here for the
 * stream's whole life; per-event values are borrowed pointers). */
struct NmChatStream
{
    NmConnection *conn;
    char rbuf[READ_BUF_CAP];
    NmSseParser *sse;
    /* Callback context, copied at begin: the NmChatRequest itself is
     * only valid during chat_begin (compose reads it); the stream's
     * lifetime outlives the caller's request object. */
    NmStreamCallback on_delta;
    void *userdata;
    int done;            /* [DONE] seen or fatal error */
    NmChatStatus status; /* final status */
    int http_status;
    char error_body[ERROR_BODY_MAX]; /* captured non-SSE error body, capped */
    size_t error_len;
    /* Body bytes that rode along with the head-completing read,
     * before the content-type verdict decided their consumer (SSE
     * parser vs error-body capture). At most one pull's worth
     * (nm_read_body returns head-adjacent body bytes exactly once);
     * routed by the head check in chat_step, then always empty. */
    char preread[READ_BUF_CAP];
    size_t preread_len;
    int head_checked; /* SSE/content-type validation done */
    int error_mode;   /* non-SSE response: draining the error body */
    int error_tapped; /* the failure's error line already emitted */
    /* Assembled tool calls (index-addressed; buffers reused across
     * chunks). Ownership moves to the on_delta receiver at the final
     * NULL-content callback (freed by the receiver with
     * nm_tool_calls_free); freed by the client when the stream dies
     * or no receiver is registered. */
    NmToolCall *tool_calls;
    size_t n_tool_calls;
    size_t tc_cap;
};

void nm_tool_calls_free(NmToolCall *calls, size_t n)
{
    if (!calls)
        return;
    for (size_t i = 0; i < n; i++) {
        free(calls[i].id);
        free(calls[i].name);
        free(calls[i].args_json);
    }
    free(calls);
}

/* Handle one complete SSE event's data payload (already JSON-parsed
 * arena tree `obj`, borrowed). Port of quoth's sse-extract-deltas +
 * merge-tool-calls; tool_calls are reported through on_delta as
 * complete JSON strings when finish_reason arrives. */
static void handle_event(NmChatStream *st, const char *data, size_t len)
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
        snprintf(st->error_body, sizeof(st->error_body), "server error "
                                                         "event: %s",
                 msg ? msg : "(unparseable error payload)");
        st->error_len = strlen(st->error_body);
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
            if (content && *content && st->on_delta)
                st->on_delta(content, NULL, 0, st->userdata);
            /* Tool-call deltas: merge fragments by index (port of
             * quoth's sse-merge-tool-calls). Arguments accumulate
             * across chunks; assembled calls are delivered from
             * finish_stream when the stream completes. */
            NmJson *tcs = nm_json_get(delta, "tool_calls");
            for (size_t i = 0; tcs && i < nm_json_len(tcs); i++) {
                NmJson *tc = nm_json_at(tcs, i);
                long idx = (long)nm_json_num(nm_json_get(tc, "index"));
                if (idx < 0)
                    continue;
                while ((size_t)idx >= st->tc_cap) {
                    size_t ncap = st->tc_cap ? st->tc_cap * 2 : 4;
                    NmToolCall *nt =
                        realloc(st->tool_calls, ncap * sizeof(*nt));
                    if (!nt)
                        continue;
                    memset(nt + st->tc_cap, 0,
                           (ncap - st->tc_cap) * sizeof(*nt));
                    st->tool_calls = nt;
                    st->tc_cap = ncap;
                }
                NmToolCall *slot = &st->tool_calls[idx];
                if ((size_t)idx + 1 > st->n_tool_calls)
                    st->n_tool_calls = (size_t)idx + 1;
                NmJson *fn = nm_json_get(tc, "function");
                if (fn) {
                    const char *args =
                        nm_json_str(nm_json_get(fn, "arguments"));
                    if (args && *args) {
                        /* Growable per-call args buffer, reused across
                         * chunks (memory-reuse principle). */
                        size_t alen = strlen(args);
                        if (slot->args_len + alen + 1 > slot->args_cap) {
                            size_t ncap = slot->args_cap ? slot->args_cap : 64;
                            while (ncap < slot->args_len + alen + 1)
                                ncap *= 2;
                            char *nb = realloc(slot->args_json, ncap);
                            if (nb) {
                                slot->args_json = nb;
                                slot->args_cap = ncap;
                            }
                        }
                        if (slot->args_len + alen + 1 <= slot->args_cap) {
                            memcpy(slot->args_json + slot->args_len, args,
                                   alen);
                            slot->args_len += alen;
                            slot->args_json[slot->args_len] = '\0';
                        }
                    }
                    /* id/name ride the first fragment for their index;
                     * copied out of the per-event arena immediately
                     * (the arena is freed below). */
                    const char *name = nm_json_str(nm_json_get(fn, "name"));
                    if (name && !slot->name)
                        slot->name = strdup(name);
                }
                const char *id = nm_json_str(nm_json_get(tc, "id"));
                if (id && !slot->id)
                    slot->id = strdup(id);
            }
        }
    }
    nm_json_free(obj);
}

/* Stream completion: deliver assembled tool calls (if any) with the
 * final NULL-content callback, exactly once. Ownership of the
 * tool-call array moves to the receiver (freed there with
 * nm_tool_calls_free); the client forgets the pointer. */
static void stream_finish(NmChatStream *st)
{
    if (st->n_tool_calls > 0 && st->on_delta) {
        st->on_delta(NULL, st->tool_calls, st->n_tool_calls,
                     st->userdata);
        st->tool_calls = NULL;
        st->n_tool_calls = 0;
    }
}

/* Teardown: release the connection + SSE parser. The handle stays
 * alive (error/result fields) until chat_end frees it. Idempotent.
 * Fatal-exit paths tap the failure first (WIRE-DEBUG §3: every
 * client failure is an error line; the HTTP status rides along when
 * the wire answered). */
static void stream_teardown(NmChatStream *h)
{
    if (!h)
        return;
    if (h->status != NM_CHAT_OK && h->status != NM_CHAT_PENDING &&
        h->conn && !h->error_tapped) {
        h->error_tapped = 1;
        /* Body/protocol-stage failure on an exchange already
         * queued: xchg is the correlation the log already carries. */
        if (h->http_status)
            nm_wire_tap_error_status(h->conn, "protocol", h->error_body,
                                     h->http_status);
        else
            nm_wire_tap_error(h->conn, "body", h->error_body);
    }
    if (h->conn) {
        nm_connection_close(h->conn);
        h->conn = NULL;
    }
    if (h->sse) {
        nm_sse_free(h->sse);
        h->sse = NULL;
    }
}

/* Feed the SSE parser with bytes from one source (preread stash or
 * the socket), dispatching complete events. Returns 1 when a
 * complete event was dispatched, 0 = need more bytes, -1 = parse
 * error. Every complete event ALSO rides the wire tap (raw SSE
 * stream only — the derived tool-call events are deliberately not
 * recorded; the stream data carries them). */
static int feed_and_dispatch(NmChatStream *st, const char *data, size_t len)
{
    if (len == 0)
        return 0;
    NmSseEvent ev;
    int r = nm_sse_feed(st->sse, data, len, &ev);
    while (r == 1) {
        nm_wire_tap_stream_event(st->conn, ev.event, ev.data,
                                 strlen(ev.data));
        handle_event(st, ev.data, strlen(ev.data));
        if (st->done)
            break;
        r = nm_sse_feed(st->sse, "", 0, &ev);
    }
    return r;
}

/* One pull: read what's ready, feed SSE, dispatch events.
 * Returns NM_CHAT_PENDING (call again later — would-block or head
 * incomplete), NM_CHAT_OK (stream complete), or an error. */
static NmChatStatus stream_one_step(NmChatStream *st)
{
    /* Body bytes stashed by the head check (their consumer was not
     * known when they arrived): feed them first, then the socket. */
    if (st->preread_len) {
        size_t take = st->preread_len;
        st->preread_len = 0;
        int r = feed_and_dispatch(st, st->preread, take);
        if (r < 0) {
            st->status = NM_CHAT_ERR_PARSE;
            return NM_CHAT_ERR_PARSE;
        }
        if (st->done)
            return NM_CHAT_OK;
    }

    NmSseEvent ev;
    long n = nm_read_body(st->conn, st->rbuf, READ_BUF_CAP);
    if (n == NM_READ_WOULD_BLOCK)
        return NM_CHAT_PENDING;
    if (n < 0) {
        st->status = NM_CHAT_ERR_TRANSPORT;
        snprintf(st->error_body, sizeof(st->error_body), "%s",
                 nm_connection_last_error(st->conn));
        st->error_len = strlen(st->error_body);
        return NM_CHAT_ERR_TRANSPORT;
    }
    if (n == 0)
        return NM_CHAT_OK; /* EOF: stream complete (or truncated; the
                              caller's status check flags it) */

    int r = nm_sse_feed(st->sse, st->rbuf, (size_t)n, &ev);
    while (r == 1) {
        nm_wire_tap_stream_event(st->conn, ev.event, ev.data,
                                 strlen(ev.data));
        handle_event(st, ev.data, strlen(ev.data));
        if (st->done)
            break;
        r = nm_sse_feed(st->sse, "", 0, &ev);
    }
    if (r < 0) {
        st->status = NM_CHAT_ERR_PARSE;
        return NM_CHAT_ERR_PARSE;
    }
    if (st->done)
        return NM_CHAT_OK;
    return NM_CHAT_PENDING;
}

/* Pull bytes until the response head completes (or stalls), WITHOUT
 * feeding the SSE parser: body bytes that ride along with the
 * head-completing read go to the preread stash, because their
 * consumer (SSE parser vs error-body capture) is decided by the
 * head's status/content-type verdict — the very next thing
 * chat_step does. This is the fix for the whole-response-in-one-
 * read race: a 401's body arrived before the verdict, and feeding
 * it to the SSE parser lost it.
 *
 * Returns NM_CHAT_PENDING (head still incomplete), NM_CHAT_OK (head
 * complete — check nm_response), or an error. */
static NmChatStatus head_pull_step(NmChatStream *st)
{
    if (st->preread_len) {
        /* A previous call completed the head and stashed the body;
         * there is nothing more to do here. */
        return NM_CHAT_OK;
    }
    long n = nm_read_body(st->conn, st->rbuf, READ_BUF_CAP);
    if (n == NM_READ_WOULD_BLOCK)
        return NM_CHAT_PENDING;
    if (n < 0) {
        st->status = NM_CHAT_ERR_TRANSPORT;
        snprintf(st->error_body, sizeof(st->error_body), "%s",
                 nm_connection_last_error(st->conn));
        st->error_len = strlen(st->error_body);
        return NM_CHAT_ERR_TRANSPORT;
    }
    if (n > 0) {
        /* nm_read_body parses the head transparently: any bytes it
         * returned AFTER the head are body bytes — stash them. */
        memcpy(st->preread, st->rbuf, (size_t)n);
        st->preread_len = (size_t)n;
        return NM_CHAT_OK;
    }
    /* n == 0: EOF. EOF before any head: dead connection. A complete
     * head with a connection-close-framed empty body is also n == 0,
     * but that returns OK with resp->status != 0 first (the head
     * landed in an earlier call, or in this one's scratch parse). */
    const NmResponse *resp = nm_response(st->conn);
    if (resp->status == 0) {
        st->status = NM_CHAT_ERR_TRANSPORT;
        snprintf(st->error_body, sizeof(st->error_body),
                 "connection closed before a response arrived");
        st->error_len = strlen(st->error_body);
        return NM_CHAT_ERR_TRANSPORT;
    }
    return NM_CHAT_OK;
}

/* ---------------------------------------------------------------- */
/* Public: chat + models                                            */
/* ---------------------------------------------------------------- */

/* Publish the stream's failure state into a result (always-set
 * contract: message is composed per status class). result may be
 * NULL (a step driven without a result out). */
static void result_publish(const NmChatStream *h, NmChatResult *result)
{
    if (!result)
        return;
    result->status = h->status;
    result->http_status = h->http_status;
    result->message[0] = '\0';
    switch (h->status) {
    case NM_CHAT_OK:
    case NM_CHAT_PENDING:
        return;
    case NM_CHAT_ERR_AUTH:
        snprintf(result->message, sizeof(result->message),
                 "auth rejected (HTTP %d): %s", h->http_status,
                 *h->error_body ? h->error_body
                                : "no error body returned");
        break;
    case NM_CHAT_ERR_HTTP:
        snprintf(result->message, sizeof(result->message),
                 "HTTP %d: %s", h->http_status,
                 *h->error_body ? h->error_body
                                : "no error body returned");
        break;
    case NM_CHAT_ERR_TRANSPORT:
        snprintf(result->message, sizeof(result->message), "%s",
                 *h->error_body ? h->error_body
                                : "connection failed");
        break;
    case NM_CHAT_ERR_PARSE:
        snprintf(result->message, sizeof(result->message),
                 "malformed response: %s",
                 *h->error_body ? h->error_body : "not valid SSE/JSON");
        break;
    }
}

NmChatStream *nm_openai_chat_begin(const NmOpenaiEndpoint *ep,
                                   const NmChatRequest *req, NmChatResult *err)
{
    if (err) {
        err->status = NM_CHAT_OK;
        err->http_status = 0;
        err->message[0] = '\0';
    }
    if (!ep || !req || !req->model) {
        if (err) {
            err->status = NM_CHAT_ERR_PARSE;
            snprintf(err->message, sizeof(err->message),
                     "internal: bad request (missing %s)",
                     !ep ? "endpoint" : "model");
        }
        return NULL;
    }

    char host[256];
    int port;
    NmTransportMode mode;
    if (nm_openai_split_base_url(ep->base_url, host, sizeof(host), &port, &mode) != 0) {
        if (err) {
            err->status = NM_CHAT_ERR_TRANSPORT;
            snprintf(err->message, sizeof(err->message),
                     "bad base url '%s' (expected http:// or https://)",
                     ep->base_url ? ep->base_url : "(null)");
        }
        return NULL;
    }

    /* Compose + queue (the async transport seam, N2): non-blocking
     * connect in flight, request serialized into the connection's
     * owned buffer. NO blocking before returning — the connect/send
     * phases are driven by chat_step, which the event loop calls on
     * writability. The stream handle owns the connection from here. */
    char *body = compose_body(ep, req);
    if (!body) {
        if (err) {
            err->status = NM_CHAT_ERR_PARSE;
            snprintf(err->message, sizeof(err->message),
                     "internal: could not compose the request body");
        }
        return NULL;
    }

    NmConnectInfo ci;
    NmConnection *conn = nm_connect_async(host, port, mode, &ci);
    if (!conn) {
        free(body);
        if (err) {
            err->status = NM_CHAT_ERR_TRANSPORT;
            snprintf(err->message, sizeof(err->message), "%s", ci.detail);
        }
        return NULL;
    }

    /* Headers: Content-Type, auth, User-Agent (nevermore as itself;
     * no Crush emulation — see openai_client.h). The auth header is
     * built here and marked secret HERE — the recorder redacts
     * marked values at log-write time; onboarding a provider via
     * NmOpenaiEndpoint carries redaction with it (WIRE-DEBUG §4). */
    NmRequestHeader hdrs[3];
    size_t nh = 0;
    hdrs[nh].name = "Content-Type";
    hdrs[nh].value = "application/json";
    hdrs[nh].secret = 0;
    nh++;
    char authbuf[512];
    if (ep->auth_header && ep->api_key && *ep->api_key) {
        snprintf(authbuf, sizeof(authbuf), ep->auth_header, ep->api_key);
        hdrs[nh].name = "Authorization";
        hdrs[nh].value = authbuf;
        hdrs[nh].secret = 1;
        nh++;
    }
    hdrs[nh].name = "User-Agent";
    hdrs[nh].value = ep->user_agent ? ep->user_agent : "nevermore";
    hdrs[nh].secret = 0;
    nh++;

    char path[512];
    snprintf(path, sizeof(path), "%s/chat/completions",
             url_path_prefix(ep->base_url));

    NmChatStream *st = calloc(1, sizeof(*st));
    if (!st) {
        free(body);
        nm_connection_close(conn);
        if (err) {
            err->status = NM_CHAT_ERR_TRANSPORT;
            snprintf(err->message, sizeof(err->message),
                     "out of memory");
        }
        return NULL;
    }
    st->conn = conn;
    st->on_delta = req->on_delta;
    st->userdata = req->userdata;
    st->status = NM_CHAT_OK;

    /* Queue the request; the connect/send phases drain inside
     * chat_step. The stream is returned in CONNECTING. */
    NmTransportStatus rs = nm_request_queue(conn, "POST", path, hdrs, nh,
                                            body, strlen(body));
    free(body);
    if (rs != NM_TRANSPORT_OK) {
        if (err) {
            err->status = NM_CHAT_ERR_TRANSPORT;
            snprintf(err->message, sizeof(err->message),
                     "could not queue the request: %s",
                     nm_connection_last_error(conn));
        }
        nm_connection_close(conn);
        free(st);
        return NULL;
    }
    /* Async sockets are already non-blocking from connect_async; TLS
     * connections still refuse the flip — the documented narrowed
     * deferral; their reads block inside chat_step, which the
     * blocking pump and the TUI both tolerate sub-second. */
    if (nm_connection_set_nonblocking(conn) == NM_TRANSPORT_OK) {
        /* plain socket: flipped, nothing more to do */
    } else if (mode == NM_TRANSPORT_PLAIN) {
        if (err) {
            err->status = NM_CHAT_ERR_TRANSPORT;
            snprintf(err->message, sizeof(err->message),
                     "could not set the socket non-blocking: %s",
                     nm_connection_last_error(conn));
        }
        nm_connection_close(conn);
        free(st);
        return NULL;
    }
    st->sse = nm_sse_new();
    if (!st->sse) {
        if (err) {
            err->status = NM_CHAT_ERR_TRANSPORT;
            snprintf(err->message, sizeof(err->message),
                     "out of memory (SSE parser)");
        }
        nm_connection_close(conn);
        free(st);
        return NULL;
    }
    return st;
}

/* Error-body drain (non-SSE response): pull what's ready into the
 * capped error buffer. Returns 1 while more bytes may follow (the
 * caller reports PENDING), 0 when the body is complete/capped. */
static int error_drain_step(NmChatStream *h)
{
    /* Body bytes stashed by the head check arrive here first. */
    if (h->preread_len) {
        size_t take = h->preread_len;
        if (take > ERROR_BODY_MAX - 1 - h->error_len)
            take = ERROR_BODY_MAX - 1 - h->error_len;
        memcpy(h->error_body + h->error_len, h->preread, take);
        h->error_len += take;
        h->preread_len = 0;
        if (h->error_len >= ERROR_BODY_MAX - 1)
            return 0; /* capped: enough for a diagnostic */
    }
    for (;;) {
        long n = nm_read_body(h->conn, h->rbuf, READ_BUF_CAP);
        if (n == NM_READ_WOULD_BLOCK)
            return 1;
        if (n <= 0)
            return 0;
        if (h->error_len < ERROR_BODY_MAX - 1) {
            size_t take = (size_t)n;
            if (take > ERROR_BODY_MAX - 1 - h->error_len)
                take = ERROR_BODY_MAX - 1 - h->error_len;
            memcpy(h->error_body + h->error_len, h->rbuf, take);
            h->error_len += take;
        }
        if (h->error_len >= ERROR_BODY_MAX - 1)
            return 0; /* capped: enough for a diagnostic */
    }
}

/* Finish the error result: trim the captured body's outer whitespace
 * (wire framing: error pages end with a trailing newline; a
 * diagnostic message is not a verbatim transcript), NULL-terminate,
 * publish to `result`, tear the stream down, return the status. */
static NmChatStatus error_result(NmChatStream *h, NmChatResult *result)
{
    while (h->error_len > 0 &&
           (h->error_body[h->error_len - 1] == '\n' ||
            h->error_body[h->error_len - 1] == '\r' ||
            h->error_body[h->error_len - 1] == ' ' ||
            h->error_body[h->error_len - 1] == '\t'))
        h->error_len--;
    h->error_body[h->error_len] = '\0';
    h->done = 1;
    result_publish(h, result);
    stream_teardown(h);
    return h->status;
}

NmChatStatus nm_openai_chat_step(NmChatStream *h, NmChatResult *result)
{
    if (result) {
        result->status = NM_CHAT_OK;
        result->http_status = 0;
        result->message[0] = '\0';
    }
    if (!h) {
        if (result) {
            result->status = NM_CHAT_ERR_PARSE;
            snprintf(result->message, sizeof(result->message),
                     "internal: stepped a NULL stream");
        }
        return NM_CHAT_ERR_PARSE;
    }
    if (h->done) {
        result_publish(h, result);
        return h->status;
    }

    /* Drive the transport phase machine first (async connect/send,
     * N2): the request is not on the wire until this returns OK at
     * the READING phase. PENDING = still connecting/sending; the
     * caller steps again per the interest bits. */
    if (h->conn) {
        NmTransportStatus ts = nm_connection_step(h->conn);
        if (ts == NM_TRANSPORT_PENDING)
            return NM_CHAT_PENDING;
        if (ts != NM_TRANSPORT_OK) {
            h->done = 1;
            h->status = NM_CHAT_ERR_TRANSPORT;
            snprintf(h->error_body, sizeof(h->error_body), "%s",
                     nm_connection_last_error(h->conn));
            h->error_len = strlen(h->error_body);
            result_publish(h, result);
            stream_teardown(h);
            return NM_CHAT_ERR_TRANSPORT;
        }
    }

    /* Error-body drain (non-SSE response): continue where the first
     * step left off; would-block means the rest arrives later. */
    if (h->error_mode) {
        if (error_drain_step(h))
            return NM_CHAT_PENDING;
        return error_result(h, result);
    }

    /* First step: the response head. It may still be incomplete
     * (PENDING until the blank line lands); once complete, validate
     * status + content-type, and on non-SSE switch to error mode.
     * The pull never feeds the SSE parser: body bytes riding the
     * head-completing read are stashed (preread) until the verdict
     * decides their consumer — a 401's body must reach the error
     * capture, not the SSE parser. */
    if (!h->head_checked && h->conn) {
        const NmResponse *resp = nm_response(h->conn);
        if (resp->status == 0) {
            NmChatStatus s = head_pull_step(h);
            if (s == NM_CHAT_PENDING)
                return NM_CHAT_PENDING; /* head still incomplete */
            if (s != NM_CHAT_OK) {
                /* Fatal during the head pull: same treatment as the
                 * SSE body path below (fill result, teardown). */
                h->done = 1;
                h->status = s;
                result_publish(h, result);
                stream_teardown(h);
                return s;
            }
            resp = nm_response(h->conn);
            if (resp->status == 0) {
                /* EOF before a parsable head: dead connection. */
                h->done = 1;
                h->status = NM_CHAT_ERR_TRANSPORT;
                result_publish(h, result);
                stream_teardown(h);
                return NM_CHAT_ERR_TRANSPORT;
            }
        }
        h->head_checked = 1;
        if (resp->status < 200 || resp->status >= 300 ||
            !resp->content_type ||
            strncmp(resp->content_type, "text/event-stream", 17) != 0) {
            h->error_mode = 1;
            h->status = (resp->status == 401 || resp->status == 403)
                            ? NM_CHAT_ERR_AUTH
                            : NM_CHAT_ERR_HTTP;
            h->http_status = resp->status;
            if (error_drain_step(h))
                return NM_CHAT_PENDING;
            return error_result(h, result);
        }
    }

    /* SSE body: pull what's ready. */
    NmChatStatus s = stream_one_step(h);
    if (s == NM_CHAT_OK) {
        /* [DONE] or EOF. EOF before [DONE] is a truncated stream:
         * deliver what arrived but flag the transport condition. */
        if (!h->done) {
            h->status = NM_CHAT_ERR_TRANSPORT;
            /* Prefer the transport's specific truncation detail
             * (byte counts); the generic note is the fallback. */
            const char *d = *h->error_body ? h->error_body
                                           : nm_connection_last_error(h->conn);
            snprintf(h->error_body, sizeof(h->error_body), "%s",
                     *d ? d : "stream ended before [DONE]");
            h->error_len = strlen(h->error_body);
        }
        stream_finish(h);
        result_publish(h, result);
        stream_teardown(h);
        return h->status;
    }
    if (s != NM_CHAT_PENDING) {
        /* Fatal mid-stream error. */
        h->done = 1;
        h->status = s;
        result_publish(h, result);
        stream_teardown(h);
    }
    return s;
}

int nm_openai_stream_fd(NmChatStream *h)
{
    return h && h->conn ? nm_connection_fd(h->conn) : -1;
}

unsigned nm_openai_stream_interest(NmChatStream *h)
{
    if (!h || !h->conn)
        return 0;
    NmConnectionInterest i = nm_connection_interest(h->conn);
    return i.fd >= 0 ? i.flags : 0;
}

void nm_openai_chat_end(NmChatStream *h)
{
    if (!h)
        return;
    stream_teardown(h);
    /* Cancelled or already-torn-down: tool calls still owned by the
     * client (never delivered) are freed here. */
    nm_tool_calls_free(h->tool_calls, h->n_tool_calls);
    free(h);
}

NmChatResult nm_openai_chat(const NmOpenaiEndpoint *ep,
                            const NmChatRequest *req)
{
    NmChatResult r = { 0 };
    NmChatStream *h = nm_openai_chat_begin(ep, req, &r);
    if (!h)
        return r;

    /* Blocking pump over the step seam: keep stepping while the
     * response head is incomplete or bytes are pending. Head reads
     * pre-non-blocking are already inside begin; the step loop's
     * PENDING returns would spin on a non-blocking socket, so block
     * it back off for the pump. (A blocking pump on a non-blocking
     * fd would busy-loop.) */
    for (;;) {
        NmChatStatus s = nm_openai_chat_step(h, &r);
        if (s != NM_CHAT_PENDING)
            break;
        /* PENDING: wait on the CURRENT interest bits (connect/send
         * phases wait writability, the response phase readability),
         * then step again. 10ms-bounded poll keeps this simple (ask
         * mode is a one-shot process, not a UI loop). */
        int fd = nm_openai_stream_fd(h);
        unsigned interest = nm_openai_stream_interest(h);
        if (fd < 0 || !interest)
            break; /* torn down mid-step (error path) */
        fd_set rd, wr;
        FD_ZERO(&rd);
        FD_ZERO(&wr);
        struct timeval tv = { 0, 10 * 1000 };
        if (interest & NM_INTEREST_READ)
            FD_SET(fd, &rd);
        if (interest & NM_INTEREST_WRITE)
            FD_SET(fd, &wr);
#ifdef _WIN32
        select(fd + 1, (interest & NM_INTEREST_READ) ? &rd : NULL,
               (interest & NM_INTEREST_WRITE) ? &wr : NULL, NULL, &tv);
#else
        select(fd + 1, &rd, &wr, NULL, &tv);
#endif
    }
    nm_openai_chat_end(h);
    return r;
}

NmJson *nm_fetch_json(const char *base_url, const char *method,
                      const char *path, const char *auth_header,
                      const char *api_key, const char *body,
                      const char **err)
{
    if (err)
        *err = NULL;
    if (!base_url || !*base_url) {
        if (err)
            *err = "no base url";
        return NULL;
    }

    char host[256];
    int port;
    NmTransportMode mode;
    if (nm_openai_split_base_url(base_url, host, sizeof(host), &port,
                                 &mode) != 0) {
        if (err)
            *err = "bad base url";
        return NULL;
    }

    NmConnectInfo ci;
    NmConnection *conn = nm_connect(host, port, mode, &ci);
    if (!conn) {
        if (err)
            *err = *ci.detail ? ci.detail : "connect failed";
        return NULL;
    }
    /* One-shot fetch is user-facing-blocking (models popup, catalog
     * refresh): bound it, so a wedged peer degrades to the static
     * fallback instead of freezing the UI. */
    nm_connection_set_recv_timeout(conn, 2);

    /* Same header set as chat: Content-Type (bodies), auth (absent
     * for tokenless catalogs, e.g. hyper /v1/models — HYPER-API.md
     * §5) + User-Agent. The auth header is marked secret at its
     * construction site (the redaction seam; WIRE-DEBUG §4). */
    NmRequestHeader hdrs[3];
    size_t nh = 0;
    if (body) {
        hdrs[nh].name = "Content-Type";
        hdrs[nh].value = "application/json";
        hdrs[nh].secret = 0;
        nh++;
    }
    char authbuf[512];
    if (auth_header && api_key && *api_key) {
        snprintf(authbuf, sizeof(authbuf), auth_header, api_key);
        hdrs[nh].name = "Authorization";
        hdrs[nh].value = authbuf;
        hdrs[nh].secret = 1;
        nh++;
    }
    hdrs[nh].name = "User-Agent";
    hdrs[nh].value = "nevermore (nevermore agent)";
    hdrs[nh].secret = 0;
    nh++;

    size_t body_len = body ? strlen(body) : 0;
    if (nm_request(conn, method, path, hdrs, nh, body, body_len) != NM_TRANSPORT_OK) {
        const char *d = nm_connection_last_error(conn);
        nm_connection_close(conn);
        if (err)
            *err = *d ? d : "request failed";
        return NULL;
    }
    const NmResponse *resp = nm_response(conn);
    if (resp->status < 200 || resp->status >= 300) {
        nm_wire_tap_error(conn, "protocol", "http error");
        nm_connection_close(conn);
        if (err)
            *err = "http error";
        return NULL;
    }

    /* Whole response body into one growing buffer (one-shot fetch,
     * not the streaming path — the catalog is a bounded document). */
    char *resp_body = NULL;
    size_t len = 0, cap = 0;
    char chunk[4096];
    long n;
    while ((n = nm_read_body(conn, chunk, sizeof(chunk))) > 0) {
        if (len + (size_t)n > cap) {
            cap = cap ? cap * 2 : 8192;
            char *grown = realloc(resp_body, cap);
            if (!grown) {
                free(resp_body);
                nm_connection_close(conn);
                if (err)
                    *err = "oom";
                return NULL;
            }
            resp_body = grown;
        }
        memcpy(resp_body + len, chunk, (size_t)n);
        len += (size_t)n;
    }
    nm_connection_close(conn);
    if (!resp_body) {
        if (err)
            *err = "empty body";
        return NULL;
    }

    /* response capture point: a complete non-streaming body (the
     * one-shot fetch paths; WIRE-DEBUG §3 "response"). */
    nm_wire_tap_response(conn, resp_body, len);

    const char *jerr = NULL;
    NmJson *doc = nm_json_parse(resp_body, len, &jerr);
    free(resp_body);
    if (!doc) {
        if (err)
            *err = jerr ? jerr : "bad json";
        return NULL;
    }
    return doc; /* caller owns: nm_json_free when done */
}

NmJson *nm_openai_models(const NmOpenaiEndpoint *ep, const char **err)
{
    if (err)
        *err = NULL;
    if (!ep || !ep->base_url || !*ep->base_url) {
        if (err)
            *err = "no base url";
        return NULL;
    }

    char path[512];
    snprintf(path, sizeof(path), "%s/models", url_path_prefix(ep->base_url));
    return nm_fetch_json(ep->base_url, "GET", path, ep->auth_header,
                         ep->api_key, NULL, err);
}