/* nm_image_codec.h - the image tier's PIXEL half.
 *
 * Container sniffing and base64 live in nm_image_bytes (the byte half,
 * headers only, never a decode). THIS module is the other half: it
 * turns bytes into pixels and back, and it is the ONE translation unit
 * that includes the vendored stb single-file libraries (D14):
 *
 *   third_party/stb/stb_image.h        decode -> RGBA (JPEG/PNG/GIF/..)
 *   third_party/stb/stb_image_write.h  encode RGBA -> PNG
 *
 * Both are public domain (Unlicense), auto-fetched at configure time
 * by scripts/fetch-stb.sh (see ../configure.ac); they are gitignored,
 * exactly as ../coffer does it.
 *
 * Why a decode at all: kitty graphics' f=100 transport is PNG-only, so
 * a JPEG the provider returns (Gemini does) has no ride to the terminal
 * without a re-encode. The display tier asks this module for that, and
 * the wire bytes are never touched (the derived PNG is display-local,
 * freed with the render).
 *
 * Pure C, no boba, linkable without boba (the same rule as the byte
 * half), so test_image and the session tests stay boba-free.
 */

#ifndef NM_IMAGE_CODEC_H
#define NM_IMAGE_CODEC_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The render-side pixel screen (D10). NOT a wire cap: the source bytes
 * are already bounded (NM_IMAGE_MAX_WIRE_BYTES, the attach), but a
 * compressed source's DECODED size is not — a few hundred KB is a
 * classic decompression bomb. stb's own guards only reach ~2 GB
 * (STBI_MAX_DIMENSIONS per side + stbi__mad3sizes_valid), far past a
 * preview's budget, so the decode is screened on the dims the sniffer
 * already reads. 32 MP => 128 MiB of RGBA, mirroring coffer's
 * IMG_LIVE_MAX (image_store.h). Not a config key, and it is a
 * *safety floor*, not policy: an over-screen source degrades to the
 * same marker rung as a corrupt one (D9/D10). */
#define NM_IMAGE_MAX_PIXELS ((size_t)32 * 1024 * 1024)

/* How a codec call ended. Typed so the caller spells its own marker
 * line (the byte half's NmImageStatus is the same idea). */
typedef enum
{
    NM_CODEC_OK = 0,
    NM_CODEC_ERR_DECODE,    /* not decodable (corrupt / unsupported) */
    NM_CODEC_ERR_TOO_LARGE, /* over NM_IMAGE_MAX_PIXELS               */
    NM_CODEC_ERR_NOMEM
} NmCodecStatus;

/* Decode `n` bytes of a container the wire takes (PNG/JPEG/GIF) to an
 * RGBA (4-channel, tightly packed) buffer, sized *w × *h. On
 * NM_CODEC_OK, *rgba_out is a malloc'd buffer the caller owns (freed
 * with free()) — stb allocates it, so it is one allocation per image
 * event, not a per-token churn. On any other status *rgba_out is NULL
 * and the dims are left untouched. The pixel screen (NM_IMAGE_MAX_PIXELS)
 * is applied from our own sniffer BEFORE stb is called, so a bomb
 * never reaches the allocator. */
NmCodecStatus nm_image_decode_rgba(const unsigned char *bytes, size_t n,
                                   int *w, int *h, unsigned char **rgba_out);

/* Worst-case PNG byte count for an RGBA (4-channel) w×h image, so a
 * caller can size a reused buffer once (the deflate stream never grows
 * past the filtered scanlines by more than a fixed block overhead).
 * 0 when w or h is not positive. */
size_t nm_png_encode_bound(int w, int h);

/* Encode a tightly packed RGBA image to a PNG in `dst` (cap bytes).
 * Returns the PNG length, or -1 when the output does not fit / the
 * encode fails (the caller falls back to the marker). `dst` should be
 * sized with nm_png_encode_bound(). */
long nm_png_encode(const unsigned char *rgba, int w, int h,
                   unsigned char *dst, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* NM_IMAGE_CODEC_H */
