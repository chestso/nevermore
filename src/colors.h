/* colors.h - semantic color roles (the one place color lives)
 *
 * Dracula Classic (https://draculatheme.com/spec) is the app's palette.
 * Values are the spec's official tokens, chosen for dark backgrounds;
 * adaptive/light (Alucard) is a non-goal. Grep this file, not the call
 * sites, when a role's color changes.
 *
 * Three consumers, one table:
 *
 *   - the system-stream byte writers in chat_app.c (tool panels, error
 *     bodies, command replies) build a line with vsnprintf, so they
 *     need byte-exact SGR strings: NM_SGR_*.
 *   - the markdown renderers (markdown_render.c) write through a
 *     boba TuiRowSink, which wants a TuiAttr: nm_attr_*().
 *   - the chat frame (prompt, popup, the input row's gutter) draws
 *     through boba styles, which want a TuiColor: nm_color_*().
 *
 * Naming symmetry: every role has a TuiAttr constructor; only the
 * roles the byte writers actually use need an NM_SGR_* alias (a
 * byte-side writer never dims or bolds - SGR aliases stay a subset on
 * purpose).
 */

#ifndef NM_COLORS_H
#define NM_COLORS_H

#include <string.h>

#include <boba/stream.h>
#include <boba/style.h>

/* ------------------------------------------------------------------ */
/* Dracula Classic palette (RGB components)                            */
/* ------------------------------------------------------------------ */

/* Comment #6272A4 - muted structural text (panels, gutters, borders,
 * delimiters, code comments). */
#define NM_DRACULA_COMMENT_R 98
#define NM_DRACULA_COMMENT_G 114
#define NM_DRACULA_COMMENT_B 164

/* Foreground #F8F8F2 - default body text. */
#define NM_DRACULA_FG_R 248
#define NM_DRACULA_FG_G 248
#define NM_DRACULA_FG_B 242

/* Selection #44475A - selected-row background. */
#define NM_DRACULA_SELECTION_R 68
#define NM_DRACULA_SELECTION_G 71
#define NM_DRACULA_SELECTION_B 90

/* Red #FF5555 - errors. */
#define NM_DRACULA_RED_R 255
#define NM_DRACULA_RED_G 85
#define NM_DRACULA_RED_B 85

/* Orange #FFB86C - numbers, constants, booleans. */
#define NM_DRACULA_ORANGE_R 255
#define NM_DRACULA_ORANGE_G 184
#define NM_DRACULA_ORANGE_B 108

/* Yellow #F1FA8C - strings, text content. */
#define NM_DRACULA_YELLOW_R 241
#define NM_DRACULA_YELLOW_G 250
#define NM_DRACULA_YELLOW_B 140

/* Green #50FA7B - inline code (functions, support). */
#define NM_DRACULA_GREEN_R 80
#define NM_DRACULA_GREEN_G 250
#define NM_DRACULA_GREEN_B 123

/* Cyan #8BE9FD - structural accent (the tool-result elbow), types. */
#define NM_DRACULA_CYAN_R 139
#define NM_DRACULA_CYAN_G 233
#define NM_DRACULA_CYAN_B 253

/* Purple #BD93F9 - minor headings (instance words, constants). */
#define NM_DRACULA_PURPLE_R 189
#define NM_DRACULA_PURPLE_G 147
#define NM_DRACULA_PURPLE_B 249

/* Pink #FF79C6 - the app accent: prompt, bullets, major headings,
 * keywords. */
#define NM_DRACULA_PINK_R 255
#define NM_DRACULA_PINK_G 121
#define NM_DRACULA_PINK_B 198

/* ------------------------------------------------------------------ */
/* The terminal's color tier                                           */
/* ------------------------------------------------------------------ */

/* Which of the spec's two tables this terminal can be handed. The spec
 * publishes BOTH on purpose (spec.draculatheme.com §1.2.1 Standard,
 * §1.2.2 ANSI): an editor paints the Standard hexes, while a 16-color
 * terminal can only be asked for a palette INDEX — and a 24-bit
 * sequence on such a terminal is the thing this tier exists to avoid.
 * boba emits exactly what a TuiColor says (an RGB TuiColor is a
 * `38;2;…` and nothing else, see style.c), so the tier is the APP's
 * decision: the caller resolves it from boba's
 * TuiTerminalProfile::truecolor and a depth-aware role picks its
 * spelling. Lives here because it is a COLOR fact, not an animation
 * one — the spinner is only its first consumer. */
