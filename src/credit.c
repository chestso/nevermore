/* credit.c - credit brackets, ladder formatters and out-of-band balance fetch
 *
 * Wire mappings, credit brackets and formatting grammar.
 */

#include "credit.h"
#include "clock.h"
#include "openai_client.h"
#include "provider_internal.h"
#include "transport.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <winsock2.h>
#define nm_usleep(us) Sleep((DWORD)((us) / 1000))
#else
#include <sys/select.h>
#include <sys/time.h>
#include <unistd.h>
#define nm_usleep(us) usleep(us)
#endif

/* ------------------------------------------------------------------ */
/* In-memory store                                                    */
/* ------------------------------------------------------------------ */

static NmCredits g_credits[NM_PROVIDER_COUNT];
static int g_credits_initialized = 0;

/* ------------------------------------------------------------------ */
/* ISO-8601 UTC parser                                                */
/* ------------------------------------------------------------------ */

static int is_leap_year(int y)
{
    return (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0));
}

long nm_credit_parse_iso8601(const char *iso)
{
    if (!iso || !*iso)
        return 0;
    int y = 0, m = 0, d = 0, h = 0, min = 0, sec = 0;
    if (sscanf(iso, "%d-%d-%dT%d:%d:%d", &y, &m, &d, &h, &min, &sec) < 6)
        return 0;
    if (y < 1970 || m < 1 || m > 12 || d < 1 || d > 31 || h < 0 || h > 23 ||
        min < 0 || min > 59 || sec < 0 || sec > 60)
        return 0;

    static const int days_before_month[12] = {
        0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334
    };
    long days = (y - 1970) * 365;
    /* Count leap days between 1970 and y - 1 */
    int y_prev = y - 1;
    long leaps = (y_prev / 4 - y_prev / 100 + y_prev / 400) -
                 (1969 / 4 - 1969 / 100 + 1969 / 400);
    days += leaps;
    days += days_before_month[m - 1];
    if (is_leap_year(y) && m > 2)
        days += 1;
    days += (d - 1);

    return days * 86400L + (long)h * 3600L + (long)min * 60L + (long)sec;
}

void nm_credit_format_age(long observed_at, char *out, size_t cap)
{
    if (!out || cap == 0)
        return;
    if (observed_at <= 0) {
        snprintf(out, cap, "unknown");
        return;
    }
    long now = (long)nm_monotonic_seconds();
    long age = now - observed_at;
    if (age < 0)
        age = 0;
    if (age < 60)
        snprintf(out, cap, "as of %lds ago", age);
    else if (age < 3600)
        snprintf(out, cap, "as of %ldm ago", age / 60);
    else
        snprintf(out, cap, "as of %ldh ago", age / 3600);
}

/* ------------------------------------------------------------------ */
/* Formatting helpers                                                 */
/* ------------------------------------------------------------------ */

/* Compact token count (mirrors chat_app.c's format_tokens). */
static void format_compact_count(long n, char *dst, size_t cap)
{
    if (n < 0) {
        snprintf(dst, cap, "-");
        return;
    }
    if (n < 1000) {
        snprintf(dst, cap, "%ld", n);
        return;
    }
    long div = n < 1000000 ? 100 : 100000;
    const char *unit = n < 1000000 ? "k" : "M";
    long tenths = n / div;
    long whole = tenths / 10;
    long frac = tenths % 10;
    if (frac == 0)
        snprintf(dst, cap, "%ld%s", whole, unit);
    else
        snprintf(dst, cap, "%ld.%ld%s", whole, frac, unit);
}

/* Format USD money: integer if whole (e.g. 60), 2 decimals otherwise. */
static void format_money_value(double val, char *dst, size_t cap)
{
    if ((double)(long long)val == val)
        snprintf(dst, cap, "%.0f", val);
    else
        snprintf(dst, cap, "%.2f", val);
}

/* Format credits (hc, cr): integer if whole, up to 2 decimals otherwise. */
static void format_credits_value(double val, char *dst, size_t cap)
{
    if ((double)(long long)val == val) {
        snprintf(dst, cap, "%.0f", val);
    } else {
        char tmp[32];
        snprintf(tmp, sizeof(tmp), "%.2f", val);
        /* Trim trailing zero if e.g. 10.50 -> 10.5 */
        size_t len = strlen(tmp);
        if (len > 2 && tmp[len - 1] == '0' && tmp[len - 2] != '.')
            tmp[len - 1] = '\0';
        snprintf(dst, cap, "%s", tmp);
    }
}

static const char *window_tag(NmCreditWindow w)
{
    switch (w) {
    case NM_CREDIT_ROLLING_5H:
        return "5h";
    case NM_CREDIT_HOUR:
        return "h";
    case NM_CREDIT_DAY:
        return "d";
    case NM_CREDIT_WEEK:
        return "w";
    case NM_CREDIT_PERIOD:
        return "mo";
    case NM_CREDIT_PURSE:
    default:
        return "";
    }
}

