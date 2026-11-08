/* CSS TRANSFORMS 1's element-level facts. See css_transform.h for which edition's section numbers these are,
   and for why a consumer asks both. */
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include <lexbor/core/base.h>
#include <lexbor/dom/dom.h>

#include "check.h"
#include "core/css/css_computed_value.h"
#include "core/css/css_transform.h"
#include "core/layout/replaced_element.h"

/* §2 Terminology's FIRST category's own exclusions, which are computed `display` values — "table-column boxes,
   and table-column-group boxes". They are css-display §2.4's `<display-internal>` keywords, and they are the
   TWO of the eight that a transform does not apply to: a row, a row group, a cell and a caption are all
   transformable, which is why this is the module's own two-name list and not the internal-table set. */
static bool css_tr_is_column_box(CssDisplay display)
{
    return css_display_spells(display, CSS_KEYWORD_TABLE_COLUMN) ||
           css_display_spells(display, CSS_KEYWORD_TABLE_COLUMN_GROUP);
}

/* An element's local name is `name`, compared as the parser and `createElementNS` intern it. */
static bool css_tr_local_name_is(lxb_dom_element_t *el, const char *name)
{
    size_t len = 0, k = strlen(name);
    const lxb_char_t *tag = lxb_dom_element_local_name(el, &len);

    DCHECK(tag != NULL, "an element in the tree has no local name; every element a parser or createElement "
                        "produces is interned with one");
    return len == k && memcmp(tag, name, k) == 0;
}

static bool css_tr_local_name_in(lxb_dom_element_t *el, const char *const *set, size_t n)
{
    size_t i;

    for (i = 0; i < n; i++)
        if (css_tr_local_name_is(el, set[i])) return true;
    return false;
}

/* SVG 2 §14.2 "Gradients" and §14.3 "Patterns" list the paint server elements — "paint server elements —
   ‘linearGradient’, ‘radialGradient’, ‘pattern’" in each content model — and css-transforms-1 §2 adds
   `clipPath`. SVG 2 §3.2.1 "Definitions"' renderable element "includes the following elements: ‘a’, ‘circle’, ‘ellipse’,
   ‘foreignObject’, ‘g’, ‘image’, ‘line’, ‘path’, ‘polygon’, ‘polyline’, ‘rect’, ‘svg’, ‘switch’, ‘text’,
   ‘textPath’, ‘tspan’ and ‘use’; it also includes a ‘symbol’ element that is the instance root of a
   use-element shadow tree" — this engine builds no use-element shadow tree, so no `symbol` is one. */
static bool css_tr_svg_category_member(lxb_dom_element_t *el)
{
    static const char *const MEMBERS[] = {
        "linearGradient", "radialGradient", "pattern", "clipPath",
        "a", "circle", "ellipse", "foreignObject", "g", "image", "line", "path", "polygon", "polyline", "rect",
        "svg", "switch", "text", "textPath", "tspan", "use",
    };

    return css_tr_local_name_in(el, MEMBERS, COUNTOF(MEMBERS));
}

/* "with the exception of any descendant element of text content elements": SVG 2 §11.1.1 "Definitions"' "The
   SVG text content elements are: ‘text’, ‘textPath’ and ‘tspan’." */
static bool css_tr_in_svg_text(lxb_dom_element_t *el)
{
    static const char *const TEXT_CONTENT[] = { "text", "textPath", "tspan" };
    lxb_dom_node_t *p;

    for (p = lxb_dom_interface_node(el)->parent;
         p != NULL && p->type == LXB_DOM_NODE_TYPE_ELEMENT && p->ns == LXB_NS_SVG; p = p->parent)
        if (css_tr_local_name_in(lxb_dom_interface_element(p), TEXT_CONTENT, COUNTOF(TEXT_CONTENT))) return true;
    return false;
}

/* The first category over the CSS box model. */
static bool css_tr_css_box_category(lxb_dom_element_t *el)
{
    CssDisplay d = css_computed_display(el);

    /* "all elements whose layout is GOVERNED BY THE CSS BOX MODEL": css-display-3 §2.5 "Box Generation: the
       none and contents keywords" gives `contents` no box of its own and `none` no box at all, so neither is
       in the category before any exclusion is reached. */
    if (d.kind == CSS_DISPLAY_BOX) return false;
    /* "except for … table-column boxes, and table-column-group boxes" */
    if (css_tr_is_column_box(d)) return false;
    /* "except for NON-REPLACED INLINE BOXES": the computed `display` of `inline` (not `inline-block`, an
       atomic inline-level box), with HTML §15.4 "Replaced elements"' question answered from the element's
       state by core/layout/replaced_element.h. */
    if (css_display_spells(d, CSS_KEYWORD_INLINE)) return replaced_element_of(el).replaced;
    return true;
}

CssTransformableCategory css_transform_category(lxb_dom_element_t *el)
{
    lxb_dom_element_t *parent;

    DCHECK(el != NULL, "css-transforms-1 §2 Terminology's transformable-element question was asked with no "
                       "element");
    parent = css_parent_element(el);
    if (lxb_dom_interface_node(el)->ns == LXB_NS_SVG && parent != NULL &&
        lxb_dom_interface_node(parent)->ns == LXB_NS_SVG)
        return css_tr_svg_category_member(el) && !css_tr_in_svg_text(el) ? CSS_TRANSFORMABLE_SVG
                                                                          : CSS_TRANSFORMABLE_NONE;
    return css_tr_css_box_category(el) ? CSS_TRANSFORMABLE_CSS_BOX : CSS_TRANSFORMABLE_NONE;
}

bool css_transform_is_transformable(lxb_dom_element_t *el)
{
    return css_transform_category(el) != CSS_TRANSFORMABLE_NONE;
}

bool css_transform_is_css_box_transformable(lxb_dom_element_t *el)
{
    return css_transform_category(el) == CSS_TRANSFORMABLE_CSS_BOX;
}

bool css_transform_composed_list(lxb_dom_element_t *el, CssTransformList *out)
{
    static const char *const INDIVIDUAL[] = { "translate", "rotate", "scale" };
    CssTransformFunction own[3];
    CssTransformList list = { NULL, 0 };
    size_t n = 0, i;
    bool has_list;

    DCHECK(el != NULL && out != NULL, "css-transforms-2 §6's list was asked of no element or into nowhere");
    has_list = css_computed_transform_list(el, &list);
    for (i = 0; i < COUNTOF(INDIVIDUAL); i++)
        if (css_computed_individual_transform(el, INDIVIDUAL[i], &own[n])) n++;
    if (n == 0) {
        *out = list;
        return has_list;
    }
    out->n = n + list.n;
    out->fn = lexbor_malloc(out->n * sizeof *out->fn);
    CHECK(out->fn != NULL, "css-transforms-2 §6: OOM composing an element's transform functions");
    memcpy(out->fn, own, n * sizeof *own);
    if (list.n > 0) memcpy(out->fn + n, list.fn, list.n * sizeof *list.fn);
    css_transform_list_free(&list);
    return true;
}

bool css_transform_is_transformed(lxb_dom_element_t *el)
{
    CssTransformList list;
    bool out;

    DCHECK(el != NULL, "css-transforms-1 §2 Terminology's transformed-element question was asked with no "
                       "element");
    out = css_transform_composed_list(el, &list);
    if (out) css_transform_list_free(&list);
    return out;
}

bool css_transform_creates_context(lxb_dom_element_t *el)
{
    return css_transform_is_css_box_transformable(el) && css_transform_is_transformed(el);
}
