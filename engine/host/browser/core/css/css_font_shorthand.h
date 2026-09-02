/* CSS Fonts 4 §2.7 SHORTHAND FONT PROPERTY: THE FONT PROPERTY — the one shorthand whose grammar is a
 * SEQUENCE rather than a rotation or a `||`, and the one whose longhand list is nineteen names long.
 *
 * WHY IT IS ITS OWN COMPONENT AND NOT A FIFTH ROW-KIND INSIDE core/css/css_shorthand.c. The four kinds that
 * file implements are each two or three lines of arithmetic over ONE component grammar — a four-side rotation,
 * a two-axis copy, a three-term `||`. §2.7 is a different shape entirely: it is a positional sequence with an
 * optional unordered PREFIX, an infix `/`, a trailing comma-list, and a second whole arm
 * (`<system-font-family-name>`); and validating it needs SIX component grammars that nothing else in this
 * engine has (§2.4's `<'font-style'>`, §2.7's `<font-variant-css2>` and `<font-width-css3>`, §2.2's
 * `<'font-weight'>`, §2.5's `<'font-size'>`, css-inline-3 §5.1's `<'line-height'>` and §2.1.1's
 * `<font-family-name>`). One problem per file: css_shorthand.c owns the TABLE and the two directions over it,
 * and this file owns §2.7.
 *
 * NOTHING VALIDATED THESE COMPONENTS BEFORE THIS FILE, which is the same reason css_shorthand.c owns
 * `<line-style>` and `<line-width>`: lexbor's property registry carries `font-family`, `font-size`,
 * `font-stretch`, `font-style`, `font-weight` and `line-height` but NOT `font`, so a `font` declaration reaches
 * the cascade as a `__CUSTOM` holding the name and the RAW TOKENS. CSS Syntax DROPS an invalid declaration
 * whole, and a `font: 12px/bogus x` that flowed through would set `line-height` to a value no grammar admits.
 *
 * THE RESET-ONLY GROUP IS PART OF THE CASCADE AND NOT A DETAIL OF IT. §2.7 states it outright: "All
 * subproperties of the font property in the Set Explicitly and Reset Implicitly groups are first reset to their
 * initial values. Then, those properties [in] the Set Explicitly group that are given explicit values in the
 * font shorthand are set to those values." So `font: 12px x` two lines below a `font-kerning: none` sets
 * `font-kerning` back to `auto`, and a longhand list that named only the seven settable ones would report the
 * `none` as the winning declaration. That is why the list below is nineteen names and why
 * CSS_SHORTHAND_MAX_LONGHANDS had to grow past `border`'s seventeen.
 *
 * §2.3.1 MAKES `font-stretch` A LEGACY NAME ALIAS OF `font-width` — "a font-stretch property exists which is a
 * legacy name alias and functions in the identical way to the font-width" — so they are ONE property under two
 * names and the list below names it ONCE. It names `font-stretch`, because that is the spelling lexbor's
 * registry carries: it has the entry, the `Initial: normal` and the value grammar every other reader of this
 * cascade goes through, and naming the level-4 spelling would put one property in the table under a name no
 * declaration in this engine can be keyed by.
 *
 * WHAT THIS FILE DOES NOT DECIDE: what a `font-size` COMPUTES to. §2.5's `Computed value: an absolute length`
 * is core/css/css_computed_value.h's step and it is the one core/css/css_length.h's font-relative units are
 * waiting on. This file produces the SPECIFIED value — the declaration's own text, put through §2.7's grammar
 * — which is the layer the whole cascade is stated over. */
#ifndef ENGINE_HOST_BROWSER_CORE_CSS_CSS_FONT_SHORTHAND_H
#define ENGINE_HOST_BROWSER_CORE_CSS_CSS_FONT_SHORTHAND_H
#include <stdbool.h>

/* css-fonts-4 §2.7 "Shorthand font property: the font property"'s LONGHANDS, and the split inside them.
   THE FIRST SEVEN ARE THE `Set Explicitly` GROUP, IN THE CANONICAL ORDER OF §2.7's OWN GRAMMAR —
   `[ <'font-style'> || <font-variant-css2> || <'font-weight'> || <font-width-css3> ]? <'font-size'>
   [ / <'line-height'> ]? <'font-family'>#`. The order is LOAD-BEARING: css_shorthand.c's §6.7.2 serialization
   hands this file a parallel value array and it is read by index.
   THE REMAINING TWELVE ARE THE `Reset Implicitly` GROUP, and css-fonts-4 §2.7 is named again here rather than
   carried down from the banner because the citation between the two is CSSOM's: "These properties are a
   reset-only sub-property of the font property and thus may not be set, but are reset to their initial
   values". Their order is the spec's own listing, which is alphabetical; nothing reads them positionally,
   only as a set. */
#define CSS_FONT_SHORTHAND_N     19
#define CSS_FONT_SHORTHAND_SET_N 7
extern const char *const CSS_FONT_SHORTHAND_LONGHANDS[CSS_FONT_SHORTHAND_N];

/* THE SPECIFIED VALUE the declaration `font: value` gives `longhand`. OWNED: the caller frees.
   NULL for a `longhand` §2.7 does not name, and for a `value` outside §2.7's grammar — which is CSS Syntax's
   INVALID DECLARATION, dropped whole, so a `font` whose size fails its grammar sets none of the nineteen
   rather than the ones parsed before the failure. */
char *css_font_shorthand_component(const char *value, const char *longhand);

/* CSSOM §6.7.2's SERIALIZE A CSS VALUE over the list — the value a hypothetical `font` declaration would carry
   given `values[i]` as the serialized value of CSS_FONT_SHORTHAND_LONGHANDS[i]. Every entry must be non-NULL.
   NULL is §6.7.2's "the shorthand cannot exactly represent the values of all the properties in list", and for
   `font` there are three ways to reach it, each one a sentence of §2.7: a reset-only longhand that does not
   hold its initial value (the shorthand would reset it and the block does not say so), a `font-variant-caps`
   outside `<font-variant-css2>` ("none of the font-variant values added in CSS Fonts Levels 3 or 4 can be used
   in the font shorthand"), and a `font-stretch` outside `<font-width-css3>` (the same sentence for widths).
   OWNED. */
char *css_font_shorthand_value(const char *const *values);

#endif