int nm_credit_format_cell(const NmCreditBracket *b, char *out, size_t cap,
                          int compress)
{
    if (!b || !out || cap == 0)
        return 0;

    out[0] = '\0';

    if (b->state == NM_CREDIT_UNKNOWN) {
        if (!compress) {
            snprintf(out, cap, "-");
            return 1;
        }
        return 0;
    }

    if (b->state == NM_CREDIT_EXHAUSTED) {
        switch (b->unit) {
        case NM_CREDIT_USD:
            snprintf(out, cap, "$!");
            break;
        case NM_CREDIT_HYPER:
            snprintf(out, cap, "hc!");
            break;
        case NM_CREDIT_ZEN:
            snprintf(out, cap, "cr!");
            break;
        case NM_CREDIT_PERCENT:
            snprintf(out, cap, "%%!");
            break;
        case NM_CREDIT_REQUESTS:
        default:
            snprintf(out, cap, "!");
            break;
        }
        return (int)strlen(out);
    }

    char amount[96];
    amount[0] = '\0';

    /* Full bracket compression: when value == limit, print single number. */
    int is_full = (b->limit > 0 && b->value == b->limit);

    switch (b->unit) {
    case NM_CREDIT_USD:
    {
        char val_buf[32];
        format_money_value(b->value, val_buf, sizeof(val_buf));
        if (b->limit > 0 && (!compress || !is_full)) {
            char lim_buf[32];
            format_money_value(b->limit, lim_buf, sizeof(lim_buf));
            snprintf(amount, sizeof(amount), "$%s/$%s", val_buf, lim_buf);
        } else {
            snprintf(amount, sizeof(amount), "$%s", val_buf);
        }
        break;
    }
    case NM_CREDIT_HYPER:
    {
        char val_buf[32];
        format_credits_value(b->value, val_buf, sizeof(val_buf));
        if (b->limit > 0 && (!compress || !is_full)) {
            char lim_buf[32];
            format_credits_value(b->limit, lim_buf, sizeof(lim_buf));
            snprintf(amount, sizeof(amount), "%s/%shc", val_buf, lim_buf);
        } else {
            snprintf(amount, sizeof(amount), "%shc", val_buf);
        }
        break;
    }
    case NM_CREDIT_ZEN:
    {
        char val_buf[32];
        format_credits_value(b->value, val_buf, sizeof(val_buf));
        if (b->limit > 0 && (!compress || !is_full)) {
            char lim_buf[32];
            format_credits_value(b->limit, lim_buf, sizeof(lim_buf));
            snprintf(amount, sizeof(amount), "%s/%scr", val_buf, lim_buf);
        } else {
            snprintf(amount, sizeof(amount), "%scr", val_buf);
        }
        break;
    }
    case NM_CREDIT_REQUESTS:
    {
        char val_buf[32];
        format_compact_count((long)b->value, val_buf, sizeof(val_buf));
        if (b->limit > 0 && (!compress || !is_full)) {
            char lim_buf[32];
            if (compress)
                format_compact_count((long)b->limit, lim_buf, sizeof(lim_buf));
            else
                snprintf(lim_buf, sizeof(lim_buf), "%ld", (long)b->limit);
            snprintf(amount, sizeof(amount), "%s/%s", val_buf, lim_buf);
        } else {
            snprintf(amount, sizeof(amount), "%s", val_buf);
        }
        break;
    }
    case NM_CREDIT_PERCENT:
    {
        /* Percent is a ratio out of 100; prints as integer percent. */
        snprintf(amount, sizeof(amount), "%.0f%%", b->value);
        break;
    }
    }

    const char *wtag = window_tag(b->window);
    const char *spent_prefix = (b->kind == NM_CREDIT_SPENT) ? "-" : "";

    if (*wtag) {
        snprintf(out, cap, "%s%s\xc2\xb7%s", spent_prefix, amount, wtag);
    } else {
        snprintf(out, cap, "%s%s", spent_prefix, amount);
    }

    return (int)strlen(out);
}

int nm_credit_format_status(const NmCredits *cr, char *out, size_t cap,
                            TuiColor *color, NmColorDepth depth)
{
    if (!cr || !out || cap == 0)
        return 0;

    out[0] = '\0';

    /* Find up to 2 cells according to the 2-cell rule:
     * First cell: exhausted bracket if any, else first non-unknown bracket.
     * Second cell: first non-unknown bracket after cell 1. */
    const NmCreditBracket *b1 = NULL;
    const NmCreditBracket *b2 = NULL;
    size_t b1_idx = 0;

    /* Check for exhausted bracket first */
    for (size_t i = 0; i < cr->n_brackets; i++) {
        if (cr->brackets[i].state == NM_CREDIT_EXHAUSTED) {
            b1 = &cr->brackets[i];
            b1_idx = i;
            break;
        }
    }

    if (!b1) {
        for (size_t i = 0; i < cr->n_brackets; i++) {
            if (cr->brackets[i].state != NM_CREDIT_UNKNOWN) {
                b1 = &cr->brackets[i];
                b1_idx = i;
                break;
            }
        }
    }

    if (!b1)
        return 0;

    /* Find second non-unknown bracket */
    for (size_t i = 0; i < cr->n_brackets; i++) {
        if (i == b1_idx)
            continue;
        if (cr->brackets[i].state != NM_CREDIT_UNKNOWN) {
            b2 = &cr->brackets[i];
            break;
        }
    }

    char c1[64], c2[64];
    nm_credit_format_cell(b1, c1, sizeof(c1), 1);
    c2[0] = '\0';
    if (b2)
        nm_credit_format_cell(b2, c2, sizeof(c2), 1);

    if (c2[0])
        snprintf(out, cap, "%s %s ", c1, c2);
    else if (c1[0])
        snprintf(out, cap, "%s ", c1);
    else
        return 0;

    /* Determine color role:
     * - Any cell EXHAUSTED -> red (nm_color_gauge_warn_hot)
     * - Any cell with limit > 0 and (value/limit) < 0.05 -> red
     * - Any cell with limit > 0 and (value/limit) < 0.15 -> orange
     * - All shown cells SPENT -> Comment (nm_color_gauge)
     * - Otherwise -> base role (nm_color_credit) */
    if (color) {
        int is_exhausted = (b1->state == NM_CREDIT_EXHAUSTED) ||
                           (b2 && b2->state == NM_CREDIT_EXHAUSTED);
        int is_red = is_exhausted;
        int is_orange = 0;

        if (!is_red) {
            if (b1->limit > 0 && (b1->value / b1->limit) < 0.05)
                is_red = 1;
            else if (b2 && b2->limit > 0 && (b2->value / b2->limit) < 0.05)
                is_red = 1;
        }
        if (!is_red) {
            if (b1->limit > 0 && (b1->value / b1->limit) < 0.15)
                is_orange = 1;
            else if (b2 && b2->limit > 0 && (b2->value / b2->limit) < 0.15)
                is_orange = 1;
        }

        if (is_red) {
            *color = nm_color_gauge_warn_hot(depth);
        } else if (is_orange) {
            *color = nm_color_gauge_warn(depth);
        } else if (b1->kind == NM_CREDIT_SPENT &&
                   (!b2 || b2->kind == NM_CREDIT_SPENT)) {
            *color = nm_color_gauge(depth);
        } else {
            *color = nm_color_credit(depth);
        }
    }

    return (int)strlen(out);
}

