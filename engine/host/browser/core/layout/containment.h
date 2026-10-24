/* css-contain-2 §3 "Types of Containment": which containment types are in effect on an element's principal
 * box. The computed `contain` (core/css/css_computed_value.h's `css_computed_contain`) names the types the
 * author asked for; each type's section then lists the boxes on which it "has no effect", and this component
 * applies those lists, so every reader asks whether a type is in effect rather than what was declared.
 *
 * Style containment (§3.4) is not answered here: it scopes counters and quote depth, and this engine generates
 * no `content` for a pseudo-element, so nothing reads either. */
#ifndef ENGINE_HOST_BROWSER_CORE_LAYOUT_CONTAINMENT_H
#define ENGINE_HOST_BROWSER_CORE_LAYOUT_CONTAINMENT_H

#include <stdbool.h>

#include <lexbor/dom/dom.h>

/* §3.1 "Size Containment": `el`'s box is a size containment box, whose intrinsic sizes in both axes are
   "determined as if the element had no content". */
bool containment_size(lxb_dom_element_t *el);

/* §3.2 "Inline-Size Containment" or §3.1: `el`'s inline-axis intrinsic sizes are determined as if it had no
   content. True wherever `containment_size` is. */
bool containment_inline_size(lxb_dom_element_t *el);

/* §3.3 "Layout Containment": `el`'s box is a layout containment box. It establishes an independent formatting
   context and both positioning containing blocks, creates a stacking context, has no baseline for anything
   outside it, and its overflow is ink overflow where `overflow` is `visible` or `clip`. */
bool containment_layout(lxb_dom_element_t *el);

/* §3.5 "Paint Containment": `el`'s box is a paint containment box. Its contents are clipped to its overflow
   clip edge, and it establishes an independent formatting context and both positioning containing blocks and
   creates a stacking context. */
bool containment_paint(lxb_dom_element_t *el);

/* §2 "Strong Containment: the contain property": "when any containments are active on either the HTML html or
   body elements, propagation of properties from the body element to the initial containing block, the
   viewport, or the canvas background, is disabled." Whether that holds for document `doc`. */
bool containment_disables_body_propagation(const lxb_dom_node_t *doc);

#endif
