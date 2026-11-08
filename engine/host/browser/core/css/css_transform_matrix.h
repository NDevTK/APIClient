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
 * AN ELEMENT'S MATRIX IS css-transforms-2 §6 "Current Transformation Matrix"'s 4x4, and the chain composes the
 * 3x3 PROJECTIVE MATRIX §7's flattening leaves of it. With `transform-style: flat`, the initial value, each
 * element's content is drawn into its parent's plane, so a point enters every element's matrix with z = 0 and
 * leaves it with its z discarded: the third row and column of the 4x4 never reach the parent, and what does is
 * the 3x3 of rows x, y, w over columns x, y, 1. The 2D matrices of css-transforms-1 are its affine case
 * (p = q = 0, s = 1).
 *
 * EVERY ENTRY IS A `CssPx`, WHICH IS A STATEMENT ABOUT WHERE AN ENVIRONMENT FACT CAN ENTER A TRANSFORM. The
 * translations are `<length-percentage>`s, so `translate(10vw)` is a function of the initial containing block,
 * and the anchor of a rotation is the element's layout position. A perspective row multiplies those lengths into
 * the linear part and the divisor, so no entry is a pure ratio in general; core/css/css_length.h's product
 * carries both operands' facts, and a product with an exact zero is the zero every world agrees on.
 */
#ifndef ENGINE_HOST_BROWSER_CORE_CSS_CSS_TRANSFORM_MATRIX_H
#define ENGINE_HOST_BROWSER_CORE_CSS_CSS_TRANSFORM_MATRIX_H

#include <lexbor/dom/dom.h>

#include "core/css/css_length.h"

/* The flattened 3x3: §12's six parameters in §12's own order (a = m11, b = m12, c = m21, d = m22, e = m41,
   f = m42) and the projective row p = m14, q = m24, s = m44, so a point maps to ((a x + c y + e) / w,
   (b x + d y + f) / w) with w = p x + q y + s. */
typedef struct {
    CssPx a, b, c, d, e, f;
    CssPx p, q, s;
} CssTransformMatrix;

/* A 4x4 in §16's notation, `m[r][c]` the entry in row r and column c of the matrix as §16 prints it, so m41 (the
   X translation) is m[0][3] and m14 is m[3][0]. */
typedef struct {
    CssPx m[4][4];
} CssTransformMatrix3d;

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
   rendered into that local coordinate system" — expressed in the initial containing block's space: the origin
   bracket is anchored at §4's origin measured from §5's reference box corner, placed at its layout position.
   The IDENTITY for an element no transform applies to, which is both arms of the conjunction
   core/css/css_transform.h states. */
CssTransformMatrix css_transform_matrix_of_element(lxb_dom_element_t *el);

/* §2's CURRENT TRANSFORMATION MATRIX — "From the perspective of the user an element effectively accumulates
   all the transform properties of its ancestors as well as any local transform applied to it", computed as
   that section says: "by post-multiplying all transformation matrices starting from the viewport coordinate
   system and ending with the transformation matrix of an element". The chain is `el` and its ancestor ELEMENTS
   in its own document, which is the same chain core/css/css_transform.h walks and the same one CSSOM VIEW §6's
   "the transforms that apply to the element and its ancestors" names. */
CssTransformMatrix css_transform_matrix_current(lxb_dom_element_t *el);

/* The layout the matrix above reads, asked under a resting drive (core/layout/layout_job.h): for each element
   on the chain, its reference box size where a percentage reads it and, where the origin bracket survives, its
   border box corner. False ("not yet") with the question suspended for the caller's drive. */
bool css_transform_matrix_current_ask(lxb_dom_element_t *el);

/* css-transforms-1 §4 "The transform-origin Property"'s used value on transformable `el`: its horizontal and
   vertical offsets from §5's reference box corner with percentages resolved against that box, and its Z
   offset, in `out[0..2]`. It is the resolved value CSSOM §9 reports ("a resolved value special case property
   like height"). */
void css_transform_origin_used(lxb_dom_element_t *el, CssPx out[3]);

/* The layout the entry above reads, asked under a resting drive: false ("not yet") when it was deferred. */
bool css_transform_origin_used_ask(lxb_dom_element_t *el);

/* css-transforms-2 §9 "The perspective-origin Property"'s used value on transformable `el`, the same way: its
   offsets from the reference box corner in `out[0..1]`, and 0 in `out[2]`. §9: "The perspective-origin
   property is a resolved value special case property like height." */
void css_perspective_origin_used(lxb_dom_element_t *el, CssPx out[3]);

/* The layout the entry above reads, asked under a resting drive: false ("not yet") when it was deferred. */
bool css_perspective_origin_used_ask(lxb_dom_element_t *el);

/* css-transforms-1 §3.2 "Resolved value of transform"'s matrix: "Let transform be a 4x4 matrix initialized to
   the identity matrix", "Post-multiply all <transform-function>s in <transform-list> to transform" — the
   product alone, with no transform-origin and no position, percentages resolved against §5's reference box.
   False for a computed `none`, which resolves to itself. */
bool css_transform_matrix_resolved(lxb_dom_element_t *el, CssTransformMatrix3d *out);

/* css-transforms-2 §2.1's serialization of a resolved 4x4: matrix() when it "is a 2D matrix", matrix3d() in
   column-major order otherwise, each number as CSSOM §6.7.2 writes a <number>. Every entry is concrete
   (asserted). OWNED. */
char *css_transform_matrix3d_serialize(const CssTransformMatrix3d *m);

/* Whether any entry of `m` derives from an environment fact. */
bool css_transform_matrix3d_derived(const CssTransformMatrix3d *m);

/* The layout the entry above reads, asked under a resting drive: false ("not yet") when it was deferred. */
bool css_transform_matrix_resolved_ask(lxb_dom_element_t *el);

#endif
