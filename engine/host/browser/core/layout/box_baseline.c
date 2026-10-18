/* css-align-3 §9.1 "Determining the Baselines of a Box". See box_baseline.h. */
#include "check.h"
#include "core/css/css_computed_value.h"
#include "core/css/css_display.h"
#include "core/layout/block_flow.h"
#include "core/layout/box_baseline.h"
#include "core/layout/box_subject.h"
#include "core/layout/flex_container_baseline.h"
#include "core/layout/flex_item.h"
#include "core/layout/grid_baseline.h"
#include "core/layout/replaced_element.h"
#include "core/layout/table_box.h"
#include "core/layout/used_value.h"

bool box_first_baseline_ask(lxb_dom_element_t *el, bool *has, CssPx *out)
{
    CssDisplay d;
    CssPx inner = css_px(0.0);
    char buf[160];

    DCHECK(el != NULL && has != NULL && out != NULL, "a box's baseline was asked for with no box or nowhere to put it");
    *has = false;
    if (replaced_element_of(el).replaced) return true;
    d = css_computed_display(el);
    if (css_display_spells(d, CSS_KEYWORD_GRID) || css_display_spells(d, CSS_KEYWORD_INLINE_GRID)) {
        if (!grid_container_first_baseline_ask(el, has, &inner)) return false;
    } else if (flex_item_display_is_flex_container(d)) {
        if (!flex_container_first_baseline_ask(el, has, &inner)) return false;
    } else if (table_box_kind_generates_table_box(table_box_kind(d))) {
        DFAILF("%s is a table whose first baseline set css-align-3 §9.1 takes from \"its first/last row\". BUILD "
               "that row's baseline, which core/layout/table_height.h computes and does not export",
               box_subject(el, buf, sizeof buf));
    } else if (block_flow_display_is_block_container(d)) {
        if (!block_flow_first_line_box_baseline_ask(el, &inner, has)) return false;
    } else {
        DFAILF("%s generates a box css-align-3 §9.1 gives no baseline rule here", box_subject(el, buf, sizeof buf));
    }
    if (*has) *out = css_px_add(used_value_leading_edge_px(el, true), inner);
    return true;
}
