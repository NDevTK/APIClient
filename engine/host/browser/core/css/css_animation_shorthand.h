/* CSS Animations Level 1 §4 "Declaring Animations": the eight animation properties' grammars, §4.9's `animation`
 * shorthand that sets all eight, and their initial and computed values.
 *
 * The eight form a coordinating list property group with `animation-name` as its base (§4), so every value is a
 * comma-separated list and the shorthand's Nth `<single-animation>` gives every longhand its Nth item. None of
 * the eight is in lexbor's property registry, so this component is their only grammar.
 *
 * Named residual: a `<time>` or `<number>` written as a math function (`calc(1s * 2)`) is not admitted, so such
 * a declaration is dropped as invalid. The next diff parses it through core/css/css_math.h's calculation trees.
 * Its absence shows as a computed `animation-duration` reading `0s` where the page wrote a calc(). */
#ifndef ENGINE_HOST_BROWSER_CORE_CSS_CSS_ANIMATION_SHORTHAND_H
#define ENGINE_HOST_BROWSER_CORE_CSS_CSS_ANIMATION_SHORTHAND_H

#include <stdbool.h>

/* The longhands, in `<single-animation>`'s own order (§4.9), which is the order the shorthand serializes its
   terms in: duration, timing function, delay, iteration count, direction, fill mode, play state, name. */
#define CSS_ANIMATION_SHORTHAND_N 8
extern const char *const CSS_ANIMATION_SHORTHAND_LONGHANDS[CSS_ANIMATION_SHORTHAND_N];

/* §4.9's expansion: the value `animation: <value>` gives `longhand`, as that longhand's comma-separated list.
   NULL for a name §4.9 does not set and for a value outside its grammar (an invalid declaration). OWNED. */
char *css_animation_shorthand_component(const char *value, const char *longhand);
/* CSSOM §6.7.2's serialization of the shorthand from its eight longhands' values (indexed as the list above).
   NULL where the lists differ in length or one holds a CSS-wide keyword the others do not. OWNED. */
char *css_animation_shorthand_value(const char *const *values);
/* Is `longhand` one of the eight? */
bool css_animation_shorthand_validates_longhand(const char *longhand);
/* A declaration of one longhand, validated against its own `Value:` line and serialized; NULL when invalid.
   OWNED. */
char *css_animation_shorthand_longhand_value(const char *longhand, const char *value);
/* A longhand's `Initial:` line; NULL for a name that is not one of the eight. Static. */
const char *css_animation_shorthand_initial(const char *longhand);
/* A longhand's computed value from its specified value (consumed): `animation-duration` and `animation-delay`
   are "list, each item a duration", serialized in seconds; the other six are their specified lists. OWNED. */
char *css_animation_shorthand_computed(const char *longhand, char *specified);

/* One item of a computed `<time>` list, in milliseconds. False for a malformed item, which a computed value of
   this component never holds. */
bool css_animation_time_ms(const char *item, double *ms);

#endif
