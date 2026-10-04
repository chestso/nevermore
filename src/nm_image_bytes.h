/* nm_image_bytes.h - the image tier's byte half.
 *
 * Container sniffing (headers only, NEVER a pixel decode), base64 in
 * both directions, the `data:` URL builder, and the one file probe.
 * Pure C — no boba, no transport — because BOTH halves of the tier
 * stand on it: the transcript's display path (nm_image.c, which owns
 * the boba transports) and the conversation's attach path (session.c,
 * which freezes the captured bytes into its image store). One place
 * knows what a PNG header is.
 *
 * The container vocabulary is nevermore's own (NmImageFormat), mapped
 * to boba's TuiImageFormat at the display boundary, so this module can
 * be linked without boba (test_session/test_agent/test_openai_client).
 */

#ifndef NM_IMAGE_BYTES_H
#define NM_IMAGE_BYTES_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum
{
    NM_IMAGE_FMT_UNKNOWN = -1,
    NM_IMAGE_FMT_PNG = 0,
    NM_IMAGE_FMT_JPEG,
    NM_IMAGE_FMT_GIF
} NmImageFormat;

/* What container the bytes ARE — a WIDER question than what the WIRE
 * takes (NmImageFormat above). Recognition is not capability: the tool
 * boundary can name a WebP and refuse it honestly, while nothing about
 * a non-wire container may reach the attach path. The two questions
 * are kept apart by TYPE so that adding a member here can never
 * silently open the wire — nm_image_format_from_kind is the ONE place
 * a kind becomes a wire format, and it answers NM_IMAGE_FMT_UNKNOWN
 * for everything the wire does not take. */
typedef enum
{
    NM_IMAGE_KIND_UNKNOWN = 0,
    NM_IMAGE_KIND_PNG,
    NM_IMAGE_KIND_JPEG,
    NM_IMAGE_KIND_GIF,
    NM_IMAGE_KIND_WEBP
} NmImageKind;

/* Wire cap on an attached image (decoded bytes). The bytes ride EVERY
 * request — chat/completions has no upload/reference endpoint — so this
 * bounds the body, not the display. Not a config key: the degradation
 * ladder is the switch, and a knob without a user is noise. It is the
 * ONE cap: there is no display-side cap (nm_image.h), because an image
 * that is in the conversation has already paid for its bytes — a 4 MiB
 * photo sends fine and renders. */
#define NM_IMAGE_MAX_WIRE_BYTES ((size_t)8 * 1024 * 1024)

/* How a byte-source load ended. Typed (not a string) so each side
 * spells its own user-visible line: the display path's marker ladder
 * and the attach path's refusal share the vocabulary, never the words. */
typedef enum
{
    NM_IMAGE_OK = 0,
    NM_IMAGE_ERR_UNREADABLE, /* open/read failed */
    NM_IMAGE_ERR_EMPTY,      /* the file held no bytes */
    NM_IMAGE_ERR_OVERSIZE,   /* over the caller's cap (a HEAD is held) */
    NM_IMAGE_ERR_UNKNOWN,    /* not attachable: no container the wire takes
                              * (unrecognised, or recognised but unattachable),
                              * or one whose dimensions could not be read —
                              * the probe's `kind`/dims say which */
    NM_IMAGE_ERR_NOMEM
} NmImageStatus;

/* Sniff the container + dimensions from the head of a buffer: header
 * fields only, no pixel decode (kitty takes PNG containers via f=100,
 * iTerm2 decodes its own). This is the RECOGNITION answer; w and h
 * (both required, non-NULL) receive the dimensions of the container
 * that matched. */
NmImageKind nm_image_sniff_kind(const unsigned char *d, size_t n, int *w,
                                int *h);

/* The ONE kind → wire-format conversion. NM_IMAGE_FMT_UNKNOWN for
 * every container the wire does not take — recognition is not
 * capability, so this function is where "may it be sent?" is answered,
 * and the only way a kind reaches the wire gates. */
NmImageFormat nm_image_format_from_kind(NmImageKind kind);

/* The container's short name for a user-visible line ("PNG" / "JPEG" /
 * "GIF" / "WebP"; "image" for unknown). The RECOGNITION vocabulary —
 * nm_image_format_name is the wire's, and is the one the display
 * marker's slot speaks. */
const char *nm_image_kind_name(NmImageKind kind);

