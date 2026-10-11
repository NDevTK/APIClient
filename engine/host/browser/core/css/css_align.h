/* css-align-3's six alignment properties' value — `align-content`, `justify-content`, `align-items`,
 * `justify-items`, `align-self` and `justify-self` — whose `Computed value:` line is "specified keyword(s)":
 * up to three keywords that one CssKeyword cannot hold (`safe center`, `last baseline`, `legacy left`). Each
 * property's `Value:` line is parsed here, over the core/css/css_keyword.h sets of css-align-3 §4's
 * productions, so the grammar a declaration is admitted by and the typed value a consumer reads are one. */

#ifndef ENGINE_HOST_BROWSER_CORE_CSS_CSS_ALIGN_H
#define ENGINE_HOST_BROWSER_CORE_CSS_CSS_ALIGN_H

#include <stdbool.h>
#include <stddef.h>

#include "core/css/css_keyword.h"
#include "core/css/css_property_id.h"

typedef struct {
    CssKeyword value;     /* the alignment keyword: a position, a distribution, `normal`, `stretch`, `auto`,
                             `baseline`, or `legacy` alone; never INVALID in a parsed value */
    CssKeyword overflow;  /* §4.4's `safe` or `unsafe`, or INVALID where none was written */
    CssKeyword baseline;  /* §4.2's `first` or `last` where `value` is `baseline`; INVALID otherwise */
    bool       legacy;    /* `justify-items`' `legacy` keyword beside `value` (`left`, `right`, `center`) */
} CssAlign;

/* `text` against `id`'s `Value:` line, `id` one of the six rows. FALSE for a value outside it, with `*out`
   unwritten: such a value is page input and is refused, never asserted. §4.2's bare `baseline` "computes to
   first baseline", so it parses to `first`. */
bool css_align_parse(CssPropertyId id, const char *text, CssAlign *out);

/* The value as CSSOM §6.7.2 "Serializing CSS Values" writes it: the grammar's order, `first baseline` as
   `baseline` (§4.2's Note). OWNED. */
char *css_align_serialize(CssAlign a);

/* Is `id` one of the six rows? */
bool css_align_property(CssPropertyId id);

#endif /* ENGINE_HOST_BROWSER_CORE_CSS_CSS_ALIGN_H */
