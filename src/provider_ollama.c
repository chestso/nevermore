/* provider_ollama.c - Ollama Cloud (+ the shared Ollama wire surface)
 *
 * Two providers, one wire (docs/OLLAMA-CLOUD-API.md, live-probed):
 * this file holds Ollama Cloud (https://ollama.com, bearer key) and
 * the wire/catalog helpers; provider_ollama_local.c is the local
 * daemon (http://localhost:11434, no auth) forwarding here with its
 * own vtable. Chat goes through the shared openai_client; only the
 * endpoint differs.
 *
 * Model catalog (phase 5): native GET /api/tags for the membership
 * list + POST /api/show per model for the real metadata (the tags
 * details sub-object is an empty stub on the cloud — OLLAMA-CLOUD-
 * API.md §6.3), cached provider-owned for the process lifetime (one
 * cache per provider: the two talk to different endpoints).
 * /api/tags and /api/show answer with or without a key (cloud §6
 * note), so the local daemon needs no key and the cloud catalog
 * works offline-key. Static list stays the fallback.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "openai_client.h"
#include "provider_internal.h" /* nm_live_catalog_enabled */
#include "provider_openaicompat.h"
#include "catalog_data.h" /* the generated fallback catalog (every provider) */
#include "json.h"

#define OLLAMA_LOCAL_DEFAULT "http://localhost:11434/v1"
#define OLLAMA_CLOUD_DEFAULT "https://ollama.com/v1"

/* The fallback catalog is GENERATED (tools/generate-catalogs.py): the
 * cloud's whole verbose list with full coverage - the label, the
 * context window and the vision claim. It is the CLOUD's catalog: the
 * local daemon's models are the user's own (whatever it has
 * installed), and no source can know them, so the local tier starts
 * empty and the daemon answers at runtime. */

/* The API root (no /v1): the native catalog endpoints /api/tags and
 * /api/show live at the host root, not under the OpenAI /v1 prefix
 * (cloud §6). Derived from the chat base: strip the /v1 suffix. */
static void ollama_api_root(const char *chat_base, char *out, size_t cap)
{
    if (!chat_base || !*chat_base || cap == 0)
        return;

    /* Copy and strip a trailing "/v1" path segment if present. */
    size_t len = strlen(chat_base);
    if (len >= 3 && strcmp(chat_base + len - 3, "/v1") == 0)
        len -= 3;
    if (len >= cap)
        len = cap - 1;
    memcpy(out, chat_base, len);
    out[len] = '\0';
}

/* The chat half is the shared OpenAI-compatible surface
 * (provider_openaicompat.h): both tiers use the plain bearer builder
 * (a NULL key means no auth header at all, which is the local daemon's
 * case), and the base URL is the tier's own vtable default. The
 * catalog half is NOT wired there — the native /api machine below is
 * ollama's own. */
const NmOpenaiCompat nm_ollama_compat = {
    nm_openaicompat_bearer_chat_endpoint,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
};

/* The provider fixed the cloud-vs-local choice when the user named
 * it; an explicit base URL still overrides (tests, proxies). */
static const char *ollama_base(const NmProvider *p, const char *base_url,
                               const char *api_key)
{
    (void)api_key;
    if (base_url && *base_url)
        return base_url;
    if (p->id == NM_PROVIDER_OLLAMA_LOCAL)
        return OLLAMA_LOCAL_DEFAULT;
    return OLLAMA_CLOUD_DEFAULT;
}

/* ---------------------------------------------------------------- */
/* Native catalog: /api/tags + /api/show                             */
/* ---------------------------------------------------------------- */

/* Live catalog cache: provider-owned, process lifetime. One per
 * provider — cloud and local daemon are different endpoints and may
 * both be exercised in one process. Indexed by the NmProviderId's
 * ollama-ness (0 = cloud, 1 = local). */
typedef struct
{
    NmModel *models;
    size_t n;
} OllamaCatalog;

static OllamaCatalog ollama_catalogs[2];

static size_t ollama_catalog_slot(const NmProvider *p)
{
    return p->id == NM_PROVIDER_OLLAMA_LOCAL ? 1 : 0;
}

/* capabilities contains "vision"? (character scan, no regex —
 * house rule). capabilities is an array of strings; scan entries. */
