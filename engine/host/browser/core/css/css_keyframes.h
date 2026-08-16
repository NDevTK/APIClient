/* WHAT A `<keyframe-block>` MAY DECLARE — CSS Animations Level 1 §3's closed membership question, which is the
 * fact CSSOM's CSSKeyframeRule is built out of and which nothing else in this engine can answer.
 *
 * IT IS ONE ENTRY BECAUSE IT IS ONE SENTENCE, AND THE SENTENCE HAS TWO HALVES. §3 states both of them about
 * the same `<declaration-list>`: "The <declaration-list> inside of <keyframe-block> accepts any CSS property
 * except those defined in this specification, but does accept the animation-timing-function property and
 * interprets it specially. None of the properties interact with the cascade (so using !important on them is
 * invalid and will cause the property to be ignored)", restated three paragraphs later as "In addition,
 * properties qualified with !important are invalid and ignored." A declaration is in the block when BOTH hold,
 * so one entry answers the block's question and two could disagree about a declaration that fails only one.
 *
 * THE LIST IS A LIST OF THIS SPECIFICATION'S OWN PROPERTIES, WHICH IS WHY IT CAN BE CLOSED. §3 does not
 * enumerate what a keyframe admits — it admits every CSS property there is — it enumerates what it REFUSES, and
 * that set is "those defined in this specification", which is §4's nine property-definition tables and nothing
 * else. So the table below is transcribed from §4's own `Name:` lines in the spec's own order, and a property
 * a later level of CSS Animations defines joins it when that level's property exists to be declared: a name
 * here that no other component knows would be a refusal nothing can trigger, which is a table entry with no
 * meaning rather than a stricter rule.
 *
 * THE PAGE CONTEXT'S RESTRICTION IS THE OTHER DIRECTION AND THEREFORE NOT THIS COMPONENT. CSS Paged Media
 * Appendix A publishes the ALLOWED names (core/css/css_page.h), so a longhand of an admitted shorthand has to
 * be admitted with it — the set grew under the list. Here the refused set is closed and the admitted set is
 * everything else, so the `animation` shorthand is refused BY NAME and its longhands are each refused by name
 * too; there is no shorthand table to read and reading one would be a mechanism with nothing to decide.
 *
 * A CUSTOM PROPERTY IS ASKED ABOUT HERE, WHERE THE PAGE CONTEXT REFUSES TO BE. `--x` is not a property CSS
 * Animations defines, so the name half admits it exactly as it admits `color`; but the IMPORTANCE half is
 * about the declaration and not about the property, so `--x: 1 !important` inside a keyframe is invalid like
 * every other important declaration there. An entry that took only a name could not say that. */
#ifndef ENGINE_HOST_BROWSER_CORE_CSS_CSS_KEYFRAMES_H
#define ENGINE_HOST_BROWSER_CORE_CSS_CSS_KEYFRAMES_H

#include <stdbool.h>

/* Is a declaration of `name` — a property name, ASCII-lowercased, a custom property included — with this
   `important` flag IN a `<keyframe-block>`'s declaration list? See above for the two halves of the sentence
   this answers, and core/css/css_style_declaration.h for the one place it is asked. */
bool css_keyframes_declaration_applies(const char *name, bool important);

#endif
