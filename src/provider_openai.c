/* provider_openai.c - OpenAI provider
 *
 * api.openai.com over the shared openai_client (TLS via the
 * configure-time backend). Chat: POST /v1/chat/completions with
 * OPENAI_API_KEY. Catalog: GET /v1/models (live) with the static
 * data/nm-openai-models.json list as fallback (offline, no key).
 *
 * Catalog memory model (memory-reuse principle): the live catalog is
 * parsed ONCE into a provider-owned array of NmModel that stays for
 * the process lifetime — repeated nevermore models calls reuse it,
 * never re-fetching, never per-call allocation.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "provider_internal.h"
#include "openai_client.h"
#include "json.h"
#include "sse.h"
#include "transport_internal.h"

#define OPENAI_DEFAULT_BASE "https://api.openai.com/v1"

/* Static fallback catalog (data/nm-openai-models.json, curated). */
static const NmModel openai_static_models[] = {
    { "gpt-5.2", "GPT-5.2", 1, 400000 },
    { "gpt-5.2-mini", "GPT-5.2 mini", 1, 400000 },
    { "gpt-5.1", "GPT-5.1", 1, 400000 },
    { "gpt-5", "GPT-5", 1, 128000 },
    { "gpt-4.1", "GPT-4.1", 1, 1047576 },
    { "gpt-4o", "GPT-4o", 1, 128000 },
    { "gpt-4o-mini", "GPT-4o mini", 1, 128000 },
    { "o3", "o3 reasoning", 1, 200000 },
    { 0 }
};

/* Live catalog cache: provider-owned, process lifetime. */
static NmModel *openai_live_models;
static size_t openai_live_n;

static NmChatResult openai_chat(const NmProvider *p, const NmChatRequest *req,
                                const char *base_url, const char *api_key)
{
    (void)p;
    NmOpenaiEndpoint ep = {
        (base_url && *base_url) ? base_url : OPENAI_DEFAULT_BASE,
        "Bearer %s",
        api_key,
        "nevermore (nevermore agent)"
    };
    return nm_openai_chat(&ep, req);
}

/* Event-driven split (phase 4): same endpoint shape, step API. */
static NmChatStream *openai_chat_begin(const NmProvider *p,
                                       const NmChatRequest *req,
                                       const char *base_url,
                                       const char *api_key, NmChatResult *err)
{
    (void)p;
    NmOpenaiEndpoint ep = {
        (base_url && *base_url) ? base_url : OPENAI_DEFAULT_BASE,
        "Bearer %s",
        api_key,
        "nevermore (nevermore agent)"
    };
    return nm_openai_chat_begin(&ep, req, err);
}

/* GET {base}/models -> data[] -> cache as NmModel[]. One-shot fetch:
 * on failure the caller gets the static fallback. */
static void openai_fetch_catalog(const char *base_url, const char *api_key)
{
    const char *base = (base_url && *base_url) ? base_url
                                               : OPENAI_DEFAULT_BASE;
    char host[256];
    int port;
    NmTransportMode mode;
    if (nm_openai_split_base_url(base, host, sizeof(host), &port, &mode) != 0)
        return;

    NmTransportStatus tst;
    NmConnection *conn = nm_connect(host, port, mode, &tst);
    if (!conn)
        return;

    NmRequestHeader hdrs[2];
    size_t nh = 0;
    if (api_key && *api_key) {
        static char authbuf[512];
        snprintf(authbuf, sizeof(authbuf), "Bearer %s", api_key);
        hdrs[nh].name = "Authorization";
        hdrs[nh].value = authbuf;
        nh++;
    }
    hdrs[nh].name = "User-Agent";
    hdrs[nh].value = "nevermore (nevermore agent)";
    nh++;

    char path[512];
    const char *prefix = strstr(base, "://");
    prefix = prefix ? strchr(prefix + 3, '/') : NULL;
    snprintf(path, sizeof(path), "%s/models", prefix ? prefix : "");

    if (nm_request(conn, "GET", path, hdrs, nh, NULL, 0) != NM_TRANSPORT_OK) {
        nm_connection_close(conn);
        return;
    }
    const NmResponse *resp = nm_response(conn);
    if (resp->status < 200 || resp->status >= 300) {
        nm_connection_close(conn);
        return;
    }

    /* Read the whole body into one reused growing buffer. */
    char *body = NULL;
    size_t len = 0, cap = 0;
    char chunk[4096];
    long n;
    while ((n = nm_read_body(conn, chunk, sizeof(chunk))) > 0) {
        if (len + (size_t)n > cap) {
            cap = cap ? cap * 2 : 8192;
            body = realloc(body, cap);
            if (!body) {
                nm_connection_close(conn);
                return;
            }
        }
        memcpy(body + len, chunk, (size_t)n);
        len += (size_t)n;
    }
    nm_connection_close(conn);
    if (!body)
        return;

    const char *jerr = NULL;
    NmJson *doc = nm_json_parse(body, len, &jerr);
    free(body);
    if (!doc)
        return;

    NmJson *data = nm_json_get(doc, "data");
    size_t count = nm_json_len(data);
    if (count == 0) {
        nm_json_free(doc);
        return;
    }
    NmModel *models = calloc(count + 1, sizeof(NmModel));
    if (!models) {
        nm_json_free(doc);
        return;
    }
    size_t out = 0;
    for (size_t i = 0; i < count; i++) {
        NmJson *e = nm_json_at(data, i);
        const char *id = nm_json_str(nm_json_get(e, "id"));
        if (!id)
            continue;
        /* Strings are owned by the parsed document — but that document
         * is freed below, so strdup them into the cache. One-time cost
         * per process, not per-call (memory reuse across calls). */
        models[out].id = strdup(id);
        models[out].label = models[out].id;
        models[out].vision = 0;
        models[out].context_length = -1;
        out++;
    }
    if (out == 0) {
        free(models);
        nm_json_free(doc);
        return;
    }
    nm_json_free(doc);
    openai_live_models = models;
    openai_live_n = out;
}

static const NmModel *openai_models(const NmProvider *p, const char *base_url,
                                    const char *api_key, size_t *n_out)
{
    (void)p;
    if (!openai_live_models)
        openai_fetch_catalog(base_url, api_key);
    if (openai_live_models) {
        if (n_out)
            *n_out = openai_live_n;
        return openai_live_models;
    }
    /* Offline / no key: the static fallback. */
    if (n_out) {
        size_t n = 0;
        while (openai_static_models[n].id)
            n++;
        *n_out = n;
    }
    return openai_static_models;
}

static int openai_needs_auth(const NmProvider *p, const char *base_url)
{
    (void)p;
    (void)base_url;
    return 1; /* every OpenAI endpoint needs a key */
}

const struct NmProvider nm_openai_provider = {
    NM_PROVIDER_OPENAI,
    "openai",
    OPENAI_DEFAULT_BASE,
    openai_chat,
    openai_chat_begin,
    nm_openai_chat_step,
    nm_openai_stream_fd,
    nm_openai_chat_end,
    openai_models,
    openai_needs_auth,
};
