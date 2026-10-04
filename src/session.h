/* session.h - conversation and context-window state
 *
 * The session is the persistent chat transcript plus context-window
 * management (the job quoth-context.el does in Elisp). Messages are
 * stored as plain-text role/content pairs with optional tool-call
 * annotations and optional image references, so the wire format stays a
 * provider concern. Images are captured ONCE at attach into the
 * session's store (NmImage); messages carry indices into it.
 *
 * Memory model: one growable message array per session, grown
 * geometrically; message strings are heap-owned copies made once at
 * append. nm_session_context returns a borrowed view (pointer into a
 * per-session view array, also reused across calls) — valid until
 * the next session mutation or free.
 */

#ifndef NM_SESSION_H
#define NM_SESSION_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct NmSession NmSession;

typedef enum
{
    NM_ROLE_SYSTEM,
    NM_ROLE_USER,
    NM_ROLE_ASSISTANT,
    NM_ROLE_TOOL
} NmRole;

/* A captured image (VISION-PLAN §3: capture, not reference). The bytes
 * are read from the file exactly ONCE, at attach, and frozen here as the
 * canonical `data:` URL — later rounds never re-read the file. Two
 * reasons, both hard: the file can change mid-conversation (a re-read
 * would silently swap the image the conversation is about), and the
 * provider's prefix cache keys on the serialized bytes (re-derived bytes
 * that differ even by a base64 line break throw the whole cached prefix
 * away).
 *
 * One representation, two consumers: `part_json` is what rides the wire
 * (embedded VERBATIM — base64 is escape-free, so no per-round copy), and
 * `data_url` is the same bytes the transcript's IMAGE block displays
 * (the display path parses, sniffs and sizes it itself). The two cannot
 * disagree because they are the same string.
 *
 * The store is owned by the session, so it dies with the chat (a
 * /provider switch rebuilds the agent and wipes the session — images
 * included). */
typedef struct NmImage
{
    char *data_url; /* "data:image/png;base64,...." — THE bytes */
    size_t data_url_len;
    char *part_json; /* the image_url content part, pre-serialized */
    size_t part_json_len;
    char alt[64]; /* the file's base name (marker/alt text) */
    int format;   /* NmImageFormat (nm_image_bytes.h) */
    int w, h;     /* source pixels (0 = unknown) */
    size_t bytes; /* decoded byte count (the attach line) */
} NmImage;

typedef struct NmSessionMessage
{
    NmRole role;
    char *content;         /* heap-owned; may be NULL (tool-call-only) */
    char *tool_calls_json; /* NM_ROLE_ASSISTANT: wire tool_calls array, or NULL */
    char *tool_call_id;    /* NM_ROLE_TOOL: answered call id, or NULL */
    char *tool_name;       /* NM_ROLE_TOOL: tool that produced this result */
    /* NM_ROLE_USER: indices into the session's image store (not copies),
     * or NULL when the message carries none. Fixed when the message is
     * appended (append-only: a prefix that gains or loses an image part
     * is a different prefix, so the list never changes afterwards). */
    size_t *images;
    size_t n_images;
    /* NM_ROLE_ASSISTANT: the round's reasoning trace. Kept for
     * display; re-sent as reasoning_content only when the agent's echo
     * mode says so (`tools` on every tool-call message, `all` on those
     * plus every assistant message with a trace — nm_agent_reasoning_echo
     * / the store's `reasoning_echo` key). What the echo is for:
     * DeepSeek's thinking-mode replay check, observed on opencode:go
     * (docs/OPENCODE-API.md §3), plus docs/HYPER-API.md's inherited,
     * unverified claim for hyper. NULL/"" when the round streamed
     * none — a tool-call round then rides back an empty
     * `reasoning_content` anyway, because the check tests the field's
     * presence, not its content. */
    char *reasoning;
} NmSessionMessage;

NmSession *nm_session_new(const char *system_prompt);
void nm_session_free(NmSession *s);

/* Append-only transcript. */
const NmSessionMessage *nm_session_append(NmSession *s, NmRole role,
                                          const char *content);
/* Append an assistant message carrying a reasoning trace (content
 * may be NULL for a tool-call-only turn). */
const NmSessionMessage *nm_session_append_reasoning(NmSession *s,
                                                    const char *reasoning,
                                                    const char *content);
