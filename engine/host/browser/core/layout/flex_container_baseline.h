/* css-flexbox-1 §8.5 "Flex Container Baselines": a flex container's first main-axis baseline set, as the
 * alphabetic baseline in `horizontal-tb`. Its readers are the inline formatting context, which aligns an
 * `inline-flex` by its first baseline (css-inline-3 §4.2.1 "Alignment Baseline Source: the baseline-source
 * longhand": `auto` "specifies last-baseline alignment for inline-block, first-baseline alignment for everything
 * else"), a block container's baseline walk, and css-align-3 §9.1's baseline of an item that is itself a flex
 * container (core/layout/box_baseline.h). */
/* Named residual. Not covered: the cross-axis baseline set of a container whose main axis is its block axis,
 * §4's anonymous item as the item that gives the baseline, an item in last-baseline alignment, and the last
 * baseline set, each refused by name. Next diff builds: the cross-axis set of a `column` container from its
 * startmost item. How its absence would show: an `inline-flex` `column` container aborting at this file's
 * refusal. */
#ifndef ENGINE_HOST_BROWSER_CORE_LAYOUT_FLEX_CONTAINER_BASELINE_H
#define ENGINE_HOST_BROWSER_CORE_LAYOUT_FLEX_CONTAINER_BASELINE_H

#include <stdbool.h>

#include <lexbor/dom/dom.h>

#include "core/css/css_length.h"

/* `container`'s first baseline, as the distance from its top content edge, with `*has` false when it has no
   flex item (§8.5: "the flex container has no first main-axis baseline set"). False is "not yet". */
bool flex_container_first_baseline_ask(lxb_dom_element_t *container, bool *has, CssPx *out);
/* The same baseline read with a drive of its own; the return value is `*has`. */
bool flex_container_first_baseline(lxb_dom_element_t *container, CssPx *out);

#endif
