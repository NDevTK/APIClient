/* css-display-3 §2 "Box Layout Modes: the display property"'s value as the pair its `Computed value:` line
 * names: "a pair of keywords representing the inner and outer display types plus optional list-item flag, or a
 * <display-internal> or <display-box> keyword". Every spelling of the `Value:` line — one keyword, two, the
 * `list-item` forms and the `<display-legacy>` precomposed ones — parses to one such value, so two spellings of
 * one box (`inline-block`, `inline flow-root`) are one value, and §2.7's blockification sets the outer half
 * and leaves the inner one instead of mapping whole keywords. */

#ifndef ENGINE_HOST_BROWSER_CORE_CSS_CSS_DISPLAY_H
#define ENGINE_HOST_BROWSER_CORE_CSS_CSS_DISPLAY_H

#include <stdbool.h>

#include "core/css/css_keyword.h"

typedef enum {
    CSS_DISPLAY_PAIR = 1,   /* `outer`, `inner` and `list_item` */
    CSS_DISPLAY_INTERNAL,   /* `keyword` is a `<display-internal>` */
    CSS_DISPLAY_BOX         /* `keyword` is a `<display-box>` */
} CssDisplayKind;

typedef struct {
    CssDisplayKind kind;
    CssKeyword     outer;      /* CSS_DISPLAY_PAIR: a `<display-outside>`; CSS_KEYWORD_INVALID otherwise */
    CssKeyword     inner;      /* CSS_DISPLAY_PAIR: a `<display-inside>`; CSS_KEYWORD_INVALID otherwise */
    bool           list_item;  /* CSS_DISPLAY_PAIR only: §2.3's flag */
    CssKeyword     keyword;    /* CSS_DISPLAY_INTERNAL and CSS_DISPLAY_BOX; CSS_KEYWORD_INVALID otherwise */
} CssDisplay;

/* `text` against §2's `Value:` line, with §2.1's, §2.2's and §2.3's defaults applied and §2.6's precomposed
   keywords expanded. FALSE for a value outside the grammar, with `*out` unwritten: lexbor validates a
   declaration, so only a substituted value can be one, and it is page input. */
bool css_display_parse(const char *text, CssDisplay *out);

/* §2.7 "Automatic Box Type Transformations"' blockification. */
CssDisplay css_display_blockify(CssDisplay d);

/* The value in §2's summary table's "Short display" column, which its Note makes the serialization ("most
   backwards-compatible, then shortest"). OWNED. */
char *css_display_serialize(CssDisplay d);

/* Is it a box of `<display-box>` keyword `k` (`none`, `contents`)? */
bool css_display_is_box(CssDisplay d, CssKeyword k);

#endif /* ENGINE_HOST_BROWSER_CORE_CSS_CSS_DISPLAY_H */
