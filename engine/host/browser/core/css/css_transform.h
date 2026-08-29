/* CSS TRANSFORMS 1 — the two ELEMENT-LEVEL facts the module defines, and the walk its consumers ask over.
 *
 * WHICH EDITION'S NUMBERS THESE ARE, BECAUSE THE TWO DISAGREE BY ONE. css-transforms-1's CR carries "§2
 * Terminology" and its current Editor's Draft renders that same heading with NO NUMBER AT ALL, so every
 * section below it differs: `The transform Property` is §4 in the CR and §3 in the ED, and `Resolved value of
 * transform` is §4.2 and §3.2. EVERY NUMBER IN THIS COMPONENT IS THE ED'S — which is what the rest of this
 * tree cites — WITH ONE NAMED EXCEPTION: "Terminology" is cited as §2, the CR's number, because the ED offers
 * none to cite. Each carries its TITLE, which is what survives the renumbering and is how a reader holding
 * either edition finds it.
 *
 * WHAT THIS COMPONENT IS FOR, AND WHY IT IS NOT A LINE IN ITS CALLERS. Three algorithms in three different
 * specifications ask ONE question — "do any transforms apply to this element and its ancestors" — and each of
 * them has to answer it before it can produce a coordinate:
 *   CSSOM VIEW §6 Extensions to the Element Interface, getClientRects() step 3's first constraint
 *     ("Apply the transforms that apply to the element and its ancestors");
 *   INTERSECTION OBSERVER §3.2.9 "Calculate a target's Effective Transformation Matrix", up a containing
 *     block chain;
 *   css-transforms-1 §3.2 Resolved value of transform, which needs only the element's own.
 * A question three files ask is a question three files can answer differently, and the answer here is a
 * CONJUNCTION of two facts that are easy to hold apart wrongly — the property's `Applies to:` line and its
 * computed value — so it is derived once, here, out of the module that defines both.
 *
 * THE TWO FACTS, AND WHY NEITHER ALONE IS THE ANSWER. §2 Terminology defines a TRANSFORMED ELEMENT as "an
 * element with a computed value other than none for the transform property", and it defines a TRANSFORMABLE
 * ELEMENT as one whose layout is governed by the CSS box model, excepting non-replaced inline boxes,
 * table-column boxes and table-column-group boxes (plus a second, SVG category). §3's own `Applies to:` line
 * is "transformable elements", so a `transform` declared on a non-replaced inline box HAS a computed value
 * and applies to NOTHING — `<span style="transform:rotate(45deg)"><b>x</b></span>` leaves `b`'s client
 * rectangle exactly where it was in every user agent. Reading only the computed value would make that span a
 * reason to refuse an answer this engine can give; reading only the applies-to line would ignore a real
 * rotation on the `div` above it. */
#ifndef ENGINE_HOST_BROWSER_CORE_CSS_CSS_TRANSFORM_H
#define ENGINE_HOST_BROWSER_CORE_CSS_CSS_TRANSFORM_H

#include <stdbool.h>

#include <lexbor/dom/dom.h>

/* §2 Terminology's TRANSFORMABLE ELEMENT, first category — "all elements whose layout is governed by the CSS
   box model except for non-replaced inline boxes, table-column boxes, and table-column-group boxes [CSS2]".
   The SECOND category is SVG's and this engine lays out no SVG, so an element in that namespace crashes by
   name rather than being answered out of the CSS box model it is not in. */
bool css_transform_is_transformable(lxb_dom_element_t *el);

/* §2 Terminology's TRANSFORMED ELEMENT — "an element with a computed value other than none for the transform
   property". The computed value is core/css/css_computed_value.h's, so a `<transform-list>` crashes THERE,
   naming §3's own `Computed value:` line; what reaches this predicate today is the `none` every element has
   that no declaration reached, and that is a REAL computed value and not a stand-in for one. */
bool css_transform_is_transformed(lxb_dom_element_t *el);

/* THE NEAREST ELEMENT AT OR ABOVE `el` THAT A TRANSFORM APPLIES TO — transformable AND transformed — or NULL
   when none does, which is the answer for the overwhelming majority of the elements on a page and is a
   DERIVATION rather than an assumption: every element in the chain was asked, and each answered out of its own
   computed value.
   NULL IS WHAT LETS A CONSUMER PROCEED, and it means precisely that the transformation matrix the consumer
   would post-multiply is the IDENTITY — so applying it is a no-op and the untransformed geometry IS the
   transformed one. A non-NULL answer is that consumer's own next subproblem, and it crashes there rather than
   here, because what has to be built differs per consumer (a client rectangle is mapped through the matrix,
   §3.2's resolved value SERIALIZES it, and §3.2.9's is post-multiplied up a containing block chain).
   THE CHAIN IS THE ELEMENT'S ANCESTOR ELEMENTS IN ITS OWN DOCUMENT and stops at the document element, which is
   what "its ancestors" means for a coordinate reported in that document's own client space. */
lxb_dom_element_t *css_transform_applied_self_or_ancestor(lxb_dom_element_t *el);

#endif
