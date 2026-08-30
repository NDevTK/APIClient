/* CSS Backgrounds and Borders 3 §2.10 "Backgrounds Shorthand: the background property" — the one shorthand in
 * this engine whose value is a COMMA-SEPARATED LIST OF LAYERS, and the reason it is a component rather than a
 * fifth row-kind inside core/css/css_shorthand.c.
 *
 * WHY THE SHAPE IS DIFFERENT. Every kind css_shorthand.c implements is arithmetic over ONE component grammar
 * applied to ONE value — a four-side rotation, a two-axis copy, a three-term `||`. §2.10's `Value:` line is
 * `<bg-layer># ?, <final-bg-layer>`, so ONE declaration produces a LIST per longhand, of a length the
 * declaration itself decides, and the FINAL layer's grammar differs from the others' by one term (§2.10's own
 * Note: "a color is permitted in <final-bg-layer>, but not in <bg-layer>"). On top of that its `||` has a term
 * with an INFIX (`<bg-position> [ / <bg-size> ]?`), two terms over the SAME keyword set whose meaning is
 * decided by ORDER (`<visual-box> || <visual-box>`), and a term whose grammar is a whole module
 * (core/css/css_image.h). One problem per file: css_shorthand.c owns the TABLE and the two directions over it,
 * this file owns §2.10, exactly as core/css/css_font_shorthand.h owns css-fonts-4 §2.7.
 *
 * WHAT WAS ACTUALLY BROKEN, AND IT WAS NOT `background-image`. `background-color` is in CSSOM §9's
 * unconditional used-value list, so `getComputedStyle(el).backgroundColor` is a used value; every consumer of
 * a used or computed value asserts css_shorthand_complete_for first, because the failure mode of an
 * unrecorded shorthand is SILENCE — a `background: red` two lines above the read would be invisible and the
 * answer would be `transparent` with a real colour to show for it. `background` is the ONLY shorthand in CSS
 * that sets `background-color`, so that assert could not pass until this grammar existed.
 *
 * SEVEN OF THE EIGHT LONGHANDS ARE NOT IN LEXBOR'S PROPERTY REGISTRY — it carries `background-color` and none
 * of the others — so nothing has applied their own value grammars either, exactly as nothing had applied the
 * four `border-*-width` and four `border-*-style` grammars. That is why this file publishes the longhand
 * direction as well: a `background-repeat: bogus` is a declaration CSS Syntax DROPS, and dropping it is what
 * the second pair of entries decides. */
#ifndef ENGINE_HOST_BROWSER_CORE_CSS_CSS_BACKGROUND_SHORTHAND_H
#define ENGINE_HOST_BROWSER_CORE_CSS_CSS_BACKGROUND_SHORTHAND_H
#include <stdbool.h>

/* §2.10's LONGHANDS, in the CANONICAL ORDER of its own `<bg-layer>` grammar — `<bg-image> || <bg-position>
   [ / <bg-size> ]? || <repeat-style> || <attachment> || <visual-box> || <visual-box>`, then
   `<'background-color'>`, which only `<final-bg-layer>` admits.
   THE ORDER IS LOAD-BEARING: css_shorthand.c's §6.7.2 serialization hands this file a parallel value array and
   it is read by index, and CSSOM §6.7.2's first syntactic rule is to write a `||`'s terms in exactly this
   order. The two `<visual-box>` slots are `background-origin` then `background-clip`, which is §2.10's own
   sentence ("if two values are present, then the first sets background-origin and the second
   background-clip") and is the only thing that tells them apart. */
#define CSS_BACKGROUND_SHORTHAND_N 8
extern const char *const CSS_BACKGROUND_SHORTHAND_LONGHANDS[CSS_BACKGROUND_SHORTHAND_N];

/* THE SPECIFIED VALUE the declaration `background: value` gives `longhand`. OWNED: the caller frees.
   NULL for a `longhand` §2.10 does not name, and for a `value` outside §2.10's grammar — which is CSS Syntax's
   INVALID DECLARATION, dropped whole, so a `background` whose third layer fails sets none of the eight rather
   than the layers parsed before the failure. */
char *css_background_shorthand_component(const char *value, const char *longhand);

/* CSSOM §6.7.2's SERIALIZE A CSS VALUE over the list — the value a hypothetical `background` declaration would
   carry, given `values[i]` as the serialized value of CSS_BACKGROUND_SHORTHAND_LONGHANDS[i]. Every entry must
   be non-NULL: the caller has already established that the block declares all eight.
   NULL is §6.7.2's "the shorthand cannot exactly represent the values of all the properties in list", and for
   `background` there are three ways to reach it, each one a sentence of §2.10: the seven list-valued longhands
   do not agree on how many LAYERS there are (the shorthand states one count for all of them); a non-final
   layer would need a colour (§2.10's Note admits one only in `<final-bg-layer>`); and one longhand carries a
   CSS-wide keyword while another does not. OWNED. */
char *css_background_shorthand_value(const char *const *values);

/* Does this component own `longhand`'s OWN value grammar? TRUE for the seven §2.10 names that lexbor's
   property registry does not carry, FALSE for `background-color` — lexbor types that one, so its parser has
   already validated and canonically serialized the declaration and a second `<color>` grammar here would be a
   second answer to one question. Asked by the cascade before it takes a longhand declaration verbatim. */
bool css_background_shorthand_validates_longhand(const char *longhand);

/* The SPECIFIED value the declaration `longhand: value` gives `longhand` itself, put through that grammar and
   through each type's own serialization rule (css-values-4 §8.3.2 for `<bg-position>`, css-backgrounds-3
   §2.9.1 for `<bg-size>`). NULL when the value matches no arm — an INVALID declaration, which the cascade
   drops. Only ever called for a name the predicate above answers TRUE for, and it asserts that. OWNED. */
char *css_background_shorthand_longhand_value(const char *longhand, const char *value);

#endif
