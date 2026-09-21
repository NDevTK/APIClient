/* CSS TRANSFORMS 1 §2 "The Transform Rendering Model" — AN ELEMENT'S TRANSFORMATION MATRIX, THE CURRENT
 * TRANSFORMATION MATRIX OVER ITS ANCESTORS, AND THE MAPPING OF A POINT THROUGH ONE.
 *
 * WHICH EDITION'S NUMBERS THESE ARE. The same answer core/css/css_transform.h and
 * core/css/css_transform_function.h give and for the same reason: css-transforms-1's CR numbers "Terminology"
 * and its Editor's Draft renders that heading with NO NUMBER AT ALL — it is emitted as an unrendered
 * `Terminology {#terminology}` inside a paragraph, so the document's own heading list skips from §1.2 "CSS
 * Values" to §2 "The Transform Rendering Model" — and every section below it therefore differs by one. EVERY
 * NUMBER HERE IS THE ED'S, which is what the committed section index resolves against. Each carries its
 * TITLE, which survives a renumbering the number does not.
 *
 * WHY THIS IS ITS OWN COMPONENT AND NOT A FUNCTION IN CSSOM VIEW'S MEMBER. Three algorithms in three
 * standards need ONE matrix and each of them is written over it rather than over the list it comes from:
 * CSSOM VIEW §6 "Extensions to the Element Interface"'s getClientRects() maps a border area through it,
 * css-transforms-1 §3.2 "Resolved value of transform" SERIALIZES it, and INTERSECTION OBSERVER §3.2.9
 * "Calculate a target's Effective Transformation Matrix" accumulates it up a containing block chain. A matrix
 * built at one of those three call sites is a second answer to one question, and §2's own arithmetic is
 * exactly the place two answers drift.
 *
 * css-transforms-1 §3.2 IS NOT THIS ALGORITHM AND MAY NOT BE SUBSTITUTED FOR IT, which is the one thing a
 * reader arriving from the crash this component replaced must not carry forward. THE RUN THAT STOOD IN THAT
 * CRASH, shown here as a spelling and not quoted as a standard's, named `§3.2 "Resolved value of transform"'s
 * reduction of a list to one 4x4` as what a client rectangle waits on. css-transforms-1 §3.2's three steps
 * are "Let transform be a 4x4 matrix initialized to the identity matrix", "Post-multiply all
 * <transform-function>s in <transform-list> to transform" and "Serialize transform to a matrix function" —
 * THE PRODUCT ALONE, with no transform-origin anywhere in it, because a resolved value is what
 * `getComputedStyle(el).transform` reports and the origin is a separate resolved-value special case beside it.
 * §2's RENDERING matrix is four steps and not two: "Start with the identity matrix.", "Translate by the
 * computed X and Y of transform-origin", "Multiply by each of the transform functions in transform property
 * from left to right", "Translate by the negated computed X and Y values of transform-origin". §4 "The
 * transform-origin Property" gives that origin an `Initial:` value of `50% 50%`, so the bracket is NON-ZERO
 * FOR EVERY ELEMENT WITH A BOX and a consumer built to §3.2 would rotate every box about its top-left corner.
 *
 * THE SHAPE IS §12's OWN 3x2 AND NOT A 4x4, and that is the spec's sentence rather than a narrowing chosen
 * here: §12 "Mathematical Description of Transform Functions" opens "Mathematically, all transform functions
 * can be represented as 4x4 transformation matrices of the following form:" and then states the equivalence
 * this engine's whole function set lives inside — "A 2D 3x2 matrix with six parameters a, b, c, d, e and f is
 * equivalent to the matrix:". css-transforms-1 §7.1 "2D Transform Functions" is the only function grammar
 * core/css/css_transform_function.h parses, and css-transforms-2 §12.2 "3D Transform Functions"' names crash
 * there by name, so the third row and third column of a 4x4 built here could never hold anything but the
 * identity — ten slots no caller writes and no caller reads. The day a level-2 function parses, the widening
 * is this struct and its two operations, and the crash that names it is in that parser.
 *
 * THE LINEAR PART IS A `double` AND THE TRANSLATION IS A `CssPx`, WHICH IS A STATEMENT ABOUT WHERE AN
 * ENVIRONMENT FACT CAN ENTER A TRANSFORM. §7.1 gives the scales and matrix() a `<number>` argument and the
 * rotation and skews an `[ <angle> | <zero> ]`, and every one of those is a literal the author wrote — no
 * viewport, no device pixel ratio, no font size can reach them — so `a`, `b`, `c` and `d` are pure ratios with
 * an empty domain and core/css/css_length.h's `css_px_scale` is the operation that says so ("`k` is a pure
 * ratio … so it changes no fact"). THE TRANSLATIONS ARE THE OPPOSITE: §7.1 gives them a
 * `<length-percentage>`, so a `translate(10vw)` is a function of the initial containing block and
 * `translate(2rem)` of the reader's own default font size, and `transform: translateX(10vw)` moves a client
 * rectangle exactly as `width: 10vw` sizes one. Carrying those as bare doubles would drop the domain at the
 * matrix and delete the arm the other viewport takes — which is why this component never serializes a list
 * and re-parses it, and why core/css/css_computed_value.h answers the PARSED list.
 */
