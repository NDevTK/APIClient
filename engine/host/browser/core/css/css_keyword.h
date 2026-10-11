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
    X(BOTTOM,      "bottom")

/* INVALID is zero so a zeroed value that is not a keyword holds no keyword. */
typedef enum {
    CSS_KEYWORD_INVALID = 0,
#define CSS_KEYWORD_ID_ROW(id, name) CSS_KEYWORD_##id,
    CSS_KEYWORD_LIST(CSS_KEYWORD_ID_ROW)
#undef CSS_KEYWORD_ID_ROW
    CSS_KEYWORD_COUNT
} CssKeyword;

/* The keywords a property's computed value can be, which is the keyword arm of its `Computed value:` line.
 * Each is cited at its row in css_keyword.c. UNTYPED is a NAMED RESIDUAL: a TEXT-shaped row's computed value
 * is still owned text, compared by spelling (core/css/css_computed_value.h's `css_computed_value`). The next
 * diff gives `display` a typed computed value over this list and a set of its own. Until each text row has
 * one, a page's case-folded or substituted spelling of such a keyword can compare unequal to the spelling a
 * consumer tests. */
typedef enum {
    CSS_KEYWORDS_UNTYPED = 1,
    CSS_KEYWORDS_NONE,
    CSS_KEYWORDS_MARGIN,
    CSS_KEYWORDS_SIZE,
    CSS_KEYWORDS_MAX_SIZE,
    CSS_KEYWORDS_INSET,
    CSS_KEYWORDS_BASELINE_SHIFT,
    CSS_KEYWORDS_FLEX_BASIS
} CssKeywordSet;

/* The keyword `ident` (its first `n` bytes) spells, ASCII case-insensitively, or CSS_KEYWORD_INVALID when it
   spells none of the list. */
CssKeyword css_keyword_of(const char *ident, size_t n);

/* `k`'s lowercase spelling. `k` must be a keyword. */
const char *css_keyword_name(CssKeyword k);

/* Is `k` in `set`? `set` must not be UNTYPED; `k` may be INVALID, which no set holds. */
bool css_keyword_set_has(CssKeywordSet set, CssKeyword k);

#endif /* ENGINE_HOST_BROWSER_CORE_CSS_CSS_KEYWORD_H */
