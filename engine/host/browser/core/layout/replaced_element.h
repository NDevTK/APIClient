/* IS THIS A REPLACED ELEMENT, AND WHAT ARE ITS NATURAL DIMENSIONS — HTML §15.4 "Replaced elements" and
 * css-images-3 §4.1 "Object-Sizing Terminology", which are the TWO facts CSS 2.1 §10.3.2 "Inline, replaced
 * elements" is stated over and the only two it needs from outside CSS.
 *
 * WHY IT IS A COMPONENT AND NOT A PREDICATE INSIDE THE LAYOUT. CSS 2.1 §3.1 defines a replaced element as "an
 * element whose content is outside the scope of the CSS formatting model" and then declines to say which
 * elements those are: "the user agent determines ... In some cases ... the element itself is not replaced, but
 * its contents are". So CSS cannot answer it, and the answer is HTML's — §15.4's own opening sentence names
 * the eight elements that CAN be replaced, and §15.4.1 "Embedded content" and §15.4.2 "Images" then decide
 * WHICH OF THEM IS ONE RIGHT NOW. That is not a tag test: for an `img` it is a fact about the image request's
 * state and about the `alt` attribute, and it CHANGES under the running flow as a reply lands. Two consumers
 * already ask it and they are in different directories — core/css/css_property_applies.c needs it for CSS 2.1
 * §10.2's "Applies to: all elements but NON-REPLACED inline elements", and core/layout/used_value.c needs it
 * to choose between §10.3.3's constraint equation and §10.3.2's intrinsic-dimension arms — so a predicate
 * living inside either would be the one fact answered from two places.
 *
 * WHAT STOOD IN ITS PLACE WAS `css_element_may_be_replaced`, AND IT WAS A DIFFERENT QUESTION WEARING THIS ONE'S
 * NAME. It tested membership of css-display Appendix B's list of HTML elements that "aren't rendered purely by
 * CSS box concepts", which is a SUPERSET drawn up for a different purpose: it carries `br`, `wbr`, `meter`,
 * `progress`, `select` and `textarea`, none of which appear in §15.4's list of elements that can be replaced,
 * and it carries `img` unconditionally, which is wrong in the other direction — §15.4.2's THIRD rule makes an
 * `img` that represents text a NON-REPLACED phrasing element. Both errors are silent: the first resolves
 * `width` on a `<select>` through an algorithm that does not apply to it, and the second would report a used
 * width for an element whose `width` does not apply and whose resolved value CSSOM §9 makes the computed
 * `auto`. It is DELETED rather than kept beside this, because a superset predicate is exactly the fallback a
 * caller reaches for when this one crashes.
 *
 * THE NATURAL DIMENSIONS ARE css-images-3's TERM AND CSS 2.1's "INTRINSIC" ONES ARE THE SAME THREE. §4.1:
 * "the term natural dimensions refers to the set of the natural height, natural width, and natural aspect
 * ratio ..., EACH OF WHICH MAY OR MAY NOT EXIST for a given object". That "may or may not exist" is the whole
 * shape of §10.3.2 — its five arms are a decision tree over WHICH of the three are present — so each is
 * carried as a presence flag beside its value and never as a sentinel number, for the reason CLAUDE.md gives
 * for every defaulted field: a natural width of 0 is a REAL answer (§15.4.2's fourth rule gives one to an
 * `img` that represents nothing) and is indistinguishable from "there is none" if the two share a spelling.
 *
 * A DEGENERATE RATIO IS NO RATIO, which is §4.1's own sentence and not a guard against division by zero: "if
 * an object has a degenerate natural aspect ratio (at least one part being zero or infinity), it is treated as
 * having no natural aspect ratio". So the 0-by-0 object above has a natural width AND a natural height AND no
 * ratio, which is a combination §4.1 says most objects cannot have and this one does.
 *
 * WHAT THIS ENGINE CAN ANSWER, AND WHY THE ABSENT DECODER IS NOT A SHRUG. §4.1's own examples do the work: an
 * embedded document "such as the iframe element in HTML" is "an example of an object with NO NATURAL
 * DIMENSIONS AT ALL", so an `iframe` is answered completely and correctly here with no layout of the child
 * document and no decoder — and CSS 2.1 §10.3.2's last arm then makes its box 300 x 150, which is the number
 * core/frame/viewport.c derives a child navigable's viewport from. An `img` is answered from the image
 * request state core/html/html_image.h already models, which is the same state a real browser is in: while the
 * fetch is outstanding the user agent has not yet learned that it cannot decode the reply, and §15.4.2's
 * second rule is written about exactly that moment. What is genuinely absent — a DECODED image's natural
 * dimensions, a plugin's, a video frame's — crashes at the arm that would have to read it, naming the decoder
 * rather than answering a plausible number.
 */
#ifndef ENGINE_HOST_BROWSER_CORE_LAYOUT_REPLACED_ELEMENT_H
#define ENGINE_HOST_BROWSER_CORE_LAYOUT_REPLACED_ELEMENT_H

#include <stdbool.h>

#include <lexbor/dom/dom.h>

#include "core/css/css_length.h"

/* HTML §15.4's classification of ONE element AT THIS MOMENT, with css-images-3 §4.1's three natural dimensions
   beside it. `replaced` false means the element is not a replaced element, and the other fields are then
   meaningless and are zeroed — a caller that reads a dimension off a non-replaced answer is asking §10.3.2 a
   question about a box §10.3.3 owns.
   THE VALUES ARE `CssPx` AND NOT `double` for used_value.h's reason: a natural dimension is an operand of the
   used-value arithmetic, that arithmetic carries the UNION of its operands' environment facts (css_length.h),
   and a bare number entering it would silently drop the domain of everything it was combined with. A decoded
   image's own dimensions are not a function of any environment fact this engine models, so they arrive with
   `CSS_ENV_NONE` — which css_length.h makes a POSITIVE statement (the value has a single-point domain), not a
   hole. */
typedef struct {
    bool   replaced;
    bool   has_width;    /* css-images-3 §4.1's NATURAL WIDTH is present */
    bool   has_height;   /* its NATURAL HEIGHT is present */
    bool   has_ratio;    /* its NATURAL ASPECT RATIO is present and is not degenerate */
    CssPx  width;
    CssPx  height;
    double ratio;        /* the ratio BETWEEN THE WIDTH AND HEIGHT (§4.1), so width = height * ratio */
} ReplacedElement;

ReplacedElement replaced_element_of(lxb_dom_element_t *el);

#endif