typedef enum
{
    NM_COLOR_16 = 0,
    NM_COLOR_RGB = 1
} NmColorDepth;

/* The spec's ANSI table (§1.2.2), by palette index: the 16-color tier's
 * spelling of a role. A terminal shows whatever ITS palette holds at
 * that index, and under Dracula's own terminal themes each index
 * carries the hex named here — which is what makes an ANSI spelling an
 * EXACT rendering rather than a nearest-neighbour guess at a hex. Only
 * the indices a depth-aware role needs live here; the spinner's two are
 * named in spinner.c, beside the animation they belong to. */
#define NM_DRACULA_ANSI_CYAN 6 /* #8BE9FD — AnsiCyan; Cyan's own hex */

/* ------------------------------------------------------------------ */
/* SGR strings (raw byte writers)                                      */
/* ------------------------------------------------------------------ */

/* Dracula Comment - the tool plan header (its emoji lead), the
 * interrupted marker, the provider separator, the /ps state of an
 * exited job. (The spinner label and the context gauge moved to the
 * input row's gutter, painted through TuiStyle spans - see
 * nm_color_gutter below.) */
#define NM_SGR_TOOL "\033[38;2;98;114;164m"

/* Dracula Foreground - the tool result body, secondary text. */
#define NM_SGR_RESULT "\033[38;2;248;248;242m"

/* Dracula Yellow - the ACTIVITY role: the /ps state of a RUNNING job
 * (its exited state is muted Comment). Not the spinner: the spinner's
 * glyph carries its own color in its frame (a green dot, or the star
 * set's yellow), so the animation and the job table no longer share a
 * role — they only ever shared one by accident of both meaning
 * "working". */
#define NM_SGR_ACTIVITY "\033[38;2;241;250;140m"

/* Dracula Cyan - the `╰─` tool-result elbow: the structural accent that
 * ties a result body to its panel. Its own role on purpose: Comment
 * would read as panel, Foreground as body text. */
#define NM_SGR_TOOL_ELBOW "\033[38;2;139;233;253m"

/* Dracula Red - errors (`nevermore: …`). */
#define NM_SGR_ERROR "\033[38;2;255;85;85m"

/* Dracula Purple - the harness's own speech: a `<system-reminder>`
 * block inside a tool result (its tag lines and body) and the reminder
 * line the UI prints for the user channel. Its own role on purpose: a
 * reminder must never read as tool output (Foreground), as the model's
 * words, or as an error — it is the harness talking, and the human must
 * be able to tell (the transparency principle). */
#define NM_SGR_REMINDER "\033[38;2;189;147;249m"

#define NM_SGR_RESET "\033[0m"

/* ------------------------------------------------------------------ */
/* TuiColor constructors (chat frame: prompt, popup)                   */
/* ------------------------------------------------------------------ */

/* User prompt `❯ ` - the app accent. */
static inline TuiColor nm_color_prompt(void)
{
    return tui_color_rgb(NM_DRACULA_PINK_R, NM_DRACULA_PINK_G,
                         NM_DRACULA_PINK_B);
}

/* Completion popup: muted border/title, Selection highlight, accent
 * marker, Foreground items. */
static inline TuiColor nm_color_popup_border(void)
{
    return tui_color_rgb(NM_DRACULA_COMMENT_R, NM_DRACULA_COMMENT_G,
                         NM_DRACULA_COMMENT_B);
}

static inline TuiColor nm_color_popup_title(void)
{
    return tui_color_rgb(NM_DRACULA_COMMENT_R, NM_DRACULA_COMMENT_G,
                         NM_DRACULA_COMMENT_B);
}

static inline TuiColor nm_color_popup_selected_bg(void)
{
    return tui_color_rgb(NM_DRACULA_SELECTION_R, NM_DRACULA_SELECTION_G,
                         NM_DRACULA_SELECTION_B);
}

static inline TuiColor nm_color_popup_selected_fg(void)
{
    return tui_color_rgb(NM_DRACULA_FG_R, NM_DRACULA_FG_G, NM_DRACULA_FG_B);
}

static inline TuiColor nm_color_popup_marker(void)
{
    return tui_color_rgb(NM_DRACULA_PINK_R, NM_DRACULA_PINK_G,
                         NM_DRACULA_PINK_B);
}

static inline TuiColor nm_color_popup_item(void)
{
    return tui_color_rgb(NM_DRACULA_FG_R, NM_DRACULA_FG_G, NM_DRACULA_FG_B);
}

