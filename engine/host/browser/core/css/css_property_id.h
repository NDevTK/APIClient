/* A CSS property as an ID: the one table of the properties whose computed value core/css/css_computed_value.h
 * derives and core/css/css_computed_style.h holds, each with its name, its lexbor registry id
 * (LXB_CSS_PROPERTY_*, or __UNDEF for a property this engine registers itself) and the shape its `Computed
 * value:` line gives it. Blink's CSSPropertyID under its own name. The registry column is checked against
 * lexbor's own table when the name index is built, so a lexbor sync that renames or drops a row crashes there.
 * A property with no row is one this engine derives no computed value for. */

#ifndef ENGINE_HOST_BROWSER_CORE_CSS_CSS_PROPERTY_ID_H
#define ENGINE_HOST_BROWSER_CORE_CSS_CSS_PROPERTY_ID_H

#include <lexbor/css/property/const.h>

#include "core/css/css_keyword.h"

/* What a computed value is, per its property's `Computed value:` line. */
typedef enum {
    /* a keyword, number, string or list whose computed value is its serialization: owned text */
    CSS_COMPUTED_SHAPE_TEXT = 1,
    /* "the percentage as specified or the absolute length", or a keyword as specified: a `CssLength` */
    CSS_COMPUTED_SHAPE_LENGTH,
    /* css-inline-3 §5.1 "Line Spacing: the line-height property": a keyword, a number or a length */
    CSS_COMPUTED_SHAPE_LINE_HEIGHT,
    /* CSS 2.1 §17.6.1 "The separated borders model": "two absolute lengths" */
    CSS_COMPUTED_SHAPE_BORDER_SPACING,
    /* "specified keyword": one of the row's keyword set, a `CssKeyword` */
    CSS_COMPUTED_SHAPE_KEYWORD
} CssComputedShape;

/* The range a property's grammar admits for its numeric values, which css-values-4 §9.1 "Numeric Functions"
   clamps a math function's result to at computed value time where the value is resolved by then, and at used
   value time otherwise. ALL is a value with no such bound, or none at all (a keyword). */
typedef enum {
    CSS_RANGE_ALL = 1,
    CSS_RANGE_NON_NEGATIVE   /* `[0,∞]` */
} CssValueRange;

/* X(ID, name, lexbor id, shape, range, keywords). `keywords` is the core/css/css_keyword.h set the computed
 * value's keyword arm holds; css_keyword.c cites each set's `Value:` line. The first eighteen rows share one
 * `Computed value:` line: CSS 2.1 §8.3 and §8.4's margins and paddings and §10.2 and §10.5's `width` and `height` ("the percentage as specified or the
 * absolute length"), css-sizing-3's four limits, and CSS 2.1 §9.3.2 "Box offsets: `top`, `right`, `bottom`,
 * `left`"'s insets. core/layout/used_value.c reads the limits for CSS 2.1 §10.4's and §10.7's clamp; an inset's
 * used value is §9.4.3's or §10.3.7's equation, not a per-property read, so sharing the line makes the insets
 * no used_value_px group. The border widths are css-backgrounds-3 §3.3's "absolute length, snapped as a border
 * width". `font-size` is css-fonts-4 §2.5's "an absolute length" with no percentage or keyword arm, so it has
 * its own rule. `baseline-shift` (css-inline-3 §4.2.3 "Post-Alignment Shift: the baseline-shift longhand") and
 * `flex-basis` (css-flexbox-1 §7.2.3 "The flex-basis property") take the first rows' rule whole but are neither
 * box-model lengths nor limits: a baseline shift's percentage refers to the used `line-height`, which no cascade
 * step holds, so it survives to the computed value; a flex basis adds `content` to `width`'s keywords.
 * `position`, `box-sizing` and `clear` are css-position-3 §2's, css-sizing-3 §3.3's and css-page-floats-3 §4's
 * `Computed value: specified keyword`, each held as its ID. */