static int caps_has_vision(NmJson *capabilities)
{
    size_t n = nm_json_len(capabilities);
    for (size_t i = 0; i < n; i++) {
        const char *s = nm_json_str(nm_json_at(capabilities, i));
        if (s && strcmp(s, "vision") == 0)
            return 1;
    }
    return 0;
}

/* model_info's context length key is architecture-prefixed
 * ("gptoss.context_length", "gemma4.context_length", ...) — match
 * the ".context_length" SUFFIX, never a fixed key (cloud §6.4,
 * verified on five families). model_info is an OBJECT: iterate
 * with nm_json_key + nm_json_get (nm_json_at is array-only).
 *
 * The suffix match accepts the key with an EMPTY architecture prefix
 * (".context_length", 15 bytes — nemotron-3-ultra's live /api/show
 * carries exactly that): the suffix IS the rule, and a length floor
 * that skipped the unprefixed spelling dropped that model's window
 * without a word. */
static long model_info_context(NmJson *model_info)
{
    size_t n = nm_json_len(model_info);
    for (size_t i = 0; i < n; i++) {
        const char *k = nm_json_key(model_info, i);
        size_t kl = k ? strlen(k) : 0;
        if (kl >= 15 && strcmp(k + kl - 15, ".context_length") == 0) {
            double v = nm_json_num(nm_json_get(model_info, k));
            if (v > 0)
                return (long)v;
        }
    }
    return -1;
}

/* POST {api_root}/api/show for one model's metadata (capabilities,
 * model_info.context_length). Returns 0 on success (fields written),
 * -1 on any failure — the caller keeps defaults for that model. */
static void show_apply(NmJson *doc, NmModel *out)
{
    NmJson *caps = nm_json_get(doc, "capabilities");
    if (caps)
        out->vision = caps_has_vision(caps);
    long ctx = model_info_context(nm_json_get(doc, "model_info"));
    if (ctx > 0)
        out->context_length = ctx;
}

/* The /api/show request body: {"model": "<id>"} — built with the JSON
 * writer (raw snprintf would break on ids carrying \ or "). */
static NmFetchStream *show_begin(const char *root, const char *api_key,
                                 const char *model)
{
    NmJson *body = nm_json_new_object();
    if (!body)
        return NULL;
    nm_json_set(body, "model", nm_json_new_string(model));
    char *dump = nm_json_dump(body);
    nm_json_free(body);
    if (!dump)
        return NULL;
    NmFetchStream *f = nm_fetch_begin(root, "POST", "/api/show", "Bearer %s",
                                      api_key, NULL, 0, dump);
    free(dump);
    return f;
}

/* The two-request shape (/api/tags, then one /api/show per model) is
 * why ollama does not use the one-request NmCatalogFetch helper: it
 * keeps its own stage machine over the same fetch primitive. */
typedef enum
{
    OC_IDLE = 0, /* nothing in flight (the zero value) */
    OC_VERBOSE,  /* the cloud's one-request catalog (see below) */
    OC_TAGS,     /* fetching /api/tags */
    OC_SHOW,     /* fetching /api/show for models[i] */
} OllamaStage;

typedef struct
{
    OllamaStage stage;
    NmFetchStream *f; /* the request in flight */
    char root[256];
    char base[256]; /* the CHAT base (the verbose path's own) */
    char api_key[512];
    NmModel *models; /* being built */
    size_t n, i;
    int failed;
} OllamaFetch;

static OllamaFetch ollama_fetches[2];

/* Drop a partial build (a failed or abandoned fetch). */
static void ollama_fetch_clear(OllamaFetch *o)
{
    if (o->f) {
        nm_fetch_end(o->f);
        o->f = NULL;
    }
    if (o->models) {
        for (size_t i = 0; i < o->n; i++) {
            free((void *)o->models[i].id);
            free((void *)o->models[i].label);
        }
        free(o->models);
        o->models = NULL;
    }
    o->n = 0;
    o->i = 0;
    o->stage = OC_IDLE;
}

