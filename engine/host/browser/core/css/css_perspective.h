/* css-transforms-2 §8 "The perspective Property" and §9 "The perspective-origin Property": each `Value:`
 * grammar and its computed value. §9's origin is a `<position>` (css-values-4 §8.3 "2D Positioning: the
 * <position> type") whose computed value is background-position's, an offset from the reference box's top-left
 * corner per axis, so a `right 50px` computes to calc(100% - 50px); the reference box is resolved at used-value
 * time by core/css/css_transform_matrix.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_CSS_CSS_PERSPECTIVE_H
#define ENGINE_HOST_BROWSER_CORE_CSS_CSS_PERSPECTIVE_H

#include <stdbool.h>

#include "quickjs.h"
#include "core/css/css_length.h"
#include "core/css/css_transform_origin.h"

/* §8's `none | <length [0,∞]>` over one specified value, the length absolutized as core/css/css_length.h's
   `css_length_parse` states it; a math function's result is clamped to the range (css-values-4 §10.12 "Range
   Checking"). False for `none` and for a value outside the grammar (page input, which computes to `Initial:
   none`). */
bool css_perspective_parse(JSContext *realm, const CssFontMetrics *font, const char *value, CssPx *out);

/* §9's grammar over one specified value. The result's `z` is always 0. False writes nothing: a value outside
   the grammar, which computes to `Initial: 50% 50%`. */
bool css_perspective_origin_parse(JSContext *realm, const CssFontMetrics *font, const char *value,
                                  CssTransformOrigin *out);

#endif
