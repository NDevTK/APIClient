/* CSS Color 5 §3 "Mixing Colors: the color-mix() Function": the grammar, css-values-5 §6.1 "Normalizing Mix
 * Percentages" and §3.3 "Calculating the Result of color-mix", over CSS Color 4 §13 "Color Interpolation".
 * Its own component because the production is Color 5's and lexbor's `<color>` grammar does not have it;
 * css_color.c routes a string whose first token is the `color-mix(` function here, and each argument back
 * through css_color_parse_in, so a nested colour is any colour that file reads. */
#ifndef ENGINE_HOST_BROWSER_CORE_CSS_CSS_COLOR_MIX_H
#define ENGINE_HOST_BROWSER_CORE_CSS_CSS_COLOR_MIX_H

#include <stddef.h>

#include "core/css/css_color.h"

typedef enum {
    CSS_COLOR_MIX_NOT_A_MIX,   /* the input does not begin with the `color-mix(` function token */
    CSS_COLOR_MIX_FAILURE,     /* it does, and it is not a valid color-mix() */
    CSS_COLOR_MIX_PARSED
} CssColorMixResult;

/* The whole of `text` as one color-mix(), resolved to its colour in the mixing colour space (CSS Color 5 §10.1
   "Resolving color-mix() Values"). `current` is what a `currentcolor` argument resolves to, NULL for CSS Color
   4 §4.5's context-free parse. `out` is written only on CSS_COLOR_MIX_PARSED. */
CssColorMixResult css_color_mix_parse(const char *text, size_t len, const CssColor *current, CssColor *out);

#endif
