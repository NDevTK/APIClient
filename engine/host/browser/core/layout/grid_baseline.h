/* css-grid-1 §10.6 "Grid Container Baselines": a grid container's first baseline set, as the alphabetic
 * baseline in a horizontal writing mode. Its readers are the inline formatting context, which aligns an
 * `inline-grid` by its first baseline (css-inline-3 §4.2.1 "Alignment Baseline Source: the baseline-source
 * longhand": `auto` "specifies last-baseline alignment for inline-block, first-baseline alignment for everything
 * else"), and an `inline-block`'s last-line walk, which takes a grid's first baseline as Chrome 155 does. */
/* Named residual. Not covered: §10.6's last baseline set, and its steps 1 and 2, which read the items' own
 * baseline alignment (`align-self: baseline` is refused by name). Next diff builds: the last set from the last
 * row's items, for `baseline-source: last` and `align-self: last baseline`. How its absence would show: a box
 * aligned by a grid's last baseline sitting on the grid's first row. */
#ifndef ENGINE_HOST_BROWSER_CORE_LAYOUT_GRID_BASELINE_H
#define ENGINE_HOST_BROWSER_CORE_LAYOUT_GRID_BASELINE_H

#include <stdbool.h>

#include <lexbor/dom/dom.h>

#include "core/css/css_length.h"

/* `container`'s first baseline, as the distance from its top content edge, with `*has` false when it has no
   grid item (§10.6's last step: "the grid container has no first baseline set"). False is "not yet". */
bool grid_container_first_baseline_ask(lxb_dom_element_t *container, bool *has, CssPx *out);
/* The same baseline read with a drive of its own; the return value is `*has`. */
bool grid_container_first_baseline(lxb_dom_element_t *container, CssPx *out);

#endif
