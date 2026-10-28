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

/* Current Line #6272A4 - the spec's semantic current-line token, and the
 * color its UI guidelines give a subtle border or separator ("Borders
 * and Separators: Subtle borders: use Current Line"). The two tokens
 * share a hex ON PURPOSE (the spec says so in both variants), so the
 * value is written once, here, and each role names the token it means. */
#define NM_DRACULA_CURRENT_LINE_R NM_DRACULA_COMMENT_R
#define NM_DRACULA_CURRENT_LINE_G NM_DRACULA_COMMENT_G
#define NM_DRACULA_CURRENT_LINE_B NM_DRACULA_COMMENT_B

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

/* The `color` key's value space: which of the two tables to paint from.
 * It lives HERE, beside the depth, because it is the same fact one step
 * earlier — the depth is the answer, the mode is who was asked (the
 * terminal's own hint, or the user's override). */
typedef enum
{
    NM_COLOR_MODE_AUTO = 0, /* the terminal's own answer (the hint) */
    NM_COLOR_MODE_16,       /* force the ANSI slots */
    NM_COLOR_MODE_RGB       /* force 24-bit */
} NmColorMode;

/* The spec's ANSI table (§1.2.2), by palette index: the 16-color tier's
 * spelling of a role. A terminal shows whatever ITS palette holds at
 * that index, and under Dracula's own terminal themes each index
 * carries the hex named here — which is what makes an ANSI spelling an
 * EXACT rendering rather than a nearest-neighbour guess at a hex. Only
 * the indices a depth-aware role needs live here; the spinner's two are
 * named in spinner.c, beside the animation they belong to. */
#define NM_DRACULA_ANSI_RED           1  /* #FF5555 */
#define NM_DRACULA_ANSI_GREEN         2  /* #50FA7B */
#define NM_DRACULA_ANSI_YELLOW        3  /* #F1FA8C */
#define NM_DRACULA_ANSI_BLUE          4  /* #BD93F9 — the Purple role */
#define NM_DRACULA_ANSI_MAGENTA       5  /* #FF79C6 — the Pink role */
#define NM_DRACULA_ANSI_CYAN          6  /* #8BE9FD */
#define NM_DRACULA_ANSI_WHITE         7  /* #F8F8F2 — the Foreground role */
#define NM_DRACULA_ANSI_BRIGHT_BLACK  8  /* #6272A4 — the Comment role */
#define NM_DRACULA_ANSI_BRIGHT_YELLOW 11 /* #FFFFA5 — the Orange role */

/* ------------------------------------------------------------------ */
/* SGR strings (raw byte writers)                                      */
/* ------------------------------------------------------------------ */

/* The byte-side roles: the raw-SGR writers' vocabulary. One enum so a
 * role is named ONCE, and nm_sgr() answers in the tier's spelling — the
 * 24-bit sequence where the terminal can show it, the spec's ANSI slot
 * otherwise. The macros these replace were compile-time string literals
 * concatenated into format strings, so the tier could never reach them;
 * a role name is the only shape that can be both tier-aware and
 * concatenated.
 *
 * Each role is the TuiColor role of the same name further down
 * (NM_SGR_TOOL is Comment, NM_SGR_RESULT Foreground, and so on): the
 * two vocabularies are one palette read by two consumers, and nm_sgr's
 * table is where that is enforced.
 *
 *   TOOL     Comment    panels, separators, muted notices
 *   RESULT   Foreground a tool result's body
 *   ACTIVITY Yellow     a RUNNING job in /ps (an exited one is TOOL)
 *   ELBOW    Cyan       the `╰─` connector tying a result to its panel
 *   ERROR    Red        `nevermore: …`
 *   REMINDER Purple     the harness's own speech — its own role on
 *                       purpose: a reminder must never read as tool
 *                       output, as the model's words, or as an error
 *                       (the transparency principle)
 */
typedef enum
{
    NM_SGR_TOOL,
    NM_SGR_RESULT,
    NM_SGR_ACTIVITY,
    NM_SGR_TOOL_ELBOW,
    NM_SGR_ERROR,
    NM_SGR_REMINDER,
    NM_SGR_ROLE_COUNT
} NmSgrRole;

/* The role's SGR prefix for this tier, as a STATIC string the byte
 * writers concatenate into a format. The ANSI spelling is built from
 * the slot table at first use (30+n for slots 0..7, 90+n-8 for 8..15),
 * never hand-written, so a wrong slot cannot be typed. */
const char *nm_sgr(NmSgrRole role, NmColorDepth depth);

