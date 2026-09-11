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

#define READ_BUF_CAP   16384
#define ERROR_BODY_MAX 2048 /* like quoth's openai error-body cap */

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
    char *error_body; /* captured non-SSE error body, capped */
    size_t error_len;
    int head_checked; /* SSE/content-type validation done */
    int error_mode;   /* non-SSE response: draining the error body */
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
 * alive (error/result fields) until chat_end frees it. Idempotent. */
static void stream_teardown(NmChatStream *h)
{
    if (!h)
        return;
    if (h->conn) {
        nm_connection_close(h->conn);
        h->conn = NULL;
    }
    if (h->sse) {
        nm_sse_free(h->sse);
        h->sse = NULL;
    }
}

/* One pull: read what's ready, feed SSE, dispatch events.
 * Returns NM_CHAT_PENDING (call again later — would-block or head
 * incomplete), NM_CHAT_OK (stream complete), or an error. */
static NmChatStatus stream_one_step(NmChatStream *st)
{
    NmSseEvent ev;
    long n = nm_read_body(st->conn, st->rbuf, READ_BUF_CAP);
    if (n == NM_READ_WOULD_BLOCK)
        return NM_CHAT_PENDING;
    if (n < 0)
        return NM_CHAT_ERR_TRANSPORT;
    if (n == 0)
        return NM_CHAT_OK; /* EOF: stream complete (or truncated; the
                              caller's status check flags it) */

    /* nm_sse_feed consumes the whole buffer each call (an unconsumed
     * tail is stashed inside the parser); each call emits at most
     * one event. Feed, then drain the stashed tail with empty feeds
     * until no more events come out. */
    int r = nm_sse_feed(st->sse, st->rbuf, (size_t)n, &ev);
    while (r == 1) {
        handle_event(st, ev.data, strlen(ev.data));
        if (st->done)
            break;
        r = nm_sse_feed(st->sse, "", 0, &ev);
    }
    if (r < 0)
        return NM_CHAT_ERR_PARSE;
    if (st->done)
        return NM_CHAT_OK;
    return NM_CHAT_PENDING;
}

/* ---------------------------------------------------------------- */
/* Public: chat + models                                            */
/* ---------------------------------------------------------------- */

NmChatStream *nm_openai_chat_begin(const NmOpenaiEndpoint *ep,
                                   const NmChatRequest *req, NmChatResult *err)
{
    if (err) {
        err->status = NM_CHAT_OK;
        err->http_status = 0;
        free(err->error_body);
        err->error_body = NULL;
    }
    if (!ep || !req || !req->model) {
        if (err)
            err->status = NM_CHAT_ERR_PARSE;
        return NULL;
    }

    char host[256];
    int port;
    NmTransportMode mode;
    if (nm_openai_split_base_url(ep->base_url, host, sizeof(host), &port, &mode) != 0) {
        if (err)
            err->status = NM_CHAT_ERR_TRANSPORT;
        return NULL;
    }

    /* Compose + queue (the async transport seam, N2): non-blocking
     * connect in flight, request serialized into the connection's
     * owned buffer. NO blocking before returning — the connect/send
     * phases are driven by chat_step, which the event loop calls on
     * writability. The stream handle owns the connection from here. */
    char *body = compose_body(ep, req);
    if (!body) {
        if (err)
            err->status = NM_CHAT_ERR_PARSE;
        return NULL;
    }

    NmTransportStatus tst;
    NmConnection *conn = nm_connect_async(host, port, mode, &tst);
    if (!conn) {
        free(body);
        if (err)
            err->status = NM_CHAT_ERR_TRANSPORT;
        return NULL;
    }

    /* Headers: Content-Type, auth, User-Agent (nevermore as itself;
     * no Crush emulation — see openai_client.h). */
    NmRequestHeader hdrs[3];
    size_t nh = 0;
    hdrs[nh].name = "Content-Type";
    hdrs[nh].value = "application/json";
    nh++;
    char authbuf[512];
    if (ep->auth_header && ep->api_key && *ep->api_key) {
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

    NmChatStream *st = calloc(1, sizeof(*st));
    if (!st) {
        free(body);
        nm_connection_close(conn);
        if (err)
            err->status = NM_CHAT_ERR_TRANSPORT;
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
        if (err)
            err->status = NM_CHAT_ERR_TRANSPORT;
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
        if (err)
            err->status = NM_CHAT_ERR_TRANSPORT;
        nm_connection_close(conn);
        free(st);
        return NULL;
    }
    st->sse = nm_sse_new();
    if (!st->sse) {
        if (err)
            err->status = NM_CHAT_ERR_TRANSPORT;
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
            if (h->error_len == 0) {
                h->error_body = malloc(ERROR_BODY_MAX);
                if (!h->error_body)
                    return 0;
            }
            memcpy(h->error_body + h->error_len, h->rbuf, take);
            h->error_len += take;
        }
        if (h->error_len >= ERROR_BODY_MAX - 1)
            return 0;
    }
}

