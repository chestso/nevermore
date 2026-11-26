/* image.h - the transcript IMAGE tier's policy half.
 *
 * boba owns the transports (kitty APC / iTerm2 1337) and the commit
 * gate; this module owns every decision around them: parsing the
 * image markdown line's source, obtaining the bytes (data URIs and
 * local paths - the two sources whose bytes are already ours; remote
 * URLs degrade by design), sniffing
 * dimensions from the container headers, choosing the transport from
 * the terminal profile, sizing the display in cells, and PREPARING the
 * display payload: every raster lane decodes the source once and
 * rebuilds it at the exact cell-box pixel rect (fit-width, top-left,
 * transparent sliver) through image_codec — a PNG for kitty/iTerm2, an
 * INDEXED8 quantization for sixel. A terminal scales the payload to the
 * reserved cells, so a payload already at that pixel size renders 1:1
 * (the chafa rule) instead of stretching the source. The prep is
 * display-local; the wire bytes are never touched.
 *
 * The state is a ONE-SLOT cache reached through
 * TuiTranscriptConfig.user_data (NmMarkdownRenderState.img): boba
 * renders emission units synchronously one at a time, calling
 * measure_image then render_image back-to-back for each IMAGE unit,
 * so a single slot keyed by TuiBlock.image_id suffices - the buffers
 * are grown geometrically and reused across images, never reallocated
 * per image (memory-reuse principle; one allocation per image event
 * is the event, not churn).
 */

#ifndef NM_IMAGE_H
#define NM_IMAGE_H

#include <stddef.h>

#include <boba/stream.h>

#include "image_bytes.h"
#include "image_codec.h"

/* There is NO display-side size cap:
 * the WIRE cap (NM_IMAGE_MAX_WIRE_BYTES, image_bytes.h) is the ONE
 * cap, and it sits where the bytes are actually committed — the
 * attach. An image that is in the conversation has already paid for
 * its bytes (they ride every request), so refusing to RENDER one here
 * would only make the transcript lie: the front door
 * (nm_image_supported) answers from the tier table alone, so a size
 * refusal at measure time posts a block the commit pass then degrades.
 * Render memory is bounded by that same wire cap — one decoded copy in
 * the slot plus boba's transient staged base64, both released with the
 * batch — and a bound beyond that belongs to the renderer that stages
 * the bytes, not to this policy. */

/* The one-slot image state. Owned by the app's render state; zeroing
 * is a valid empty state (image_id 0 can never match a real unit:
 * boba's counter starts at 1). */
typedef struct NmImageSlot
{
    int image_id;             /* the unit this slot holds; 0 = empty          */
    unsigned char *data;      /* grown, reused across images           */
    size_t cap;               /* allocation size of data                      */
    size_t len;               /* decoded byte count (the bytes in data)       */
    size_t src_bytes;         /* the SOURCE's byte count — what the marker
                               * prints; equals len except when a bounded
                               * probe held only the head */
    int w, h;                 /* source pixels (0 = unknown)                  */
    int format;               /* NmImageFormat of the SOURCE
                               * (image_bytes.h); boba's TuiImageFormat
                               * appears only at the spec */
    int transport;            /* TuiImageTransport, -1 = degrade            */
    int disp_cols, disp_rows; /* the committed display size (cells) */
    char reason[48];          /* degradation reason (empty = rendered)     */

    /* The DISPLAY payload: the cell-box raster derived from the source
     * by image_codec, owned by the slot and reused across images (grown
     * geometrically like `data`). Every raster lane fills it — a PNG
     * (kitty, iTerm2) or one palette index per pixel (sixel, pix_w *
     * pix_h bytes) with the palette below. The spec takes `enc`/
     * `enc_len` and render_format; the source bytes in `data` are the
     * wire's and never ride to the terminal. */
    unsigned char *enc;
    size_t enc_cap, enc_len;
    int render_format; /* the format handed to boba (an NmImageFormat) */

    /* The DERIVED payload's pixel dims — the cell-box rect the terminal
     * will occupy (disp_cols*cell_w × disp_rows*cell_h, band-aligned for
     * sixel), which differ from the source's w/h; the spec's src_w/src_h
     * take them. ncolors is the sixel palette's entry count, 0 for the
     * PNG lanes. */
    int pix_w, pix_h;
    int ncolors;
    unsigned char palette[NM_IMAGE_SIXEL_MAX_COLORS * 3];
} NmImageSlot;

/* Drop the held unit but KEEP the buffer (it is the reuse); used by
 * render-state re-init on transcript clear. Zeroed memory is a valid
 * empty slot (image_id 0 never matches: boba's counter starts at 1),
 * so there is no separate init. */
void nm_image_slot_reset(NmImageSlot *s);

/* Release the slot's buffer (app teardown). */
void nm_image_slot_free(NmImageSlot *s);

/* Would a source in this container render as an image under this
 * profile? The tier table's front door, asked BEFORE any bytes exist:
 * 1 = the transcript will render it, 0 = it would degrade to the
 * marker (no graphics support, a container neither a native transport
 * nor a transcode can carry, or a profile that has not reached its
 * verdict yet). This is the "if supported" gate chat_app's /image reads
 * when it decides whether to display an attached image right there at
 * the point of attach; the same table nm_image_measure picks the
 * transport from, so the answer cannot drift from what the commit pass
 * will do. It answers the TIER, not "these bytes decode": a corrupt
 * payload (or one past image_codec's pixel screen) still degrades
 * at commit, exactly as a PNG always could. */
int nm_image_supported(const TuiTerminalProfile *p, int format);

/* TuiTranscriptConfig.measure_image: parse the unit's image line,
 * load the source bytes, sniff the dimensions, pick the tier from the
 * profile, compute the display cells and prepare the cell-box payload
 * (decode + fit-pad + encode). Fills the slot (user_data:
 * NmMarkdownRenderState). Returns 1 with *out_rows >= 1 when the unit
 * will render as an image, 0 to degrade to the render_block marker
 * (the slot still carries what it learned - dims, format, reason -
 * for the marker's text). */
int nm_image_measure(const TuiBlock *blk, const char *text, size_t len,
                     const TuiTerminalProfile *profile, int *out_rows,
                     void *user_data);

/* TuiTranscriptConfig.render_image: emit the measured unit through
 * tui_row_image (borrowing the slot's buffer for the call's
 * duration) and close its row — boba writes the terminators the
 * transport needs (a sixel or an iTerm2 row closes with none: the
 * terminal lands the cursor below the picture). Only reached when
 * measure returned 1. */
void nm_image_render(const TuiBlock *blk, const char *text, size_t len,
                     int col_span, int rows, TuiRowSink *sink,
                     void *user_data);

/* The container's short name for markers ("PNG" / "JPEG" / "GIF";
 * "image" for unknown) lives in image_bytes.h, with the sniffer
 * that decides it — re-exported through this header's include so the
 * renderer pair (markdown_render.c) reads it from the slot's
 * vocabulary. */

#endif /* NM_IMAGE_H */
