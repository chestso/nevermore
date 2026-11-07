/* credit.h - credit brackets and provider balance meters
 *
 * Wire truth for each provider lives in docs/.
 *
 * One store per provider id, in memory only (NmCredits array indexed by
 * NmProviderId): no balance survives a restart.
 */

#ifndef NM_CREDIT_H
#define NM_CREDIT_H

#include "colors.h"
#include "json.h"
#include "provider.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum
{                     /* what the number is denominated in */
  NM_CREDIT_USD,      /* dollars */
  NM_CREDIT_ZEN,      /* Zen credits (no readable purse) */
  NM_CREDIT_REQUESTS, /* request counts in a window */
  NM_CREDIT_PERCENT   /* a percent of an unpublished cap */
} NmCreditUnit;

typedef enum
{ /* what the window is; PURSE never resets */
  NM_CREDIT_PURSE,
  NM_CREDIT_ROLLING_5H, /* Go: starts at first use */
  NM_CREDIT_HOUR,
  NM_CREDIT_DAY,
  NM_CREDIT_WEEK,
  NM_CREDIT_PERIOD /* a paid/included period; formats as "mo" */
} NmCreditWindow;

typedef enum
{
    NM_CREDIT_LEFT,
    NM_CREDIT_SPENT
} NmCreditKind;

typedef enum
{                              /* where the fact came from */
  NM_CREDIT_FROM_ROUND_BODY,   /* usage.cost / usage.remaining / usage.opencode */
  NM_CREDIT_FROM_ROUND_HEADER, /* x-ratelimit-* */
  NM_CREDIT_FROM_ENDPOINT,     /* a dedicated GET */
  NM_CREDIT_FROM_REFUSAL       /* a 402 "insufficient funds" */
} NmCreditOrigin;

typedef enum
{
    NM_CREDIT_UNKNOWN = 0, /* never reported: render nothing here */
    NM_CREDIT_KNOWN,       /* a reported figure */
    NM_CREDIT_EXHAUSTED    /* a refusal proved "cannot pay", no number */
} NmCreditState;

typedef struct NmCreditBracket
{
    NmCreditUnit unit;
    NmCreditWindow window;
    NmCreditKind kind; /* LEFT = remaining, SPENT = burned */
    NmCreditState state;
    double value;        /* LEFT: remaining; SPENT: used; -1 unknown */
    double limit;        /* -1 = no published cap; 100 for capped percent */
    double percent_used; /* -1 unknown; set when wire reports used % */
    long resets_at;      /* epoch seconds (UTC); 0 = not reported */
    NmCreditOrigin origin;
    long observed_at;     /* monotonic seconds; 0 = never */
    char label[96];       /* description for /credits breakdown */
    char origin_desc[48]; /* wire source string e.g. "GET /api/v1/credits" */
} NmCreditBracket;

#define NM_CREDIT_BRACKETS_MAX 8

typedef struct
{
    NmCreditBracket brackets[NM_CREDIT_BRACKETS_MAX];
    size_t n_brackets;
    /* TWO clocks, never one. observed_at is the NEWEST fact of any
     * origin (an in-band round fold counts), and it is what `/credits`
     * reports as the age.
     * fetched_at is the last OUT-OF-BAND endpoint fetch's landing
     * (0 = never), and it is the ONLY field the background refresh TTL
     * may gate on: a provider whose rounds fold nothing (openrouter,
     * ollama:cloud, opencode:go) would otherwise keep the bar frozen at
     * the startup warm forever, because every 200 round restamps
     * observed_at while folding no bracket. */
    long observed_at;             /* monotonic seconds of latest update */
    long fetched_at;              /* monotonic seconds of latest GET; 0 none */
    double next_fetch_allowed_at; /* monotonic deadline; Retry-After / floor */
    int fetch_in_flight;
} NmCredits;

/* ------------------------------------------------------------------ */
/* Date / time & formatting helpers                                   */
/* ------------------------------------------------------------------ */

/* Parse an ISO-8601 UTC timestamp ("YYYY-MM-DDTHH:MM:SSZ" or with
 * fractional seconds) into epoch seconds (UTC). Returns 0 on error. */
long nm_credit_parse_iso8601(const char *iso);

/* Format monotonic observation age (e.g. "as of 2m ago", "as of 5s ago",
 * "as of 1h ago", or "unknown" if observed_at == 0). */
void nm_credit_format_age(long observed_at, char *out, size_t cap);

/* Format the reset countdown for an epoch instant: "in 4h 51m",
 * "in 12d 3h", "now" (at or past). Writes "" and returns 0 when `at` is
 * 0 (no reset reported). PURE in (now, at) — the wall-clock epoch is the
 * caller's, so a test owns it and the ladder supplies time(NULL). */
int nm_credit_format_until(long now, long at, char *out, size_t cap);

