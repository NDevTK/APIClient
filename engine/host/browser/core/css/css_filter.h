/* filter-effects-1 §5 "Graphic filters: the filter property"'s `none | <filter-value-list>`, which
 * filter-effects-2 §2 "Backdrop filters: the backdrop-filter property" shares, with filter-effects-1 §6.1
 * "Supported Filter Functions"' arguments and filter-effects-1 §6.2 "Computed Values of Filter Functions".
 * The grammar a declaration is admitted by and the computed value a consumer reads are one parse. */

#ifndef ENGINE_HOST_BROWSER_CORE_CSS_CSS_FILTER_H
#define ENGINE_HOST_BROWSER_CORE_CSS_CSS_FILTER_H

#include <stdbool.h>

#include "quickjs.h"
#include "core/css/css_length.h"

/* `value` against the grammar: the specified value with function names lowercased and one space between
   components, or NULL for a value outside it (page input, so the declaration is dropped). OWNED. */
char *css_filter_specified(const char *value);

/* filter-effects-1 §6.2's computed value of a specified value `css_filter_specified` produced: omitted
   arguments included at their defaults, lengths absolutized through core/css/css_length.h, and a drop-shadow's
   colour first. OWNED. */
char *css_filter_computed(JSContext *realm, const CssFontMetrics *font, const char *specified);

/* Whether a computed value is `none`, the value that applies no filter and forms no stacking context. */
bool css_filter_is_none(const char *computed);

#endif
