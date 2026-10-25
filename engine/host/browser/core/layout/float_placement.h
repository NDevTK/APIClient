/* CSS 2.2 §9.5.1 "Positioning the float: the float property"' placement of one float among the floats of its
 * block formatting context. Every rule that names another box names one "earlier in the source document", so the
 * floats of a context are placed in document order, each against the margin boxes of those before it, and each
 * placement is kept in core/layout/flow_placement.h's record.
 * Coordinates are the context root's content-box frame: x from its left content edge, y from its top content
 * edge, the frame core/layout/block_flow.c's walks record each block container's top content edge in. */
#ifndef ENGINE_HOST_BROWSER_CORE_LAYOUT_FLOAT_PLACEMENT_H
#define ENGINE_HOST_BROWSER_CORE_LAYOUT_FLOAT_PLACEMENT_H

#include <stdbool.h>

#include <lexbor/dom/dom.h>

#include "core/layout/flow_placement.h"

/* The margin box of float `f` in its context root's content-box frame. Places every earlier float of the context
   that is not yet placed first. False ("not yet") when a walk or measurement it needs was deferred; `*out` is
   then unwritten. Asked with a placement record held: the record holds the walks' frames it reads. */
bool float_placement_ask(lxb_dom_element_t *f, FlowFloatBox *out);

#endif
