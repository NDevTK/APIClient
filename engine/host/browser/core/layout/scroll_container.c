/* CSSOM View §2 "Terminology"'s scrolling box, joined to css-overflow-3 §3.1 "Managing Overflow: the
   overflow-x, overflow-y, and overflow properties"' scroll container. See scroll_container.h. */
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include <lexbor/dom/dom.h>

#include "check.h"
#include "core/css/css_computed_value.h"
#include "core/css/css_property_applies.h"
#include "core/dom/document.h"
#include "core/dom/element_view.h"
#include "core/layout/containment.h"
#include "core/layout/scroll_container.h"

/* The computed keyword on one axis, classified once. The scroll-container and content-clip questions differ
   only at `clip`, which is non-scrollable but clips to the padding edge, so both read this one classification
   rather than one bit. */
typedef enum { SC_OVERFLOW_VISIBLE = 0, SC_OVERFLOW_CLIP, SC_OVERFLOW_SCROLLABLE } ScOverflow;

static ScOverflow sc_axis_overflow(lxb_dom_element_t *el, bool vertical)
{
    CssKeyword v = css_computed_keyword(el, vertical ? "overflow-y" : "overflow-x");

    switch (v) {
    case CSS_KEYWORD_SCROLL:
    case CSS_KEYWORD_AUTO:
    case CSS_KEYWORD_HIDDEN:  return SC_OVERFLOW_SCROLLABLE;
    case CSS_KEYWORD_CLIP:    return SC_OVERFLOW_CLIP;
    case CSS_KEYWORD_VISIBLE: return SC_OVERFLOW_VISIBLE;
    default:
        DFAILF("a computed `overflow-x`/`overflow-y` is keyword %u, outside the row's set", (unsigned)v);
        return SC_OVERFLOW_VISIBLE;
    }
}

/* §3.1's scrollable values: "The scroll, auto, and hidden values are known as the scrollable values of
   overflow." `visible` and `clip` are its "non-scrollable values". */
static bool sc_axis_value_is_scrollable(lxb_dom_element_t *el, bool vertical)
{
    return sc_axis_overflow(el, vertical) == SC_OVERFLOW_SCROLLABLE;
}

/* CSSOM View's content clip on one axis: every keyword but `visible` clips to the padding edge. */
static bool sc_axis_value_clips(lxb_dom_element_t *el, bool vertical)
{
    return sc_axis_overflow(el, vertical) != SC_OVERFLOW_VISIBLE;
}

/* §3.1's `Applies to:` line, "block containers [CSS2], flex containers [CSS-FLEXBOX-1], grid containers
   [CSS-GRID-1], and table grid boxes [CSS-TABLES-3]", asked once per element: both longhands carry it. */
static bool sc_overflow_applies(lxb_dom_element_t *el)
{
    return css_property_applies(el, "overflow-x");
}

/* css-overflow-3 §3.1.4 "Overflow Viewport Propagation": "UAs must apply the overflow values set on the root
 * element to the viewport when the root element's display value is not none. However, when the root element is
 * an [HTML] html element … whose overflow value is visible (in both axes), and that element has as a child a
 * body element whose display value is also not none, user agents must instead apply the overflow values of the
 * first such child element to the viewport. The element from which the value is propagated must then have a
 * used overflow value of visible." See scroll_container.h for the callers and why this is not in the cascade.
 * The `html`-and-`body` arm affects only the body: when the body propagates, the root's overflow is already
 * `visible` in both axes. */
bool scroll_container_propagates_overflow_to_viewport(lxb_dom_element_t *el)
{
    lxb_dom_node_t *n, *root;
    lxb_dom_element_t *rel;
    size_t taglen = 0;
    const lxb_char_t *tag;

    DCHECK(el != NULL, "css-overflow-3 §3.1.4's propagation question was asked with no element");
    n = lxb_dom_interface_node(el);
    DCHECK(n->owner_document != NULL, "an element with no owner document reached §3.1.4's propagation test");
    root = document_document_element_of(lxb_dom_interface_node(n->owner_document));
    if (root == NULL) return false;
    /* Neither candidate: the answer for all but two elements, asked first because it reads no cascade. */
    if (n != root && n != document_body_of(lxb_dom_interface_node(n->owner_document))) return false;
    /* §3.1.4's first sentence: "when the root element's display value is not none". Asked here because on
       the body arm the root is not the element the caller checked. */
    if (!element_view_has_box(root)) return false;
    if (n == root) return true;
    /* §3.1.4's second sentence: the root is an HTML-namespace `html` element whose computed `overflow` is
       `visible` in both axes, and this body element's "display value is also not none". */
    if (!element_view_has_box(n)) return false;
    /* css-contain-2 §2: containment active on `html` or `body` disables the body's propagation. */
    if (containment_disables_body_propagation(lxb_dom_interface_node(n->owner_document))) return false;
    rel = lxb_dom_interface_element(root);
    tag = lxb_dom_element_local_name(rel, &taglen);
    if (root->ns != LXB_NS_HTML || taglen != 4 || memcmp(tag, "html", 4) != 0) return false;
    return css_computed_keyword_is(rel, "overflow-x", CSS_KEYWORD_VISIBLE) &&
           css_computed_keyword_is(rel, "overflow-y", CSS_KEYWORD_VISIBLE);
}

bool scroll_container_is(lxb_dom_element_t *el)
{
    DCHECK(el != NULL, "css-overflow-3 §3.1's scroll-container question was asked with no element");
    /* A scroll container is a box; an element that generates none establishes no scrolling box. */
    if (!element_view_has_box(lxb_dom_interface_node(el))) return false;
    /* §3.1.4's used value comes first, because it is what §3.1 is applied to. */
    if (scroll_container_propagates_overflow_to_viewport(el)) return false;
    if (!sc_overflow_applies(el)) return false;
    /* "If neither axis computes to a scrollable value, the box is not a scroll container", so one axis is
       enough (§3.1's single-axis scroll container). */
    return sc_axis_value_is_scrollable(el, false) || sc_axis_value_is_scrollable(el, true);
}

/* CSSOM View's content clip; see scroll_container.h. Same preconditions as above, differing at `clip`. */
bool scroll_container_content_clip_is(lxb_dom_element_t *el)
{
    DCHECK(el != NULL, "CSSOM VIEW's content-clip question was asked with no element");
    /* A content clip is a box's; an element that generates none clips nothing. */
    if (!element_view_has_box(lxb_dom_interface_node(el))) return false;
    /* css-contain-2 §3.5 "Paint Containment": "The contents of the element including any ink or scrollable
       overflow must be clipped to the overflow clip edge of the paint containment box", which its Note states
       as `visible` becoming `clip` at used-value time. It is a separate effect from §3.1.4's propagation, so it
       is asked first and holds on the propagating element too. */
    if (containment_paint(el)) return true;
    /* §3.1.4: the propagating element "must then have a used overflow value of visible", so it clips nothing
       and the viewport clips instead. `overflow-x: hidden` on `html` or `body` is common, and reading the
       computed value would report a clip on the root or body of those pages. */
    if (scroll_container_propagates_overflow_to_viewport(el)) return false;
    if (!sc_overflow_applies(el)) return false;
    return sc_axis_value_clips(el, false) || sc_axis_value_clips(el, true);
}