/* Finish the error result: NULL-terminate, publish to `result`,
 * tear the stream down, return the status. */
static NmChatStatus error_result(NmChatStream *h, NmChatResult *result)
{
    if (h->error_body)
        h->error_body[h->error_len] = '\0';
    h->done = 1;
    if (result) {
        result->status = h->status;
        result->http_status = h->http_status;
        result->error_body = h->error_body ? strdup(h->error_body) : NULL;
    }
    stream_teardown(h);
    return h->status;
}

NmChatStatus nm_openai_chat_step(NmChatStream *h, NmChatResult *result)
{
    if (result) {
        result->status = NM_CHAT_OK;
        result->http_status = 0;
        free(result->error_body);
        result->error_body = NULL;
    }
    if (!h)
        return result->status = NM_CHAT_ERR_PARSE, NM_CHAT_ERR_PARSE;
    if (h->done)
        return result->status = h->status, h->status;

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
            if (result) {
                result->status = NM_CHAT_ERR_TRANSPORT;
                result->http_status = 0;
                result->error_body = NULL;
            }
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
     * status + content-type, and on non-SSE switch to error mode. */
    if (!h->head_checked && h->conn) {
        const NmResponse *resp = nm_response(h->conn);
        if (resp->status == 0) {
            /* Head not parsed yet: pull bytes into the head scanner
             * (this is what nm_read_body does pre-body_started). */
            NmChatStatus s = stream_one_step(h);
            if (s == NM_CHAT_PENDING) {
                resp = nm_response(h->conn);
                if (resp->status == 0)
                    return NM_CHAT_PENDING; /* head still incomplete */
            } else if (s == NM_CHAT_OK) {
                /* The whole response (head + body) rode one read —
                 * normal on loopback and whenever the server wins
                 * the race. Fall through to the shared completion
                 * path below instead of returning early: an early
                 * return here would skip stream_finish() and lose
                 * the assembled tool calls, turning a tool round
                 * into a silent empty answer. */
                resp = nm_response(h->conn);
                if (resp->status == 0) {
                    /* EOF before a parsable head: dead connection. */
                    h->done = 1;
                    h->status = NM_CHAT_ERR_TRANSPORT;
                    if (result) {
                        result->status = NM_CHAT_ERR_TRANSPORT;
                        result->http_status = 0;
                        result->error_body = NULL;
                    }
                    stream_teardown(h);
                    return NM_CHAT_ERR_TRANSPORT;
                }
            } else {
                /* Fatal during the head pull: same treatment as the
                 * SSE body path below (fill result, teardown). */
                h->done = 1;
                h->status = s;
                if (result) {
                    result->status = s;
                    result->http_status = h->http_status;
                    result->error_body = h->error_body
                                             ? strdup(h->error_body)
                                             : NULL;
                }
                stream_teardown(h);
                return s;
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
        if (!h->done)
            h->status = NM_CHAT_ERR_TRANSPORT;
        stream_finish(h);
        if (result) {
            result->status = h->status;
            result->http_status = h->http_status;
            result->error_body = h->error_body ? strdup(h->error_body)
                                               : NULL;
        }
        stream_teardown(h);
        return h->status;
    }
    if (s != NM_CHAT_PENDING) {
        /* Fatal mid-stream error. */
        h->done = 1;
        h->status = s;
        if (result) {
            result->status = s;
            result->http_status = h->http_status;
            result->error_body = h->error_body ? strdup(h->error_body)
                                               : NULL;
        }
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
    free(h->error_body);
    free(h);
}

NmChatResult nm_openai_chat(const NmOpenaiEndpoint *ep,
                            const NmChatRequest *req)
{
    NmChatResult r = { NM_CHAT_OK, 0, NULL };
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

NmJson *nm_openai_models(const NmOpenaiEndpoint *ep, const char **err)
{
    /* GET {base_url}/models — phase 2 wires this for openai; the
     * ollama provider uses its static catalog until phase 5. */
    (void)ep;
    if (err)
        *err = "not yet implemented";
    return NULL; /* TODO(phase 2/5) */
}