/* /api/tags -> the id/label rows; 1 on success. */
static int ollama_tags_parse(OllamaFetch *o, const NmJson *doc)
{
    NmJson *list = nm_json_get(doc, "models");
    size_t count = nm_json_len(list);
    if (count == 0)
        return 0;
    NmModel *models = calloc(count + 1, sizeof(NmModel));
    if (!models)
        return 0;
    size_t out = 0;
    for (size_t i = 0; i < count; i++) {
        NmJson *e = nm_json_at(list, i);
        const char *name = nm_json_str(nm_json_get(e, "name"));
        if (!name)
            continue;
        /* Strings are owned by the parsed document (freed by the
         * caller), so copy into the cache. One-time per process. */
        models[out].id = strdup(name);
        models[out].label = strdup(name);
        models[out].vision = 0;
        models[out].context_length = -1;
        if (!models[out].id || !models[out].label) {
            free((void *)models[out].id);
            free((void *)models[out].label);
            continue;
        }
        out++;
    }
    if (out == 0) {
        free(models);
        return 0;
    }
    o->models = models;
    o->n = out;
    return 1;
}

/* Commit the built rows as the slot's live catalog (the parse is done
 * or has given up on the rest). */
static void ollama_commit(size_t slot, OllamaFetch *o)
{
    ollama_catalogs[slot].models = o->models;
    ollama_catalogs[slot].n = o->n;
    o->models = NULL;
    o->n = 0;
    o->stage = OC_IDLE;
}

/* ---------------------------------------------------------------- */
/* The cloud's ONE-request catalog                                    */
/* ---------------------------------------------------------------- */

/* GET {chat_base}/models?verbose=true answers the OpenAI-shaped list
 * AND a parallel "models" array (live-verified 2026-10-23): every row
 * carries slug, display_name, context_window and input_modalities —
 * the id, the label, the context window and the vision claim, which is
 * exactly what the native machine assembles from GET /api/tags plus one
 * POST /api/show per model.
 *
 * The native machine needs N+1 round trips and the cloud charges
 * 0.6-1.4 s each: 23 models took 29.9 s (measured), and the picker
 * waited for every one of them. The one-request shape answers the same
 * facts in ~1.2 s.
 *
 * The two shapes AGREE where both speak: the context_window of every
 * probed model (minimax-m2.7 196608, minimax-m3 512000, gemma4:31b
 * 262144, kimi-k3 1048576, minimax-m3.1-flash 1000000) is the
 * /api/show context_length, and an "image" in input_modalities is the
 * /api/show capabilities' "vision".
 *
 * The LOCAL daemon keeps the native machine: its /v1/models is the
 * OpenAI list without the verbose half, and its round trips are
 * milliseconds, so there is nothing to win there. */
static const char *base_path_prefix(const char *url)
{
    const char *p = strstr(url, "://");
    if (!p)
        return "";
    p = strchr(p + 3, '/');
    return p ? p : "";
}

/* The verbose request: {chat_base} + {prefix}/models?verbose=true. */
static NmFetchStream *verbose_begin(const char *base, const char *api_key)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/models?verbose=true",
             base_path_prefix(base));
    return nm_fetch_begin(base, "GET", path, "Bearer %s", api_key, NULL, 0,
                          NULL);
}

/* input_modalities contains "image"? (the verbose shape's vision
 * claim — the twin of caps_has_vision above). */
static int mods_has_image(NmJson *mods)
{
    size_t n = nm_json_len(mods);
    for (size_t i = 0; i < n; i++) {
        const char *s = nm_json_str(nm_json_at(mods, i));
        if (s && strcmp(s, "image") == 0)
            return 1;
    }
    return 0;
}

/* The verbose "models" array -> the rows; 1 on success.
 *
 * The rows are keyed by "slug" (the model's id), NOT by the position:
 * the "data" and "models" arrays of the same document are in
 * DIFFERENT orders (same set, different order — live-verified), so an
 * index-based merge would attribute one model's window to another. */