int nm_credit_format_ladder(const NmCredits *cr, const char *provider_name,
                            char *out, size_t cap)
{
    if (!provider_name || !out || cap == 0)
        return 0;

    out[0] = '\0';
    size_t off = 0;

    if (strcmp(provider_name, "ollama:local") == 0) {
        off += (size_t)snprintf(out + off, cap > off ? cap - off : 0,
                                "credits: %s\n  (unmetered)\n", provider_name);
        return (int)off;
    }

    if (strcmp(provider_name, "opencode:zen") == 0) {
        int exhausted = (cr && cr->n_brackets > 0 &&
                         cr->brackets[0].state == NM_CREDIT_EXHAUSTED);
        off += (size_t)snprintf(
            out + off, cap > off ? cap - off : 0,
            "credits: %s\n"
            "  -  purse: no balance API (open upstream request; see docs/OPENCODE-API.md §7.3)\n"
            "  %s  exhausted: %s\n",
            provider_name, exhausted ? "cr!" : "-",
            exhausted ? "yes (refused: Insufficient account funds)"
                      : "no (last round was not refused)");
        return (int)off;
    }

    char age[32];
    nm_credit_format_age(cr ? cr->observed_at : 0, age, sizeof(age));

    off += (size_t)snprintf(out + off, cap > off ? cap - off : 0,
                            "credits: %s (%s)\n", provider_name, age);

    if (!cr || cr->n_brackets == 0)
        return (int)off;

    for (size_t i = 0; i < cr->n_brackets; i++) {
        const NmCreditBracket *b = &cr->brackets[i];
        char cell_buf[32];
        nm_credit_format_cell(b, cell_buf, sizeof(cell_buf), 0);

        const char *desc = b->label[0] ? b->label : "";
        const char *orig = b->origin_desc[0] ? b->origin_desc : "";

        if (*orig) {
            off += (size_t)snprintf(out + off, cap > off ? cap - off : 0,
                                    "  %-16s %-46s %s\n", cell_buf, desc, orig);
        } else {
            off += (size_t)snprintf(out + off, cap > off ? cap - off : 0,
                                    "  %-16s %s\n", cell_buf, desc);
        }
    }

    return (int)off;
}

/* ------------------------------------------------------------------ */
/* Store access & in-band folding                                     */
/* ------------------------------------------------------------------ */

NmCredits *nm_credits_get(NmProviderId id)
{
    if (!g_credits_initialized) {
        nm_credits_reset();
    }
    if ((int)id < 0 || id >= NM_PROVIDER_COUNT)
        id = NM_PROVIDER_HYPER;
    return &g_credits[id];
}

void nm_credits_reset(void)
{
    memset(g_credits, 0, sizeof(g_credits));
    for (int i = 0; i < NM_PROVIDER_COUNT; i++) {
        nm_credits_init_provider((NmProviderId)i);
    }
    g_credits_initialized = 1;
}

void nm_credits_init_provider(NmProviderId id)
{
    if ((int)id < 0 || id >= NM_PROVIDER_COUNT)
        return;

    NmCredits *cr = &g_credits[id];
    memset(cr, 0, sizeof(*cr));

    const NmProvider *p = nm_provider_get(id);
    if (p && p->credits && p->credits->init_brackets) {
        p->credits->init_brackets(cr->brackets, NM_CREDIT_BRACKETS_MAX,
                                  &cr->n_brackets);
    }
}

void nm_credit_fold_round(NmProviderId id, const NmUsage *usage,
                          const NmRoundMeta *meta, int http_status,
                          const char *msg)
{
    if ((int)id < 0 || id >= NM_PROVIDER_COUNT)
        return;

    NmCredits *cr = nm_credits_get(id);
    long now = (long)nm_monotonic_seconds();

    if (http_status == 402) {
        nm_credit_fold_refusal(id, msg);
        /* Hyper may still report rate-limit headers on 402 */
        if (id == NM_PROVIDER_HYPER && meta && cr->n_brackets >= 3) {
            if (meta->req_remaining_day >= 0) {
                cr->brackets[1].state = NM_CREDIT_KNOWN;
                cr->brackets[1].value = (double)meta->req_remaining_day;
                cr->brackets[1].limit = (double)meta->req_limit_day;
                cr->brackets[1].origin = NM_CREDIT_FROM_ROUND_HEADER;
                cr->brackets[1].observed_at = now;
            }
            if (meta->req_remaining_hour >= 0) {
                cr->brackets[2].state = NM_CREDIT_KNOWN;
                cr->brackets[2].value = (double)meta->req_remaining_hour;
                cr->brackets[2].limit = (double)meta->req_limit_hour;
                cr->brackets[2].origin = NM_CREDIT_FROM_ROUND_HEADER;
                cr->brackets[2].observed_at = now;
            }
        }
        return;
    }

    if (http_status != 200)
        return;

    cr->observed_at = now;

    if (id == NM_PROVIDER_HYPER && cr->n_brackets >= 3) {
        if (usage && usage->remaining_credits >= 0) {
            cr->brackets[0].state = NM_CREDIT_KNOWN;
            cr->brackets[0].value = usage->remaining_credits;
            cr->brackets[0].limit = -1;
            cr->brackets[0].origin = NM_CREDIT_FROM_ROUND_BODY;
            cr->brackets[0].observed_at = now;
        }
        if (meta) {
            if (meta->req_remaining_day >= 0) {
                cr->brackets[1].state = NM_CREDIT_KNOWN;
                cr->brackets[1].value = (double)meta->req_remaining_day;
                cr->brackets[1].limit = (double)meta->req_limit_day;
                cr->brackets[1].origin = NM_CREDIT_FROM_ROUND_HEADER;
                cr->brackets[1].observed_at = now;
            }
            if (meta->req_remaining_hour >= 0) {
                cr->brackets[2].state = NM_CREDIT_KNOWN;
                cr->brackets[2].value = (double)meta->req_remaining_hour;
                cr->brackets[2].limit = (double)meta->req_limit_hour;
                cr->brackets[2].origin = NM_CREDIT_FROM_ROUND_HEADER;
                cr->brackets[2].observed_at = now;
            }
        }
    }
}

void nm_credit_fold_refusal(NmProviderId id, const char *msg)
{
    (void)msg;
    if ((int)id < 0 || id >= NM_PROVIDER_COUNT)
        return;

    NmCredits *cr = nm_credits_get(id);
    if (cr->n_brackets > 0) {
        cr->brackets[0].state = NM_CREDIT_EXHAUSTED;
        cr->brackets[0].origin = NM_CREDIT_FROM_REFUSAL;
        cr->brackets[0].observed_at = (long)nm_monotonic_seconds();
    }
    cr->observed_at = (long)nm_monotonic_seconds();
}

/* ------------------------------------------------------------------ */
/* Provider slot tables & endpoint mappers                            */
/* ------------------------------------------------------------------ */