/* The tier in force, from the two facts that decide it: the `color`
 * key's mode, and the terminal's env-derived truecolor hint. ONE
 * resolution, so the frame, the renderer and the banner can never
 * disagree about which of the spec's two tables is being painted from. */
NmColorDepth nm_color_resolve(NmColorMode mode, NmColorDepth hint);

/* The environment's own answer, through BOBA's grammar (the COLORTERM
 * values, the direct-color TERM variants) rather than a second reading
 * of the environment here. It is the same answer the runtime's probe
 * applies at the first flush — which is why the banner, emitted before
 * that flush, can still name the tier. */
NmColorDepth nm_color_env_hint(void);

/* The reset both tiers share. */
#define NM_SGR_RESET "\033[0m"

/* ------------------------------------------------------------------ */
/* TuiColor constructors (chat frame: prompt, popup)                   */
/* ------------------------------------------------------------------ */

/* The ONE place a role becomes tier-aware: the spec's Standard hex when
 * the terminal can show 24-bit, the spec's ANSI slot otherwise. Every
 * role goes through this, so no role can be left RGB-only by accident.
 *
 * The slot is not a nearest-neighbour guess at the hex: for every role
 * the spec's two tables AGREE (AnsiCyan IS Cyan's #8BE9FD, and so on),
 * so a terminal running Dracula's palette draws the identical color.
 * The two roles the spec leaves OUT of its ANSI table (Orange, and
 * Selection) borrow a slot, and say so where they are declared. */
static inline TuiColor nm_color_pick(NmColorDepth depth, int r, int g, int b,
                                     int ansi)
{
    if (depth == NM_COLOR_RGB)
        return tui_color_rgb((uint8_t)r, (uint8_t)g, (uint8_t)b);
    return tui_color_ansi(ansi);
}

/* User prompt `❯ ` - the app accent (Pink, the spec's AnsiMagenta). */
static inline TuiColor nm_color_prompt(NmColorDepth depth)
{
    return nm_color_pick(depth, NM_DRACULA_PINK_R, NM_DRACULA_PINK_G,
                         NM_DRACULA_PINK_B, NM_DRACULA_ANSI_MAGENTA);
}

/* Completion popup: muted border/title, Selection highlight, accent
 * marker, Foreground items. */
static inline TuiColor nm_color_popup_border(NmColorDepth depth)
{
    return nm_color_pick(depth, NM_DRACULA_COMMENT_R, NM_DRACULA_COMMENT_G,
                         NM_DRACULA_COMMENT_B, NM_DRACULA_ANSI_BRIGHT_BLACK);
}

static inline TuiColor nm_color_popup_title(NmColorDepth depth)
{
    return nm_color_pick(depth, NM_DRACULA_COMMENT_R, NM_DRACULA_COMMENT_G,
                         NM_DRACULA_COMMENT_B, NM_DRACULA_ANSI_BRIGHT_BLACK);
}

/* The selected row's background. On a 24-bit terminal it is the spec's
 * Selection; at 16 colors it is NOTHING, and the highlight is REVERSE
 * VIDEO instead (see nm_popup_selected_reverse) — because the spec keeps
 * Selection OUT of its ANSI table on purpose. Selection and Background
 * are the two surfaces the spec publishes for the terminal to own, and
 * the official terminal themes agree: kitty and Windows Terminal set
 * `selection_background` beside the palette, never in it. Inventing a
 * slot for it would claim a palette entry the spec does not give it, so
 * the 16-color tier gets the ANSI-idiomatic highlight instead. */
static inline TuiColor nm_color_popup_selected_bg(NmColorDepth depth)
{
    if (depth != NM_COLOR_RGB)
        return tui_color_none();
    return tui_color_rgb(NM_DRACULA_SELECTION_R, NM_DRACULA_SELECTION_G,
                         NM_DRACULA_SELECTION_B);
}

static inline TuiColor nm_color_popup_selected_fg(NmColorDepth depth)
{
    return nm_color_pick(depth, NM_DRACULA_FG_R, NM_DRACULA_FG_G,
                         NM_DRACULA_FG_B, NM_DRACULA_ANSI_WHITE);
}

static inline TuiColor nm_color_popup_marker(NmColorDepth depth)
{
    return nm_color_pick(depth, NM_DRACULA_PINK_R, NM_DRACULA_PINK_G,
                         NM_DRACULA_PINK_B, NM_DRACULA_ANSI_MAGENTA);
}

