/* css-shapes-1 §3 "Basic Shapes"' `<basic-shape>` grammar (§3.1 "Supported Shapes" and §3.1.1 "The shape()
 * Function"), and css-masking-1 §5.1 "Clipping Shape: the clip-path property"'s `<clip-source> | [
 * <basic-shape> || <geometry-box> ] | none` over it. */

#ifndef ENGINE_HOST_BROWSER_CORE_CSS_CSS_BASIC_SHAPE_H
#define ENGINE_HOST_BROWSER_CORE_CSS_CSS_BASIC_SHAPE_H

#include <stdbool.h>
#include <stddef.h>

/* Whether `text` (its first `n` bytes) is one `<basic-shape>` function. */
bool css_basic_shape_is(const char *text, size_t n);

/* `value` against clip-path's grammar: the specified value with one space between components, or NULL for a
   value outside it (page input, so the declaration is dropped). OWNED. */
char *css_clip_path_specified(const char *value);

#endif