/* The same sniff, answered as the WIRE's question: NM_IMAGE_FMT_UNKNOWN
 * for a container we recognise but cannot send. The wire-side consumers
 * (session.c's attach, nm_image.c's tier) ask this; the tool boundary
 * asks nm_image_sniff_kind. */
NmImageFormat nm_image_sniff(const unsigned char *d, size_t n, int *w, int *h);

/* The container's short name ("PNG" / "JPEG" / "GIF"; "image" for
 * unknown) and its MIME type ("image/png" ...; NULL when unknown). */
const char *nm_image_format_name(int format);
const char *nm_image_format_mime(int format);

/* MIME → format ("image/png" etc.); NM_IMAGE_FMT_UNKNOWN when the type
 * is not one of ours. */
NmImageFormat nm_image_format_from_mime(const char *mime, size_t len);

/* Standard-alphabet base64. Decode returns the decoded length, or -1 on
 * malformed input (bad length, garbage, misplaced padding — data URIs
 * are well-formed or they degrade, never half-decode). Encode returns a
 * heap string (NUL-terminated; *out_len set), or NULL on OOM. */
long nm_image_b64_decode(const char *src, size_t len, unsigned char *dst,
                         size_t dst_cap);
char *nm_image_b64_encode(const unsigned char *src, size_t len,
                          size_t *out_len);

/* "data:<mime>;base64,<payload>" — heap-owned, NUL-terminated, with
 * *out_len set. NULL when the format has no MIME type or on OOM. */
char *nm_image_data_url(int format, const unsigned char *bytes, size_t len,
                        size_t *out_len);

/* Worst case for nm_image_describe: a 5-byte name + space + the longest
 * dims ("2147483647x2147483647", 21) + ", " + the longest size text
 * (13) + NUL, with room to spare. */
#define NM_IMAGE_DESC_MAX 64

/* "PNG 64x32, 24 B" — the ONE spelling of an image's facts for the
 * lines that name one: read_file's summary and its refusals, /img's
 * attach and list lines, ask mode's `-i` line. `name` comes from
 * either vocabulary — the wire's (nm_image_format_name) for an image
 * that IS attached, the recognition one (nm_image_kind_name) for one
 * being refused — and the dims are omitted when they are unknown. The
 * display marker does NOT use this: its separators are its own. */
void nm_image_describe(const char *name, int w, int h, size_t bytes,
                       char *out, size_t cap);

/* The attachable containers as prose — "PNG/JPEG/GIF" — derived from
 * the wire's own answer (a format with a MIME type is one the data URL
 * builder can carry), so a refusal line that names the set cannot drift
 * from what the wire takes. Returns the length written, NUL excluded. */
size_t nm_image_attachable_list(char *out, size_t cap);

/* One file read + sniff, shared by both halves. */
typedef struct NmImageProbe
{
    unsigned char *bytes; /* heap: the file's bytes — or a 64-byte HEAD
                           * when the file is over `max`, so the marker
                           * can still name the container and dims */
    size_t len;           /* bytes held */
    size_t file_bytes;    /* the size the OS reported */
    NmImageKind kind;     /* what the bytes ARE, from the HEAD (so a
                           * JPEG is recognised even when its dimensions
                           * sit behind megabytes of metadata);
                           * NM_IMAGE_KIND_UNKNOWN when unrecognised.
                           * The WIRE answer is
                           * nm_image_format_from_kind(kind) — a
                           * recognised container the wire does not take
                           * (WebP) reads NM_IMAGE_FMT_UNKNOWN there */
    int w, h;             /* source pixels (0 = unknown: not in the bytes
                           * we read, or not a container we size) */
    NmImageStatus status; /* NM_IMAGE_OK only for a readable container
                           * the WIRE takes, with positive dims */
} NmImageProbe;

/* Read the file ONCE (bounded by `max`) and sniff it. On any failure
 * the probe still carries whatever was learned (a HEAD's kind/dims, the
 * file's size), so a marker and a refusal line can both be honest.
 * out->bytes is heap-owned: release it with nm_image_probe_free. */
NmImageStatus nm_image_file_probe(const char *path, size_t max,
                                  NmImageProbe *out);
void nm_image_probe_free(NmImageProbe *p);

#ifdef __cplusplus
}
#endif

#endif /* NM_IMAGE_BYTES_H */