/* 1. Hyper */
static void hyper_init_brackets(NmCreditBracket *b, size_t max_b, size_t *n_out)
{
    if (max_b < 3)
        return;
    memset(b, 0, sizeof(*b) * 3);

    b[0].unit = NM_CREDIT_HYPER;
    b[0].window = NM_CREDIT_PURSE;
    b[0].kind = NM_CREDIT_LEFT;
    b[0].state = NM_CREDIT_UNKNOWN;
    b[0].limit = -1;
    b[0].percent_used = -1;
    snprintf(b[0].label, sizeof(b[0].label), "hypercredits purse");
    snprintf(b[0].origin_desc, sizeof(b[0].origin_desc), "GET /v1/credits");

    b[1].unit = NM_CREDIT_REQUESTS;
    b[1].window = NM_CREDIT_DAY;
    b[1].kind = NM_CREDIT_LEFT;
    b[1].state = NM_CREDIT_UNKNOWN;
    b[1].limit = -1;
    b[1].percent_used = -1;
    snprintf(b[1].label, sizeof(b[1].label), "requests day limit");
    snprintf(b[1].origin_desc, sizeof(b[1].origin_desc), "x-ratelimit-*-day");

    b[2].unit = NM_CREDIT_REQUESTS;
    b[2].window = NM_CREDIT_HOUR;
    b[2].kind = NM_CREDIT_LEFT;
    b[2].state = NM_CREDIT_UNKNOWN;
    b[2].limit = -1;
    b[2].percent_used = -1;
    snprintf(b[2].label, sizeof(b[2].label), "requests hour limit");
    snprintf(b[2].origin_desc, sizeof(b[2].origin_desc), "x-ratelimit-*-hour");

    *n_out = 3;
}

static int hyper_map_endpoint(const NmJson *doc, int step,
                              NmCreditBracket *brackets, size_t max_brackets,
                              size_t *n_brackets)
{
    (void)step;
    (void)max_brackets;
    if (!doc || *n_brackets < 1)
        return 0;

    long now = (long)nm_monotonic_seconds();
    NmJson *bal = nm_json_get(doc, "balance");
    if (bal && nm_json_type(bal) == NM_JSON_NUMBER) {
        brackets[0].state = NM_CREDIT_KNOWN;
        brackets[0].value = nm_json_num(bal);
        brackets[0].limit = -1;
        brackets[0].origin = NM_CREDIT_FROM_ENDPOINT;
        brackets[0].observed_at = now;
    } else {
        /* A wire that says "no balance member" clears it */
        brackets[0].state = NM_CREDIT_UNKNOWN;
        brackets[0].value = -1;
    }
    return 1;
}

const NmProviderCredits nm_hyper_credits = {
    "/v1/credits",
    "Bearer %s",
    0,
    1,
    NULL,
    hyper_init_brackets,
    hyper_map_endpoint,
};

/* 2. Ollama Cloud */
static void ollama_init_brackets(NmCreditBracket *b, size_t max_b,
                                 size_t *n_out)
{
    if (max_b < 2)
        return;
    memset(b, 0, sizeof(*b) * 2);

    b[0].unit = NM_CREDIT_USD;
    b[0].window = NM_CREDIT_PURSE;
    b[0].kind = NM_CREDIT_LEFT;
    b[0].state = NM_CREDIT_UNKNOWN;
    b[0].limit = -1;
    b[0].percent_used = -1;
    snprintf(b[0].label, sizeof(b[0].label), "included allowance");
    snprintf(b[0].origin_desc, sizeof(b[0].origin_desc), "GET /api/balance");

    b[1].unit = NM_CREDIT_USD;
    b[1].window = NM_CREDIT_PURSE;
    b[1].kind = NM_CREDIT_LEFT;
    b[1].state = NM_CREDIT_UNKNOWN;
    b[1].limit = -1;
    b[1].percent_used = -1;
    snprintf(b[1].label, sizeof(b[1].label), "purchased balance");
    snprintf(b[1].origin_desc, sizeof(b[1].origin_desc), "GET /api/balance");

    *n_out = 2;
}

static int ollama_map_endpoint(const NmJson *doc, int step,
                               NmCreditBracket *brackets, size_t max_brackets,
                               size_t *n_brackets)
{
    (void)step;
    if (!doc || max_brackets < 2)
        return 0;

    long now = (long)nm_monotonic_seconds();
    NmJson *inc = nm_json_get(doc, "included");
    int legacy = 0;

    if (inc) {
        NmJson *bal_usd = nm_json_get(inc, "balance_usd");
        if (bal_usd && nm_json_type(bal_usd) == NM_JSON_NUMBER) {
            /* Standard plan */
            brackets[0].unit = NM_CREDIT_USD;
            brackets[0].window = NM_CREDIT_PURSE;
            brackets[0].kind = NM_CREDIT_LEFT;
            brackets[0].state = NM_CREDIT_KNOWN;
            brackets[0].value = nm_json_num(bal_usd);
            NmJson *all_usd = nm_json_get(inc, "allowance_usd");
            brackets[0].limit = (all_usd && nm_json_type(all_usd) == NM_JSON_NUMBER)
                                    ? nm_json_num(all_usd)
                                    : -1;
            brackets[0].percent_used = -1;
            NmJson *period = nm_json_get(inc, "period");
            if (period) {
                NmJson *until = nm_json_get(period, "until");
                brackets[0].resets_at =
                    until ? nm_credit_parse_iso8601(nm_json_str(until)) : 0;
            }
            brackets[0].origin = NM_CREDIT_FROM_ENDPOINT;
            brackets[0].observed_at = now;
            snprintf(brackets[0].label, sizeof(brackets[0].label),
                     "included allowance");
            snprintf(brackets[0].origin_desc, sizeof(brackets[0].origin_desc),
                     "GET /api/balance");
        } else {
            /* Check legacy session + weekly */
            NmJson *sess = nm_json_get(inc, "session");
            NmJson *wk = nm_json_get(inc, "weekly");
            if (sess && wk && max_brackets >= 3) {
                legacy = 1;
                /* Bracket 0: session */
                NmJson *rem_sess = nm_json_get(sess, "remaining_percent");
                NmJson *res_sess = nm_json_get(sess, "resets_at");
                brackets[0].unit = NM_CREDIT_PERCENT;
                brackets[0].window = NM_CREDIT_HOUR;
                brackets[0].kind = NM_CREDIT_LEFT;
                brackets[0].state = NM_CREDIT_KNOWN;
                brackets[0].value =
                    rem_sess ? nm_json_num(rem_sess) : -1;
                brackets[0].limit = 100.0;
                brackets[0].percent_used = -1;
                brackets[0].resets_at =
                    res_sess ? nm_credit_parse_iso8601(nm_json_str(res_sess)) : 0;
                brackets[0].origin = NM_CREDIT_FROM_ENDPOINT;
                brackets[0].observed_at = now;
                snprintf(brackets[0].label, sizeof(brackets[0].label),
                         "session allowance");
                snprintf(brackets[0].origin_desc, sizeof(brackets[0].origin_desc),
                         "GET /api/balance");

                /* Bracket 1: weekly */
                NmJson *rem_wk = nm_json_get(wk, "remaining_percent");
                NmJson *res_wk = nm_json_get(wk, "resets_at");
                brackets[1].unit = NM_CREDIT_PERCENT;
                brackets[1].window = NM_CREDIT_WEEK;
                brackets[1].kind = NM_CREDIT_LEFT;
                brackets[1].state = NM_CREDIT_KNOWN;
                brackets[1].value = rem_wk ? nm_json_num(rem_wk) : -1;
                brackets[1].limit = 100.0;
                brackets[1].percent_used = -1;
                brackets[1].resets_at =
                    res_wk ? nm_credit_parse_iso8601(nm_json_str(res_wk)) : 0;
                brackets[1].origin = NM_CREDIT_FROM_ENDPOINT;
                brackets[1].observed_at = now;
                snprintf(brackets[1].label, sizeof(brackets[1].label),
                         "weekly allowance");
                snprintf(brackets[1].origin_desc, sizeof(brackets[1].origin_desc),
                         "GET /api/balance");
            }
        }
    }

    /* Purchased balance */
    size_t p_idx = legacy ? 2 : 1;
    NmJson *pur = nm_json_get(doc, "purchased");
    if (pur) {
        NmJson *p_bal = nm_json_get(pur, "balance_usd");
        brackets[p_idx].unit = NM_CREDIT_USD;
        brackets[p_idx].window = NM_CREDIT_PURSE;
        brackets[p_idx].kind = NM_CREDIT_LEFT;
        brackets[p_idx].limit = -1;
        brackets[p_idx].percent_used = -1;
        brackets[p_idx].origin = NM_CREDIT_FROM_ENDPOINT;
        brackets[p_idx].observed_at = now;
        snprintf(brackets[p_idx].label, sizeof(brackets[p_idx].label),
                 "purchased balance");
        snprintf(brackets[p_idx].origin_desc, sizeof(brackets[p_idx].origin_desc),
                 "GET /api/balance");
        if (p_bal && nm_json_type(p_bal) == NM_JSON_NUMBER) {
            double pb = nm_json_num(p_bal);
            brackets[p_idx].value = pb;
            brackets[p_idx].state = (pb > 0) ? NM_CREDIT_KNOWN : NM_CREDIT_UNKNOWN;
        } else {
            brackets[p_idx].value = -1;
            brackets[p_idx].state = NM_CREDIT_UNKNOWN;
        }
    }

    *n_brackets = legacy ? 3 : 2;
    return 1;
}

