/* A CSS keyword as an ID: the one list of the identifiers a typed computed value can hold, and the keyword
 * sets core/css/css_property_id.h's table gives each property over it. An identifier becomes its ID once, at
 * parse; every consumer after that compares IDs and switches on them. css-values-4 §4.1 "Pre-defined
 * Keywords": "Keywords are identifiers and are interpreted ASCII case-insensitively (i.e., [a-z] and [A-Z]
 * are equivalent)", so the lookup folds case and a name is the keyword's lowercase spelling, which is also its
 * serialization. lexbor's `lxb_css_value_type_t` is not this list because its value table has no
 * `fit-content`, which css-sizing-3 §3.2's `<box-size>` admits. */

#ifndef ENGINE_HOST_BROWSER_CORE_CSS_CSS_KEYWORD_H
#define ENGINE_HOST_BROWSER_CORE_CSS_CSS_KEYWORD_H

#include <stdbool.h>
#include <stddef.h>

/* X(ID, name). */
#define CSS_KEYWORD_LIST(X)                \
    X(AUTO,        "auto")                 \
    X(NONE,        "none")                 \
    X(CONTENT,     "content")              \
    X(STRETCH,     "stretch")              \
    X(MIN_CONTENT, "min-content")          \
    X(MAX_CONTENT, "max-content")          \
    X(FIT_CONTENT, "fit-content")          \
    X(SUB,         "sub")                  \
    X(SUPER,       "super")                \
    X(TOP,         "top")                  \
    X(CENTER,      "center")               \
    X(BOTTOM,      "bottom")               \
    X(STATIC,      "static")               \
    X(RELATIVE,    "relative")             \
    X(ABSOLUTE,    "absolute")             \
    X(STICKY,      "sticky")               \
    X(FIXED,       "fixed")                \
    X(CONTENT_BOX, "content-box")          \
    X(BORDER_BOX,  "border-box")           \
    X(INLINE_START, "inline-start")        \
    X(INLINE_END,  "inline-end")           \
    X(BLOCK_START, "block-start")          \
    X(BLOCK_END,   "block-end")            \
    X(LEFT,        "left")                 \
    X(RIGHT,       "right")                \
    X(BOTH_INLINE, "both-inline")          \
    X(BOTH_BLOCK,  "both-block")           \
    X(BOTH,        "both")                 \
    X(THIN,        "thin")                 \
    X(MEDIUM,      "medium")               \
    X(THICK,       "thick")                \
    X(XX_SMALL,    "xx-small")             \
    X(X_SMALL,     "x-small")              \
    X(SMALL,       "small")                \
    X(LARGE,       "large")                \
    X(X_LARGE,     "x-large")              \
    X(XX_LARGE,    "xx-large")             \
    X(XXX_LARGE,   "xxx-large")            \
    X(LARGER,      "larger")               \
    X(SMALLER,     "smaller")              \
    X(MATH,        "math")                 \
    X(BLOCK,              "block") \
    X(INLINE,             "inline") \
    X(RUN_IN,             "run-in") \
    X(FLOW,               "flow") \
    X(FLOW_ROOT,          "flow-root") \
    X(TABLE,              "table") \
    X(FLEX,               "flex") \
    X(GRID,               "grid") \
    X(RUBY,               "ruby") \
    X(LIST_ITEM,          "list-item") \
    X(TABLE_ROW_GROUP,    "table-row-group") \
    X(TABLE_HEADER_GROUP, "table-header-group") \
    X(TABLE_FOOTER_GROUP, "table-footer-group") \
    X(TABLE_ROW,          "table-row") \
    X(TABLE_CELL,         "table-cell") \
    X(TABLE_COLUMN_GROUP, "table-column-group") \
    X(TABLE_COLUMN,       "table-column") \
    X(TABLE_CAPTION,      "table-caption") \
    X(RUBY_BASE,          "ruby-base") \
    X(RUBY_TEXT,          "ruby-text") \
    X(RUBY_BASE_CONTAINER, "ruby-base-container") \
    X(RUBY_TEXT_CONTAINER, "ruby-text-container") \
    X(CONTENTS,           "contents") \
    X(INLINE_BLOCK,       "inline-block") \
    X(INLINE_TABLE,       "inline-table") \
    X(INLINE_FLEX,        "inline-flex") \
    X(INLINE_GRID,        "inline-grid") \
    X(ROW,                "row") \
    X(ROW_REVERSE,        "row-reverse") \
    X(COLUMN,             "column") \
    X(COLUMN_REVERSE,     "column-reverse") \
    X(NOWRAP,             "nowrap") \
    X(WRAP,               "wrap") \
    X(WRAP_REVERSE,       "wrap-reverse") \
    X(LTR,                "ltr") \
    X(RTL,                "rtl") \
    X(HORIZONTAL_TB,      "horizontal-tb") \
    X(VERTICAL_RL,        "vertical-rl") \
    X(VERTICAL_LR,        "vertical-lr") \
    X(SIDEWAYS_RL,        "sideways-rl") \
    X(SIDEWAYS_LR,        "sideways-lr") \
    X(SEPARATE,           "separate") \
    X(COLLAPSE,           "collapse")