static int ollama_verbose_parse(OllamaFetch *o, const NmJson *doc)
{
    NmJson *list = nm_json_get(doc, "models");
    size_t count = nm_json_len(list);
    if (count == 0)
        return 0;
    NmModel *models = calloc(count + 1, sizeof(NmModel));
    if (!models)
        return 0;
    size_t out = 0;
    long with_ctx = 0;
    for (size_t i = 0; i < count; i++) {
        NmJson *e = nm_json_at(list, i);
        const char *slug = nm_json_str(nm_json_get(e, "slug"));
        if (!slug || !*slug)
            continue;
        /* Strings are owned by the parsed document (freed by the
         * caller), so copy into the cache. One-time per process. */
        models[out].id = strdup(slug);
        const char *name = nm_json_str(nm_json_get(e, "display_name"));
        models[out].label = strdup((name && *name) ? name : slug);
        models[out].vision = mods_has_image(nm_json_get(e, "input_modalities"));
        double ctx = nm_json_num(nm_json_get(e, "context_window"));
        models[out].context_length = ctx > 0 ? (long)ctx : -1;
        if (models[out].context_length > 0)
            with_ctx++;
        /* tools stays 0 ("the catalog says nothing"): the verbose
         * shape's experimental_supported_tools is empty for every
         * probed model, and an empty list is not a claim. The native
         * machine claims nothing either, so the two agree. */
        if (!models[out].id || !models[out].label) {
            free((void *)models[out].id);
            free((void *)models[out].label);
            continue;
        }
        out++;
    }
    if (out == 0) {
        free(models);
        return 0;
    }
    /* The shape is only USEFUL if it carries the context window: a
     * peer that answers the OpenAI list without the verbose half (the
     * local daemon) must fall through to the native machine, which
     * knows the window. */
    if (with_ctx == 0) {
        for (size_t i = 0; i < out; i++) {
            free((void *)models[i].id);
            free((void *)models[i].label);
        }
        free(models);
        return 0;
    }
    o->models = models;
    o->n = out;
    return 1;
}

/* The async seam (the /model popup's non-blocking drive). */
int nm_ollama_models_begin(const NmProvider *p, const char *base_url,
                           const char *api_key)
{
    size_t slot = ollama_catalog_slot(p);
    OllamaFetch *o = &ollama_fetches[slot];
    if (ollama_catalogs[slot].models || o->stage != OC_IDLE)
        return 0; /* cached, or a fetch is in flight */
    if (!base_url && !nm_live_catalog_enabled())
        return 0;

    memset(o, 0, sizeof(*o));
    /* The API root derives from the CHAT base (which defaults by
     * provider: the local daemon or the cloud). */
    snprintf(o->base, sizeof(o->base), "%s",
             ollama_base(p, base_url, api_key));
    ollama_api_root(o->base, o->root, sizeof(o->root));
    snprintf(o->api_key, sizeof(o->api_key), "%s", api_key ? api_key : "");
    /* The ONE-request verbose shape comes first; a peer that does not
     * answer it falls through to the native machine (see
     * nm_ollama_models_step). One machine for both tiers: the local
     * daemon's /v1/models is the plain OpenAI list today, so it pays
     * one extra (localhost, millisecond) round and then takes the
     * native path it always took. */
    o->stage = OC_VERBOSE;
    o->f = verbose_begin(o->base, api_key);
    if (o->f)
        return 1;
    o->stage = OC_IDLE; /* the verbose request could not start */
    o->stage = OC_TAGS;
    o->f = nm_fetch_begin(o->root, "GET", "/api/tags", "Bearer %s", api_key,
                          NULL, 0, NULL);
    if (!o->f) {
        o->stage = OC_IDLE;
        o->failed = 1;
        return 0;
    }
    return 1;
}

NmCatalogStatus nm_ollama_models_step(const NmProvider *p)
{
    size_t slot = ollama_catalog_slot(p);
    OllamaFetch *o = &ollama_fetches[slot];
    for (;;) {
        if (o->stage == OC_IDLE)
            return o->failed ? NM_CATALOG_ERR : NM_CATALOG_OK;
        if (!o->f)
            return NM_CATALOG_ERR;

        NmCatalogStatus s = nm_fetch_step(o->f);
        if (s == NM_CATALOG_PENDING)
            return NM_CATALOG_PENDING;

        NmJson *doc = nm_fetch_take(o->f);
        nm_fetch_end(o->f);
        o->f = NULL;

        if (o->stage == OC_VERBOSE) {
            /* The one-request shape: the whole catalog in one round
             * trip. A peer that does not speak it (no "models" array,
             * or one without a context window) falls through to the
             * native machine, which is the documented path. */
            int ok = doc ? ollama_verbose_parse(o, doc) : 0;
            nm_json_free(doc);
            if (ok) {
                ollama_commit(slot, o);
                return NM_CATALOG_OK;
            }
            o->stage = OC_TAGS;
            o->f = nm_fetch_begin(o->root, "GET", "/api/tags", "Bearer %s",
                                  o->api_key, NULL, 0, NULL);
            if (!o->f) {
                ollama_fetch_clear(o);
                o->failed = 1;
                return NM_CATALOG_ERR;
            }
            continue;
        }

        if (o->stage == OC_TAGS) {
            int ok = doc ? ollama_tags_parse(o, doc) : 0;
            nm_json_free(doc);
            if (!ok) {
                ollama_fetch_clear(o);
                o->failed = 1;
                return NM_CATALOG_ERR;
            }
            o->stage = OC_SHOW;
            o->i = 0;
        } else { /* OC_SHOW: one model's metadata (best-effort) */
            if (doc && o->i < o->n)
                show_apply(doc, &o->models[o->i]);
            nm_json_free(doc); /* a failure keeps the defaults */
            o->i++;
            if (o->i >= o->n) {
                ollama_commit(slot, o);
                return NM_CATALOG_OK;
            }
        }

        /* Start the next request (tags -> first show, or show i). */
        o->f = show_begin(o->root, o->api_key, o->models[o->i].id);
        if (!o->f) {
            /* Could not queue a show: commit what we have (defaults for
             * the rest) rather than throwing the tags away. */
            ollama_commit(slot, o);
            return NM_CATALOG_OK;
        }
    }
}