static inline TuiColor nm_color_popup_item(NmColorDepth depth)
{
    return nm_color_pick(depth, NM_DRACULA_FG_R, NM_DRACULA_FG_G,
                         NM_DRACULA_FG_B, NM_DRACULA_ANSI_WHITE);
}

/* Completion popup metadata column (the model picker's capability
 * badges and context window): muted Comment, so the id stays the
 * primary text and the metadata recedes. */
static inline TuiColor nm_color_popup_meta(NmColorDepth depth)
{
    return nm_color_pick(depth, NM_DRACULA_COMMENT_R, NM_DRACULA_COMMENT_G,
                         NM_DRACULA_COMMENT_B, NM_DRACULA_ANSI_BRIGHT_BLACK);
}

/* Whether the popup's selected row is drawn in REVERSE VIDEO rather than
 * on a Selection background: the 16-color tier's highlight, since the
 * spec gives Selection no ANSI slot (see nm_color_popup_selected_bg).
 * Reverse is the ANSI-idiomatic highlight and needs no color at all —
 * which is the point: at 16 colors the terminal's own convention does
 * the work, and the app claims no palette entry it was not given. */
static inline int nm_popup_selected_reverse(NmColorDepth depth)
{
    return depth != NM_COLOR_RGB;
}

/* The input row's gutter (P2): the status chrome left of the prompt —
 * the spinner glyph and the context gauge. The gauge takes its tier from
 * how full the window is: Comment at rest, Orange at ~85 %, Red at
 * ~95 %. The spinner glyph's color is NOT here: it rides its own frame
 * (nm_spinner_tick), because the dot pulses and blinks and the stars
 * rotate — one live pixel with a color that changes per tick has no
 * business being a static role. */
static inline TuiColor nm_color_gutter(NmColorDepth depth)
{
    return nm_color_pick(depth, NM_DRACULA_COMMENT_R, NM_DRACULA_COMMENT_G,
                         NM_DRACULA_COMMENT_B, NM_DRACULA_ANSI_BRIGHT_BLACK);
}

/* Orange #FFB86C — numbers, constants, booleans — and the gauge's ~85 %
 * warn tier. This is one of the TWO roles the spec leaves out of its
 * ANSI table: there is no AnsiOrange in the spec and no orange slot in
 * any official terminal theme, so the 16-color tier must BORROW. It
 * borrows AnsiBrightYellow, not AnsiYellow, so that the two roles stay
 * two colors (Orange would otherwise be indistinguishable from Yellow's
 * slot 3), and not AnsiBrightRed, which is the error role. The gauge's
 * ladder therefore reads 8 (rest) → 11 (85 %) → 1 (95 %). */
static inline TuiColor nm_color_gutter_warn(NmColorDepth depth)
{
    return nm_color_pick(depth, NM_DRACULA_ORANGE_R, NM_DRACULA_ORANGE_G,
                         NM_DRACULA_ORANGE_B, NM_DRACULA_ANSI_BRIGHT_YELLOW);
}

static inline TuiColor nm_color_gutter_warn_hot(NmColorDepth depth)
{
    return nm_color_pick(depth, NM_DRACULA_RED_R, NM_DRACULA_RED_G,
                         NM_DRACULA_RED_B, NM_DRACULA_ANSI_RED);
}

/* The status row's output-token field — the `↓` and the count it
 * labels: Dracula Cyan. The spec's own vocabulary for a marker is Cyan
 * (MarkupListBulletOrNumber is Cyan, as are ClassName and
 * MarkupLinkUrl), and Cyan is the one hue this row has left: the gauge's
 * counts are Comment (Orange/Red under pressure), the identity
 * Foreground, the spinner glyph its own frame's Green or Yellow.
 *
 * The ANSI tier is EXACT, not an approximation: the spec's AnsiCyan is
 * #8BE9FD, the same value as Cyan, so a terminal running Dracula's
 * palette draws the identical color at 16 colors. */
static inline TuiColor nm_color_gauge_output(NmColorDepth depth)
{
    return nm_color_pick(depth, NM_DRACULA_CYAN_R, NM_DRACULA_CYAN_G,
                         NM_DRACULA_CYAN_B, NM_DRACULA_ANSI_CYAN);
}

/* The status row's right-aligned identity block (`provider · model`):
 * Dracula Foreground. The rule and the chrome are structure; this is the
 * one hard fact the row states, so it reads as content, not as chrome
 * (nm_color_gutter's Comment is the row's own muted role). */
static inline TuiColor nm_color_status_identity(NmColorDepth depth)
{
    return nm_color_pick(depth, NM_DRACULA_FG_R, NM_DRACULA_FG_G,
                         NM_DRACULA_FG_B, NM_DRACULA_ANSI_WHITE);
}

