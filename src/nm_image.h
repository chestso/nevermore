/* nm_image.h - the transcript IMAGE tier's policy half.
 *
 * boba owns the transports (kitty APC / iTerm2 1337) and the commit
 * gate; this module owns every decision around them: parsing the
 * image markdown line's source, obtaining the bytes (data URIs and
 * local paths - the two sources whose bytes are already ours; remote
 * URLs degrade by design, see TRANSCRIPT-IMAGE-PLAN), sniffing
 * dimensions from the container headers (NO pixel decode: kitty
 * takes PNG via f=100 and iTerm2 decodes its own containers),
 * choosing the transport from the terminal profile, and sizing the
 * display in cells.
 *
 * The state is a ONE-SLOT cache reached through
 * TuiTranscriptConfig.user_data (NmMarkdownRenderState.img): boba
 * renders emission units synchronously one at a time, calling
 * measure_image then render_image back-to-back for each IMAGE unit,
 * so a single slot keyed by TuiBlock.image_id suffices - the buffer
 * is grown geometrically and reused across images, never reallocated
 * per image (memory-reuse principle; one allocation per image event
 * is the event, not churn).
 */

#ifndef NM_IMAGE_H
#define NM_IMAGE_H

#include <stddef.h>

#include <boba/stream.h>

/* Policy cap on a decoded payload: images larger than this degrade
 * to their marker ("too large"). A memory bound, not correctness;
 * sized so the base64 copy in boba's staging stays comfortably under
 * TUI_TRANSCRIPT_STAGED_CAP. Not a config key (D13): the degradation
 * ladder is the switch, dumb terminals see markers. */
#define NM_IMAGE_MAX_BYTES (1024 * 1024)

/* The one-slot image state. Owned by the app's render state; zeroing
 * is a valid empty state (image_id 0 can never match a real unit:
 * boba's counter starts at 1). */
typedef struct NmImageSlot
{
    int image_id;             /* the unit this slot holds; 0 = empty          */
    unsigned char *data;      /* grown, reused across images           */
    size_t cap;               /* allocation size of data                      */
    size_t len;               /* decoded byte count                           */
    int w, h;                 /* source pixels (0 = unknown)                  */
    int format;               /* TuiImageFormat (TUI_IMAGE_PNG/...)           */
    int transport;            /* TuiImageTransport, -1 = degrade            */
    int disp_cols, disp_rows; /* the committed display size (cells) */
    char reason[48];          /* degradation reason (empty = rendered)     */
} NmImageSlot;

/* Drop the held unit but KEEP the buffer (it is the reuse); used by
 * render-state re-init on transcript clear. Zeroed memory is a valid
 * empty slot (image_id 0 never matches: boba's counter starts at 1),
 * so there is no separate init. */
void nm_image_slot_reset(NmImageSlot *s);

/* Release the slot's buffer (app teardown). */
void nm_image_slot_free(NmImageSlot *s);

/* TuiTranscriptConfig.measure_image: parse the unit's image line,
 * load the source bytes, sniff the dimensions, pick the transport
 * from the profile and compute the display cells. Fills the slot
 * (user_data: NmMarkdownRenderState). Returns 1 with *out_rows >= 1
 * when the unit will render as an image, 0 to degrade to the
 * render_block marker (the slot still carries what it learned -
 * dims, format, reason - for the marker's text). */
int nm_image_measure(const TuiBlock *blk, const char *text, size_t len,
                     const TuiTerminalProfile *profile, int *out_rows,
                     void *user_data);

/* TuiTranscriptConfig.render_image: emit the measured unit through
 * tui_row_image (borrowing the slot's buffer for the call's
 * duration) and end the reserved rows. Only reached when measure
 * returned 1. */
void nm_image_render(const TuiBlock *blk, const char *text, size_t len,
                     int col_span, int rows, TuiRowSink *sink,
                     void *user_data);

/* The container's short name for markers ("PNG" / "JPEG" / "GIF");
 * "image" for unknown. */
const char *nm_image_format_name(int format);

#endif /* NM_IMAGE_H */