/* Append an assistant message carrying a tool_calls array, with the
 * round's reasoning echoed back (reasoning may be NULL/""). */
const NmSessionMessage *nm_session_append_tool_call(
    NmSession *s, const char *tool_calls_json, const char *reasoning);
const NmSessionMessage *nm_session_append_tool_result(NmSession *s,
                                                      const char *tool_call_id,
                                                      const char *tool_name,
                                                      const char *output);

/* --- images (VISION-PLAN) ----------------------------------------- */

/* Attach a file to the conversation: read it ONCE (bounded by
 * NM_IMAGE_MAX_WIRE_BYTES — the bytes ride every request, so the wire
 * cap is the attach cap), sniff the container, and freeze the canonical
 * data URL + its pre-serialized wire part. Returns the new image's
 * index into the session's store (>= 0), or -1 with `reason` filled
 * (a short, user-visible phrase: "source unreadable", "unknown
 * container", "too large", "no memory").
 *
 * Attaching does not touch the transcript: the returned id is the
 * caller's handle until a message claims it (the app's pending set). */
long nm_session_attach_image(NmSession *s, const char *path, char *reason,
                             size_t reason_cap);

/* Attach bytes already in hand (a tool that read the file itself, e.g.
 * read_file's image branch): the same core as the path variant — sniff
 * the buffer (header-only), enforce the wire cap, freeze the data URL +
 * wire part into the store — but from a caller-owned buffer, so the
 * file is read ONCE (the tool's read) and capture-not-reference holds
 * even if the file changes between the read and the attach. `alt` is
 * the marker/alt text (the tool passes the base name). The store copies
 * the bytes into the frozen base64 URL; the caller's buffer is not
 * retained. Returns the new index, or -1 with `reason` filled — the
 * same vocabulary as the path variant ("too large", "unknown
 * container", "no memory", "empty file"). */
long nm_session_attach_image_bytes(NmSession *s, const unsigned char *bytes,
                                   size_t len, const char *alt, char *reason,
                                   size_t reason_cap);

/* The store: a borrowed image by index (NULL when out of range), and
 * how many are attached. */
const NmImage *nm_session_image(const NmSession *s, size_t idx);
size_t nm_session_image_count(const NmSession *s);

/* Append the user message that owns images (the text part is ALWAYS
 * present — the wire shape is frozen: text first, then the image parts
 * in order). An empty/NULL text with images pending becomes the
 * deterministic "<alt> attached" (one name, or "N images attached"),
 * never an empty part and never a missing one. Returns NULL without
 * appending when any id is out of range. */
const NmSessionMessage *nm_session_append_user_images(NmSession *s,
                                                      const char *text,
                                                      const size_t *image_ids,
                                                      size_t n_images);

size_t nm_session_len(const NmSession *s);
const NmSessionMessage *nm_session_get(const NmSession *s, size_t i);

/* Context-window management. Returns a view of the messages the agent
 * should send.
 *
 *   budget_tokens <= 0  NO TRIM: the whole transcript (system prompt +
 *                       every message). This is the default — the
 *                       agent sends everything and lets the provider
 *                       report "too large", rather than silently
 *                       capping. A window that slid every turn would
 *                       also defeat the provider's prefix cache (cached
 *                       input bills far cheaper), so trimming is
 *                       opt-in.
 *   budget_tokens  > 0  the newest tail that fits `budget_tokens`
 *                       (rough 4-chars-per-token estimate): the system
 *                       prompt, the most recent turns, and never a
 *                       dangling tool-result without its matching tool
 *                       call.
 *
 * The view is valid until the next session mutation. */
typedef struct NmContextView
{
    const NmSessionMessage *const *messages;
    size_t n;
} NmContextView;

NmContextView nm_session_context(const NmSession *s, long budget_tokens);

/* Persistence: load/save as markdown transcript with metadata header.
 * Attached images are NOT written (a data URL is megabytes and the save
 * is an inspection surface); session persistence including images is a
 * post-1.0 item, where images are blobs in the save format and never
 * re-read paths. */
int nm_session_save(const NmSession *s, const char *path);
NmSession *nm_session_load(const char *path);

#ifdef __cplusplus
}
#endif

#endif // NM_SESSION_H
