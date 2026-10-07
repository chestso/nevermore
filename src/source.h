/* source.h - listing plane: uniform pickable entries behind sources
 *
 * The picker design (TODO.md): "provider" at scale means a catalog
 * entry, not a hand-written vtable. The routing plane (struct
 * NmProvider) stays as-is — 4 compiled-in transports routed by
 * name. The listing plane is this file: everything pickable
 * (providers, models, later tools/servers/sessions) comes from a
 * SOURCE returning a uniform record. The picker never knows which
 * source it drives.
 *
 * Sources are synchronous today (static + registry); phase 5's wire
 * source (ollama /api/tags, openrouter /models) rides the same seam
 * with fetch_begin/step/fd so the popup can poll it without
 * blocking the UI path (event-driven principle).
 *
 * Memory (reuse principle): entries live in the source, rebuilt in
 * place per fetch; items() hands out borrowed pointers valid until
 * the next call into the source. No per-keystroke allocation.
 */

#ifndef NM_SOURCE_H
#define NM_SOURCE_H

/* This module's NmListSource is the LISTING-plane type: a pickable
 * source of NmEntry rows. It is deliberately not named NmSource —
 * transport.h owns that name for a waitable I/O source, and the two
 * meet in chat_app.c (the catalog fetch is a listing source driven by
 * the event loop). */

#include <stddef.h>

#include "transport.h" /* NmSource: what the event loop waits on */

#ifdef __cplusplus
extern "C" {
#endif

/* One shape for everything pickable. Generalizes NmModel; models are
 * a view of it via the catalog source. */
typedef struct NmEntry
{
    const char *id;          /* wire name: "qwen3-coder:latest", "openai" */
    const char *label;       /* display: "Qwen3 Coder" */
    const char *const *tags; /* "vision", "free", "cloud", "tools"… */
    size_t n_tags;
    long context_length; /* -1 unknown */
    /* The catalog's capability CLAIMS, copied from NmModel so the model
     * picker's metadata column and its @-filters read one record.
     * Positive claims only (0 = the catalog says nothing, never "known
     * absent"); tools is tri-state like NmModel.tools (1 claimed,
     * 0 says nothing, -1 listed without tools). */
    int vision;
    int image_gen;
    int tools;
} NmEntry;

typedef enum
{
    NM_FETCH_PENDING = 0, /* more to read; poll nm_source_fd */
    NM_FETCH_OK,          /* complete; items() is the answer */
    NM_FETCH_ERR          /* fetch failed; items() is empty/last-good */
} NmFetchStatus;

typedef struct NmListSource NmListSource;

struct NmListSource
{
    /* Begin a fetch. query_hint may be NULL. Returns 0 on success
     * (sync sources: the answer is immediately ready, step()
     * returns OK without ever pending), -1 on immediate failure. */
    int (*fetch_begin)(NmListSource *s, const char *query_hint);

    /* Drive a fetch to completion. PENDING means more to read (wire
     * sources); OK/ERR are terminal. */
    NmFetchStatus (*step)(NmListSource *s);

    /* What to wait on while PENDING: handle + interest + kind, or a
     * {-1} source when nothing (sync sources always). The listing
     * plane speaks the transport's NmSource so the event loop waits on
     * it directly. */
    NmSource (*fd)(const NmListSource *s);

    /* Abandon an in-flight fetch (modal popups cancel the old fetch
     * before opening a new one). Safe when idle. */
    void (*cancel)(NmListSource *s);

    /* Borrowed view, valid until the next call into the source.
     * Rebuilt in place — no per-refresh churn. NULL before the first
     * fetch or on immediate failure. */
    const NmEntry *(*items)(const NmListSource *s, size_t *n);

    /* Destroy the source and everything it owns. NULL-safe via
     * nm_source_free. */
    void (*free)(NmListSource *s);
};

/* ---- source constructors ---- */

/* The registry source: every registered provider as an entry — so
 * /providers and /provider read the same truth as the router. */
NmListSource *nm_source_registry_create(void);

/* The catalog source: one provider's model catalog (static today;
 * the phase-5 wire source rides the same NmListSource seam under it).
 * base_url/api_key select the endpoint (as the provider's models()
 * takes them); either may be NULL. */
NmListSource *nm_source_catalog_create(const NmProvider *provider,
                                       const char *base_url,
                                       const char *api_key);

/* Generic vtable driver (NULL-safe). */
int nm_source_fetch_begin(NmListSource *s, const char *query_hint);
NmFetchStatus nm_source_step(NmListSource *s);
NmSource nm_source_fd(const NmListSource *s);
void nm_source_cancel(NmListSource *s);
const NmEntry *nm_source_items(const NmListSource *s, size_t *n);
void nm_source_free(NmListSource *s);

#ifdef __cplusplus
}
#endif

#endif /* NM_SOURCE_H */
