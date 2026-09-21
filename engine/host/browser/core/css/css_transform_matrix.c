/* css-transforms-1 §2 "The Transform Rendering Model". See css_transform_matrix.h for which edition's section
   numbers these are, for why §3.2's two-step product is NOT this algorithm, and for why the linear part of the
   matrix is a `double` while its translation is a `CssPx`.

   WHAT THE THREE RELEASE ARMS OF THIS ONE ALGORITHM LEAVE BEHIND, STATED TOGETHER BECAUSE NO GATE HERE CAN
   REACH THEM. Two crashes below and one assert are dev-only, so a release build takes all three arms at once
   and they must compose rather than merely each be defensible: `css_tm_of_function`'s `default:` returns the
   IDENTITY for a rotation, a scale, a skew or a `matrix()`; `css_tm_translation_px` returns a percentage
   translation's `px`, which core/css/css_length.c initialises to ZERO with an empty environment set and
   DCHECKs it holds there, so it is a defined number and not an uninitialised read; and
   `css_transform_matrix_current`'s linear-part assert is compiled out. THE THREE AGREE, and that is the point:
   the first arm makes every matrix a translation, so the third arm's invariant still HOLDS in release rather
   than merely going unchecked, and the composition in the initial containing block's space stays exact. What
   a release build reports for a rotated box is the UNTRANSFORMED border area — which is the answer this engine
   gave before it read `transform` at all, is what a user agent with no transform support reports, and is a
   rectangle rather than a state a consumer has no step for. NOTHING HERE RETURNS A PARTIALLY-MAPPED
   RECTANGLE, which is the incoherent shape: a release arm that mapped the translations and dropped the
   rotation would report a box in a place neither the layout nor the page ever put one. */
#include <stdbool.h>
#include <stddef.h>

#include <lexbor/dom/dom.h>

#include "check.h"
#include "core/css/css_computed_value.h"
#include "core/css/css_length.h"
#include "core/css/css_transform.h"
#include "core/css/css_transform_function.h"
#include "core/css/css_transform_matrix.h"

CssTransformMatrix css_transform_matrix_identity(void)
{
    CssTransformMatrix out;

    out.a = 1.0; out.b = 0.0; out.c = 0.0; out.d = 1.0;
    out.e = css_px(0.0); out.f = css_px(0.0);
    return out;
}

CssTransformMatrix css_transform_matrix_multiply(CssTransformMatrix outer, CssTransformMatrix inner)
{
    CssTransformMatrix out;

    /* THE LINEAR PART IS A PRODUCT OF PURE RATIOS and carries no environment fact, for the reason the header
       states: §7.1 "2D Transform Functions" gives every argument that reaches it a `<number>` or an
       `[ <angle> | <zero> ]`, both of which are literals an author wrote. */
    out.a = outer.a * inner.a + outer.c * inner.b;
    out.b = outer.b * inner.a + outer.d * inner.b;
    out.c = outer.a * inner.c + outer.c * inner.d;
    out.d = outer.b * inner.c + outer.d * inner.d;
    /* THE TRANSLATION IS `inner`'s MAPPED THROUGH `outer`'s LINEAR PART AND THEN OFFSET BY `outer`'s, which is
       what the third column of the product is. `css_px_scale` is the multiplication because the scalar is the
       ratio and the length is the operand — the other spelling would ask core/css/css_length.h to multiply two
       lengths, which is Intersection Observer's AREA and carries a different dimension. */
    out.e = css_px_add(css_px_add(css_px_scale(inner.e, outer.a), css_px_scale(inner.f, outer.c)), outer.e);
    out.f = css_px_add(css_px_add(css_px_scale(inner.e, outer.b), css_px_scale(inner.f, outer.d)), outer.f);
    return out;
}

void css_transform_matrix_map(CssTransformMatrix m, CssPx x, CssPx y, CssPx *out_x, CssPx *out_y)
{
    DCHECK(out_x != NULL && out_y != NULL,
           "css-transforms-1 §2 \"The Transform Rendering Model\"'s \"post-multiply the transformation matrix "
           "TM of the element by p local\" was asked with nowhere to put the mapped point");
    *out_x = css_px_add(css_px_add(css_px_scale(x, m.a), css_px_scale(y, m.c)), m.e);
    *out_y = css_px_add(css_px_add(css_px_scale(x, m.b), css_px_scale(y, m.d)), m.f);
}

