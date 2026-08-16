/* THE GRAMMAR OF A DECLARATION LEXBOR'S REGISTRY DOES NOT TYPE — which is CSS Cascade §Shorthand Properties
 * for a shorthand, and the property's own value grammar for a longhand.
 *
 * WHY LEXBOR DOES NOT DO IT. Lexbor's property registry carries `overflow-x` and `overflow-y` and does NOT
 * carry `overflow`, so `overflow: hidden` reaches the cascade as an UNKNOWN declaration — which lexbor still
 * parses, still names and still serializes (it becomes a `LXB_CSS_PROPERTY__CUSTOM` holding the name and the
 * raw value tokens), so nothing is lost, it is simply not expanded. Expanding it HERE rather than adding a
 * property to lexbor's GENERATED registry keeps the vendored parser at its pinned tag, which is the whole
 * reason this engine binds to it.
 *
 * THE CASCADE IS OVER LONGHANDS ONLY, which is why the first half is a component and not a line inside the
 * cascade: a declaration sets a longhand either by BEING it or by being a shorthand of it, and the second way
 * is a per-shorthand GRAMMAR (`overflow: <'overflow-block'>{1,2}` is not `margin`'s four-side rotation and is
 * not `border`'s any-order triple). Each is its own small parse with its own invalid case, and an invalid
 * shorthand value is a DROPPED declaration rather than a partial one — which is exactly the contract a caller
 * wants back: a value, or nothing.
 *
 * AND THE FIRST WAY NEEDS THE SAME GRAMMAR, which is why both halves are one component rather than two. For a
 * longhand lexbor DOES carry, its own parser has already validated the declaration and serialized it back
 * canonically, so the value is taken verbatim. For one it does not — the four `border-*-width` and the four
 * `border-*-style`, which are `__CUSTOM` like every shorthand here — NOTHING has applied the property's
 * grammar, so `border-top-style: bogus` would win the cascade and be reported as a computed value no grammar
 * admits (css_style_declaration.c records `display: bogus` doing exactly that through lexbor's `__UNDEF`), and
 * `border-top-style: SOLID` would fail to compare equal to the keyword it is. Those are the same two jobs the
 * `border-style` shorthand's expansion does to each of its components, off the same list — so a longhand
 * declaration goes through this file too, and one grammar answers both spellings. */
#ifndef ENGINE_HOST_BROWSER_CORE_CSS_CSS_SHORTHAND_H
#define ENGINE_HOST_BROWSER_CORE_CSS_CSS_SHORTHAND_H
#include <stdbool.h>

/* The SPECIFIED value that the declaration `shorthand: value` gives to `longhand`. NULL when `shorthand` is not
   one this component expands, when it does not set `longhand`, or when `value` does not match the shorthand's
   grammar — the last is an INVALID declaration, which the cascade drops, and returning NULL is how it is
   dropped. OWNED: the caller frees. */
char *css_shorthand_component(const char *shorthand, const char *value, const char *longhand);

/* Does this component own `longhand`'s OWN value grammar — is it one lexbor's property registry does not carry
   and nothing else has validated? The cascade asks before it takes a declaration's value verbatim, because the
   answer is what decides whether that value has been through a grammar at all. FALSE for every property lexbor
   types (its parser is the grammar) and for a CSS custom property (`--brand`, whose value is by definition
   whatever the author wrote). */
bool css_shorthand_validates_longhand(const char *longhand);

/* The SPECIFIED value the declaration `longhand: value` gives `longhand` itself, put through that grammar:
   canonicalized where the grammar is a keyword, verbatim where it is a length. NULL when the value does not
   match — an INVALID declaration, which the cascade drops. Only ever called for a name the predicate above
   answers TRUE for, and it asserts that. OWNED: the caller frees. */
char *css_shorthand_longhand_value(const char *longhand, const char *value);

/* Is the set of shorthands that can set `longhand` recorded here IN FULL? A consumer that derives a longhand's
   COMPUTED value asserts this before it trusts the cascade, because the failure mode of an unrecorded
   shorthand is silence: `margin: 0` would leave `margin-top` reading its initial value, with a real number to
   show for it and nothing to say the declaration was never looked at. */
bool css_shorthand_complete_for(const char *longhand);

#endif