const NmProviderCredits nm_ollama_credits = {
    "/api/balance",
    "Bearer %s",
    60.0, /* 60s minimum refresh floor */
    1,
    NULL,
    ollama_init_brackets,
    ollama_map_endpoint,
};

/* 3. OpenRouter */
static void openrouter_init_brackets(NmCreditBracket *b, size_t max_b,
                                     size_t *n_out)
{
    if (max_b < 6)
        return;
    memset(b, 0, sizeof(*b) * 6);

    b[0].unit = NM_CREDIT_USD;
    b[0].window = NM_CREDIT_PURSE;
    b[0].kind = NM_CREDIT_LEFT;
    b[0].state = NM_CREDIT_UNKNOWN;
    b[0].limit = -1;
    b[0].percent_used = -1;
    snprintf(b[0].label, sizeof(b[0].label), "account purse");
    snprintf(b[0].origin_desc, sizeof(b[0].origin_desc), "GET /api/v1/credits");

    b[1].unit = NM_CREDIT_USD;
    b[1].window = NM_CREDIT_PURSE;
    b[1].kind = NM_CREDIT_LEFT;
    b[1].state = NM_CREDIT_UNKNOWN;
    b[1].limit = -1;
    b[1].percent_used = -1;
    snprintf(b[1].label, sizeof(b[1].label), "key purse: no key limit (limit_remaining null)");
    snprintf(b[1].origin_desc, sizeof(b[1].origin_desc), "GET /api/v1/key");

    b[2].unit = NM_CREDIT_REQUESTS;
    b[2].window = NM_CREDIT_DAY;
    b[2].kind = NM_CREDIT_LEFT;
    b[2].state = NM_CREDIT_UNKNOWN;
    b[2].limit = -1;
    b[2].percent_used = -1;
    snprintf(b[2].label, sizeof(b[2].label), "free-model requests");
    snprintf(b[2].origin_desc, sizeof(b[2].origin_desc), "GET /api/v1/key");

    b[3].unit = NM_CREDIT_USD;
    b[3].window = NM_CREDIT_DAY;
    b[3].kind = NM_CREDIT_SPENT;
    b[3].state = NM_CREDIT_UNKNOWN;
    b[3].limit = -1;
    b[3].percent_used = -1;
    snprintf(b[3].label, sizeof(b[3].label), "used today (no cap)");
    snprintf(b[3].origin_desc, sizeof(b[3].origin_desc), "GET /api/v1/key");

    b[4].unit = NM_CREDIT_USD;
    b[4].window = NM_CREDIT_WEEK;
    b[4].kind = NM_CREDIT_SPENT;
    b[4].state = NM_CREDIT_UNKNOWN;
    b[4].limit = -1;
    b[4].percent_used = -1;
    snprintf(b[4].label, sizeof(b[4].label), "used this week (no cap)");
    snprintf(b[4].origin_desc, sizeof(b[4].origin_desc), "GET /api/v1/key");

    b[5].unit = NM_CREDIT_USD;
    b[5].window = NM_CREDIT_PERIOD;
    b[5].kind = NM_CREDIT_SPENT;
    b[5].state = NM_CREDIT_UNKNOWN;
    b[5].limit = -1;
    b[5].percent_used = -1;
    snprintf(b[5].label, sizeof(b[5].label), "used this month (no cap)");
    snprintf(b[5].origin_desc, sizeof(b[5].origin_desc), "GET /api/v1/key");

    *n_out = 6;
}