#ifndef ENGINE_HOST_BROWSER_CORE_CSS_CSS_TRANSFORM_MATRIX_H
#define ENGINE_HOST_BROWSER_CORE_CSS_CSS_TRANSFORM_MATRIX_H

#include <lexbor/dom/dom.h>

#include "core/css/css_length.h"

/* §12's six parameters, in §12's own order. `a` and `d` scale, `b` and `c` shear, and `e` and `f` translate —
   "One translation unit on a matrix is equivalent to 1 pixel in the local coordinate system of the element",
   which is why the pair is a LENGTH in this engine's own vocabulary and the other four are not. */
typedef struct {
    double a, b, c, d;
    CssPx  e, f;
} CssTransformMatrix;

/* §2's step 1 — "Start with the identity matrix." It is also the whole answer for an element no transform
   applies to, and that is a DERIVATION rather than a stand-in: mapping a point through the identity is the
   point, so an untransformed element's client rectangle IS its border area. */
CssTransformMatrix css_transform_matrix_identity(void);

/* §2's POST-MULTIPLICATION — the word that section uses three times ("The transformation matrix TM gets
   computed by post-multiplying", "post-multiply the transformation matrix TM of the element by p local", and
   "The current transformation matrix is computed by post-multiplying all transformation matrices"). `outer` is
   applied AFTER `inner`, so mapping a point through the result is mapping it through `inner` and then through
   `outer`; the ancestor is the `outer` operand and the element the `inner` one, which is the order §2's own
   SVG example states — "The CTM for the SVG rect element is the result of multiplying T1, T2 and T3 in
   order.". */
CssTransformMatrix css_transform_matrix_multiply(CssTransformMatrix outer, CssTransformMatrix inner);

/* §2's "post-multiply the transformation matrix TM of the element by p local. The result is the mapped point p
   parent". The two out-parameters carry the UNION of the point's environment facts and the matrix's
   translation's, which core/css/css_length.h's arithmetic assembles — a `translate(10vw)` applied to a border
   area derived from the initial containing block is one function of that block and not two numbers. */
void css_transform_matrix_map(CssTransformMatrix m, CssPx x, CssPx y, CssPx *out_x, CssPx *out_y);

/* §2's FOUR STEPS for `el` — its own TRANSFORMATION MATRIX, the map "from where the element would have
   rendered into that local coordinate system". The IDENTITY for an element no transform applies to, which is
   both arms of the conjunction core/css/css_transform.h states: `Applies to: transformable elements` and a
   computed `transform` of `none`.
   IT ASKS FOR NO TRANSFORM-ORIGIN AND THAT IS NOT A GAP ON THE PATH IT ANSWERS — see the .c for the
   cancellation, which is a property of §2's own bracket rather than an approximation — and it CRASHES BY NAME
   on the path where the bracket survives. */
CssTransformMatrix css_transform_matrix_of_element(lxb_dom_element_t *el);

/* §2's CURRENT TRANSFORMATION MATRIX — "From the perspective of the user an element effectively accumulates
   all the transform properties of its ancestors as well as any local transform applied to it", computed as
   that section says: "by post-multiplying all transformation matrices starting from the viewport coordinate
   system and ending with the transformation matrix of an element". The chain is `el` and its ancestor ELEMENTS
   in its own document, which is the same chain core/css/css_transform.h walks and the same one CSSOM VIEW §6's
   "the transforms that apply to the element and its ancestors" names. */
CssTransformMatrix css_transform_matrix_current(lxb_dom_element_t *el);

#endif