/* INVALID is zero so a zeroed value that is not a keyword holds no keyword. */
typedef enum {
    CSS_KEYWORD_INVALID = 0,
#define CSS_KEYWORD_ID_ROW(id, name) CSS_KEYWORD_##id,
    CSS_KEYWORD_LIST(CSS_KEYWORD_ID_ROW)
#undef CSS_KEYWORD_ID_ROW
    CSS_KEYWORD_COUNT
} CssKeyword;

/* The keywords a property's `Value:` line admits, each set cited in css_keyword.c. A length row whose own
 * computed-value rule resolves a keyword to a number (`thin`, `x-large`) keeps the rest in its computed value.
 * ABSOLUTE_SIZE is no row's: it is css-fonts-4 §2.5's `<absolute-size>`, which FONT_SIZE contains. The five
 * DISPLAY_ sets are no row's either: they are css-display-3 §2's productions, which core/css/css_display.h
 * composes into the property's grammar; DISPLAY, the `display` row's, is every keyword that grammar names.
 * UNTYPED is a NAMED RESIDUAL: a TEXT-shaped row's computed value is still owned text, compared by spelling
 * (core/css/css_computed_value.h's `css_computed_value`). The next diffs move each `specified keyword` row to
 * the KEYWORD shape over a set of its own. Until each text row has one, a page's case-folded or substituted
 * spelling of such a keyword can compare unequal to the spelling a consumer tests. */
typedef enum {
    CSS_KEYWORDS_UNTYPED = 1,
    CSS_KEYWORDS_NONE,
    CSS_KEYWORDS_MARGIN,
    CSS_KEYWORDS_SIZE,
    CSS_KEYWORDS_MAX_SIZE,
    CSS_KEYWORDS_INSET,
    CSS_KEYWORDS_BASELINE_SHIFT,
    CSS_KEYWORDS_FLEX_BASIS,
    CSS_KEYWORDS_POSITION,
    CSS_KEYWORDS_BOX_SIZING,
    CSS_KEYWORDS_CLEAR,
    CSS_KEYWORDS_LINE_WIDTH,
    CSS_KEYWORDS_FONT_SIZE,
    CSS_KEYWORDS_ABSOLUTE_SIZE,
    CSS_KEYWORDS_DISPLAY_OUTSIDE,
    CSS_KEYWORDS_DISPLAY_INSIDE,
    CSS_KEYWORDS_DISPLAY_INTERNAL,
    CSS_KEYWORDS_DISPLAY_BOX,
    CSS_KEYWORDS_DISPLAY_LEGACY,
    CSS_KEYWORDS_DISPLAY,
    CSS_KEYWORDS_FLEX_DIRECTION,
    CSS_KEYWORDS_FLEX_WRAP,
    CSS_KEYWORDS_DIRECTION,
    CSS_KEYWORDS_WRITING_MODE,
    CSS_KEYWORDS_TABLE_LAYOUT,
    CSS_KEYWORDS_BORDER_COLLAPSE,
    CSS_KEYWORDS_CAPTION_SIDE
} CssKeywordSet;

/* The keyword `ident` (its first `n` bytes) spells, ASCII case-insensitively, or CSS_KEYWORD_INVALID when it
   spells none of the list. */
CssKeyword css_keyword_of(const char *ident, size_t n);

/* `k`'s lowercase spelling. `k` must be a keyword. */
const char *css_keyword_name(CssKeyword k);

/* Is `k` in `set`? `set` must not be UNTYPED; `k` may be INVALID, which no set holds. */
bool css_keyword_set_has(CssKeywordSet set, CssKeyword k);

#endif /* ENGINE_HOST_BROWSER_CORE_CSS_CSS_KEYWORD_H */
