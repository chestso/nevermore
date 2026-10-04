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

/* Wire cap on an attached image (decoded bytes). The bytes ride EVERY
 * request — chat/completions has no upload/reference endpoint — so this
 * bounds the body, not the display. Not a config key: the degradation
 * ladder is the switch, and a knob without a user is noise. The display
 * cap (NM_IMAGE_MAX_BYTES, nm_image.h) is a DIFFERENT fact — render
 * memory — and the two are allowed to disagree: a 4 MiB photo sends
 * fine and renders as its marker. */
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
    NM_IMAGE_ERR_UNKNOWN,    /* no container the sniffer knows */
    NM_IMAGE_ERR_NOMEM
} NmImageStatus;

/* Sniff the container + dimensions from the head of a buffer: header
 * fields only, no pixel decode (kitty takes PNG containers via f=100,
 * iTerm2 decodes its own). Returns NM_IMAGE_FMT_UNKNOWN when the bytes
 * are not a container we know; w and h (both required, non-NULL)
 * receive the dimensions of the container that matched. */
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

/* "24 B" / "812.0 KiB" / "1.2 MiB" — the attach line's size field, in
 * both the TUI and ask mode (one spelling, one place). */
void nm_image_size_text(size_t bytes, char *out, size_t cap);

/* One file read + sniff, shared by both halves. */
typedef struct NmImageProbe
{
    unsigned char *bytes; /* heap: the file's bytes — or a 64-byte HEAD
                           * when the file is over `max`, so the marker
                           * can still name the format and dims */
    size_t len;           /* bytes held */
    size_t file_bytes;    /* the size the OS reported */
    NmImageFormat format; /* NM_IMAGE_FMT_UNKNOWN when unrecognised */
    int w, h;             /* source pixels (0 = unknown) */
    NmImageStatus status; /* NM_IMAGE_OK only for a readable container
                           * with positive dims */
} NmImageProbe;

/* Read the file ONCE (bounded by `max`) and sniff it. On any failure
 * the probe still carries whatever was learned (a HEAD's format/dims,
 * the file's size), so a marker and a refusal line can both be honest.
 * out->bytes is heap-owned: release it with nm_image_probe_free. */
NmImageStatus nm_image_file_probe(const char *path, size_t max,
                                  NmImageProbe *out);
void nm_image_probe_free(NmImageProbe *p);

#ifdef __cplusplus
}
#endif

#endif /* NM_IMAGE_BYTES_H */
