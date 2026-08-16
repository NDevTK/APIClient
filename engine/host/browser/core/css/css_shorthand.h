/* CSS Cascade §Shorthand Properties — a shorthand declaration sets its LONGHANDS.
 *
 * THE CASCADE IS OVER LONGHANDS ONLY, which is why this is a component and not a line inside the cascade: a
 * declaration sets a longhand either by BEING it or by being a shorthand of it, and the second half is a
 * per-shorthand GRAMMAR (`overflow: <'overflow-block'>{1,2}` is not `margin`'s four-side rotation and is not
 * `border`'s any-order triple). Each is its own small parse with its own invalid case, and an invalid shorthand
 * value is a DROPPED declaration rather than a partial one — which is exactly the contract a caller wants back:
 * a value, or nothing.
 *
 * WHY LEXBOR DOES NOT DO IT. Lexbor's property registry carries `overflow-x` and `overflow-y` and does NOT
 * carry `overflow`, so `overflow: hidden` reaches the cascade as an UNKNOWN declaration — which lexbor still
 * parses, still names and still serializes (it becomes a `LXB_CSS_PROPERTY__CUSTOM` holding the name and the
 * raw value tokens), so nothing is lost, it is simply not expanded. Expanding it HERE rather than adding a
 * property to lexbor's GENERATED registry keeps the vendored parser at its pinned tag, which is the whole
 * reason this engine binds to it. */
#ifndef ENGINE_HOST_BROWSER_CORE_CSS_CSS_SHORTHAND_H
#define ENGINE_HOST_BROWSER_CORE_CSS_CSS_SHORTHAND_H
#include <stdbool.h>

/* The SPECIFIED value that the declaration `shorthand: value` gives to `longhand`. NULL when `shorthand` is not
   one this component expands, when it does not set `longhand`, or when `value` does not match the shorthand's
   grammar — the last is an INVALID declaration, which the cascade drops, and returning NULL is how it is
   dropped. OWNED: the caller frees. */
char *css_shorthand_component(const char *shorthand, const char *value, const char *longhand);

/* Is the set of shorthands that can set `longhand` recorded here IN FULL? A consumer that derives a longhand's
   COMPUTED value asserts this before it trusts the cascade, because the failure mode of an unrecorded
   shorthand is silence: `margin: 0` would leave `margin-top` reading its initial value, with a real number to
   show for it and nothing to say the declaration was never looked at. */
bool css_shorthand_complete_for(const char *longhand);

#endif
