/* CSSOM VIEW §2 "Terminology"'s SCROLLING BOX, joined to css-overflow-3 §3.1 "Managing Overflow: the
   overflow-x, overflow-y, and overflow properties"' SCROLL CONTAINER. See scroll_container.h for why the term
   needs a component at all and for why §2's own note about a non-overflowing `auto` box changes no answer. */
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include <lexbor/dom/dom.h>

#include "check.h"
#include "core/css/css_computed_value.h"
#include "core/css/css_property_applies.h"
#include "core/dom/document.h"
#include "core/dom/element_view.h"
#include "core/layout/scroll_container.h"

/* §3.1's SCROLLABLE VALUES, as the spec lists them: "The scroll, auto, and hidden values are known as the
   scrollable values of overflow." `visible` and `clip` are its "non-scrollable values" and there is no sixth
   keyword — §3.1's `Value:` line is `visible | hidden | clip | scroll | auto` — so a value outside the five is
   a cascade that produced something css-overflow does not define, and it crashes rather than being read as
   non-scrollable. `overlay` is a LEGACY ALIAS of `auto` ("User agents must also support the overlay keyword as
   a legacy value alias of auto") and is therefore a value the COMPUTED value can never be: an alias is
   resolved at parse time, which lexbor's own value table does, so meeting one here is that resolution having
   been skipped rather than a sixth keyword to model. */
static bool sc_axis_value_is_scrollable(lxb_dom_element_t *el, bool vertical)
{
    char *v = css_computed_value(el, vertical ? "overflow-y" : "overflow-x");
    bool scrollable;

    DCHECK(v != NULL, "the cascade produced no computed `overflow-x`/`overflow-y` — css-overflow-3 §3.1 gives "
                      "the properties an `Initial:` value of `visible`, so the cascade's last layer answers for "
                      "every element");
    scrollable = strcmp(v, "scroll") == 0 || strcmp(v, "auto") == 0 || strcmp(v, "hidden") == 0;
    DCHECK(scrollable || strcmp(v, "visible") == 0 || strcmp(v, "clip") == 0,
           "a computed `overflow-x`/`overflow-y` is none of the five keywords css-overflow-3 §3.1's `Value:` "
           "line declares (`visible | hidden | clip | scroll | auto`). `overlay` is that section's legacy ALIAS "
           "of `auto` and is resolved before a computed value exists, so this is a declaration the cascade "
           "should have refused or an alias that was carried through");
    free(v);
    return scrollable;
}

/* §3.1's `Applies to:` line — "block containers [CSS2], flex containers [CSS-FLEXBOX-1], grid containers
   [CSS-GRID-1], and table grid boxes [CSS-TABLES-3]". It is asked ONCE per element rather than per axis: the
   line is the property's and both longhands carry it, so an element the line excludes is not a scroll
   container in either axis. */
static bool sc_overflow_applies(lxb_dom_element_t *el)
{
    return css_property_applies(el, "overflow-x");
}

static bool sc_computed_is(lxb_dom_element_t *el, const char *name, const char *kw)
{
    char *v = css_computed_value(el, name);
    bool same;

    DCHECK(v != NULL, "the cascade produced no computed value for a property this engine models");
    same = strcmp(v, kw) == 0;
    free(v);
    return same;
}

/* css-overflow-3 §3.1.4 "Overflow Viewport Propagation" — the ONE element per document whose `overflow` does
 * not make its own box a scroll container, because the value was taken away from it and given to the viewport.
 * §3.1.4: "UAs must apply the overflow values set on the ROOT ELEMENT to the viewport when the root element's
 * display value is not none. However, when the root element is an [HTML] html element … whose overflow value is
 * VISIBLE (in both axes), and that element has as a child a BODY element whose display value is also not none,
 * user agents must instead apply the overflow values of the first such child element to the viewport. THE
 * ELEMENT FROM WHICH THE VALUE IS PROPAGATED MUST THEN HAVE A USED OVERFLOW VALUE OF VISIBLE."
 * THAT LAST SENTENCE IS THE WHOLE OF WHY THIS FUNCTION EXISTS, and it is a USED value where everything else
 * here is a computed one — which is exactly the case a component reading the computed value alone gets wrong.
 * `<html style="overflow:auto">` computes `auto` on the root and §3.1 would call its box a scroll container, so
 * CSSOM VIEW §6.1's ancestor walk would visit the root AND the viewport and align the same target against two
 * boxes that are the same scrolling box. §3.1.4 says the root's used value is `visible` instead: there is one
 * box, it is the viewport's, and the walk visits it once.
 * THE `html`-AND-`body` ARM IS NOT AN EXTRA CASE FOR THE ROOT, and reading it as one is the trap: when the body
 * is the propagator the root's own overflow was `visible` in both axes to begin with, so the root is not a
 * scroll container by §3.1's own sentence and needs no help from this one. The arm's only effect is on the
 * BODY. */
static bool sc_propagates_overflow_to_viewport(lxb_dom_element_t *el)
{
    lxb_dom_node_t *n = lxb_dom_interface_node(el), *root;
    lxb_dom_element_t *rel;
    size_t taglen = 0;
    const lxb_char_t *tag;

    DCHECK(n->owner_document != NULL, "an element with no owner document reached §3.1.4's propagation test");
    root = document_document_element_of(lxb_dom_interface_node(n->owner_document));
    if (root == NULL) return false;
    /* §3.1.4's FIRST sentence. Its "when the root element's display value is not none" is already established:
       the caller has asked whether this element has a box. */
    if (n == root) return true;
    if (n != document_body_of(lxb_dom_interface_node(n->owner_document))) return false;
    /* §3.1.4's SECOND sentence, over the ROOT: an `html` element in the HTML namespace, whose computed
       `overflow` is `visible` in both axes. The body half of its condition ("has as a child a body element
       whose display value is also not none") is this element, whose box the caller established. */
    rel = lxb_dom_interface_element(root);
    tag = lxb_dom_element_local_name(rel, &taglen);
    if (root->ns != LXB_NS_HTML || taglen != 4 || memcmp(tag, "html", 4) != 0) return false;
    return sc_computed_is(rel, "overflow-x", "visible") && sc_computed_is(rel, "overflow-y", "visible");
}

bool scroll_container_is(lxb_dom_element_t *el)
{
    DCHECK(el != NULL, "css-overflow-3 §3.1's scroll-container question was asked with no element");
    /* A SCROLL CONTAINER IS A BOX. An element that generates none has no principal box for §3.1's sentence to
       be about, so it establishes no scrolling box whatever its `overflow` computed to. */
    if (!element_view_has_box(lxb_dom_interface_node(el))) return false;
    /* §3.1.4's USED value comes before §3.1's computed one, because it is what §3.1 is then applied to. */
    if (sc_propagates_overflow_to_viewport(el)) return false;
    if (!sc_overflow_applies(el)) return false;
    /* "If neither axis computes to a scrollable value, the box is not a scroll container" — so ONE is enough,
       and §3.1's own single-axis case ("If only one axis computes to a scrollable value … the box is a
       single-axis scroll container") is why this is a disjunction rather than a conjunction. */
    return sc_axis_value_is_scrollable(el, false) || sc_axis_value_is_scrollable(el, true);
}
