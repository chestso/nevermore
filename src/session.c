/* session.c - conversation transcript (see the header).
 *
 * One growable message array per session (geometric growth,
 * memory-reuse principle); the whole transcript is what the agent
 * sends — nevermore never trims it.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "image_bytes.h"
#include "json.h"
#include "session.h"

struct NmSession
{
    NmSessionMessage *msgs;
    size_t n;
    size_t cap;
    /* The image store: attached files, read once and
     * frozen as data URLs. Grown geometrically — an attach is an event,
     * not churn. Messages carry INDICES into this array. */
    NmImage *images;
    size_t n_images;
    size_t image_cap;
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

int nm_session_set_system(NmSession *s, const char *prompt)
{
    if (!s || !prompt)
        return -1;
    char *dup = strdup(prompt);
    if (!dup)
        return -1;
    if (s->n > 0 && s->msgs[0].role == NM_ROLE_SYSTEM) {
        free(s->msgs[0].content);
        s->msgs[0].content = dup;
        return 0;
    }
    /* No system message yet: insert one at the front (message 0 is the
     * system prompt by contract, so it must lead). */
    NmSessionMessage *m = push_slot(s);
    if (!m) {
        free(dup);
        return -1;
    }
    memmove(&s->msgs[1], &s->msgs[0], (s->n - 1) * sizeof(*s->msgs));
    memset(&s->msgs[0], 0, sizeof(s->msgs[0]));
    s->msgs[0].role = NM_ROLE_SYSTEM;
    s->msgs[0].content = dup;
    return 0;
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
/* Images (capture, not reference)                                   */
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

/* The one attach core (both the path and the bytes variants land here):
 * sniff the container from the buffer's head, enforce the wire cap,
 * freeze the data URL + its pre-serialized wire part into a store slot.
 * The store COPIES the bytes (via base64) into the frozen URL, so the
 * caller's buffer is never retained — the tool that read the file frees
 * it with the result. `alt` is the file path (or a base name): the
 * marker/alt text is its last component. On refusal nothing is stored
 * and `reason` carries the shared vocabulary. */
static long attach_core(NmSession *s, const unsigned char *bytes, size_t len,
                        const char *alt, char *reason, size_t reason_cap)
{
    int w = 0, h = 0;
    NmImageFormat fmt = nm_image_sniff(bytes, len, &w, &h);
    if (fmt == NM_IMAGE_FMT_UNKNOWN) {
        attach_reason(reason, reason_cap, "unknown container");
        return -1;
    }
    /* The wire cap is the attach cap: these bytes ride EVERY request
     * (chat/completions has no upload endpoint), so refusing locally
     * keeps the message ours instead of a provider 400. */
    if (len > NM_IMAGE_MAX_WIRE_BYTES) {
        attach_reason(reason, reason_cap, "too large");
        return -1;
    }

    size_t url_len = 0;
    char *url = nm_image_data_url(fmt, bytes, len, &url_len);
    if (!url) {
        attach_reason(reason, reason_cap, "no memory");
        return -1;
    }
    size_t part_len = 0;
    char *part = build_part_json(url, url_len, &part_len);
    if (!part) {
        free(url);
        attach_reason(reason, reason_cap, "no memory");
        return -1;
    }

    NmImage *img = image_slot(s);
    if (!img) {
        free(url);
        free(part);
        attach_reason(reason, reason_cap, "no memory");
        return -1;
    }
    img->data_url = url;
    img->data_url_len = url_len;
    img->part_json = part;
    img->part_json_len = part_len;
    img->format = fmt;
    img->w = w;
    img->h = h;
    img->bytes = len;
    snprintf(img->alt, sizeof(img->alt), "%s", base_name(alt ? alt : ""));
    return (long)(s->n_images - 1);
}

long nm_session_attach_image(NmSession *s, const char *path, char *reason,
                             size_t reason_cap)
{
    attach_reason(reason, reason_cap, "");
    if (!s || !path || !*path) {
        attach_reason(reason, reason_cap, "no path");
        return -1;
    }
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
    long id = attach_core(s, p.bytes, p.len, path, reason, reason_cap);
    nm_image_probe_free(&p);
    return id;
}

long nm_session_attach_image_bytes(NmSession *s, const unsigned char *bytes,
                                   size_t len, const char *alt, char *reason,
                                   size_t reason_cap)
{
    attach_reason(reason, reason_cap, "");
    if (!s || !bytes || len == 0) {
        attach_reason(reason, reason_cap, "empty file");
        return -1;
    }
    return attach_core(s, bytes, len, alt ? alt : "image", reason, reason_cap);
}

long nm_session_attach_image_url(NmSession *s, const char *url, size_t len,
                                 char *reason, size_t reason_cap)
{
    attach_reason(reason, reason_cap, "");
    if (!s || !url || len == 0) {
        attach_reason(reason, reason_cap, "not a base64 data URL");
        return -1;
    }
    NmImageFormat fmt;
    const char *b64;
    size_t b64_len;
    if (nm_image_data_url_split(url, len, &fmt, &b64, &b64_len) != 0) {
        attach_reason(reason, reason_cap, "not a base64 data URL");
        return -1;
    }

    /* Decode the payload ONCE, into scratch, for the marker facts
     * (container, dims, decoded size). The store keeps the URL
     * verbatim, never the decoded bytes — a re-encode would change
     * the bytes the replay prefix keys on. */
    unsigned char *scratch = malloc(b64_len / 4 * 3 + 1);
    if (!scratch) {
        attach_reason(reason, reason_cap, "no memory");
        return -1;
    }
    long n = nm_image_b64_decode(b64, b64_len, scratch, b64_len / 4 * 3 + 1);
    if (n < 0) {
        free(scratch);
        attach_reason(reason, reason_cap, "undecodable payload");
        return -1;
    }
    int w = 0, h = 0;
    NmImageKind kind = nm_image_sniff_kind(scratch, (size_t)n, &w, &h);
    free(scratch);
    /* The MIME's claim and the bytes' own answer usually agree; the
     * bytes win when they name a container (they are what replays). */
    NmImageFormat sniffed = nm_image_format_from_kind(kind);
    if (sniffed != NM_IMAGE_FMT_UNKNOWN)
        fmt = sniffed;

    /* Freeze the URL VERBATIM (the received bytes are the canonical
     * part) and pre-serialize the wire part around it. */
    char *url_copy = malloc(len + 1);
    if (!url_copy) {
        attach_reason(reason, reason_cap, "no memory");
        return -1;
    }
    memcpy(url_copy, url, len);
    url_copy[len] = '\0';
    size_t part_len = 0;
    char *part = build_part_json(url_copy, len, &part_len);
    if (!part) {
        free(url_copy);
        attach_reason(reason, reason_cap, "no memory");
        return -1;
    }
    NmImage *img = image_slot(s);
    if (!img) {
        free(url_copy);
        free(part);
        attach_reason(reason, reason_cap, "no memory");
        return -1;
    }
    img->data_url = url_copy;
    img->data_url_len = len;
    img->part_json = part;
    img->part_json_len = part_len;
    img->format = fmt;
    img->w = w;
    img->h = h;
    img->bytes = (size_t)n;
    /* A received image has no name of its own (a generated picture did
     * not come from a file): the store calls it what it is, and the
     * handle a person uses is the CHAT-scoped index this call returns,
     * which the UI prints as the block's caption (`image #<id>`) and
     * /image save takes. */
    memcpy(img->alt, "image", sizeof("image"));
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
 * frozen for the prefix cache anyway. */
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

const NmSessionMessage *nm_session_append_assistant_images(
    NmSession *s, const char *reasoning, const char *content,
    const char *tool_calls_json, const size_t *image_ids, size_t n_images)
{
    if (!s)
        return NULL;
    for (size_t i = 0; i < n_images; i++) {
        if (image_ids[i] >= s->n_images)
            return NULL; /* caller bug: an id that does not resolve */
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
    m->role = NM_ROLE_ASSISTANT;
    /* The wire shape is frozen at append: content stays a plain string
     * ("" when the round streamed no text — the probed replay shape,
     * OPENROUTER-API.md §5.1) and the images ride the message-level
     * array the composer emits for an assistant message. */
    m->content = dup_or_null(content ? content : "");
    m->reasoning = dup_or_null(reasoning);
    m->tool_calls_json = dup_or_null(tool_calls_json);
    m->images = ids;
    m->n_images = n_images;
    return m;
}

const NmSessionMessage *nm_session_get(const NmSession *s, size_t i)
{
    return (s && i < s->n) ? &s->msgs[i] : NULL;
}

size_t nm_session_len(const NmSession *s) { return s ? s->n : 0; }

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

/* The save is BOTH a transcript and a markdown document, and the two
 * jobs only coexist if the document's structure is the transcript's,
 * never the content's. Which body is markdown is a question about its
 * PRODUCER, not about how it looks:
 *
 *   - `user` and `assistant` bodies are prose — a person typed it or a
 *     model wrote it, and both mean markdown — so they are emitted
 *     live and render as the speaker's own markdown. That includes the
 *     round's REASONING trace: a model's CoT is markdown from the same
 *     producer (the transcript renders it as markdown too, dimmed —
 *     markdown.h gives every stream its own classifier, so a trace's
 *     fence state and the answer's never mix).
 *   - the `system` prompt and every `tool` result are bytes we
 *     assembled (`<env>`, `<project_context>`) or relayed verbatim (a
 *     file's contents, a diff, a command's output). They are not
 *     markdown, and emitting them live hands the document to them: a
 *     `#` joins the outline, a diff's `- `/`+ ` lines become bullets,
 *     an unbalanced ``` swallows everything after it, and a `<env>`
 *     opens an HTML block that ends at the next blank line, releasing
 *     the markdown inside it. So they ride a fenced block — opaque,
 *     byte-exact, and still one screen of markdown.
 *
 * The fence is length-adaptive (a body can only close a fence as long
 * as its own longest backtick run), which is what makes the block
 * opaque without escaping a single byte of the transcript.
 *
 * THE FILE IS ALSO A TRANSCRIPT THAT READS BACK, and that is the other
 * half of the design. Prose bodies cannot be fenced — a trace rendered
 * as a code block is not a trace — so they carry the other unforgeable
 * device instead: a DECLARED BYTE COUNT, in the `<!-- meta: ... -->`
 * line that follows every heading. A body is arbitrary bytes, so no
 * delimiter it can contain (`## user`, `### content`, `-->`, an
 * unbalanced fence) may decide the structure; with the count, a reader
 * consumes an exact number of bytes and never scans inside a body. The
 * `### reasoning` / `### content` labels are for the READER alone: what
 * tells a human where the trace ended and the answer began. The
 * machine facts the meta line carries are in meta_json() below, and the
 * NULL/empty distinctions a request cannot see in the first place are
 * in message_part().
 *
 * THAT GUARANTEE IS ABOUT BODIES, AND ONLY BODIES. The heading line
 * (`## tool (name) id=…`) is the format's one uncounted channel: its
 * tool name and call id are WIRE strings, and a `\n` in one would split
 * the heading and put a `<!-- meta: … -->` line exactly where a reader
 * looks for the real one — the writer's own device turned against the
 * writer, invisible in a rendered document. So the heading escapes
 * control bytes to visible `\xNN` (heading_field below). Nothing is
 * lost: the truth is in meta, the heading is display-only, and the
 * escape is a pure function of the same fields, so a re-save derives
 * the same bytes. The reader's half of the rule: the meta comment is
 * the FIRST line after the heading LF (anything between them is a
 * structural error), and the tool facts come from meta, never here. */

static size_t fence_len(const char *body)
{
    size_t best = 0;
    size_t run = 0;
    for (const char *p = body ? body : ""; *p; p++) {
        run = (*p == '`') ? run + 1 : 0;
        if (run > best)
            best = run;
    }
    return best < 3 ? 3 : best + 1;
}

/* One fenced block, opening fence on the line the caller left us on,
 * closing fence ending the last line (no blank line either side). */
static void fenced_write(FILE *f, const char *body)
{
    const char *text = body ? body : "";
    size_t n = fence_len(text);
    for (size_t i = 0; i < n; i++)
        fputc('`', f);
    /* Info string `text`: honest, and highlighting that guesses a
     * language would lie about a diff or a file dump. */
    fputs("text\n", f);
    fputs(text, f);
    if (*text && text[strlen(text) - 1] != '\n')
        fputc('\n', f);
    for (size_t i = 0; i < n; i++)
        fputc('`', f);
    fputc('\n', f);
}

/* A comment ends at the first `-->`, and this JSON holds tool
 * arguments — an edit whose new text contains `-->` would end the
 * comment early and spill its arguments into the document as prose.
 * JSON has no structural `>`, so `--\u003e` is the same value: the
 * escape keeps the metadata inert and the JSON valid. */
static void comment_write(FILE *f, const char *json)
{
    for (const char *p = json; p && *p; p++) {
        if (p[0] == '-' && p[1] == '-' && p[2] == '>') {
            fputs("--\\u003e", f);
            p += 2;
        } else {
            fputc(*p, f);
        }
    }
}

/* The parts a message writes, in SPEECH order: the round's trace, then
 * the answer (reasoning streams first — the transcript's own commit
 * order). ONE source of truth: the meta line's counts and the bodies
 * themselves both come from here, so they cannot drift.
 *
 * A part exists iff its body is non-empty. NULL and "" are the same
 * bytes in a request — the client serializes content with
 * nm_json_new_string(NULL) → "" and skips empty tool ids/calls with an
 * `&& *s` test — so the save records what a request can see and never a
 * distinction it cannot use (which is why no part ever has a count of
 * zero). */
static int message_part(const NmSessionMessage *m, int idx, const char **name,
                        const char **body)
{
    if (idx == 0 && m->reasoning && *m->reasoning) {
        *name = "reasoning";
        *body = m->reasoning;
        return 1;
    }
    if (idx == 1 && m->content && *m->content) {
        *name = "content";
        *body = m->content;
        return 1;
    }
    return 0;
}

/* The machine half of one message, as JSON: its role, the tool facts
 * (a heading cannot carry them safely — a tool name holding `)` would
 * make it ambiguous), the ordered parts with their exact byte counts,
 * and the images the message carried. The images are DECLARED, never
 * carried: a data URL is megabytes, and a load that silently dropped
 * the parts would rebuild a request prefix the chat never had. Built
 * with the JSON writer, escaped by comment_write like tool_calls, so no
 * payload can end the comment early. Returns a malloc'd string, NULL on
 * OOM (the caller turns that into a failed save, never a file with no
 * meta). */
static char *meta_json(const NmSession *s, const NmSessionMessage *m)
{
    NmJson *o = nm_json_new_object();
    if (!o)
        return NULL;
    nm_json_set(o, "role", nm_json_new_string(role_tag(m->role)));
    if (m->tool_name && *m->tool_name)
        nm_json_set(o, "tool_name", nm_json_new_string(m->tool_name));
    if (m->tool_call_id && *m->tool_call_id)
        nm_json_set(o, "tool_call_id", nm_json_new_string(m->tool_call_id));
    NmJson *parts = nm_json_new_array();
    for (int k = 0; k < 2; k++) {
        const char *name, *body;
        if (!message_part(m, k, &name, &body))
            continue;
        NmJson *pair = nm_json_new_array();
        nm_json_push(pair, nm_json_new_string(name));
        nm_json_push(pair, nm_json_new_number((double)strlen(body)));
        nm_json_push(parts, pair);
    }
    nm_json_set(o, "parts", parts);
    if (m->n_images) {
        /* The alts, in the order the message holds them: enough for a
         * reader to see what the chat had, and enough for a loader to
         * refuse the file rather than load a different transcript. */
        NmJson *imgs = nm_json_new_array();
        for (size_t k = 0; k < m->n_images; k++) {
            const NmImage *img = nm_session_image(s, m->images[k]);
            if (img)
                nm_json_push(imgs, nm_json_new_string(img->alt));
        }
        nm_json_set(o, "images", imgs);
    }
    char *js = nm_json_dump(o);
    nm_json_free(o);
    return js;
}

/* One body of a message: the optional label line, then the bytes.
 * The label is the READER's marker — a loader uses meta's counts,
 * because a body may contain a line that looks exactly like this one.
 * The body goes out verbatim, and one LF follows it only when it does
 * not already end with one, so the count is what tells the body's own
 * trailing newline from the syntactic one. Relayed roles ride the
 * fence (see the producer note above). */
static void part_write(FILE *f, const char *label, NmRole role,
                       const char *body)
{
    if (!body || !*body)
        return; /* a part exists iff it has bytes */
    if (label)
        fprintf(f, "\n### %s\n\n", label);
    else
        fputc('\n', f);
    if (role == NM_ROLE_SYSTEM || role == NM_ROLE_TOOL) {
        fenced_write(f, body);
        return;
    }
    fputs(body, f);
    if (body[strlen(body) - 1] != '\n')
        fputc('\n', f);
}

/* One heading field — a tool name or a call id — with control bytes
 * escaped to visible `\xNN`. The heading is the file's ONE uncounted
 * line (see the producer note above): every other channel is defended
 * by a count or by the `--\u003e` escape, and this one was printed raw,
 * so a `\n` in a wire string split it and fabricated a line a reader
 * takes as the message's own. Escaping costs no fidelity — the truth is
 * in meta and the heading is display-only — and it is deterministic, so
 * the round trip stays byte-identical. `\` is deliberately NOT escaped:
 * the heading is a display, not a channel, and a Windows path in an id
 * is commoner than a name spelling out `\x0a`. */
static void heading_field(FILE *f, const char *s)
{
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (*p < 0x20 || *p == 0x7f)
            fprintf(f, "\\x%02x", *p);
        else
            fputc(*p, f);
    }
}

int nm_session_save(const NmSession *s, const char *path)
{
    if (!s || !path)
        return -1;
    /* BINARY on purpose: the transcript is an artifact (diffed, shared,
     * re-read by a test), so it is LF on every platform — text mode would
     * translate every newline to CRLF on Windows and make the same save
     * two different files. */
    FILE *f = fopen(path, "wb");
    if (!f)
        return -1;
    fprintf(f, "<!-- nevermore session: %zu messages -->\n", s->n);
    /* The body rule above is format 2 (format 1 emitted every body raw);
     * 3 adds the per-message meta line and the trace's labelled passage.
     * `nm_session_load` is the half that is still missing; this is the
     * comment it will read, and every body's bytes are counted in meta
     * so that reading it back needs no delimiter at all. */
    fputs("<!-- nevermore session-format: 3 -->\n", f);
    for (size_t i = 0; i < s->n; i++) {
        const NmSessionMessage *m = &s->msgs[i];
        fprintf(f, "\n## %s", role_tag(m->role));
        if (m->tool_name) {
            fputs(" (", f);
            heading_field(f, m->tool_name);
            fputc(')', f);
        }
        if (m->tool_call_id) {
            fputs(" id=", f);
            heading_field(f, m->tool_call_id);
        }
        fputc('\n', f);
        char *meta = meta_json(s, m);
        if (!meta) {
            /* A message without its meta is a lie about its own
             * structure: no file at all beats a file that cannot be
             * read back. */
            fclose(f);
            remove(path);
            return -1;
        }
        fputs("<!-- meta: ", f);
        comment_write(f, meta);
        fputs(" -->\n", f);
        free(meta);
        /* A message carrying a trace labels EVERY part — that is what
         * tells a reader where the trace ended and the answer began.
         * Without a trace the section keeps format 2's shape: one
         * unlabelled body. */
        int labeled = m->reasoning && *m->reasoning;
        for (int k = 0; k < 2; k++) {
            const char *name, *body;
            if (!message_part(m, k, &name, &body))
                continue;
            part_write(f, labeled ? name : NULL, m->role, body);
        }
        if (m->tool_calls_json && *m->tool_calls_json) {
            fputs("\n<!-- tool_calls: ", f);
            comment_write(f, m->tool_calls_json);
            fputs(" -->\n", f);
        }
    }
    fclose(f);
    return 0;
}

NmSession *nm_session_load(const char *path)
{
    /* The load half, not built yet: read each meta line, consume exactly
     * the counted bodies, refuse on any mismatch, on a format it does not
     * know, or on a message declaring images (the bytes are not in the
     * file). Two obligations it must keep from the format's own rules:
     * the meta comment is the FIRST line after the heading LF (anything
     * between them is a structural error — that is what the heading's
     * escape makes checkable), and every tool fact comes from meta, never
     * from the heading, which is display-only. test_session.c's reader is
     * its prototype; save is the inspection surface today. */
    (void)path;
    return NULL;
}