#define CSS_PROPERTY_LIST(X)                                                                          \
    X(MARGIN_TOP,          "margin-top",          LXB_CSS_PROPERTY_MARGIN_TOP,     LENGTH, ALL, MARGIN)           \
    X(MARGIN_RIGHT,        "margin-right",        LXB_CSS_PROPERTY_MARGIN_RIGHT,   LENGTH, ALL, MARGIN)           \
    X(MARGIN_BOTTOM,       "margin-bottom",       LXB_CSS_PROPERTY_MARGIN_BOTTOM,  LENGTH, ALL, MARGIN)           \
    X(MARGIN_LEFT,         "margin-left",         LXB_CSS_PROPERTY_MARGIN_LEFT,    LENGTH, ALL, MARGIN)           \
    X(PADDING_TOP,         "padding-top",         LXB_CSS_PROPERTY_PADDING_TOP,    LENGTH, NON_NEGATIVE, NONE)    \
    X(PADDING_RIGHT,       "padding-right",       LXB_CSS_PROPERTY_PADDING_RIGHT,  LENGTH, NON_NEGATIVE, NONE)    \
    X(PADDING_BOTTOM,      "padding-bottom",      LXB_CSS_PROPERTY_PADDING_BOTTOM, LENGTH, NON_NEGATIVE, NONE)    \
    X(PADDING_LEFT,        "padding-left",        LXB_CSS_PROPERTY_PADDING_LEFT,   LENGTH, NON_NEGATIVE, NONE)    \
    X(WIDTH,               "width",               LXB_CSS_PROPERTY_WIDTH,          LENGTH, NON_NEGATIVE, SIZE)    \
    X(HEIGHT,              "height",              LXB_CSS_PROPERTY_HEIGHT,         LENGTH, NON_NEGATIVE, SIZE)    \
    X(MIN_WIDTH,           "min-width",           LXB_CSS_PROPERTY_MIN_WIDTH,      LENGTH, NON_NEGATIVE, SIZE)    \
    X(MAX_WIDTH,           "max-width",           LXB_CSS_PROPERTY_MAX_WIDTH,      LENGTH, NON_NEGATIVE, MAX_SIZE) \
    X(MIN_HEIGHT,          "min-height",          LXB_CSS_PROPERTY_MIN_HEIGHT,     LENGTH, NON_NEGATIVE, SIZE)    \
    X(MAX_HEIGHT,          "max-height",          LXB_CSS_PROPERTY_MAX_HEIGHT,     LENGTH, NON_NEGATIVE, MAX_SIZE) \
    X(TOP,                 "top",                 LXB_CSS_PROPERTY_TOP,            LENGTH, ALL, INSET)            \
    X(RIGHT,               "right",               LXB_CSS_PROPERTY_RIGHT,          LENGTH, ALL, INSET)            \
    X(BOTTOM,              "bottom",              LXB_CSS_PROPERTY_BOTTOM,         LENGTH, ALL, INSET)            \
    X(LEFT,                "left",                LXB_CSS_PROPERTY_LEFT,           LENGTH, ALL, INSET)            \
    X(BORDER_TOP_WIDTH,    "border-top-width",    LXB_CSS_PROPERTY__UNDEF,         LENGTH, NON_NEGATIVE, NONE)    \
    X(BORDER_RIGHT_WIDTH,  "border-right-width",  LXB_CSS_PROPERTY__UNDEF,         LENGTH, NON_NEGATIVE, NONE)    \
    X(BORDER_BOTTOM_WIDTH, "border-bottom-width", LXB_CSS_PROPERTY__UNDEF,         LENGTH, NON_NEGATIVE, NONE)    \
    X(BORDER_LEFT_WIDTH,   "border-left-width",   LXB_CSS_PROPERTY__UNDEF,         LENGTH, NON_NEGATIVE, NONE)    \
    X(FONT_SIZE,           "font-size",           LXB_CSS_PROPERTY_FONT_SIZE,      LENGTH, NON_NEGATIVE, NONE)    \
    X(BASELINE_SHIFT,      "baseline-shift",      LXB_CSS_PROPERTY_BASELINE_SHIFT, LENGTH, ALL, BASELINE_SHIFT)   \
    X(FLEX_BASIS,          "flex-basis",          LXB_CSS_PROPERTY_FLEX_BASIS,     LENGTH, NON_NEGATIVE, FLEX_BASIS) \
    X(LINE_HEIGHT,         "line-height",         LXB_CSS_PROPERTY_LINE_HEIGHT,    LINE_HEIGHT, NON_NEGATIVE, NONE) \
    X(BORDER_SPACING,      "border-spacing",      LXB_CSS_PROPERTY__UNDEF,         BORDER_SPACING, NON_NEGATIVE, NONE) \
    X(POSITION,            "position",            LXB_CSS_PROPERTY_POSITION,       KEYWORD, ALL, POSITION)        \
    X(BOX_SIZING,          "box-sizing",          LXB_CSS_PROPERTY_BOX_SIZING,     KEYWORD, ALL, BOX_SIZING)      \
    X(CLEAR,               "clear",               LXB_CSS_PROPERTY_CLEAR,          KEYWORD, ALL, CLEAR)           \
    CSS_PROPERTY_TEXT_LIST(X)

