/* colors.h - semantic color roles (the one place SGR lives)
 *
 * Two consumers, one table:
 *
 *   - the system-stream byte writers in chat_app.c (tool panels, error
 *     bodies, command replies) build a line with vsnprintf, so they
 *     need byte-exact SGR strings: NM_SGR_*.
 *   - the markdown renderers (nm_markdown_render.c) write through a
 *     boba TuiRowSink, which wants a TuiAttr: nm_attr_*().
 *
 * Values are CharmTone (boba/charmtones.h), chosen for dark
 * backgrounds; adaptive colors are a non-goal. Grep this file, not the
 * call sites, when a role's color changes.
 *
 * Naming symmetry: every role has a TuiAttr constructor; only the
 * roles the byte writers actually use need an NM_SGR_* alias (a
 * byte-side writer never dims or bolds — SGR aliases stay a subset on
 * purpose).
 */

#ifndef NM_COLORS_H
#define NM_COLORS_H

#include <string.h>

#include <boba/stream.h>

/* ------------------------------------------------------------------ */
/* SGR strings (raw byte writers)                                      */
/* ------------------------------------------------------------------ */

/* CharmTone Oyster #605F6B - tool panel `▌`, the interrupted marker,
 * the provider separator. */
#define NM_SGR_TOOL "\033[38;2;96;95;107m"

/* CharmTone Smoke #BFBCC8 - the tool result body, inline spans,
 * secondary text. */
#define NM_SGR_RESULT "\033[38;2;191;188;200m"

/* CharmTone Zinc #10B1AE - the `╰─` tool-result elbow: the structural
 * accent that ties a result body to its panel. Its own role on
 * purpose: Oyster would read as panel, Smoke as body text. */
#define NM_SGR_TOOL_ELBOW "\033[38;2;16;177;174m"

/* CharmTone Coral #FF577D - errors (`nevermore: …`). */
#define NM_SGR_ERROR "\033[38;2;255;87;125m"

#define NM_SGR_RESET "\033[0m"

/* ------------------------------------------------------------------ */
/* TuiAttr constructors (renderers)                                    */
/* ------------------------------------------------------------------ */

/* CharmTone values as (r,g,b); the constructors below are the API. */
#define NM_CT_OYSTER_R  96
#define NM_CT_OYSTER_G  95
#define NM_CT_OYSTER_B  107
#define NM_CT_CORAL_R   255
#define NM_CT_CORAL_G   87
#define NM_CT_CORAL_B   125
#define NM_CT_SMOKE_R   191
#define NM_CT_SMOKE_G   188
#define NM_CT_SMOKE_B   200
#define NM_CT_MUSTARD_R 245
#define NM_CT_MUSTARD_G 239
#define NM_CT_MUSTARD_B 52
#define NM_CT_SARDINE_R 79
#define NM_CT_SARDINE_G 190
#define NM_CT_SARDINE_B 254
#define NM_CT_HAZY_R    139
#define NM_CT_HAZY_G    117
#define NM_CT_HAZY_B    255
#define NM_CT_GUAC_R    18
#define NM_CT_GUAC_G    199
#define NM_CT_GUAC_B    143
#define NM_CT_ZINC_R    16
#define NM_CT_ZINC_G    177
#define NM_CT_ZINC_B    174

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

/* Headings: h1-h2 bold + Coral, h3-h6 bold + Mustard. */
static inline TuiAttr nm_attr_heading_major(void)
{
    TuiAttr a =
        nm_attr_foreground(NM_CT_CORAL_R, NM_CT_CORAL_G, NM_CT_CORAL_B);
    a.bold = 1;
    return a;
}

static inline TuiAttr nm_attr_heading_minor(void)
{
    TuiAttr a =
        nm_attr_foreground(NM_CT_MUSTARD_R, NM_CT_MUSTARD_G, NM_CT_MUSTARD_B);
    a.bold = 1;
    return a;
}

/* List bullet marker (Coral; item text stays the row's base attr). */
static inline TuiAttr nm_attr_list_bullet(void)
{
    return nm_attr_foreground(NM_CT_CORAL_R, NM_CT_CORAL_G, NM_CT_CORAL_B);
}

/* Quote gutter `│` and quoted text. */
static inline TuiAttr nm_attr_quote_gutter(void)
{
    return nm_attr_foreground(NM_CT_OYSTER_R, NM_CT_OYSTER_G, NM_CT_OYSTER_B);
}

static inline TuiAttr nm_attr_quote_text(void)
{
    return nm_attr_foreground(NM_CT_SMOKE_R, NM_CT_SMOKE_G, NM_CT_SMOKE_B);
}

/* Tool-result elbow (`╰─`): the connector that ties a result body to
 * its panel - its own role, neither panel (Oyster) nor body (Smoke). */
static inline TuiAttr nm_attr_tool_elbow(void)
{
    return nm_attr_foreground(NM_CT_ZINC_R, NM_CT_ZINC_G, NM_CT_ZINC_B);
}

/* Table borders and header cells (borders Oyster, header bold). */
static inline TuiAttr nm_attr_table_border(void)
{
    return nm_attr_foreground(NM_CT_OYSTER_R, NM_CT_OYSTER_G, NM_CT_OYSTER_B);
}

static inline TuiAttr nm_attr_table_header(void)
{
    TuiAttr a = nm_attr_plain();
    a.bold = 1;
    return a;
}

/* Inline code spans and link text (Sardine). */
static inline TuiAttr nm_attr_code(void)
{
    return nm_attr_foreground(NM_CT_SARDINE_R, NM_CT_SARDINE_G,
                              NM_CT_SARDINE_B);
}

/* Link URL (dim). */
static inline TuiAttr nm_attr_link_url(void) { return nm_attr_dim(); }

/* Fences: delimiter Oyster, info string Mustard, body Smoke. */
static inline TuiAttr nm_attr_fence_delim(void)
{
    return nm_attr_foreground(NM_CT_OYSTER_R, NM_CT_OYSTER_G, NM_CT_OYSTER_B);
}

static inline TuiAttr nm_attr_fence_info(void)
{
    return nm_attr_foreground(NM_CT_MUSTARD_R, NM_CT_MUSTARD_G,
                              NM_CT_MUSTARD_B);
}

static inline TuiAttr nm_attr_fence_body(void)
{
    return nm_attr_foreground(NM_CT_SMOKE_R, NM_CT_SMOKE_G, NM_CT_SMOKE_B);
}

/* Fence token highlights (step 4b): keyword Hazy, string Guac,
 * comment Oyster (recedes past the Smoke body tint), number Mustard.
 * Composed onto the fence body attr, so plain code keeps its tint. */
static inline TuiAttr nm_attr_hl_keyword(void)
{
    return nm_attr_foreground(NM_CT_HAZY_R, NM_CT_HAZY_G, NM_CT_HAZY_B);
}

static inline TuiAttr nm_attr_hl_string(void)
{
    return nm_attr_foreground(NM_CT_GUAC_R, NM_CT_GUAC_G, NM_CT_GUAC_B);
}

static inline TuiAttr nm_attr_hl_comment(void)
{
    return nm_attr_foreground(NM_CT_OYSTER_R, NM_CT_OYSTER_G, NM_CT_OYSTER_B);
}

static inline TuiAttr nm_attr_hl_number(void)
{
    return nm_attr_foreground(NM_CT_MUSTARD_R, NM_CT_MUSTARD_G,
                              NM_CT_MUSTARD_B);
}

#endif /* NM_COLORS_H */