/* Completion popup metadata column (the model picker's capability
 * badges and context window): muted Comment, so the id stays the
 * primary text and the metadata recedes. */
static inline TuiColor nm_color_popup_meta(void)
{
    return tui_color_rgb(NM_DRACULA_COMMENT_R, NM_DRACULA_COMMENT_G,
                         NM_DRACULA_COMMENT_B);
}

/* The input row's gutter (P2): the status chrome left of the prompt —
 * the spinner glyph and the context gauge. The gauge takes its tier from
 * how full the window is: Comment at rest, Orange at ~85 %, Red at
 * ~95 %. The spinner glyph's color is NOT here: it rides its own frame
 * (nm_spinner_tick), because the dot pulses and blinks and the stars
 * rotate — one live pixel with a color that changes per tick has no
 * business being a static role. */
static inline TuiColor nm_color_gutter(void)
{
    return tui_color_rgb(NM_DRACULA_COMMENT_R, NM_DRACULA_COMMENT_G,
                         NM_DRACULA_COMMENT_B);
}

static inline TuiColor nm_color_gutter_warn(void)
{
    return tui_color_rgb(NM_DRACULA_ORANGE_R, NM_DRACULA_ORANGE_G,
                         NM_DRACULA_ORANGE_B);
}

static inline TuiColor nm_color_gutter_warn_hot(void)
{
    return tui_color_rgb(NM_DRACULA_RED_R, NM_DRACULA_RED_G,
                         NM_DRACULA_RED_B);
}

/* The status row's output-token field — the `↓` and the count it
 * labels: Dracula Cyan. The spec's own vocabulary for a marker is Cyan
 * (MarkupListBulletOrNumber is Cyan, as are ClassName and
 * MarkupLinkUrl), and Cyan is the one hue this row has left: the gauge's
 * counts are Comment (Orange/Red under pressure), the identity
 * Foreground, the spinner glyph its own frame's Green or Yellow. Orange
 * — the spec's numbers role, and what a bare counter might suggest — is
 * not a candidate: it has NO entry in the spec's ANSI table, so it
 * cannot degrade to a 16-color terminal at all.
 *
 * The ANSI tier is EXACT, not an approximation: the spec's AnsiCyan is
 * #8BE9FD, the same value as Cyan, so a terminal running Dracula's
 * palette draws the identical color at 16 colors. */
static inline TuiColor nm_color_gauge_output(NmColorDepth depth)
{
    if (depth == NM_COLOR_RGB)
        return tui_color_rgb(NM_DRACULA_CYAN_R, NM_DRACULA_CYAN_G,
                             NM_DRACULA_CYAN_B);
    return tui_color_ansi(NM_DRACULA_ANSI_CYAN);
}

/* The status row's right-aligned identity block (`provider · model`):
 * Dracula Foreground. The rule and the chrome are structure; this is the
 * one hard fact the row states, so it reads as content, not as chrome
 * (nm_color_gutter's Comment is the row's own muted role). */
static inline TuiColor nm_color_status_identity(void)
{
    return tui_color_rgb(NM_DRACULA_FG_R, NM_DRACULA_FG_G, NM_DRACULA_FG_B);
}

/* ------------------------------------------------------------------ */
/* TuiAttr constructors (renderers)                                    */
/* ------------------------------------------------------------------ */

static inline TuiAttr nm_attr_plain(void)
{
    TuiAttr a;
    memset(&a, 0, sizeof(a));
    return a;
}

static inline TuiAttr nm_attr_foreground(int r, int g, int b)
{
    TuiAttr a = nm_attr_plain();
    a.has_fg = 1;
    a.fg_r = r;
    a.fg_g = g;
    a.fg_b = b;
    return a;
}

/* Reasoning (CoT) text. */
static inline TuiAttr nm_attr_dim(void)
{
    TuiAttr a = nm_attr_plain();
    a.dim = 1;
    return a;
}

/* Headings: h1-h2 bold + Pink, h3-h6 bold + Purple. */
static inline TuiAttr nm_attr_heading_major(void)
{
    TuiAttr a =
        nm_attr_foreground(NM_DRACULA_PINK_R, NM_DRACULA_PINK_G,
                           NM_DRACULA_PINK_B);
    a.bold = 1;
    return a;
}

static inline TuiAttr nm_attr_heading_minor(void)
{
    TuiAttr a =
        nm_attr_foreground(NM_DRACULA_PURPLE_R, NM_DRACULA_PURPLE_G,
                           NM_DRACULA_PURPLE_B);
    a.bold = 1;
    return a;
}