static CssPx css_tm_translation_px(const CssLength *len)
{
    DCHECK(len->kind != CSS_LENGTH_KEYWORD,
           "css-transforms-1 §7.1 \"2D Transform Functions\" gives every translation function a "
           "`<length-percentage>` argument, which admits no keyword at all — so a keyword here is a value "
           "core/css/css_transform_function.c's grammar accepted and its own `Value:` line does not");
    if (len->kind != CSS_LENGTH_ABSOLUTE)
        DFAIL("css-transforms-1 §7 \"The Transform Functions\" states that \"A percentage for horizontal "
              "translations is relative to the width of the reference box\", and §5 \"Transform reference box: "
              "the transform-box property\" is what names that box — its `Initial:` line is `view-box` and its "
              "own resolution for this engine's elements is \"For elements with associated CSS layout box, the "
              "used value for fill-box is content-box and for stroke-box and view-box is border-box\". THE "
              "PERCENTAGE IS NOT RESOLVED AT COMPUTED-VALUE TIME AND THAT IS CORRECT — css-values-4 §10.11 "
              "\"Computed Value\" leaves an unresolved percentage as specified and §3 \"The transform "
              "Property\"'s line moves LENGTHS — so this is a USED value and the box is its operand. BUILD §5's "
              "`transform-box` as a computed value (lexbor's registry carries no entry for it and "
              "core/css/css_style_declaration.c carries no initial-value row, so the cascade answers nothing "
              "for every element today), then resolve the percentage against "
              "core/layout/used_value.h's `used_value_border_edge_px` on this element's own axis. THE "
              "CANCELLATION THIS COMPONENT RESTS ON IS UNTOUCHED BY THAT: a percentage translation is still a "
              "TRANSLATION, so §2's transform-origin bracket still cancels and the origin is still not part of "
              "the answer — what is missing is only the number. `translate(-50%, -50%)` is the centring idiom "
              "this refuses and is the reason to build it first");
    return len->px;
}

/* §12 "Mathematical Description of Transform Functions"' MATRIX FOR ONE `<transform-function>`.
 *
 * THREE OF §7.1's ELEVEN ARE WRITTEN HERE AND THE OTHER EIGHT CRASH, and the line between them is NOT which
 * matrices §12 states — it states all of them — but which ones §2's OWN BRACKET LETS THIS ENGINE REACH. §2's
 * four steps are "Start with the identity matrix.", "Translate by the computed X and Y of transform-origin",
 * "Multiply by each of the transform functions in transform property from left to right" and "Translate by the
 * negated computed X and Y values of transform-origin", so the product of step 3 is CONJUGATED by the origin.
 * TRANSLATIONS COMMUTE WITH TRANSLATIONS, so where every function in the list is one of §7.1's three
 * translations that conjugation cancels EXACTLY — T(o)·T(t)·T(-o) is T(t) for EVERY value of `o` whatever,
 * including one no component here can compute — and §5 "Transform reference box: the transform-box property"'s
 * own additional term ("A reference box adds an additional offset to the origin specified by the
 * transform-origin property") is a translation too and cancels with it. That is a derivation over §2's
 * arithmetic and not an assumption about the page: the origin is not defaulted, guessed or ignored here, it is
 * ABSENT FROM THE ANSWER because the algorithm removes it.
 * FOR EVERY OTHER FUNCTION THE BRACKET SURVIVES and the origin is the first thing that would have to exist, so
 * that is what the crash names rather than §12's remaining rows, which are the easy half.
 *
 * A PERCENTAGE TRANSLATION IS IN THE SECOND POPULATION AND NOT THE FIRST, which is one step past where a
 * reader would draw the line: it commutes like any other translation, and §7's own sentence is that "A
 * percentage for horizontal translations is relative to the width of the reference box" — so resolving it
 * needs §5's reference box, which needs the computed `transform-box` this engine also does not derive. The
 * cancellation is untouched; what is missing is the number. */
