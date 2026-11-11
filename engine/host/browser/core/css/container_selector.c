/* CSS Conditional 5 §5's query container selection. See container_selector.h. */
#include <stdbool.h>
#include <stdlib.h>

#include <lexbor/dom/dom.h>

#include "check.h"
#include "core/css/container_selector.h"
#include "core/css/css_computed_value.h"
#include "core/css/css_container.h"
#include "core/css/css_logical.h"
#include "core/dom/element_view.h"
#include "core/layout/containment.h"
#include "core/layout/used_value.h"

/* The physical axes `c` accepts container size queries on. §5.1: `size` "establishes a query container for
   container size queries on both the inline and block axis" and applies size containment; `inline-size` does
   so "on the container's own inline axis" and applies inline-size containment. Where that containment cannot
   apply to `c`'s principal box, or `c` has no box (§6.1: "If the query container does not have a principal
   box ... the result of evaluating the size feature is unknown"), `c` accepts none. */
static unsigned cs_accepted_axes(lxb_dom_element_t *c)
{
    unsigned type = css_computed_container_type(c);

    if (!(type & (CSS_CONTAINER_TYPE_SIZE | CSS_CONTAINER_TYPE_INLINE_SIZE))) return 0;
    if (!element_view_has_box(lxb_dom_interface_node(c))) return 0;
    if (type & CSS_CONTAINER_TYPE_SIZE)
        return containment_size(c) ? (CONTAINER_AXIS_HORIZONTAL | CONTAINER_AXIS_VERTICAL) : 0;
    if (type & CSS_CONTAINER_TYPE_INLINE_SIZE) {
        if (!containment_inline_size(c)) return 0;
        return container_selector_physical(c, true);
    }
    return 0;
}

unsigned container_selector_physical(lxb_dom_element_t *c, bool inline_axis)
{
    DCHECK(c != NULL, "a container's logical axis was mapped with no container");
    return css_logical_axis_is_vertical(c, inline_axis ? CSS_LOGICAL_AXIS_INLINE : CSS_LOGICAL_AXIS_BLOCK)
               ? CONTAINER_AXIS_VERTICAL : CONTAINER_AXIS_HORIZONTAL;
}

static bool cs_named(lxb_dom_element_t *c, const char *name)
{
    char *names = css_computed_value(c, "container-name");
    bool has;

    DCHECK(names != NULL, "CSS Conditional 5 §5.2's computed `container-name` came back as no value; its row "
                          "derives one always");
    has = css_container_name_has(names, name);
    free(names);
    return has;
}

lxb_dom_element_t *container_selector_find(lxb_dom_element_t *el, unsigned axes, const char *name)
{
    lxb_dom_element_t *c;

    DCHECK(el != NULL, "a query container was selected for no element");
    DCHECKF((axes & ~(CONTAINER_AXIS_HORIZONTAL | CONTAINER_AXIS_VERTICAL | CONTAINER_AXIS_INLINE |
                      CONTAINER_AXIS_BLOCK)) == 0,
            "a query container was selected over axis set %u, which names an axis that is none of the four", axes);
    for (c = css_parent_element(el); c != NULL; c = css_parent_element(c)) {
        unsigned need = axes & (CONTAINER_AXIS_HORIZONTAL | CONTAINER_AXIS_VERTICAL);

        if (axes & CONTAINER_AXIS_INLINE) need |= container_selector_physical(c, true);
        if (axes & CONTAINER_AXIS_BLOCK) need |= container_selector_physical(c, false);
        if ((cs_accepted_axes(c) & need) != need) continue;
        if (name != NULL && !cs_named(c, name)) continue;
        return c;
    }
    return NULL;
}

bool container_selector_unit_extent(lxb_dom_element_t *el, bool vertical, CssPx *out)
{
    lxb_dom_element_t *c = container_selector_find(el, vertical ? CONTAINER_AXIS_VERTICAL : CONTAINER_AXIS_HORIZONTAL,
                                                   NULL);

    DCHECK(out != NULL, "a container unit's extent was asked for with nowhere to put it");
    if (c == NULL) return false;
    /* §6.1.1 "Width: the width feature" and §6.1.2: the query container's content box. */
    *out = used_value_content_px(c, vertical);
    return true;
}

void container_selector_extents(lxb_dom_element_t *el)
{
    lxb_dom_element_t *c;

    DCHECK(el != NULL, "a query container's extents were read for no element");
    for (c = css_parent_element(el); c != NULL; c = css_parent_element(c)) {
        unsigned axes = cs_accepted_axes(c);

        if (axes & CONTAINER_AXIS_HORIZONTAL) (void)used_value_content_px(c, false);
        if (axes & CONTAINER_AXIS_VERTICAL) (void)used_value_content_px(c, true);
    }
}

bool container_selector_extents_ask(lxb_dom_element_t *el)
{
    lxb_dom_element_t *c;
    CssPx px;

    DCHECK(el != NULL, "a query container's extents were asked for no element");
    for (c = css_parent_element(el); c != NULL; c = css_parent_element(c)) {
        unsigned axes = cs_accepted_axes(c);

        if ((axes & CONTAINER_AXIS_HORIZONTAL) && !used_value_content_ask(c, false, &px)) return false;
        if ((axes & CONTAINER_AXIS_VERTICAL) && !used_value_content_ask(c, true, &px)) return false;
    }
    return true;
}
