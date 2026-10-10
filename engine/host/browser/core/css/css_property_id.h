/* A CSS property as an ID: the one table of the properties whose computed value core/css/css_computed_style.h
 * holds, each with its name, its lexbor registry id (LXB_CSS_PROPERTY_*, or __UNDEF for a property this engine
 * registers itself) and the shape its `Computed value:` line gives it. Blink's CSSPropertyID under its own name.
 * The registry column is checked against lexbor's own table when the name index is built, so a lexbor sync that
 * renames or drops a row crashes there. */

/* NAMED RESIDUAL, the keyword family. Not covered: the properties css_computed_value answers as text have no
 * row, so their computed values are still held by name in core/css/css_cascade_pass.h's string record. Next:
 * their rows, with a keyword shape, and the text record deleted. How its absence shows: cp_probe and
 * css_computed_models' strcmp leaves in a layout unit's CPU samples. */
#ifndef ENGINE_HOST_BROWSER_CORE_CSS_CSS_PROPERTY_ID_H
#define ENGINE_HOST_BROWSER_CORE_CSS_CSS_PROPERTY_ID_H

#include <lexbor/css/property/const.h>

/* What a computed value is, per its property's `Computed value:` line. */
typedef enum {
    /* "the percentage as specified or the absolute length", or a keyword as specified: a `CssLength` */
    CSS_COMPUTED_SHAPE_LENGTH = 1,
    /* css-inline-3 §5.1 "Line Spacing: the line-height property": a keyword, a number or a length */
    CSS_COMPUTED_SHAPE_LINE_HEIGHT,
    /* CSS 2.1 §17.6.1 "The separated borders model": "two absolute lengths" */
    CSS_COMPUTED_SHAPE_BORDER_SPACING
} CssComputedShape;

/* X(ID, name, lexbor id, shape). The first eighteen rows share one `Computed value:` line: CSS 2.1 §8.3 and
 * §8.4's margins and paddings and §10.2 and §10.5's `width` and `height` ("the percentage as specified or the
 * absolute length"), css-sizing-3's four limits, and CSS 2.1 §9.3.2 "Box offsets: `top`, `right`, `bottom`,
 * `left`"'s insets. core/layout/used_value.c reads the limits for CSS 2.1 §10.4's and §10.7's clamp; an inset's
 * used value is §9.4.3's or §10.3.7's equation, not a per-property read, so sharing the line makes the insets
 * no used_value_px group. The border widths are css-backgrounds-3 §3.3's "absolute length, snapped as a border
 * width". `font-size` is css-fonts-4 §2.5's "an absolute length" with no percentage or keyword arm, so it has
 * its own rule. `baseline-shift` (css-inline-3 §4.2.3 "Post-Alignment Shift: the baseline-shift longhand") and
 * `flex-basis` (css-flexbox-1 §7.2.3 "The flex-basis property") take the first rows' rule whole but are neither
 * box-model lengths nor limits: a baseline shift's percentage refers to the used `line-height`, which no cascade
 * step holds, so it survives to the computed value; a flex basis adds `content` to `width`'s keywords. */
