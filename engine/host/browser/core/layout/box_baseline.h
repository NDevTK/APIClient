/* css-align-3 §9.1 "Determining the Baselines of a Box": the first baseline set of a box that an alignment
 * context takes from one of its items, as the alphabetic baseline in `horizontal-tb`. Each layout model answers
 * for its own box: a block container from its first in-flow line box or block-level child "that contributes a
 * set of first/last baselines", a grid container from css-grid-1 §10.6 and a flex container from css-flexbox-1
 * §8.5. A replaced element has none. A table's first row is refused by name. */
#ifndef ENGINE_HOST_BROWSER_CORE_LAYOUT_BOX_BASELINE_H
#define ENGINE_HOST_BROWSER_CORE_LAYOUT_BOX_BASELINE_H

#include <stdbool.h>

#include <lexbor/dom/dom.h>

#include "core/css/css_length.h"

/* `el`'s first baseline as the distance from its border-box top, with `*has` false when it has no first
   baseline set, which the caller's alignment context then synthesizes. False is "not yet". */
bool box_first_baseline_ask(lxb_dom_element_t *el, bool *has, CssPx *out);

#endif
