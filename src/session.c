/* session.c - conversation transcript + context-window management
 *
 * Phase 3: real implementation. One growable message array per
 * session (geometric growth, memory-reuse principle); the context
 * view is a reused pointer array filled per call, borrowed by the
 * caller until the next mutation.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nm_image_bytes.h"
#include "session.h"

struct NmSession
{
    NmSessionMessage *msgs;
    size_t n;
    size_t cap;
    const NmSessionMessage **view; /* reused context-view array */
    size_t view_cap;
    /* The image store (VISION-PLAN §3): attached files, read once and
     * frozen as data URLs. Grown geometrically — an attach is an event,
     * not churn. Messages carry INDICES into this array. */
    NmImage *images;
    size_t n_images;
    size_t image_cap;
};

/* Rough token estimate for one attached image. Real image tokenization
 * is provider-side and pixel-based (the probed ballpark for a
 * full-resolution tile set is ~1.2k tokens), so it cannot be derived
 * from the byte count — the base64 length would overcount by orders of
 * magnitude. A flat per-image allowance keeps the estimate in the right
 * neighbourhood; the authoritative number is always the provider's
 * prompt_tokens (the context gauge reads that, never this). */
#define NM_SESSION_IMAGE_TOKEN_ESTIMATE 1200

static char *dup_or_null(const char *s)
{
    return s ? strdup(s) : NULL;
}

static NmSessionMessage *push_slot(NmSession *s)
{
    if (s->n == s->cap) {
        size_t ncap = s->cap ? s->cap * 2 : 16;
        NmSessionMessage *nm =
            realloc(s->msgs, ncap * sizeof(*s->msgs));
        if (!nm)
            return NULL;
        memset(nm + s->cap, 0, (ncap - s->cap) * sizeof(*nm));
        s->msgs = nm;
        s->cap = ncap;
    }
    NmSessionMessage *m = &s->msgs[s->n++];
    memset(m, 0, sizeof(*m));
    return m;
}

NmSession *nm_session_new(const char *system_prompt)
{
    NmSession *s = calloc(1, sizeof(*s));
    if (!s)
        return NULL;
    if (system_prompt && *system_prompt) {
        NmSessionMessage *m = push_slot(s);
        if (!m) {
            free(s);
            return NULL;
        }
        m->role = NM_ROLE_SYSTEM;
        m->content = strdup(system_prompt);
    }
    return s;
}

void nm_session_free(NmSession *s)
{
    if (!s)
        return;
    for (size_t i = 0; i < s->n; i++) {
        free(s->msgs[i].content);
        free(s->msgs[i].tool_calls_json);
        free(s->msgs[i].tool_call_id);
        free(s->msgs[i].tool_name);
        free(s->msgs[i].reasoning);
        free(s->msgs[i].images);
    }
    for (size_t i = 0; i < s->n_images; i++) {
        free(s->images[i].data_url);
        free(s->images[i].part_json);
    }
    free(s->images);
    free(s->msgs);
    free(s->view);
    free(s);
}

const NmSessionMessage *nm_session_append(NmSession *s, NmRole role,
                                          const char *content)
{
    if (!s)
        return NULL;
    NmSessionMessage *m = push_slot(s);
    if (!m)
        return NULL;
    m->role = role;
    m->content = dup_or_null(content);
    return m;
}

const NmSessionMessage *nm_session_append_reasoning(NmSession *s,
                                                    const char *reasoning,
                                                    const char *content)
{
    if (!s)
        return NULL;
    NmSessionMessage *m = push_slot(s);
    if (!m)
        return NULL;
    m->role = NM_ROLE_ASSISTANT;
    m->content = dup_or_null(content);
    m->reasoning = dup_or_null(reasoning);
    return m;
}

const NmSessionMessage *nm_session_append_tool_call(NmSession *s,
                                                    const char *tool_calls_json,
                                                    const char *reasoning)
{
    if (!s)
        return NULL;
    NmSessionMessage *m = push_slot(s);
    if (!m)
        return NULL;
    m->role = NM_ROLE_ASSISTANT;
    m->tool_calls_json = dup_or_null(tool_calls_json);
    m->reasoning = dup_or_null(reasoning);
    return m;
}

