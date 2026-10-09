/* session.h - conversation transcript state
 *
 * The session is the persistent chat transcript. Messages are
 * stored as plain-text role/content pairs with optional tool-call
 * annotations and optional image references, so the wire format stays a
 * provider concern. Images are captured ONCE at attach into the
 * session's store (NmImage); messages carry indices into it.
 *
 * Memory model: one growable message array per session, grown
 * geometrically; message strings are heap-owned copies made once at
 * append. nm_session_get returns a borrowed pointer into that array —
 * valid until the next session mutation or free. The WHOLE transcript
 * rides every request (nevermore never trims it: no token-budget
 * guessing), which is what keeps the request prefix byte-stable.
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
    char alt[64]; /* the display name: the file's base name for an
                   * attached image, "image" for a received one (a
                   * generated picture has no file) */
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
    /* NM_ROLE_USER / NM_ROLE_ASSISTANT: indices into the session's
     * image store (not copies), or NULL when the message carries none.
     * Fixed when the message is appended (append-only: a prefix that
     * gains or loses an image part is a different prefix, so the list
     * never changes afterwards). User images ride the content-parts
     * array; assistant images (IMAGEGEN — the round's generated
     * output) ride the message-level "images" array. */
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

/* Replace the session's system prompt (message 0). The transcript is
 * otherwise append-only; this is the ONE mutation, and it exists for
 * the async <env> stage: the agent seeds the session before the git
 * section has landed and swaps the final prompt in when it does. Only
 * safe while no request has been built from the session (the agent
 * calls it before the first round), because a system prompt that
 * changes after a request is on the wire is a different prefix (the
 * provider's prompt cache keys on it). Creates the system message when
 * the session has none. Returns 0, or -1 on OOM. */
int nm_session_set_system(NmSession *s, const char *prompt);

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

/* Attach a RECEIVED image (IMAGEGEN-PLAN §3): the whole image arrived
 * as one `delta.images` event whose payload IS this data URL. The URL
 * is frozen VERBATIM — the received bytes are the canonical part, never
 * parse-and-rebuilt (a re-encode would change the bytes and break the
 * replay prefix AND hand the model a different image than the one it
 * made). No wire cap: the cap bounds what WE choose to send, and a
 * received image is bounded by the provider's own output size (record,
 * do not refuse).
 *
 * The payload is base64-decoded ONCE here, into scratch, for the
 * marker facts (container, dims, decoded size); the decoded bytes are
 * not kept. Refusals: not a base64 `data:` URL ("not a base64 data
 * URL"), an undecodable payload ("undecodable payload"), OOM ("no
 * memory"). Returns the new index, or -1 with `reason` filled.
 *
 * A received image has no name of its own, so the store gives it the
 * noun ("image") and the ID returned here — the CHAT-scoped index —
 * is the handle: the UI prints it as the block's caption and /image
 * save takes it. */
long nm_session_attach_image_url(NmSession *s, const char *url, size_t len,
                                 char *reason, size_t reason_cap);

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

/* Append the ASSISTANT message for a round that produced images
 * (IMAGEGEN-PLAN §3/§4): the round's content (NULL/"" stays a plain
 * empty string on the wire), reasoning trace, optional tool_calls
 * array, and the image ids — one message, so the round's output is
 * one prefix-stable unit. The wire shape for these is the probed
 * message-level "images" array (docs/OPENROUTER-API.md §5.1). Returns
 * NULL without appending when any id is out of range. */
const NmSessionMessage *nm_session_append_assistant_images(
    NmSession *s, const char *reasoning, const char *content,
    const char *tool_calls_json, const size_t *image_ids, size_t n_images);

/* The transcript, as the agent sends it: message 0 is the system
 * prompt when the session has one, and EVERY stored message rides the
 * next request. nevermore never trims the transcript (a guessed token
 * budget would silently cap the conversation and break the request
 * prefix the provider's cache keys on), so a too-large context is the
 * provider's error to report, verbatim. */
size_t nm_session_len(const NmSession *s);
const NmSessionMessage *nm_session_get(const NmSession *s, size_t i);

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