/* The status row's SEPARATOR rule (`─`): the spec's Current Line token,
 * which is what its UI guidelines give a subtle border or separator
 * ("Borders and Separators: Subtle borders: use Current Line"). It is
 * the same hex as Comment — the spec makes the two tokens share a value
 * — so the row's rule and its muted chrome look identical; the roles are
 * still two roles, because the row means two things by them (a divider
 * vs. muted text) and a future change to either must not move the other.
 * AnsiBrightBlack is the slot whose hex IS #6272A4, so the 16-color tier
 * is exact. */
static inline TuiColor nm_color_status_rule(NmColorDepth depth)
{
    return nm_color_pick(depth, NM_DRACULA_CURRENT_LINE_R,
                         NM_DRACULA_CURRENT_LINE_G, NM_DRACULA_CURRENT_LINE_B,
                         NM_DRACULA_ANSI_BRIGHT_BLACK);
}

/* The status row paints FOREGROUND ONLY: the app owns no background on
 * the row, so it sits on the terminal's own canvas — a pitch-black or
 * transparent one in chest.so's projects, where a painted band would
 * show as a rectangle under the chrome. That is the one deliberate
 * departure from the spec's UI Color Palette (which otherwise governs
 * every role): the row's colors are all foreground roles, and boba's
 * declaration never sets a background here. */

/* ------------------------------------------------------------------ */
/* TuiAttr constructors (renderers)                                    */
/* ------------------------------------------------------------------ */

static inline TuiAttr nm_attr_plain(void)
{
    TuiAttr a;
    memset(&a, 0, sizeof(a));
    return a;
}

/* One row attribute with a foreground. The TuiColor carries the TIER
 * (see nm_color_pick), so a renderer never has to know which of the
 * spec's two tables the terminal can show; TUI_COLOR_NONE — what
 * nm_attr_plain's zeroed struct means — is "no color". */