static int openrouter_map_endpoint(const NmJson *doc, int step,
                                   NmCreditBracket *brackets,
                                   size_t max_brackets, size_t *n_brackets)
{
    (void)max_brackets;
    if (!doc || *n_brackets < 6)
        return 0;

    long now = (long)nm_monotonic_seconds();
    NmJson *data = nm_json_get(doc, "data");
    if (!data)
        return 0;

    if (step == 0) {
        /* Phase 0: /api/v1/credits -> account purse */
        NmJson *tc = nm_json_get(data, "total_credits");
        NmJson *tu = nm_json_get(data, "total_usage");
        if (tc && tu && nm_json_type(tc) == NM_JSON_NUMBER &&
            nm_json_type(tu) == NM_JSON_NUMBER) {
            double c_val = nm_json_num(tc);
            double u_val = nm_json_num(tu);
            brackets[0].state = NM_CREDIT_KNOWN;
            brackets[0].value = c_val - u_val;
            brackets[0].limit = -1;
            brackets[0].origin = NM_CREDIT_FROM_ENDPOINT;
            brackets[0].observed_at = now;
            snprintf(brackets[0].label, sizeof(brackets[0].label),
                     "account purse (%.2f credits - %.2f used)", c_val, u_val);
            snprintf(brackets[0].origin_desc, sizeof(brackets[0].origin_desc),
                     "GET /api/v1/credits");
        }
        return 1;
    }

    if (step == 1) {
        /* Phase 1: /api/v1/key -> key purse, free requests, spend meters */
        NmJson *lr = nm_json_get(data, "limit_remaining");
        NmJson *l = nm_json_get(data, "limit");
        if (lr && nm_json_type(lr) == NM_JSON_NUMBER) {
            brackets[1].state = NM_CREDIT_KNOWN;
            brackets[1].value = nm_json_num(lr);
            brackets[1].limit = (l && nm_json_type(l) == NM_JSON_NUMBER)
                                    ? nm_json_num(l)
                                    : -1;
            snprintf(brackets[1].label, sizeof(brackets[1].label), "key purse");
        } else {
            brackets[1].state = NM_CREDIT_UNKNOWN;
            brackets[1].value = -1;
            brackets[1].limit = -1;
            snprintf(brackets[1].label, sizeof(brackets[1].label),
                     "key purse: no key limit (limit_remaining null)");
        }
        brackets[1].origin = NM_CREDIT_FROM_ENDPOINT;
        brackets[1].observed_at = now;
        snprintf(brackets[1].origin_desc, sizeof(brackets[1].origin_desc),
                 "GET /api/v1/key");

        /* Free model daily requests */
        NmJson *fmdr = nm_json_get(data, "free_model_daily_requests");
        if (fmdr) {
            NmJson *rem = nm_json_get(fmdr, "remaining");
            NmJson *lim = nm_json_get(fmdr, "limit");
            brackets[2].state = NM_CREDIT_KNOWN;
            brackets[2].value = rem ? nm_json_num(rem) : -1;
            brackets[2].limit = lim ? nm_json_num(lim) : -1;
        } else {
            brackets[2].state = NM_CREDIT_UNKNOWN;
            brackets[2].value = -1;
            brackets[2].limit = -1;
        }
        brackets[2].origin = NM_CREDIT_FROM_ENDPOINT;
        brackets[2].observed_at = now;
        snprintf(brackets[2].label, sizeof(brackets[2].label),
                 "free-model requests");
        snprintf(brackets[2].origin_desc, sizeof(brackets[2].origin_desc),
                 "GET /api/v1/key");

        /* Spend meters */
        NmJson *ud = nm_json_get(data, "usage_daily");
        if (ud && nm_json_type(ud) == NM_JSON_NUMBER) {
            brackets[3].state = NM_CREDIT_KNOWN;
            brackets[3].value = nm_json_num(ud);
        } else {
            brackets[3].state = NM_CREDIT_UNKNOWN;
            brackets[3].value = -1;
        }
        brackets[3].limit = -1;
        brackets[3].origin = NM_CREDIT_FROM_ENDPOINT;
        brackets[3].observed_at = now;
        snprintf(brackets[3].label, sizeof(brackets[3].label),
                 "used today (no cap)");
        snprintf(brackets[3].origin_desc, sizeof(brackets[3].origin_desc),
                 "GET /api/v1/key");

        NmJson *uw = nm_json_get(data, "usage_weekly");
        if (uw && nm_json_type(uw) == NM_JSON_NUMBER) {
            brackets[4].state = NM_CREDIT_KNOWN;
            brackets[4].value = nm_json_num(uw);
        } else {
            brackets[4].state = NM_CREDIT_UNKNOWN;
            brackets[4].value = -1;
        }
        brackets[4].limit = -1;
        brackets[4].origin = NM_CREDIT_FROM_ENDPOINT;
        brackets[4].observed_at = now;
        snprintf(brackets[4].label, sizeof(brackets[4].label),
                 "used this week (no cap)");
        snprintf(brackets[4].origin_desc, sizeof(brackets[4].origin_desc),
                 "GET /api/v1/key");

        NmJson *um = nm_json_get(data, "usage_monthly");
        if (um && nm_json_type(um) == NM_JSON_NUMBER) {
            brackets[5].state = NM_CREDIT_KNOWN;
            brackets[5].value = nm_json_num(um);
        } else {
            brackets[5].state = NM_CREDIT_UNKNOWN;
            brackets[5].value = -1;
        }
        brackets[5].limit = -1;
        brackets[5].origin = NM_CREDIT_FROM_ENDPOINT;
        brackets[5].observed_at = now;
        snprintf(brackets[5].label, sizeof(brackets[5].label),
                 "used this month (no cap)");
        snprintf(brackets[5].origin_desc, sizeof(brackets[5].origin_desc),
                 "GET /api/v1/key");

        return 1;
    }

    return 0;
}

const NmProviderCredits nm_openrouter_credits = {
    "/api/v1/credits",
    "Bearer %s",
    0,
    2,
    "/api/v1/key",
    openrouter_init_brackets,
    openrouter_map_endpoint,
};

