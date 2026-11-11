/* CSS Conditional 5 §5 "Container Queries"' selection of a query container for an element: "the query container
 * to be queried is selected from among the element's ancestor query containers that are established as a valid
 * query container for all the container features" (§5.4 "Container Queries: the @container rule"), and §7
 * "Container Relative Lengths: the cqw, cqh, cqi, cqb, cqmin, cqmax units"' "the query container for each axis
 * is the nearest ancestor container that accepts container size queries on that axis". Blink's
 * ContainerSelector and ContainerQueryEvaluator::FindContainer under their own names.
 *
 * Axes are physical. A container's own writing mode decides which physical axis its `inline-size` type
 * accepts, and the querying element's decides which physical axis a `cqi` or `cqb` names
 * (WPT css/css-conditional/container-queries/container-units-selection.html). A container whose principal box
 * size containment cannot apply to (css-contain-2 §3.1's and §3.2's exclusions: no principal box, a
 * non-atomic inline, a table), or that generates no box at all, is not eligible and the walk passes it
 * (WPT css/css-conditional/container-queries/container-units-ineligible-container.html). */
#ifndef ENGINE_HOST_BROWSER_CORE_CSS_CONTAINER_SELECTOR_H
#define ENGINE_HOST_BROWSER_CORE_CSS_CONTAINER_SELECTOR_H

#include <stdbool.h>

#include <lexbor/dom/dom.h>

#include "core/css/css_length.h"

/* A set of axes a size query reads: physical ones, and the candidate container's own logical ones (§6.1.3's
   `inline-size` is "in the query container's inline axis"). The empty set is a query naming no size feature. */
enum {
    CONTAINER_AXIS_HORIZONTAL = 1u << 0,
    CONTAINER_AXIS_VERTICAL = 1u << 1,
    CONTAINER_AXIS_INLINE = 1u << 2,
    CONTAINER_AXIS_BLOCK = 1u << 3
};

/* The physical axis `c`'s own inline (`inline_axis`) or block axis is: CONTAINER_AXIS_HORIZONTAL or _VERTICAL. */
unsigned container_selector_physical(lxb_dom_element_t *c, bool inline_axis);

/* The nearest flat tree ancestor of `el` (`el` excluded) that is an eligible size query container on every axis
   in `axes` and, where `name` is not NULL, whose computed `container-name` holds `name`; NULL when there is
   none. With `axes` empty every ancestor is a container (§5: "all elements are query containers for the
   purpose of container style queries"), so only the name filters. */
lxb_dom_element_t *container_selector_find(lxb_dom_element_t *el, unsigned axes, const char *name);

/* §7's unit extent on one physical axis (`vertical`) for `el`: the content box size of the query container
   §7 selects, through core/layout/used_value.h, into `*out`. False where no eligible container exists, which
   §7 answers with "the small viewport size for that axis" — the caller's, since the viewport's facts are
   core/css/css_length.c's. */
bool container_selector_unit_extent(lxb_dom_element_t *el, bool vertical, CssPx *out);

/* The same layout read now, for a reader that cannot rest: the author match of an element under a sheet with a
   CSS Conditional 5 §5.4 `@container` rule resolves its query containers before its collection begins, since
   the collection may not nest another element's (core/css/css_style_declaration.c). */
void container_selector_extents(lxb_dom_element_t *el);

/* The layout every container query of `el` and of its ancestors can read, asked under a resting drive: the
   content box of each ancestor that is a size query container, on each axis it accepts. False ("not yet") with
   the question suspended on the job when one was deferred. */
bool container_selector_extents_ask(lxb_dom_element_t *el);

#endif
