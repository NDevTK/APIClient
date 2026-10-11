/* css-flexbox-1 §8.3 "Cross-axis Alignment: the align-items and align-self properties"' `baseline` value over one
 * flex line: which items participate (§9.4 "Cross Size Determination"' step 8.1), each item's alignment baseline
 * (css-align-3 §9.1 "Determining the Baselines of a Box"), the baseline-sharing groups of css-align-3 §9.2
 * "Baseline Alignment Grouping", and an item's cross-axis offset under css-align-3 §9.3 "Aligning Boxes by
 * Baseline". The container must be `horizontal-tb`. Nothing is stored (per-flow state, solver/dom_cow.h). */
#ifndef ENGINE_HOST_BROWSER_CORE_LAYOUT_FLEX_BASELINE_H
#define ENGINE_HOST_BROWSER_CORE_LAYOUT_FLEX_BASELINE_H

#include <stdbool.h>

#include <lexbor/dom/dom.h>

#include "core/css/css_length.h"
#include "core/layout/flex_line.h"

/* An alignment baseline as two distances on the cross axis, both from the box's hypothetical outer edges (its
   margin edges): `before` from the cross-start edge down to the baseline, `after` from the baseline down to the
   cross-end edge. A group's extent is the two maxima over its members. */
typedef struct {
    CssPx before;
    CssPx after;
} FlexBaselineExtent;

/* Step 8.1's collection: "the flex items whose inline-axis is parallel to the main-axis, whose align-self is
   baseline, and whose cross-axis margins are both non-auto". §4's anonymous item takes the container's
   `align-items` and has no margins. */
bool flex_baseline_participates(lxb_dom_element_t *container, const FlexLineMember *m);

/* Step 8.1's two maxima over every participating member of `line`, whose sum the step adds to the line's cross
   size. FALSE when no member of the line participates. */
bool flex_baseline_line_extent(lxb_dom_element_t *container, size_t line, FlexBaselineExtent *out);

/* css-align-3 §9.3 for the baseline-aligned element `item` on a line `line_size` long in the cross axis: the
   distance from the line's cross-start edge to the item's cross-start margin edge, its group aligned by baseline
   and positioned by its fallback alignment (css-align-3 §4.2: `safe self-start` for `first baseline`, `safe
   self-end` for `last baseline`). Each member's baseline is its first or last baseline set as the preference
   names, from its first or last in-flow line box, or synthesized from its border box where it has none or has
   none in the cross axis (§8.3: "If the item does not have a baseline in the necessary axis, then one is
   synthesized from the flex item's border box"). */
CssPx flex_baseline_margin_offset(lxb_dom_element_t *container, lxb_dom_element_t *item, CssPx line_size);

#endif /* ENGINE_HOST_BROWSER_CORE_LAYOUT_FLEX_BASELINE_H */
