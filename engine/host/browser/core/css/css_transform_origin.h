/* css-transforms-1 §4 "The transform-origin Property": its `Value:` grammar and its computed value, "see
 * background-position" — a horizontal and a vertical offset from the reference box's top-left corner, each an
 * absolute length, a percentage or both, with every keyword computed to its percentage. A third component is
 * the Z offset, a `<length>`. The reference box the percentages refer to is §5's and is resolved at used-value
 * time by core/css/css_transform_matrix.c, as css-values-4 §10.11 "Computed Value" leaves a percentage. */
#ifndef ENGINE_HOST_BROWSER_CORE_CSS_CSS_TRANSFORM_ORIGIN_H
#define ENGINE_HOST_BROWSER_CORE_CSS_CSS_TRANSFORM_ORIGIN_H

#include <stdbool.h>

#include "quickjs.h"
#include "core/css/css_length.h"

/* `x` and `y` are absolute, percentage or calculated, never a keyword; `z` is absolute. */
typedef struct {
    CssLength x;
    CssLength y;
    CssPx     z;
} CssTransformOrigin;

/* css-transforms-1 §4 "The transform-origin Property"'s grammar over one specified value, with its lengths
   absolutized against `realm` and `font` as core/css/css_length.h's `css_length_parse` states them. False
   writes nothing: the value is outside the grammar, which is page input and CSS Syntax §2.2 "Error
   Handling"'s dropped declaration. */
bool css_transform_origin_parse(JSContext *realm, const CssFontMetrics *font, const char *value,
                                CssTransformOrigin *out);

/* css-transforms-1 §4's `Initial:` line, `50% 50%`, with the Z offset that section assumes: "If one or two
   values are specified, the third value is assumed to be 0px." */
CssTransformOrigin css_transform_origin_initial(void);

/* The computed value as CSSOM §6.7.2 "Serializing CSS Values" writes it, horizontal first. The Z offset is
   written unless it is a zero no environment fact reaches, which is how a two-value origin reads back. OWNED. */
char *css_transform_origin_serialize(const CssTransformOrigin *o);

#endif