NmSource nm_ollama_models_source(const NmProvider *p)
{
    OllamaFetch *o = &ollama_fetches[ollama_catalog_slot(p)];
    NmSource none = { -1, 0, NM_SRC_FD };
    return o->f ? nm_fetch_source(o->f) : none;
}

void nm_ollama_models_end(const NmProvider *p)
{
    ollama_fetch_clear(&ollama_fetches[ollama_catalog_slot(p)]);
}

const NmModel *nm_ollama_models_cached(const NmProvider *p, size_t *n_out)
{
    OllamaCatalog *c = &ollama_catalogs[ollama_catalog_slot(p)];
    if (c->models) {
        if (n_out)
            *n_out = c->n;
        return c->models;
    }
    /* Not resolved yet (or the peer is down): the generated fallback.
     * The CLOUD has one; the LOCAL tier has none - its models are the
     * user's own, and no source can know them, so the daemon answers
     * at runtime and an empty list is the honest floor. */
    const NmModel *fallback = (p->id == NM_PROVIDER_OLLAMA_LOCAL)
                                  ? NULL
                                  : nm_catalog_ollama;
    if (n_out) {
        size_t n = 0;
        if (fallback)
            while (fallback[n].id)
                n++;
        *n_out = n;
    }
    return fallback;
}

const NmModel *nm_ollama_models(const NmProvider *p, const char *base_url,
                                const char *api_key, size_t *n_out)
{
    /* The BLOCKING drive of the async seam (one implementation, two
     * drives). */
    if (nm_ollama_models_begin(p, base_url, api_key))
        nm_catalog_run(p);
    return nm_ollama_models_cached(p, n_out);
}

int nm_ollama_needs_auth(const NmProvider *p, const char *base_url)
{
    /* Local daemon needs no auth; anything else (cloud) does. */
    if (base_url && *base_url && strncmp(base_url, "http://localhost", 16) == 0)
        return 0;
    if (base_url && *base_url && strncmp(base_url, "http://127.", 11) == 0)
        return 0;
    if (p->id == NM_PROVIDER_OLLAMA_LOCAL)
        return 0; /* the daemon is keyless by construction */
    return 1;
}

static const char *ollama_env_key(const NmProvider *p)
{
    (void)p;
    return "OLLAMA_API_KEY";
}

const struct NmProvider nm_ollama_provider = {
    NM_PROVIDER_OLLAMA,
    "ollama:cloud",
    OLLAMA_CLOUD_DEFAULT,
    "ollama.com",
    nm_openaicompat_chat,
    nm_openaicompat_chat_begin,
    nm_openai_chat_step,
    nm_openai_stream_fd,
    nm_openai_stream_interest,
    nm_openai_stream_wait_ms,
    nm_openai_stream_finishing,
    nm_openai_chat_end,
    nm_ollama_models,
    nm_ollama_models_cached,
    nm_ollama_models_begin,
    nm_ollama_models_step,
    nm_ollama_models_source,
    nm_ollama_models_end,
    nm_ollama_needs_auth,
    ollama_env_key,
    NULL, /* knobs: no pricing/routing vocabulary */
    NULL, /* request_model: the id rides verbatim */
    NULL, /* catalog_model: exact match */
};
