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

#include "session.h"

struct NmSession
{
    NmSessionMessage *msgs;
    size_t n;
    size_t cap;
    const NmSessionMessage **view; /* reused context-view array */
    size_t view_cap;
};

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
    }
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

const NmSessionMessage *nm_session_append_tool_call(NmSession *s,
                                                    const char *tool_calls_json)
{
    if (!s)
        return NULL;
    NmSessionMessage *m = push_slot(s);
    if (!m)
        return NULL;
    m->role = NM_ROLE_ASSISTANT;
    m->tool_calls_json = dup_or_null(tool_calls_json);
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

size_t nm_session_len(const NmSession *s) { return s ? s->n : 0; }

const NmSessionMessage *nm_session_get(const NmSession *s, size_t i)
{
    return (s && i < s->n) ? &s->msgs[i] : NULL;
}

/* Rough token estimate: 4 chars per token (the quoth convention). */
static long est_tokens(const NmSessionMessage *m)
{
    size_t chars = 0;
    if (m->content)
        chars += strlen(m->content);
    if (m->tool_calls_json)
        chars += strlen(m->tool_calls_json);
    return (long)((chars + 3) / 4) + 4; /* +4: per-message framing overhead */
}

NmContextView nm_session_context(const NmSession *s, long budget_tokens)
{
    NmContextView v = { NULL, 0 };
    if (!s || s->n == 0)
        return v;

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
    while (i > start) {
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
    if (i == s->n)
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