/* List bullet marker (Pink; item text stays the row's base attr). */
static inline TuiAttr nm_attr_list_bullet(void)
{
    return nm_attr_foreground(NM_DRACULA_PINK_R, NM_DRACULA_PINK_G,
                              NM_DRACULA_PINK_B);
}

/* Quote gutter `│` and quoted text. */
static inline TuiAttr nm_attr_quote_gutter(void)
{
    return nm_attr_foreground(NM_DRACULA_COMMENT_R, NM_DRACULA_COMMENT_G,
                              NM_DRACULA_COMMENT_B);
}

static inline TuiAttr nm_attr_quote_text(void)
{
    return nm_attr_foreground(NM_DRACULA_FG_R, NM_DRACULA_FG_G,
                              NM_DRACULA_FG_B);
}

/* Tool-result elbow (`╰─`): the connector that ties a result body to
 * its panel - its own role, neither panel (Comment) nor body
 * (Foreground). */
static inline TuiAttr nm_attr_tool_elbow(void)
{
    return nm_attr_foreground(NM_DRACULA_CYAN_R, NM_DRACULA_CYAN_G,
                              NM_DRACULA_CYAN_B);
}

/* Table borders and header cells (borders Comment, header bold). */
static inline TuiAttr nm_attr_table_border(void)
{
    return nm_attr_foreground(NM_DRACULA_COMMENT_R, NM_DRACULA_COMMENT_G,
                              NM_DRACULA_COMMENT_B);
}

/* Image degradation marker: the one row a terminal without graphics
 * (or a payload that cannot ride one) shows instead of the image. */
static inline TuiAttr nm_attr_image_marker(void)
{
    return nm_attr_foreground(NM_DRACULA_COMMENT_R, NM_DRACULA_COMMENT_G,
                              NM_DRACULA_COMMENT_B);
}

static inline TuiAttr nm_attr_table_header(void)
{
    TuiAttr a = nm_attr_plain();
    a.bold = 1;
    return a;
}

/* Inline code spans and link text (Green). */
static inline TuiAttr nm_attr_code(void)
{
    return nm_attr_foreground(NM_DRACULA_GREEN_R, NM_DRACULA_GREEN_G,
                              NM_DRACULA_GREEN_B);
}

/* Link URL (dim). */
static inline TuiAttr nm_attr_link_url(void) { return nm_attr_dim(); }

/* Fences: delimiter Comment, info string Yellow, body Foreground. */
static inline TuiAttr nm_attr_fence_delim(void)
{
    return nm_attr_foreground(NM_DRACULA_COMMENT_R, NM_DRACULA_COMMENT_G,
                              NM_DRACULA_COMMENT_B);
}

static inline TuiAttr nm_attr_fence_info(void)
{
    return nm_attr_foreground(NM_DRACULA_YELLOW_R, NM_DRACULA_YELLOW_G,
                              NM_DRACULA_YELLOW_B);
}

static inline TuiAttr nm_attr_fence_body(void)
{
    return nm_attr_foreground(NM_DRACULA_FG_R, NM_DRACULA_FG_G,
                              NM_DRACULA_FG_B);
}

/* Fence token highlights (step 4b): keyword Pink, string Yellow,
 * comment Comment (recedes past the Foreground body), number Orange -
 * the spec's token rules. Composed onto the fence body attr, so plain
 * code keeps its tint. */
static inline TuiAttr nm_attr_hl_keyword(void)
{
    return nm_attr_foreground(NM_DRACULA_PINK_R, NM_DRACULA_PINK_G,
                              NM_DRACULA_PINK_B);
}

static inline TuiAttr nm_attr_hl_string(void)
{
    return nm_attr_foreground(NM_DRACULA_YELLOW_R, NM_DRACULA_YELLOW_G,
                              NM_DRACULA_YELLOW_B);
}

static inline TuiAttr nm_attr_hl_comment(void)
{
    return nm_attr_foreground(NM_DRACULA_COMMENT_R, NM_DRACULA_COMMENT_G,
                              NM_DRACULA_COMMENT_B);
}

static inline TuiAttr nm_attr_hl_number(void)
{
    return nm_attr_foreground(NM_DRACULA_ORANGE_R, NM_DRACULA_ORANGE_G,
                              NM_DRACULA_ORANGE_B);
}

#endif /* NM_COLORS_H */