static CssTransformMatrix css_tm_of_function(const CssTransformFunction *fn)
{
    CssTransformMatrix out = css_transform_matrix_identity();

    DCHECK(fn->argc > 0, "css-transforms-1 §3 \"The transform Property\"'s `<transform-list>` production is "
                         "`<transform-function>+` and every one of §7.1 \"2D Transform Functions\"' functions "
                         "has at least one required argument, so a function that parsed wrote at least one");
    switch (fn->kind) {
    /* "A 2D translation with the parameters tx and ty is equivalent to a 3D translation where tz has zero as a
       value" — §12's own row, and its `tz` is the third dimension this engine's 3x2 does not carry. */
    case CSS_TF_TRANSLATE:
        out.e = css_tm_translation_px(&fn->length[0]);
        /* §7.1's own default, applied HERE because core/css/css_transform_function.h records what the author
           WROTE: "If <ty> is not provided, ty has zero as a value". */
        if (fn->argc > 1) out.f = css_tm_translation_px(&fn->length[1]);
        break;
    case CSS_TF_TRANSLATE_X:
        out.e = css_tm_translation_px(&fn->length[0]);
        break;
    case CSS_TF_TRANSLATE_Y:
        out.f = css_tm_translation_px(&fn->length[0]);
        break;
    default:
        DFAIL("css-transforms-1 §2 \"The Transform Rendering Model\" computes an element's transformation "
              "matrix in FOUR steps — \"Start with the identity matrix.\", \"Translate by the computed X and Y "
              "of transform-origin\", \"Multiply by each of the transform functions in transform property from "
              "left to right\", \"Translate by the negated computed X and Y values of transform-origin\" — and "
              "the list this element computed holds a function that is NOT one of §7.1 \"2D Transform "
              "Functions\"' three translations, so steps 2 and 4 do not cancel and the ORIGIN is part of the "
              "answer. THE MISSING VALUE IS §4 \"The transform-origin Property\"'s, and it is missing at the "
              "root rather than here: its `Initial:` line is `50% 50%` and its `Computed value:` line is `see "
              "background-position`, and core/css/css_computed_value.c models the property NOT AT ALL — "
              "`css_computed_models` answers false for it, lexbor's registry carries no entry, and "
              "core/css/css_style_declaration.c carries no initial-value row, so the cascade answers nothing "
              "for every element on every page and a declared one arrives as a raw `__CUSTOM` value no grammar "
              "has looked at. WHAT TO BUILD, IN ORDER: (1) §4's `Value:` grammar and its `see "
              "background-position` computed value, beside core/css/css_transform_function.c, with the "
              "initial-value row and the `css_computed_models` entry that make an undeclared element "
              "answerable; (2) §5 \"Transform reference box: the transform-box property\", whose `Initial:` is "
              "`view-box` and whose own sentence resolves it for this engine — \"For elements with associated "
              "CSS layout box, the used value for fill-box is content-box and for stroke-box and view-box is "
              "border-box\" — so a percentage origin resolves against the border box "
              "core/layout/used_value.h's `used_value_border_edge_px` already answers; (3) §12 \"Mathematical "
              "Description of Transform Functions\"' matrices for the remaining eight functions, which are "
              "arithmetic over arguments core/css/css_transform_function.h has already parsed; and (4) THE "
              "POSITION CONJUGATION THIS COMPONENT DOES NOT PERFORM — §5's \"A reference box adds an "
              "additional offset to the origin specified by the transform-origin property\" makes an element's "
              "origin its BORDER BOX's corner plus that offset, and `css_transform_matrix_current` composes "
              "its chain in the initial containing block's space, where a translation is position-independent "
              "and a rotation is not. That function asserts the invariant so this cannot be forgotten");
    }
    return out;
}

CssTransformMatrix css_transform_matrix_of_element(lxb_dom_element_t *el)
{
    CssTransformMatrix out = css_transform_matrix_identity();
    CssTransformList list;
    size_t i;

    DCHECK(el != NULL, "css-transforms-1 §2 \"The Transform Rendering Model\"'s transformation matrix was "
                       "asked for with no element");
    /* §3 "The transform Property"'s `Applies to:` line is `transformable elements`, so an element the
       property does not apply to answers the identity however its `transform` computed — which is what keeps
       a `<span style="transform:rotate(45deg)">` from reaching the crash below over a declaration that moves
       nothing. BOTH conjuncts, in this order, exactly as core/css/css_transform.h states them. */
    if (!css_transform_is_transformable(el)) return out;
    /* §2's step 3 over css-transforms-1 §3 "The transform Property"'s computed value — "Multiply by each of
       the transform functions in transform property from left to right". FALSE is `none`, which that section's
       `Initial:` line gives every element no declaration reached, and the identity is such an element's whole
       matrix. */
    if (!css_computed_transform_list(el, &list)) return out;
    DCHECK(list.n > 0, "css-transforms-1 §3 \"The transform Property\"'s `<transform-list>` production is "
                       "`<transform-function>+`, which admits no empty match, so a list that parsed is never "
                       "empty and `none` is the other arm of the `Value:` line rather than a list of none");
    /* LEFT TO RIGHT, with each function POST-MULTIPLIED onto what is already there: the accumulated matrix is
       the `outer` operand because the functions written earlier are applied later to a point, which is §8 "The
       Transform Function Lists"' own equivalence — "a nested set of transforms is equivalent to a single list
       of transform functions applied from the coordinate system of the ancestor to the local coordinate system
       of a given element". */
    for (i = 0; i < list.n; i++)
        out = css_transform_matrix_multiply(out, css_tm_of_function(&list.fn[i]));
    css_transform_list_free(&list);
    return out;
}