/* 4. OpenCode Go */
static void opencode_init_brackets(NmCreditBracket *b, size_t max_b,
                                   size_t *n_out)
{
    if (max_b < 3)
        return;
    memset(b, 0, sizeof(*b) * 3);

    b[0].unit = NM_CREDIT_PERCENT;
    b[0].window = NM_CREDIT_ROLLING_5H;
    b[0].kind = NM_CREDIT_LEFT;
    b[0].state = NM_CREDIT_UNKNOWN;
    b[0].limit = 100.0;
    b[0].percent_used = -1;
    snprintf(b[0].label, sizeof(b[0].label), "rolling 5h allowance");
    snprintf(b[0].origin_desc, sizeof(b[0].origin_desc), "GET /zen/go/v1/usage");

    b[1].unit = NM_CREDIT_PERCENT;
    b[1].window = NM_CREDIT_WEEK;
    b[1].kind = NM_CREDIT_LEFT;
    b[1].state = NM_CREDIT_UNKNOWN;
    b[1].limit = 100.0;
    b[1].percent_used = -1;
    snprintf(b[1].label, sizeof(b[1].label), "weekly allowance");
    snprintf(b[1].origin_desc, sizeof(b[1].origin_desc), "GET /zen/go/v1/usage");

    b[2].unit = NM_CREDIT_PERCENT;
    b[2].window = NM_CREDIT_PERIOD;
    b[2].kind = NM_CREDIT_LEFT;
    b[2].state = NM_CREDIT_UNKNOWN;
    b[2].limit = 100.0;
    b[2].percent_used = -1;
    snprintf(b[2].label, sizeof(b[2].label), "monthly allowance");
    snprintf(b[2].origin_desc, sizeof(b[2].origin_desc), "GET /zen/go/v1/usage");

    *n_out = 3;
}

static int opencode_map_endpoint(const NmJson *doc, int step,
                                 NmCreditBracket *brackets, size_t max_brackets,
                                 size_t *n_brackets)
{
    (void)step;
    (void)max_brackets;
    if (!doc || *n_brackets < 3)
        return 0;

    long now = (long)nm_monotonic_seconds();
    NmJson *u = nm_json_get(doc, "usage");
    if (!u)
        return 0;

    /* rolling */
    NmJson *r = nm_json_get(u, "rolling");
    if (r) {
        NmJson *pct = nm_json_get(r, "percent");
        NmJson *res = nm_json_get(r, "resetsAt");
        NmJson *st = nm_json_get(r, "status");
        double pused = pct ? nm_json_num(pct) : 0;
        brackets[0].percent_used = pused;
        brackets[0].value = 100.0 - pused;
        brackets[0].limit = 100.0;
        brackets[0].resets_at =
            res ? nm_credit_parse_iso8601(nm_json_str(res)) : 0;
        if (st && strcmp(nm_json_str(st), "rate-limited") == 0)
            brackets[0].state = NM_CREDIT_EXHAUSTED;
        else
            brackets[0].state = NM_CREDIT_KNOWN;
        brackets[0].origin = NM_CREDIT_FROM_ENDPOINT;
        brackets[0].observed_at = now;
        snprintf(brackets[0].label, sizeof(brackets[0].label),
                 "rolling 5h allowance (%.0f%% used)", pused);
        snprintf(brackets[0].origin_desc, sizeof(brackets[0].origin_desc),
                 "GET /zen/go/v1/usage");
    }

    /* weekly */
    NmJson *w = nm_json_get(u, "weekly");
    if (w) {
        NmJson *pct = nm_json_get(w, "percent");
        NmJson *res = nm_json_get(w, "resetsAt");
        NmJson *st = nm_json_get(w, "status");
        double pused = pct ? nm_json_num(pct) : 0;
        brackets[1].percent_used = pused;
        brackets[1].value = 100.0 - pused;
        brackets[1].limit = 100.0;
        brackets[1].resets_at =
            res ? nm_credit_parse_iso8601(nm_json_str(res)) : 0;
        if (st && strcmp(nm_json_str(st), "rate-limited") == 0)
            brackets[1].state = NM_CREDIT_EXHAUSTED;
        else
            brackets[1].state = NM_CREDIT_KNOWN;
        brackets[1].origin = NM_CREDIT_FROM_ENDPOINT;
        brackets[1].observed_at = now;
        snprintf(brackets[1].label, sizeof(brackets[1].label),
                 "weekly allowance (%.0f%% used)", pused);
        snprintf(brackets[1].origin_desc, sizeof(brackets[1].origin_desc),
                 "GET /zen/go/v1/usage");
    }

    /* monthly */
    NmJson *m = nm_json_get(u, "monthly");
    if (m) {
        NmJson *pct = nm_json_get(m, "percent");
        NmJson *res = nm_json_get(m, "resetsAt");
        NmJson *st = nm_json_get(m, "status");
        double pused = pct ? nm_json_num(pct) : 0;
        brackets[2].percent_used = pused;
        brackets[2].value = 100.0 - pused;
        brackets[2].limit = 100.0;
        brackets[2].resets_at =
            res ? nm_credit_parse_iso8601(nm_json_str(res)) : 0;
        if (st && strcmp(nm_json_str(st), "rate-limited") == 0)
            brackets[2].state = NM_CREDIT_EXHAUSTED;
        else
            brackets[2].state = NM_CREDIT_KNOWN;
        brackets[2].origin = NM_CREDIT_FROM_ENDPOINT;
        brackets[2].observed_at = now;
        snprintf(brackets[2].label, sizeof(brackets[2].label),
                 "monthly allowance (%.0f%% used)", pused);
        snprintf(brackets[2].origin_desc, sizeof(brackets[2].origin_desc),
                 "GET /zen/go/v1/usage");
    }

    return 1;
}

const NmProviderCredits nm_opencode_credits = {
    /* ABSOLUTE from the origin: the fetch driver sends `endpoint` as the
     * request target and takes only scheme/host/port from base_url, which
     * here ends in "/zen/go/v1" (docs/OPENCODE-API.md §7.1). "/usage"
     * asked opencode.ai/usage — the marketing site's 404 — and every Go
     * bracket stayed unknown. */
    "/zen/go/v1/usage",
    "Bearer %s",
    0,
    1,
    NULL,
    opencode_init_brackets,
    opencode_map_endpoint,
};

/* 5. OpenCode Zen */
static void opencode_zen_init_brackets(NmCreditBracket *b, size_t max_b,
                                       size_t *n_out)
{
    if (max_b < 1)
        return;
    memset(b, 0, sizeof(*b));

    b[0].unit = NM_CREDIT_ZEN;
    b[0].window = NM_CREDIT_PURSE;
    b[0].kind = NM_CREDIT_LEFT;
    b[0].state = NM_CREDIT_UNKNOWN;
    b[0].limit = -1;
    b[0].percent_used = -1;
    snprintf(b[0].label, sizeof(b[0].label),
             "purse: no balance API (open upstream request; see docs/OPENCODE-API.md §7.3)");

    *n_out = 1;
}

const NmProviderCredits nm_opencode_zen_credits = {
    NULL,
    NULL,
    0,
    0,
    NULL,
    opencode_zen_init_brackets,
    NULL,
};

/* ------------------------------------------------------------------ */
/* Out-of-band fetch driver                                           */
/* ------------------------------------------------------------------ */

