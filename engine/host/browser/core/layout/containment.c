/* css-contain-2 §3 "Types of Containment". See containment.h. */
#include <stdbool.h>

#include <lexbor/dom/dom.h>

#include "check.h"
#include "core/css/css_computed_value.h"
#include "core/css/css_contain.h"
#include "core/css/css_display.h"
#include "core/dom/document.h"
#include "core/layout/containment.h"
#include "core/layout/replaced_element.h"
#include "core/layout/table_box.h"

/* The two clauses every type's list shares: "the element does not generate a principal box (as is the case
   with display: contents or display: none)" and "its principal box is an internal ruby box or a non-atomic
   inline-level box". A `<display-internal>` value that is not a table box is a ruby-internal one. An inline
   box is non-atomic when its inner display is flow or ruby and it is not replaced (CSS 2.2 §10.3.2 sizes a
   replaced inline as an atomic box). */
static bool ct_shared_exclusion(lxb_dom_element_t *el, CssDisplay d)
{
    if (css_display_is_box(d, CSS_KEYWORD_NONE) || css_display_is_box(d, CSS_KEYWORD_CONTENTS)) return true;
    if (d.kind == CSS_DISPLAY_INTERNAL && table_box_kind(d) == TABLE_BOX_NOT_A_TABLE_BOX) return true;
    if (d.kind == CSS_DISPLAY_PAIR && d.outer == CSS_KEYWORD_INLINE &&
        (d.inner == CSS_KEYWORD_FLOW || d.inner == CSS_KEYWORD_RUBY))
        return !replaced_element_is(el);
    return false;
}

/* §3.1's list: the shared clauses, "its inner display type is table" and "its principal box is an internal
   table box". §3.2 applies "size containment to the inline-axis sizing", so it is decided by the same list. */
static bool ct_size_applies(lxb_dom_element_t *el)
{
    CssDisplay d = css_computed_display(el);
    TableBoxKind kind = table_box_kind(d);

    if (ct_shared_exclusion(el, d)) return false;
    return !table_box_kind_generates_table_box(kind) && !table_box_kind_is_internal(kind);
}

/* §3.3's and §3.5's list, which is one list: the shared clauses and "its principal box is an internal table
   box other than table-cell". */
static bool ct_layout_paint_applies(lxb_dom_element_t *el)
{
    CssDisplay d = css_computed_display(el);
    TableBoxKind kind = table_box_kind(d);

    if (ct_shared_exclusion(el, d)) return false;
    return !table_box_kind_is_internal(kind) || kind == TABLE_BOX_CELL;
}

bool containment_size(lxb_dom_element_t *el)
{
    DCHECK(el != NULL, "css-contain-2 §3.1's size containment was asked of no element");
    return (css_computed_contain(el) & CSS_CONTAIN_SIZE) && ct_size_applies(el);
}

bool containment_inline_size(lxb_dom_element_t *el)
{
    DCHECK(el != NULL, "css-contain-2 §3.2's inline-size containment was asked of no element");
    return (css_computed_contain(el) & (CSS_CONTAIN_SIZE | CSS_CONTAIN_INLINE_SIZE)) && ct_size_applies(el);
}

bool containment_layout(lxb_dom_element_t *el)
{
    DCHECK(el != NULL, "css-contain-2 §3.3's layout containment was asked of no element");
    return (css_computed_contain(el) & CSS_CONTAIN_LAYOUT) && ct_layout_paint_applies(el);
}

bool containment_paint(lxb_dom_element_t *el)
{
    DCHECK(el != NULL, "css-contain-2 §3.5's paint containment was asked of no element");
    return (css_computed_contain(el) & CSS_CONTAIN_PAINT) && ct_layout_paint_applies(el);
}

/* Whether any containment type is in effect on `el`. Style containment (§3.4) has no effect only where "the
   element does not generate a principal box". */
static bool ct_any_active(lxb_dom_element_t *el)
{
    unsigned bits = css_computed_contain(el);
    CssDisplay d;

    if (bits == 0) return false;
    if (containment_inline_size(el) || containment_layout(el) || containment_paint(el)) return true;
    d = css_computed_display(el);
    return (bits & CSS_CONTAIN_STYLE) && !css_display_is_box(d, CSS_KEYWORD_NONE) &&
           !css_display_is_box(d, CSS_KEYWORD_CONTENTS);
}

bool containment_disables_body_propagation(const lxb_dom_node_t *doc)
{
    lxb_dom_node_t *root, *body;

    DCHECK(doc != NULL && doc->type == LXB_DOM_NODE_TYPE_DOCUMENT,
           "css-contain-2 §2's body-propagation question was asked of a node that is not a Document");
    root = document_document_element_of(doc);
    body = document_body_of(doc);
    if (root != NULL && root->type == LXB_DOM_NODE_TYPE_ELEMENT && root->ns == LXB_NS_HTML &&
        ct_any_active(lxb_dom_interface_element(root)))
        return true;
    return body != NULL && ct_any_active(lxb_dom_interface_element(body));
}
