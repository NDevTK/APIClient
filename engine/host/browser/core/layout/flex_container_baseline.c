/* css-flexbox-1 §8.5 "Flex Container Baselines". See flex_container_baseline.h. */
#include <stdint.h>
#include <stdlib.h>

#include "check.h"
#include "core/css/css_computed_value.h"
#include "core/layout/block_flow.h"
#include "core/layout/box_baseline.h"
#include "core/layout/box_subject.h"
#include "core/layout/containment.h"
#include "core/layout/box_tree.h"
#include "core/layout/flex_baseline.h"
#include "core/layout/flex_container_baseline.h"
#include "core/layout/flex_cross_size.h"
#include "core/layout/flex_item.h"
#include "core/layout/flex_line.h"
#include "core/layout/flex_position.h"
#include "core/layout/layout_job.h"
#include "core/layout/used_value.h"

/* The element of member `m`, which gives the container its baseline; §4's anonymous item is refused by name. */
static lxb_dom_element_t *fcb_element(lxb_dom_element_t *container, const FlexLineMember *m)
{
    char buf[160];

    if (m->anonymous)
        DFAILF("%s: css-flexbox-1 §8.5 takes the container's baseline from §4's anonymous item around its text, "
               "whose position core/layout/flex_position.h does not answer. BUILD that item's placement",
               box_subject(container, buf, sizeof buf));
    return lxb_dom_interface_element(m->first);
}

/* The startmost item of a single-line `column` container: the first in order-modified document order, which
   orders by `order` and keeps document order within one value. NULL when it has none. */
static lxb_dom_element_t *fcb_column_startmost(lxb_dom_element_t *container)
{
    BoxTreeChild c;
    lxb_dom_element_t *best = NULL;
    long best_order = 0;
    char buf[160];

    for (c = box_tree_first_child(container); box_tree_child_exists(c); c = box_tree_next_sibling(container, c)) {
        lxb_dom_node_t *node = box_tree_child_node(c);
        FlexItemChildKind kind = flex_item_child_kind(container, node);
        long order;

        if (kind == FLEX_ITEM_CHILD_NONE) continue;
        if (kind == FLEX_ITEM_CHILD_TEXT) {
            order = 0;
        } else {
            order = flex_item_order(lxb_dom_interface_element(node));
        }
        if (best != NULL && order >= best_order) continue;
        if (kind == FLEX_ITEM_CHILD_TEXT)
            DFAILF("%s: css-flexbox-1 §8.5 takes the container's baseline from §4's anonymous item around its "
                   "text, whose position core/layout/flex_position.h does not answer. BUILD that item's placement",
                   box_subject(container, buf, sizeof buf));
        best = lxb_dom_interface_element(node);
        best_order = order;
    }
    return best;
}

/* §8.5's cross-axis set: "If the flex container has at least one flex item, the flex container's first/last
   cross-axis baseline set is generated from the alignment baseline of the startmost/endmost flex item. (If that
   item has no alignment baseline parallel to the flex container's cross axis, then one is first synthesized
   from its border edges.)" */
static bool fcb_cross_axis_ask(lxb_dom_element_t *container, bool *has, CssPx *out)
{
    lxb_dom_element_t *item = fcb_column_startmost(container);
    bool item_has = false;
    CssPx b = css_px(0.0), x, y;

    *has = item != NULL;
    if (item == NULL) return true;
    if (!box_first_baseline_ask(item, &item_has, &b)) return false;
    flex_position_item_offset(container, item, &x, &y);
    *out = css_px_add(y, item_has ? b : used_value_border_edge_px(item, true));
    return true;
}

bool flex_container_first_baseline_ask(lxb_dom_element_t *container, bool *has, CssPx *out)
{
    FlexLineMember *m = NULL;
    size_t n, i, pick = SIZE_MAX;
    lxb_dom_element_t *item = NULL;
    bool item_has = false;
    CssPx b = css_px(0.0), x, y;
    char buf[160];

    DCHECK(container != NULL && has != NULL && out != NULL,
           "a flex container's baseline was asked for with no container or nowhere to put it");
    /* css-contain-2 §3.3 "Layout Containment": "For the purpose of the vertical-align property, or any other
       property whose effects need to relate the position of the layout containment box's baseline to
       something other than its descendants, the containment box is treated as having no baseline." Every
       caller of this entry is such a reader. */
    if (containment_layout(container)) {
        *has = false;
        return true;
    }
    if (flex_container_main_axis(container) != FLEX_MAIN_AXIS_INLINE) return fcb_cross_axis_ask(container, has, out);
    n = flex_line_members(container, &m);
    *has = n > 0;
    if (n == 0) return true;
    /* "If any of the flex items on the flex container's startmost flex line participate in first baseline
       alignment along the main axis, generate a baseline set from their shared alignment baseline": the
       group shares one baseline, so its first member's position and baseline give it. */
    for (i = 0; i < n && m[i].line == 0; i++) {
        if (!flex_baseline_participates(container, &m[i])) continue;
        item = fcb_element(container, &m[i]);
        if (flex_cross_size_resolved_align(container, item).baseline == CSS_KEYWORD_LAST)
            DFAILF("%s is in last-baseline alignment, whose shared baseline css-flexbox-1 §8.5's second step "
                   "takes. BUILD the last baseline set", box_subject(item, buf, sizeof buf));
        pick = i;
        break;
    }
    /* The third step takes "the baseline set from the first such flex item in order-modified document order"
       among the items with one, and the fourth synthesizes from "the startmost flex item on the startmost
       line". Chrome 155 takes the startmost item whether or not it has a baseline set, synthesizing from it
       when it has none, and is followed here: a line of an empty item then an item with text puts the
       container's baseline at the empty item's border-box bottom in that browser. A flex item synthesizes from
       its border edges (css-align-3 §9.1), the alphabetic baseline from the line-under edge. */
    if (pick == SIZE_MAX) pick = 0;
    item = fcb_element(container, &m[pick]);
    free(m);
    if (!box_first_baseline_ask(item, &item_has, &b)) return false;
    flex_position_item_offset(container, item, &x, &y);
    *out = css_px_add(y, item_has ? b : used_value_border_edge_px(item, true));
    return true;
}

bool flex_container_first_baseline(lxb_dom_element_t *container, CssPx *out)
{
    LayoutJobDrive d;
    bool has = false;

    layout_job_drive_begin(&d);
    while (!flex_container_first_baseline_ask(container, &has, out)) (void)block_flow_drive_deferred(&d);
    layout_job_drive_end(&d);
    return has;
}