typedef struct
{
    NmFetchStream *f;
    NmProviderId id;
    int step;
    char base_url[256];
    char api_key[256];
} NmCreditsFetchState;

static NmCreditsFetchState g_fetch[NM_PROVIDER_COUNT];

int nm_credits_fetch_begin(NmProviderId id, const char *base_url,
                           const char *api_key)
{
    if ((int)id < 0 || id >= NM_PROVIDER_COUNT)
        return 0;

    NmCredits *cr = nm_credits_get(id);
    if (cr->fetch_in_flight)
        return 0;

    double now = nm_monotonic_seconds();
    if (now < cr->next_fetch_allowed_at)
        return 0;

    const NmProvider *p = nm_provider_get(id);
    if (!p || !p->credits || !p->credits->endpoint)
        return 0;

    const char *base = (base_url && *base_url) ? base_url : p->default_base_url;
    if (!base || !*base)
        return 0;

    NmCreditsFetchState *st = &g_fetch[id];
    memset(st, 0, sizeof(*st));
    st->id = id;
    st->step = 0;
    snprintf(st->base_url, sizeof(st->base_url), "%s", base);
    if (api_key)
        snprintf(st->api_key, sizeof(st->api_key), "%s", api_key);

    st->f = nm_fetch_begin(st->base_url, "GET", p->credits->endpoint,
                           p->credits->auth_header,
                           st->api_key[0] ? st->api_key : NULL, NULL, 0, NULL);
    if (!st->f)
        return 0;

    cr->fetch_in_flight = 1;
    return 1;
}

NmCatalogStatus nm_credits_fetch_step(NmProviderId id)
{
    if ((int)id < 0 || id >= NM_PROVIDER_COUNT)
        return NM_CATALOG_ERR;

    NmCredits *cr = nm_credits_get(id);
    NmCreditsFetchState *st = &g_fetch[id];
    if (!cr->fetch_in_flight || !st->f)
        return NM_CATALOG_ERR;

    NmCatalogStatus s = nm_fetch_step(st->f);
    if (s == NM_CATALOG_PENDING)
        return NM_CATALOG_PENDING;

    const NmProvider *p = nm_provider_get(id);

    if (s == NM_CATALOG_OK) {
        NmJson *doc = nm_fetch_take(st->f);
        if (p && p->credits && p->credits->map_endpoint) {
            p->credits->map_endpoint(doc, st->step, cr->brackets,
                                     NM_CREDIT_BRACKETS_MAX, &cr->n_brackets);
        }
        if (doc)
            nm_json_free(doc);
        nm_fetch_end(st->f);
        st->f = NULL;

        /* Multi-step chain */
        if (p && p->credits && p->credits->n_steps > 1 && st->step == 0 &&
            p->credits->step1_endpoint) {
            st->step = 1;
            st->f = nm_fetch_begin(st->base_url, "GET",
                                   p->credits->step1_endpoint,
                                   p->credits->auth_header,
                                   st->api_key[0] ? st->api_key : NULL, NULL, 0,
                                   NULL);
            if (st->f)
                return NM_CATALOG_PENDING;
        }

        /* Success termination */
        cr->observed_at = (long)nm_monotonic_seconds();
        cr->fetch_in_flight = 0;
        if (p && p->credits && p->credits->min_interval_sec > 0)
            cr->next_fetch_allowed_at =
                nm_monotonic_seconds() + p->credits->min_interval_sec;
        return NM_CATALOG_OK;
    }

    /* Failure handling */
    int http_status = nm_fetch_http_status(st->f);
    const char *retry_after = nm_fetch_header(st->f, "Retry-After");

    if (http_status == 429) {
        long secs = retry_after ? strtol(retry_after, NULL, 10) : 0;
        if (secs <= 0)
            secs = 60;
        cr->next_fetch_allowed_at = nm_monotonic_seconds() + (double)secs;
    } else if (p && p->credits && p->credits->min_interval_sec > 0) {
        cr->next_fetch_allowed_at =
            nm_monotonic_seconds() + p->credits->min_interval_sec;
    }

    /* OpenCode Go 403: no Go subscription (not a failure, marks unknown) */
    if (id == NM_PROVIDER_OPENCODE && http_status == 403) {
        for (size_t i = 0; i < cr->n_brackets; i++)
            cr->brackets[i].state = NM_CREDIT_UNKNOWN;
    }

    nm_fetch_end(st->f);
    st->f = NULL;
    cr->fetch_in_flight = 0;
    return NM_CATALOG_ERR;
}

NmSource nm_credits_fetch_source(NmProviderId id)
{
    NmSource none = { -1, 0, NM_SRC_FD };
    if ((int)id < 0 || id >= NM_PROVIDER_COUNT)
        return none;
    NmCreditsFetchState *st = &g_fetch[id];
    if (!st->f)
        return none;
    return nm_fetch_source(st->f);
}

void nm_credits_fetch_end(NmProviderId id)
{
    if ((int)id < 0 || id >= NM_PROVIDER_COUNT)
        return;
    NmCredits *cr = nm_credits_get(id);
    NmCreditsFetchState *st = &g_fetch[id];
    if (st->f) {
        nm_fetch_end(st->f);
        st->f = NULL;
    }
    cr->fetch_in_flight = 0;
}

int nm_credits_fetch_in_flight(NmProviderId id)
{
    if ((int)id < 0 || id >= NM_PROVIDER_COUNT)
        return 0;
    return g_credits[id].fetch_in_flight;
}

int nm_credits_refresh_sync(NmProviderId id, const char *base_url,
                            const char *api_key)
{
    if (!nm_credits_fetch_begin(id, base_url, api_key))
        return 0;

    for (;;) {
        NmCatalogStatus st = nm_credits_fetch_step(id);
        if (st == NM_CATALOG_OK)
            return 1;
        if (st == NM_CATALOG_ERR)
            return 0;

        NmSource src = nm_credits_fetch_source(id);
        if (src.handle < 0) {
            nm_usleep(5000);
            continue;
        }
#ifdef _WIN32
        /* Transport handles socket waits via nm_connection_step */
        Sleep(5);
#else
        fd_set rfds, wfds;
        FD_ZERO(&rfds);
        FD_ZERO(&wfds);
        if (src.flags & NM_INTEREST_READ)
            FD_SET((int)src.handle, &rfds);
        if (src.flags & NM_INTEREST_WRITE)
            FD_SET((int)src.handle, &wfds);
        struct timeval tv = { 0, 10000 };
        select((int)src.handle + 1, &rfds, &wfds, NULL, &tv);
#endif
    }
}
