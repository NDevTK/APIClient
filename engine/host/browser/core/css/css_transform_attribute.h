/* css-transforms-1 §6 "The SVG transform Attribute": §6.2 "Syntax of the SVG transform attribute"'s grammar
 * for the `transform`, `patternTransform` and `gradientTransform` attributes, and §6.3 "SVG transform
 * functions"' mapping of each function to the CSS `<transform-function>` the attribute stands for as a
 * presentation attribute of the `transform` property (§6.1 "SVG presentation attributes"). */
#ifndef ENGINE_HOST_BROWSER_CORE_CSS_CSS_TRANSFORM_ATTRIBUTE_H
#define ENGINE_HOST_BROWSER_CORE_CSS_CSS_TRANSFORM_ATTRIBUTE_H

#include <stddef.h>

/* The `transform` property value the attribute's `len` bytes at `value` map to: §6.3's CSS functions, numbers
   in px or deg, or `none` for a list with no function. NULL when the bytes are outside §6.2's grammar, which is
   page input. OWNED. */
char *css_transform_attribute_to_css(const char *value, size_t len);

#endif