#define CSS_PROPERTY_LIST(X)                                                                          \
    X(MARGIN_TOP,          "margin-top",          LXB_CSS_PROPERTY_MARGIN_TOP,     LENGTH)            \
    X(MARGIN_RIGHT,        "margin-right",        LXB_CSS_PROPERTY_MARGIN_RIGHT,   LENGTH)            \
    X(MARGIN_BOTTOM,       "margin-bottom",       LXB_CSS_PROPERTY_MARGIN_BOTTOM,  LENGTH)            \
    X(MARGIN_LEFT,         "margin-left",         LXB_CSS_PROPERTY_MARGIN_LEFT,    LENGTH)            \
    X(PADDING_TOP,         "padding-top",         LXB_CSS_PROPERTY_PADDING_TOP,    LENGTH)            \
    X(PADDING_RIGHT,       "padding-right",       LXB_CSS_PROPERTY_PADDING_RIGHT,  LENGTH)            \
    X(PADDING_BOTTOM,      "padding-bottom",      LXB_CSS_PROPERTY_PADDING_BOTTOM, LENGTH)            \
    X(PADDING_LEFT,        "padding-left",        LXB_CSS_PROPERTY_PADDING_LEFT,   LENGTH)            \
    X(WIDTH,               "width",               LXB_CSS_PROPERTY_WIDTH,          LENGTH)            \
    X(HEIGHT,              "height",              LXB_CSS_PROPERTY_HEIGHT,         LENGTH)            \
    X(MIN_WIDTH,           "min-width",           LXB_CSS_PROPERTY_MIN_WIDTH,      LENGTH)            \
    X(MAX_WIDTH,           "max-width",           LXB_CSS_PROPERTY_MAX_WIDTH,      LENGTH)            \
    X(MIN_HEIGHT,          "min-height",          LXB_CSS_PROPERTY_MIN_HEIGHT,     LENGTH)            \
    X(MAX_HEIGHT,          "max-height",          LXB_CSS_PROPERTY_MAX_HEIGHT,     LENGTH)            \
    X(TOP,                 "top",                 LXB_CSS_PROPERTY_TOP,            LENGTH)            \
    X(RIGHT,               "right",               LXB_CSS_PROPERTY_RIGHT,          LENGTH)            \
    X(BOTTOM,              "bottom",              LXB_CSS_PROPERTY_BOTTOM,         LENGTH)            \
    X(LEFT,                "left",                LXB_CSS_PROPERTY_LEFT,           LENGTH)            \
    X(BORDER_TOP_WIDTH,    "border-top-width",    LXB_CSS_PROPERTY__UNDEF,         LENGTH)            \
    X(BORDER_RIGHT_WIDTH,  "border-right-width",  LXB_CSS_PROPERTY__UNDEF,         LENGTH)            \
    X(BORDER_BOTTOM_WIDTH, "border-bottom-width", LXB_CSS_PROPERTY__UNDEF,         LENGTH)            \
    X(BORDER_LEFT_WIDTH,   "border-left-width",   LXB_CSS_PROPERTY__UNDEF,         LENGTH)            \
    X(FONT_SIZE,           "font-size",           LXB_CSS_PROPERTY_FONT_SIZE,      LENGTH)            \
    X(BASELINE_SHIFT,      "baseline-shift",      LXB_CSS_PROPERTY_BASELINE_SHIFT, LENGTH)            \
    X(FLEX_BASIS,          "flex-basis",          LXB_CSS_PROPERTY_FLEX_BASIS,     LENGTH)            \
    X(LINE_HEIGHT,         "line-height",         LXB_CSS_PROPERTY_LINE_HEIGHT,    LINE_HEIGHT)       \
    X(BORDER_SPACING,      "border-spacing",      LXB_CSS_PROPERTY__UNDEF,         BORDER_SPACING)

typedef enum {
#define CSS_PROPERTY_ID_ROW(id, name, lxb, shape) CSS_PROPERTY_##id,
    CSS_PROPERTY_LIST(CSS_PROPERTY_ID_ROW)
#undef CSS_PROPERTY_ID_ROW
    CSS_PROPERTY_COUNT,
    /* A name with no row: answered by the name index, never stored. */
    CSS_PROPERTY_INVALID = CSS_PROPERTY_COUNT
} CssPropertyId;

/* The row `name` names, or CSS_PROPERTY_INVALID. One hash probe and one string compare. */
CssPropertyId css_property_id_of(const char *name);

/* A row's name and shape. `id` must be a row. */
const char *css_property_name(CssPropertyId id);
CssComputedShape css_property_shape(CssPropertyId id);

#endif /* ENGINE_HOST_BROWSER_CORE_CSS_CSS_PROPERTY_ID_H */