static inline TuiAttr nm_attr_fg(TuiColor fg)
{
    TuiAttr a = nm_attr_plain();
    a.fg = fg;
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
static inline TuiAttr nm_attr_heading_major(NmColorDepth depth)
{
    TuiAttr a = nm_attr_fg(nm_color_pick(
        depth, NM_DRACULA_PINK_R, NM_DRACULA_PINK_G, NM_DRACULA_PINK_B,
        NM_DRACULA_ANSI_MAGENTA));
    a.bold = 1;
    return a;
}

static inline TuiAttr nm_attr_heading_minor(NmColorDepth depth)
{
    TuiAttr a = nm_attr_fg(nm_color_pick(
        depth, NM_DRACULA_PURPLE_R, NM_DRACULA_PURPLE_G, NM_DRACULA_PURPLE_B,
        NM_DRACULA_ANSI_BLUE));
    a.bold = 1;
    return a;
}

/* List bullet marker (Pink; item text stays the row's base attr). */
static inline TuiAttr nm_attr_list_bullet(NmColorDepth depth)
{
    TuiAttr a = nm_attr_fg(nm_color_pick(
        depth, NM_DRACULA_PINK_R, NM_DRACULA_PINK_G, NM_DRACULA_PINK_B,
        NM_DRACULA_ANSI_MAGENTA));
    return a;
}

/* Quote gutter `│` and quoted text. */
static inline TuiAttr nm_attr_quote_gutter(NmColorDepth depth)
{
    TuiAttr a = nm_attr_fg(nm_color_pick(
        depth, NM_DRACULA_COMMENT_R, NM_DRACULA_COMMENT_G, NM_DRACULA_COMMENT_B,
        NM_DRACULA_ANSI_BRIGHT_BLACK));
    return a;
}

static inline TuiAttr nm_attr_quote_text(NmColorDepth depth)
{
    TuiAttr a = nm_attr_fg(nm_color_pick(
        depth, NM_DRACULA_FG_R, NM_DRACULA_FG_G, NM_DRACULA_FG_B,
        NM_DRACULA_ANSI_WHITE));
    return a;
}

/* Tool-result elbow (`╰─`): the connector that ties a result body to
 * its panel - its own role, neither panel (Comment) nor body
 * (Foreground). */
static inline TuiAttr nm_attr_tool_elbow(NmColorDepth depth)
{
    TuiAttr a = nm_attr_fg(nm_color_pick(
        depth, NM_DRACULA_CYAN_R, NM_DRACULA_CYAN_G, NM_DRACULA_CYAN_B,
        NM_DRACULA_ANSI_CYAN));
    return a;
}

/* Table borders and header cells (borders Comment, header bold). */
static inline TuiAttr nm_attr_table_border(NmColorDepth depth)
{
    TuiAttr a = nm_attr_fg(nm_color_pick(
        depth, NM_DRACULA_COMMENT_R, NM_DRACULA_COMMENT_G, NM_DRACULA_COMMENT_B,
        NM_DRACULA_ANSI_BRIGHT_BLACK));
    return a;
}

/* Image degradation marker: the one row a terminal without graphics
 * (or a payload that cannot ride one) shows instead of the image. */
static inline TuiAttr nm_attr_image_marker(NmColorDepth depth)
{
    TuiAttr a = nm_attr_fg(nm_color_pick(
        depth, NM_DRACULA_COMMENT_R, NM_DRACULA_COMMENT_G, NM_DRACULA_COMMENT_B,
        NM_DRACULA_ANSI_BRIGHT_BLACK));
    return a;
}

/* Inline code spans and link text (Green). */
static inline TuiAttr nm_attr_code(NmColorDepth depth)
{
    TuiAttr a = nm_attr_fg(nm_color_pick(
        depth, NM_DRACULA_GREEN_R, NM_DRACULA_GREEN_G, NM_DRACULA_GREEN_B,
        NM_DRACULA_ANSI_GREEN));
    return a;
}

/* Fences: delimiter Comment, info string Yellow, body Foreground. */
static inline TuiAttr nm_attr_fence_delim(NmColorDepth depth)
{
    TuiAttr a = nm_attr_fg(nm_color_pick(
        depth, NM_DRACULA_COMMENT_R, NM_DRACULA_COMMENT_G, NM_DRACULA_COMMENT_B,
        NM_DRACULA_ANSI_BRIGHT_BLACK));
    return a;
}

static inline TuiAttr nm_attr_fence_info(NmColorDepth depth)
{
    TuiAttr a = nm_attr_fg(nm_color_pick(
        depth, NM_DRACULA_YELLOW_R, NM_DRACULA_YELLOW_G, NM_DRACULA_YELLOW_B,
        NM_DRACULA_ANSI_YELLOW));
    return a;
}

static inline TuiAttr nm_attr_fence_body(NmColorDepth depth)
{
    TuiAttr a = nm_attr_fg(nm_color_pick(
        depth, NM_DRACULA_FG_R, NM_DRACULA_FG_G, NM_DRACULA_FG_B,
        NM_DRACULA_ANSI_WHITE));
    return a;
}

/* Fence token highlights (step 4b): keyword Pink, string Yellow,
 * comment Comment (recedes past the Foreground body), number Orange -
 * the spec's token rules. Composed onto the fence body attr, so plain
 * code keeps its tint. */
static inline TuiAttr nm_attr_hl_keyword(NmColorDepth depth)
{
    TuiAttr a = nm_attr_fg(nm_color_pick(
        depth, NM_DRACULA_PINK_R, NM_DRACULA_PINK_G, NM_DRACULA_PINK_B,
        NM_DRACULA_ANSI_MAGENTA));
    return a;
}

static inline TuiAttr nm_attr_hl_string(NmColorDepth depth)
{
    TuiAttr a = nm_attr_fg(nm_color_pick(
        depth, NM_DRACULA_YELLOW_R, NM_DRACULA_YELLOW_G, NM_DRACULA_YELLOW_B,
        NM_DRACULA_ANSI_YELLOW));
    return a;
}

static inline TuiAttr nm_attr_hl_comment(NmColorDepth depth)
{
    TuiAttr a = nm_attr_fg(nm_color_pick(
        depth, NM_DRACULA_COMMENT_R, NM_DRACULA_COMMENT_G, NM_DRACULA_COMMENT_B,
        NM_DRACULA_ANSI_BRIGHT_BLACK));
    return a;
}

static inline TuiAttr nm_attr_hl_number(NmColorDepth depth)
{
    TuiAttr a = nm_attr_fg(nm_color_pick(
        depth, NM_DRACULA_ORANGE_R, NM_DRACULA_ORANGE_G, NM_DRACULA_ORANGE_B,
        NM_DRACULA_ANSI_BRIGHT_YELLOW));
    return a;
}

static inline TuiAttr nm_attr_table_header(void)
{
    TuiAttr a = nm_attr_plain();
    a.bold = 1;
    return a;
}

/* Link URL (dim). */
static inline TuiAttr nm_attr_link_url(void) { return nm_attr_dim(); }

#endif /* NM_COLORS_H */
