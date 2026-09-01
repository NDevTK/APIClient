/* CSS TRANSFORMS 1's element-level facts. See css_transform.h for which edition's section numbers these are,
   and for why the two facts below are one component rather than a line in each of the three callers. */
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include <lexbor/dom/dom.h>

#include "check.h"
#include "core/css/css_computed_value.h"
#include "core/css/css_transform.h"
#include "core/layout/replaced_element.h"

/* §2 Terminology's FIRST category's own exclusions, which are computed `display` values — "table-column boxes,
   and table-column-group boxes". They are css-display §2.4's `<display-internal>` keywords, and they are the
   TWO of the eight that a transform does not apply to: a row, a row group, a cell and a caption are all
   transformable, which is why this is the module's own two-name list and not the internal-table set. */
static bool css_tr_is_column_box(const char *display)
{
    return strcmp(display, "table-column") == 0 || strcmp(display, "table-column-group") == 0;
}

bool css_transform_is_transformable(lxb_dom_element_t *el)
{
    lxb_dom_element_t *parent;
    char *d;
    bool out;

    DCHECK(el != NULL, "css-transforms-1 §2 Terminology's transformable-element question was asked with no "
                       "element");
    parent = css_parent_element(el);
    /* §2's SECOND CATEGORY — "all SVG paint server elements, the clipPath element and SVG renderable elements
       with the exception of any descendant element of text content elements [SVG2]" — is a membership test
       over SVG 2's own element classes and not over the CSS box model at all, so answering it out of the
       computed `display` below would be an answer from the wrong category. THE OUTERMOST `svg` IS NOT THIS
       CASE: it is a replaced element the CSS box model lays out, so it is the first category and reaches this
       function through the ordinary path — core/dom/element_view.c states the same split for the same reason.
       What arrives here is therefore an SVG element UNDER another one, which this engine lays out not at all. */
    if (lxb_dom_interface_node(el)->ns == LXB_NS_SVG && parent != NULL &&
        lxb_dom_interface_node(parent)->ns == LXB_NS_SVG)
        DFAIL("css-transforms-1 §2 \"Terminology\" puts SVG PAINT SERVER ELEMENTS, the `clipPath` element and "
              "SVG RENDERABLE ELEMENTS in a second category of transformable element, defined by SVG 2's "
              "element classes rather than by the CSS box model — and this engine lays out no SVG, so it can "
              "neither place such an element nor say which class it is in. The two categories are not "
              "alternative spellings of one test: an SVG renderable element has no margins, borders or "
              "paddings for CSS 2 §8.1's box model to be stated over, and the exception in §2's own sentence "
              "(\"any descendant element of text content elements\") is a fact about SVG 2's text model. BUILD "
              "SVG 2's element classification beside SVG layout, in its own component, the same way "
              "core/dom/element_view.c's getClientRects step 2 asks for SVG 2's own bounding box rather than a "
              "special case of the CSS one");
    /* A RELEASE BUILD CANNOT CLASSIFY AN SVG ELEMENT, so it answers out of the FIRST category below —
       the one this engine models — exactly as every other member past a DFAIL in this tree does. */
    d = css_computed_value(el, "display");
    DCHECK(d != NULL, "the cascade produced no computed `display` for an element whose transformability was "
                      "asked — the UA layer answers `inline` for every element it does not name, so this "
                      "cannot be unset");
    /* "all elements whose layout is GOVERNED BY THE CSS BOX MODEL" is the category's opening clause, and an
       element that generates no box is outside it before any exclusion is reached: css-display-3 §2.5 "Box
       Generation: the none and contents keywords" gives `contents` no box of its own and `none` no box at all
       ("The element and its descendants generate no boxes or text sequences."). This is not the same statement as the exclusions
       below — those name boxes that EXIST and that a transform does not apply to — and it matters for the
       ancestor walk, where a `display: contents` wrapper between a rotated `div` and its child must not stop
       the walk and must not be reported as the rotated element either. */
    if (strcmp(d, "none") == 0 || strcmp(d, "contents") == 0) out = false;
    /* "except for … table-column boxes, and table-column-group boxes" */
    else if (css_tr_is_column_box(d)) out = false;
    /* "except for NON-REPLACED INLINE BOXES". An inline box is css-display's `inline` outer type with a `flow`
       inner one — the computed `display` of `inline`, and NOT `inline-block`, which is a `<display-legacy>`
       for `inline flow-root` and is an atomic inline-level box a transform does apply to. The `non-replaced`
       half is HTML §15.4 "Replaced elements"' question and core/layout/replaced_element.h answers it from the
       element's actual state, which is what makes a loading `<img>` transformable and the same `<img>` broken
       with an `alt` present not. */
    else if (strcmp(d, "inline") == 0) out = replaced_element_of(el).replaced;
    else out = true;
    free(d);
    return out;
}

bool css_transform_is_transformed(lxb_dom_element_t *el)
{
    char *v;
    bool out;

    DCHECK(el != NULL, "css-transforms-1 §2 Terminology's transformed-element question was asked with no "
                       "element");
    /* "An element with a computed value other than none for the transform property" — the whole definition,
       and it is a test on the COMPUTED value rather than the specified one because that is what §2 says. The
       two differ only by §3's absolutization of lengths, which cannot turn a list into `none` nor `none` into
       a list, so the distinction this predicate draws is one the entry below can already make for every value
       it can answer at all. */
    v = css_computed_value(el, "transform");
    DCHECK(v != NULL, "CSS Cascade §7.1 \"Initial Values\" produced no computed `transform` — css-transforms-1 §3 \"The "
                      "transform Property\" gives it an `Initial:` value of `none` and "
                      "core/css/css_style_declaration.c "
                      "carries that row for lexbor's registry, which has no entry of its own, so the cascade's "
                      "last layer always answers");
    out = strcmp(v, "none") != 0;
    free(v);
    return out;
}

lxb_dom_element_t *css_transform_applied_self_or_ancestor(lxb_dom_element_t *el)
{
    lxb_dom_element_t *on;

    DCHECK(el != NULL, "the transform chain was walked from no element");
    /* §3's `Applies to:` line is "transformable elements", so BOTH conjuncts are asked of each element in the
       chain and in this order: an element the property does not apply to is not asked for its computed value
       at all, which is what keeps a `<span style="transform:rotate(45deg)">` from crashing a consumer over a
       declaration that moves nothing. */
    for (on = el; on != NULL; on = css_parent_element(on))
        if (css_transform_is_transformable(on) && css_transform_is_transformed(on)) return on;
    return NULL;
}