CssTransformMatrix css_transform_matrix_current(lxb_dom_element_t *el)
{
    CssTransformMatrix out = css_transform_matrix_identity();
    lxb_dom_element_t *on;

    DCHECK(el != NULL, "css-transforms-1 §2 \"The Transform Rendering Model\"'s current transformation matrix "
                       "was asked for with no element");
    /* "The current transformation matrix is computed by post-multiplying all transformation matrices starting
       from the viewport coordinate system and ending with the transformation matrix of an element", which is
       the chain walked from `el` UPWARD with each ancestor becoming the `outer` operand — the root's matrix
       ends up outermost and the element's innermost, which is the order the section's own SVG example states.
       THE CHAIN IS THE ELEMENT'S ANCESTOR ELEMENTS IN ITS OWN DOCUMENT and stops at the document element,
       which is the same chain core/css/css_transform.h walks and what CSSOM VIEW §6 "Extensions to the Element
       Interface" means by "the transforms that apply to the element and its ancestors". */
    for (on = el; on != NULL; on = css_parent_element(on)) {
        CssTransformMatrix tm = css_transform_matrix_of_element(on);

        /* THE COMPOSITION IS PERFORMED IN THE INITIAL CONTAINING BLOCK'S SPACE AND NOT IN EACH ELEMENT'S OWN,
           WHICH IS EXACT FOR A TRANSLATION AND WRONG FOR EVERYTHING ELSE. §2 defines an element's matrix as
           the map into its PARENT'S coordinate system, anchored at the element's own transform-origin, and §5
           "Transform reference box: the transform-box property" adds that element's reference box's position
           to that origin — "A reference box adds an additional offset to the origin specified by the
           transform-origin property". A TRANSLATION IS POSITION-INDEPENDENT, so conjugating one by its
           element's border-box corner is the same translation and the offset is not merely small but ABSENT
           from the answer; a matrix with any other linear part is not, and composing one here without first
           conjugating it by `flow_border_box_origin` would report a rectangle rotated about the document's
           origin instead of the element's own box.
           THE ASSERT IS THE FORCING FUNCTION AND NOT A DESCRIPTION OF ONE. `css_tm_of_function` crashes for
           every function that is not a translation, so no other linear part can exist today and this condition
           is an invariant rather than a filter — the day that crash is built out, this fires and names the
           conjugation as part of the same diff. The comparison is EXACT because both spellings are written
           literals: `css_transform_matrix_identity` writes 1.0 and 0.0 and a translation matrix keeps them, so
           nothing here is the result of an arithmetic that could round. */
        DCHECK(tm.a == 1.0 && tm.b == 0.0 && tm.c == 0.0 && tm.d == 1.0,
               "css-transforms-1 §2 \"The Transform Rendering Model\"'s current transformation matrix was "
               "composed over an element whose own transformation matrix has a linear part other than the "
               "identity. This walk multiplies each ancestor's matrix in the INITIAL CONTAINING BLOCK's space, "
               "which is exact for a translation (a translation is the same map wherever it is anchored) and "
               "wrong for a rotation, a scale or a skew: §2 anchors an element's matrix at its own "
               "transform-origin and §5 \"Transform reference box: the transform-box property\" adds that "
               "element's reference box's position to it — \"A reference box adds an additional offset to the "
               "origin specified by the transform-origin property\" — so such a matrix must be CONJUGATED by "
               "that element's own border-box corner before it is composed here. BUILD that conjugation in the "
               "SAME DIFF as the matrix that fired this: take the corner from core/layout/flow_position.h's "
               "`flow_border_box_origin`, add §4 \"The transform-origin Property\"'s own offset, and compose "
               "T(corner+origin) * M * T(-(corner+origin)) rather than M");
        out = css_transform_matrix_multiply(tm, out);
    }
    return out;
}