const NmSessionMessage *nm_session_append_tool_result(NmSession *s,
                                                      const char *tool_call_id,
                                                      const char *tool_name,
                                                      const char *output)
{
    if (!s)
        return NULL;
    NmSessionMessage *m = push_slot(s);
    if (!m)
        return NULL;
    m->role = NM_ROLE_TOOL;
    m->tool_call_id = dup_or_null(tool_call_id);
    m->tool_name = dup_or_null(tool_name);
    m->content = dup_or_null(output);
    return m;
}

/* ---------------------------------------------------------------- */
/* Images (VISION-PLAN §3: capture, not reference)                   */
/* ---------------------------------------------------------------- */

static void attach_reason(char *reason, size_t cap, const char *msg)
{
    if (reason && cap)
        snprintf(reason, cap, "%s", msg);
}

/* The last path component ("/a/b/foo.png" and "C:\a\foo.png" both give
 * "foo.png") — the image's alt/marker text. Both separators are
 * honoured whatever the host is: a Windows path can appear in a
 * transcript read on POSIX. */
static const char *base_name(const char *path)
{
    const char *b = path;
    for (const char *p = path; *p; p++) {
        if (*p == '/' || *p == '\\')
            b = p + 1;
    }
    return *b ? b : path;
}

/* The pre-serialized wire part for one image. Assembled by hand (not
 * through nm_json) because the payload is the point: a megabyte of
 * base64 must not be copied into a node and copied again on dump. The
 * data URL is base64, whose alphabet is JSON-escape-free, so embedding
 * it inside the JSON string is exact — that is the raw-node contract
 * (json.h). */
static char *build_part_json(const char *data_url, size_t url_len,
                             size_t *out_len)
{
    static const char pre[] =
        "{\"type\":\"image_url\",\"image_url\":{\"url\":\"";
    static const char post[] = "\"}}";
    size_t n = sizeof(pre) - 1 + url_len + sizeof(post) - 1;
    char *p = malloc(n + 1);
    if (!p)
        return NULL;
    size_t o = 0;
    memcpy(p + o, pre, sizeof(pre) - 1);
    o += sizeof(pre) - 1;
    memcpy(p + o, data_url, url_len);
    o += url_len;
    memcpy(p + o, post, sizeof(post) - 1);
    o += sizeof(post) - 1;
    p[o] = '\0';
    if (out_len)
        *out_len = o;
    return p;
}

static NmImage *image_slot(NmSession *s)
{
    if (s->n_images == s->image_cap) {
        size_t ncap = s->image_cap ? s->image_cap * 2 : 4;
        NmImage *ni = realloc(s->images, ncap * sizeof(*ni));
        if (!ni)
            return NULL;
        memset(ni + s->image_cap, 0, (ncap - s->image_cap) * sizeof(*ni));
        s->images = ni;
        s->image_cap = ncap;
    }
    return &s->images[s->n_images++];
}

long nm_session_attach_image(NmSession *s, const char *path, char *reason,
                             size_t reason_cap)
{
    attach_reason(reason, reason_cap, "");
    if (!s || !path || !*path) {
        attach_reason(reason, reason_cap, "no path");
        return -1;
    }
    /* The wire cap is the attach cap: these bytes ride EVERY request
     * (chat/completions has no upload endpoint), so refusing locally
     * keeps the message ours instead of a provider 400. */
    NmImageProbe p;
    NmImageStatus st = nm_image_file_probe(path, NM_IMAGE_MAX_WIRE_BYTES, &p);
    if (st != NM_IMAGE_OK) {
        const char *msg;
        switch (st) {
        case NM_IMAGE_ERR_UNREADABLE:
            msg = "source unreadable";
            break;
        case NM_IMAGE_ERR_EMPTY:
            msg = "empty file";
            break;
        case NM_IMAGE_ERR_OVERSIZE:
            msg = "too large";
            break;
        case NM_IMAGE_ERR_NOMEM:
            msg = "no memory";
            break;
        case NM_IMAGE_ERR_UNKNOWN:
        default:
            msg = "unknown container";
            break;
        }
        nm_image_probe_free(&p);
        attach_reason(reason, reason_cap, msg);
        return -1;
    }

    size_t url_len = 0;
    char *url = nm_image_data_url(p.format, p.bytes, p.len, &url_len);
    if (!url) {
        nm_image_probe_free(&p);
        attach_reason(reason, reason_cap, "no memory");
        return -1;
    }
    size_t part_len = 0;
    char *part = build_part_json(url, url_len, &part_len);
    if (!part) {
        free(url);
        nm_image_probe_free(&p);
        attach_reason(reason, reason_cap, "no memory");
        return -1;
    }

    NmImage *img = image_slot(s);
    if (!img) {
        free(url);
        free(part);
        nm_image_probe_free(&p);
        attach_reason(reason, reason_cap, "no memory");
        return -1;
    }
    img->data_url = url;
    img->data_url_len = url_len;
    img->part_json = part;
    img->part_json_len = part_len;
    img->format = p.format;
    img->w = p.w;
    img->h = p.h;
    img->bytes = p.len;
    snprintf(img->alt, sizeof(img->alt), "%s", base_name(path));
    nm_image_probe_free(&p);
    return (long)(s->n_images - 1);
}

