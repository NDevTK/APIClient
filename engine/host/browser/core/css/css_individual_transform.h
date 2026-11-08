/* css-transforms-2 §5 "Individual Transform Properties: the translate, scale, and rotate properties": each
 * value's grammar, read as the one `<transform-function>` §5 makes it equivalent to (translate3d(), the
 * rotations, scale3d()), and its computed value written back in the property's own grammar. */
#ifndef ENGINE_HOST_BROWSER_CORE_CSS_CSS_INDIVIDUAL_TRANSFORM_H
#define ENGINE_HOST_BROWSER_CORE_CSS_CSS_INDIVIDUAL_TRANSFORM_H

#include <stdbool.h>

#include "quickjs.h"
#include "core/css/css_length.h"
#include "core/css/css_transform_function.h"

/* `value` of property `name` (`translate`, `rotate` or `scale`) as its equivalent function, lengths
   absolutized as core/css/css_transform_function.h's parse does. False for `none` and for a value outside the
   property's grammar (page input, CSS Syntax §2.2's dropped declaration, which leaves `Initial: none`). */
bool css_individual_transform_parse(JSContext *realm, const CssFontMetrics *font, const char *name,
                                    const char *value, CssTransformFunction *out);

/* The computed value of `name` for the function the entry above wrote. OWNED. */
char *css_individual_transform_serialize(const char *name, const CssTransformFunction *fn);

#endif
