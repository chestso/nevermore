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
 *   - the markdown renderers (nm_markdown_render.c) write through a
 *     boba TuiRowSink, which wants a TuiAttr: nm_attr_*().
 *   - the chat frame (prompt, popup) draws through boba styles, which
 *     want a TuiColor: nm_color_*().
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
/* SGR strings (raw byte writers)                                      */
/* ------------------------------------------------------------------ */

/* Dracula Comment - the tool plan header (its emoji lead), the
 * interrupted marker, the provider separator, the spinner label. */
#define NM_SGR_TOOL "\033[38;2;98;114;164m"

/* Dracula Foreground - the tool result body, secondary text. */
#define NM_SGR_RESULT "\033[38;2;248;248;242m"

/* Dracula Cyan - the `╰─` tool-result elbow: the structural accent that
 * ties a result body to its panel. Its own role on purpose: Comment
 * would read as panel, Foreground as body text. */
#define NM_SGR_TOOL_ELBOW "\033[38;2;139;233;253m"

/* Dracula Red - errors (`nevermore: …`). */
#define NM_SGR_ERROR "\033[38;2;255;85;85m"

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