const NmImage *nm_session_image(const NmSession *s, size_t idx)
{
    return (s && idx < s->n_images) ? &s->images[idx] : NULL;
}

size_t nm_session_image_count(const NmSession *s)
{
    return s ? s->n_images : 0;
}

/* The deterministic text part for an image-only send: the alt names
 * when they fit, else a count. Never empty, never absent — some
 * upstreams dislike a textless user message, and the shape has to be
 * frozen for the prefix cache anyway (VISION-PLAN §5). */
static void fallback_text(const NmSession *s, const size_t *ids, size_t n,
                          char *out, size_t cap)
{
    if (n == 1) {
        const NmImage *img = nm_session_image(s, ids[0]);
        snprintf(out, cap, "%s attached", img ? img->alt : "image");
        return;
    }
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        const NmImage *img = nm_session_image(s, ids[i]);
        const char *name = img ? img->alt : "image";
        size_t len = strlen(name);
        /* ", " between names, " attached" at the end, NUL */
        if (o + len + (i ? 2 : 0) + 10 > cap)
            break;
        if (i)
            o += (size_t)snprintf(out + o, cap - o, ", ");
        o += (size_t)snprintf(out + o, cap - o, "%s", name);
    }
    if (o == 0 || o + 10 > cap)
        snprintf(out, cap, "%zu images attached", n);
    else
        snprintf(out + o, cap - o, " attached");
}

const NmSessionMessage *nm_session_append_user_images(NmSession *s,
                                                      const char *text,
                                                      const size_t *image_ids,
                                                      size_t n_images)
{
    if (!s)
        return NULL;
    for (size_t i = 0; i < n_images; i++) {
        if (image_ids[i] >= s->n_images)
            return NULL; /* caller bug: an id that does not resolve */
    }
    char fallback[256];
    if ((!text || !*text) && n_images > 0) {
        fallback_text(s, image_ids, n_images, fallback, sizeof(fallback));
        text = fallback;
    }
    size_t *ids = NULL;
    if (n_images > 0) {
        ids = malloc(n_images * sizeof(*ids));
        if (!ids)
            return NULL;
        memcpy(ids, image_ids, n_images * sizeof(*ids));
    }
    NmSessionMessage *m = push_slot(s);
    if (!m) {
        free(ids);
        return NULL;
    }
    m->role = NM_ROLE_USER;
    m->content = dup_or_null(text);
    m->images = ids;
    m->n_images = n_images;
    return m;
}

const NmSessionMessage *nm_session_get(const NmSession *s, size_t i)
{
    return (s && i < s->n) ? &s->msgs[i] : NULL;
}

size_t nm_session_len(const NmSession *s) { return s ? s->n : 0; }

/* Rough token estimate: 4 chars per token (the quoth convention), plus
 * a flat allowance per attached image (its token cost is pixel-based
 * and provider-side — see NM_SESSION_IMAGE_TOKEN_ESTIMATE). */
static long est_tokens(const NmSessionMessage *m)
{
    size_t chars = 0;
    if (m->content)
        chars += strlen(m->content);
    if (m->tool_calls_json)
        chars += strlen(m->tool_calls_json);
    return (long)((chars + 3) / 4) + 4 + /* +4: per-message framing */
           (long)m->n_images * NM_SESSION_IMAGE_TOKEN_ESTIMATE;
}