/* The text-shaped rows. Which `Computed value:` line each derives, and which algorithm reads it, is stated at
 * its arm of css_computed_value.c's derivation. */
#define CSS_PROPERTY_TEXT_LIST(X)                                                                     \
    X(OVERFLOW_X,          "overflow-x",          LXB_CSS_PROPERTY_OVERFLOW_X,     TEXT, ALL, UNTYPED)            \
    X(OVERFLOW_Y,          "overflow-y",          LXB_CSS_PROPERTY_OVERFLOW_Y,     TEXT, ALL, UNTYPED)            \
    X(DISPLAY,             "display",             LXB_CSS_PROPERTY_DISPLAY,        TEXT, ALL, UNTYPED)            \
    X(FLOAT,               "float",               LXB_CSS_PROPERTY_FLOAT,          TEXT, ALL, UNTYPED)            \
    X(WHITE_SPACE,         "white-space",         LXB_CSS_PROPERTY_WHITE_SPACE,    TEXT, ALL, UNTYPED)            \
    X(DIRECTION,           "direction",           LXB_CSS_PROPERTY_DIRECTION,      TEXT, ALL, UNTYPED)            \
    X(WRITING_MODE,        "writing-mode",        LXB_CSS_PROPERTY_WRITING_MODE,   TEXT, ALL, UNTYPED)            \
    X(ALIGNMENT_BASELINE,  "alignment-baseline",  LXB_CSS_PROPERTY_ALIGNMENT_BASELINE, TEXT, ALL, UNTYPED)            \
    X(BASELINE_SOURCE,     "baseline-source",     LXB_CSS_PROPERTY_BASELINE_SOURCE, TEXT, ALL, UNTYPED)            \
    X(TEXT_ALIGN_ALL,      "text-align-all",      LXB_CSS_PROPERTY_TEXT_ALIGN_ALL, TEXT, ALL, UNTYPED)            \
    X(TEXT_ALIGN_LAST,     "text-align-last",     LXB_CSS_PROPERTY_TEXT_ALIGN_LAST, TEXT, ALL, UNTYPED)            \
    X(TRANSFORM,           "transform",           LXB_CSS_PROPERTY__UNDEF,         TEXT, ALL, UNTYPED)            \
    X(CAPTION_SIDE,        "caption-side",        LXB_CSS_PROPERTY__UNDEF,         TEXT, ALL, UNTYPED)            \
    X(TABLE_LAYOUT,        "table-layout",        LXB_CSS_PROPERTY__UNDEF,         TEXT, ALL, UNTYPED)            \
    X(BORDER_COLLAPSE,     "border-collapse",     LXB_CSS_PROPERTY__UNDEF,         TEXT, ALL, UNTYPED)            \
    X(FLEX_DIRECTION,      "flex-direction",      LXB_CSS_PROPERTY_FLEX_DIRECTION, TEXT, ALL, UNTYPED)            \
    X(FLEX_WRAP,           "flex-wrap",           LXB_CSS_PROPERTY_FLEX_WRAP,      TEXT, ALL, UNTYPED)            \
    X(FLEX_GROW,           "flex-grow",           LXB_CSS_PROPERTY_FLEX_GROW,      TEXT, NON_NEGATIVE, UNTYPED)   \
    X(FLEX_SHRINK,         "flex-shrink",         LXB_CSS_PROPERTY_FLEX_SHRINK,    TEXT, NON_NEGATIVE, UNTYPED)   \
    X(ALIGN_ITEMS,         "align-items",         LXB_CSS_PROPERTY_ALIGN_ITEMS,    TEXT, ALL, UNTYPED)            \
    X(ALIGN_SELF,          "align-self",          LXB_CSS_PROPERTY_ALIGN_SELF,     TEXT, ALL, UNTYPED)            \
    X(ALIGN_CONTENT,       "align-content",       LXB_CSS_PROPERTY_ALIGN_CONTENT,  TEXT, ALL, UNTYPED)            \
    X(ORDER,               "order",               LXB_CSS_PROPERTY_ORDER,          TEXT, ALL, UNTYPED)            \
    X(VISIBILITY,          "visibility",          LXB_CSS_PROPERTY_VISIBILITY,     TEXT, ALL, UNTYPED)            \
    X(Z_INDEX,             "z-index",             LXB_CSS_PROPERTY_Z_INDEX,        TEXT, ALL, UNTYPED)            \
    X(POINTER_EVENTS,      "pointer-events",      LXB_CSS_PROPERTY__UNDEF,         TEXT, ALL, UNTYPED)            \
    X(LIST_STYLE_TYPE,     "list-style-type",     LXB_CSS_PROPERTY__UNDEF,         TEXT, ALL, UNTYPED)            \
    X(LIST_STYLE_POSITION, "list-style-position", LXB_CSS_PROPERTY__UNDEF,         TEXT, ALL, UNTYPED)            \
    X(LIST_STYLE_IMAGE,    "list-style-image",    LXB_CSS_PROPERTY__UNDEF,         TEXT, ALL, UNTYPED)            \
    X(BORDER_TOP_STYLE,    "border-top-style",    LXB_CSS_PROPERTY__UNDEF,         TEXT, ALL, UNTYPED)            \
    X(BORDER_RIGHT_STYLE,  "border-right-style",  LXB_CSS_PROPERTY__UNDEF,         TEXT, ALL, UNTYPED)            \
    X(BORDER_BOTTOM_STYLE, "border-bottom-style", LXB_CSS_PROPERTY__UNDEF,         TEXT, ALL, UNTYPED)            \
    X(BORDER_LEFT_STYLE,   "border-left-style",   LXB_CSS_PROPERTY__UNDEF,         TEXT, ALL, UNTYPED)

typedef enum {
#define CSS_PROPERTY_ID_ROW(id, name, lxb, shape, range, keywords) CSS_PROPERTY_##id,
    CSS_PROPERTY_LIST(CSS_PROPERTY_ID_ROW)
#undef CSS_PROPERTY_ID_ROW
    CSS_PROPERTY_COUNT,
    /* A name with no row: answered by the name index, never stored. */
    CSS_PROPERTY_INVALID = CSS_PROPERTY_COUNT
} CssPropertyId;

/* The row `name` names, or CSS_PROPERTY_INVALID. One hash probe and one string compare. */
CssPropertyId css_property_id_of(const char *name);

/* A row's name, shape, range and keyword set. `id` must be a row. */
const char *css_property_name(CssPropertyId id);
CssComputedShape css_property_shape(CssPropertyId id);
CssValueRange css_property_range(CssPropertyId id);
CssKeywordSet css_property_keywords(CssPropertyId id);

#endif /* ENGINE_HOST_BROWSER_CORE_CSS_CSS_PROPERTY_ID_H */