/* Format a single credit bracket cell according to the prose grammar:
 *   cell       ::= ["-"] <amount> [ "·" <window-tag> ] | <exhausted>
 *   amount     ::= "$" <money> [ "/" "$" <money> ]
 *                | <number> [ "/" <number> ] <unit-tag>
 *                | <count> [ "/" <count> ]
 *   exhausted  ::= ( <window-tag> | <unit-tag> ) "!"
 * When `compress` is nonzero and `value == limit`, compresses to one number
 * (e.g. "100k·d" instead of "100k/100k·d"). Both halves of a pair take the
 * SAME formatter — `compress` collapses a full pair, it never abbreviates
 * one number and not the other ("1k/1000" read as two units in one cell).
 * If `state == NM_CREDIT_UNKNOWN`, writes "-" if not compressed, or empty if
 * compressed. Returns string length written. */
int nm_credit_format_cell(const NmCreditBracket *b, char *out, size_t cap,
                          int compress);

/* One cell of the status row: the formatted text (with its trailing
 * space, so a dropped cell leaves no gap), the colour its OWN pressure
 * earns, and the ADMISSION RANK the row admits it by (1 first = dropped
 * last; higher ranks are dropped first when the row has no columns for
 * them). The rank is not boba's shrink priority: a cell is all-or-
 * nothing there, never half-elided. */
typedef struct
{
    char text[24];
    TuiColor color;
    int rank;
} NmCreditCell;

#define NM_CREDIT_CELLS_MAX 3

/* Rank the store's brackets for the status row.
 *
 * Selection is SEMANTIC, never positional (position used to be the only
 * rule, so opencode:go's monthly window — declared third — could never
 * win a slot):
 *   - the BUDGET is picked first: the durable number a plan decision
 *     hangs on, i.e. the longest window (a purse, a paid period) — a
 *     busy month must never demote it to "the third thing left".
 *   - the GATE is the short window that can refuse a request soonest:
 *     an EXHAUSTED one, else the LEFT bracket with the smallest
 *     remaining fraction; ties break toward the earlier bracket, which
 *     is the shorter window in every provider's ladder.
 *   - the MIDDLE cell is whatever known bracket remains.
 * Cells are WRITTEN in declaration order (the provider's own ladder),
 * so the row's positions are learnable and never reorder — an exhausted
 * window keeps its slot and turns red instead of jumping to cell 1.
 * `rank` is what makes the row narrow gracefully: the budget is rank 1
 * and the middle window the highest, so the longest window is the last
 * datum to leave the row.
 * Returns the number of cells written (0 = nothing to show). */
size_t nm_credit_status_cells(const NmCredits *cr, NmCreditCell *cells,
                              size_t max_cells, NmColorDepth depth);

/* Format the full `/credits` breakdown text for `provider_name` into `out`.
 * Returns length written. */
int nm_credit_format_ladder(const NmCredits *cr, const char *provider_name,
                            char *out, size_t cap);

/* ------------------------------------------------------------------ */
/* Store access & in-band folding                                     */
/* ------------------------------------------------------------------ */

/* Return the in-memory credits store for `id`. Guaranteed non-NULL for any
 * valid provider id. */
NmCredits *nm_credits_get(NmProviderId id);

/* Reset all provider credit stores to unobserved/initial state (tests). */
void nm_credits_reset(void);

/* Initialize a provider's initial brackets from its vtable. */
void nm_credits_init_provider(NmProviderId id);

/* Fold in-band round facts (usage members, rate-window headers, 402 refusal)
 * into `id`'s credit store without network traffic. */
void nm_credit_fold_round(NmProviderId id, const NmUsage *usage,
                          const NmRoundMeta *meta, int http_status,
                          const char *msg);

/* Mark provider's purse exhausted from a 402 refusal. */
void nm_credit_fold_refusal(NmProviderId id, const char *msg);

/* ------------------------------------------------------------------ */
/* Out-of-band fetch driver                                           */
/* ------------------------------------------------------------------ */

/* Begin an out-of-band balance fetch for `id`.
 * Returns 1 if a fetch started, 0 if blocked by rate latch, no endpoint,
 * already in flight, or allocation error. */
int nm_credits_fetch_begin(NmProviderId id, const char *base_url,
                           const char *api_key);

/* Non-blocking pump for the in-flight fetch. */
NmCatalogStatus nm_credits_fetch_step(NmProviderId id);

/* What socket/interest to wait on for `id`'s in-flight fetch. */
NmSource nm_credits_fetch_source(NmProviderId id);

/* The in-flight fetch's deadline seam: ms until it wants a step with no
 * fd ready, -1 when purely readiness-driven or nothing is in flight. */
int nm_credits_fetch_wait_ms(NmProviderId id);

/* Cancel/finish any in-flight fetch for `id`. */
void nm_credits_fetch_end(NmProviderId id);

/* Whether an out-of-band fetch is currently in flight for `id`. */
int nm_credits_fetch_in_flight(NmProviderId id);

/* Synchronous blocking refresh of `id`'s credits (pumps the async seam).
 * Returns 1 on success, 0 on failure or rate-limit latch. */
int nm_credits_refresh_sync(NmProviderId id, const char *base_url,
                            const char *api_key);

#ifdef __cplusplus
}
#endif

#endif /* NM_CREDIT_H */