NmContextView nm_session_context(const NmSession *s, long budget_tokens)
{
    NmContextView v = { NULL, 0 };
    if (!s || s->n == 0)
        return v;

    /* A non-positive budget is "no trim": the whole transcript, system
     * prompt included. This is the default the agent uses (rolling
     * window off) — nevermore sends everything and lets the provider
     * report "too large", instead of silently capping. A window that
     * slid per turn would also defeat the provider's prefix cache
     * (cached input bills far cheaper), so trimming is opt-in. */
    int untrimmed = budget_tokens <= 0;

    /* The system prompt (message 0, when present) always leads and is
     * always in; the rest is the most recent tail that fits. */
    size_t start = (s->msgs[0].role == NM_ROLE_SYSTEM) ? 1 : 0;
    long spent = (start == 1) ? est_tokens(&s->msgs[0]) : 0;
    long budget = budget_tokens;

    /* Walk backwards accumulating the newest messages that fit; never
     * break a tool-result from its assistant tool-call message. Tool
     * results follow their calls, so walking backwards a run of
     * NM_ROLE_TOOL messages must extend to the call's assistant
     * message. */
    long tail = 0;
    size_t i = s->n;
    size_t newest_group = s->n; /* degenerate-fallback group start */
    while (!untrimmed && i > start) {
        size_t j = i;
        /* Extend backwards over any run of tool results (plus their
         * call message) as one unbreakable group. */
        if (s->msgs[j - 1].role == NM_ROLE_TOOL) {
            while (j > start && s->msgs[j - 1].role == NM_ROLE_TOOL)
                j--;
            /* The assistant tool-call message above the run pairs
             * with the results; include it (a tool result without
             * its call dangles). */
            if (j > start && s->msgs[j - 1].role == NM_ROLE_ASSISTANT && s->msgs[j - 1].tool_calls_json)
                j--;
        } else {
            j = i - 1; /* plain message: a group of one */
        }
        if (i == s->n)
            newest_group = j;
        long group_tokens = 0;
        for (size_t k = j; k < i; k++)
            group_tokens += est_tokens(&s->msgs[k]);
        if (spent + tail + group_tokens > budget)
            break;
        tail += group_tokens;
        i = j;
    }
    if (untrimmed)
        i = start; /* keep everything: system + all messages */
    else if (i == s->n)
        i = newest_group; /* degenerate: budget too small for even
                             the newest group; keep it whole so a
                             tool result never dangles alone */

    size_t n_view = s->n - i + (start == 1 ? 1 : 0);
    if (s->view_cap < n_view) {
        /* s is logically const here; the view array is scratch. */
        NmSession *mut = (NmSession *)s;
        size_t ncap = s->view_cap ? s->view_cap : 16;
        while (ncap < n_view)
            ncap *= 2;
        const NmSessionMessage **nv =
            realloc(s->view, ncap * sizeof(*s->view));
        if (!nv)
            return v;
        mut->view = nv;
        mut->view_cap = ncap;
    }
    size_t vi = 0;
    if (start == 1)
        s->view[vi++] = &s->msgs[0];
    for (size_t k = i; k < s->n; k++)
        s->view[vi++] = &s->msgs[k];
    v.messages = s->view;
    v.n = vi;
    return v;
}

/* ---------------------------------------------------------------- */
/* Persistence (markdown transcript with a metadata header)          */
/* ---------------------------------------------------------------- */

static const char *role_tag(NmRole r)
{
    switch (r) {
    case NM_ROLE_SYSTEM:
        return "system";
    case NM_ROLE_USER:
        return "user";
    case NM_ROLE_ASSISTANT:
        return "assistant";
    case NM_ROLE_TOOL:
        return "tool";
    }
    return "?";
}

int nm_session_save(const NmSession *s, const char *path)
{
    if (!s || !path)
        return -1;
    FILE *f = fopen(path, "w");
    if (!f)
        return -1;
    fprintf(f, "<!-- nevermore session: %zu messages -->\n", s->n);
    for (size_t i = 0; i < s->n; i++) {
        const NmSessionMessage *m = &s->msgs[i];
        fprintf(f, "\n## %s", role_tag(m->role));
        if (m->tool_name)
            fprintf(f, " (%s)", m->tool_name);
        if (m->tool_call_id)
            fprintf(f, " id=%s", m->tool_call_id);
        fputc('\n', f);
        if (m->content)
            fputs(m->content, f);
        if (m->tool_calls_json)
            fprintf(f, "\n<!-- tool_calls: %s -->", m->tool_calls_json);
        fputc('\n', f);
    }
    fclose(f);
    return 0;
}

NmSession *nm_session_load(const char *path)
{
    /* Markdown round-trip is a post-1.0 idea (deferred); save is the
     * debugging/inspection surface today. */
    (void)path;
    return NULL;
}
