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
   then unwritten. Asked only with a placement record held (checked): the record holds the walks' frames it
   reads. */
bool float_placement_ask(lxb_dom_element_t *f, FlowFloatBox *out);

/* The placement of float `f`, which sits on a line (`block_flow_float_on_line`), from `top` in the context root's
   frame: the line box fill's choice of the line it starts at (CSS 2.2 §9.5: "if there is a line box, the outer
   top of the floated box is aligned with the top of the current line box"). Recorded like `float_placement_ask`'s,
   which defers to the fill for such a float. */
bool float_placement_place_at(lxb_dom_element_t *f, CssPx top, FlowFloatBox *out);

/* The width of float `f`'s margin box. False ("not yet") when its shrink-to-fit measurement was deferred. */
bool float_placement_margin_width_ask(lxb_dom_element_t *f, CssPx *out);

/* The left content edge of block container `cb` in the content-box frame of its context root `root`. */
CssPx float_placement_content_left(lxb_dom_element_t *cb, lxb_dom_element_t *root);

/* The band the placed margin boxes `placed[0..n)` leave for a box spanning `[y, y + h)` (the line `y` alone when
   `h` is zero): `*left` and `*right`, given as the edges with no float, are narrowed by each float beside the
   band, and `*next` becomes the nearest bottom among those floats. Whether any float is beside it. */
bool float_placement_band(const FlowFloatBox *placed, size_t n, CssPx y, CssPx h, CssPx *left, CssPx *right,
                          CssPx *next);

#endif
