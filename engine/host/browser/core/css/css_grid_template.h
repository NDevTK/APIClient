/* css-grid-1 §7.4 "Explicit Grid Shorthand: the grid-template property" and §7.8 "Grid Definition Shorthand:
 * the grid property", with the value grammars of the six longhands they set: §7.2 "Explicit Track Sizing: the
 * grid-template-rows and grid-template-columns properties", §7.3 "Named Areas: the grid-template-areas
 * property", §7.6 "Implicit Track Sizing: the grid-auto-rows and grid-auto-columns properties" and §7.7
 * "Automatic Placement: the grid-auto-flow property".
 *
 * Lexbor's property registry carries none of the eight, so a declaration reaches the cascade as raw tokens with
 * nothing having applied a grammar to it. This component is that grammar in both spellings: a longhand
 * declaration is validated and put in its canonical specified form, and a shorthand declaration is expanded
 * into the same canonical forms, so one grammar answers both. A value outside the grammar is CSS Syntax's
 * invalid declaration, dropped whole: a shorthand that fails anywhere sets none of its longhands.
 * core/css/css_shorthand.c owns the two table rows; this file owns the grammars and the serializations. */
#ifndef ENGINE_HOST_BROWSER_CORE_CSS_CSS_GRID_TEMPLATE_H
#define ENGINE_HOST_BROWSER_CORE_CSS_CSS_GRID_TEMPLATE_H
#include <stdbool.h>

/* §7.4's longhands in the order css_shorthand.c's serialization hands their values back, and §7.8's: the same
   three, then the three implicit-grid properties §7.8 resets. */
#define CSS_GRID_TEMPLATE_N 3
#define CSS_GRID_N 6
extern const char *const CSS_GRID_LONGHANDS[CSS_GRID_N];

/* The specified value `grid-template: value` (`grid` false) or `grid: value` gives `longhand`. OWNED: the
   caller frees. NULL for a longhand the shorthand does not set and for a value outside its grammar. */
char *css_grid_template_component(bool grid, const char *value, const char *longhand);

/* CSSOM §6.7.2's serialization of a hypothetical `grid-template` (`grid` false, three values) or `grid` (six)
   declaration from the longhands' serialized values, in CSS_GRID_LONGHANDS order. NULL where the shorthand
   cannot exactly represent them. OWNED. */
char *css_grid_template_value(bool grid, const char *const *values);

/* Whether this component owns `longhand`'s value grammar: the six CSS_GRID_LONGHANDS. */
bool css_grid_template_validates_longhand(const char *longhand);

/* The specified value `longhand: value` gives `longhand`, in canonical form, or NULL for a value outside its
   grammar. `longhand` must be one this component owns (asserted). OWNED. */
char *css_grid_template_longhand_value(const char *longhand, const char *value);

#endif
