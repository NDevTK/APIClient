/* css-grid-1 §10.6 "Grid Container Baselines". See grid_baseline.h. */
#include "check.h"
#include "core/css/css_computed_value.h"
#include "core/layout/block_flow.h"
#include "core/layout/box_baseline.h"
#include "core/layout/box_subject.h"
#include "core/layout/containment.h"
#include "core/layout/grid_baseline.h"
#include "core/layout/grid_placement.h"
#include "core/layout/grid_track.h"
#include "core/layout/layout_job.h"
#include "core/layout/used_value.h"

/* §10.6's steps 1 and 2 ask whether any item in the track participates in baseline alignment. */
static void gb_require_no_baseline_alignment(lxb_dom_element_t *container, lxb_dom_element_t *item)
{
    CssAlign self = css_computed_align(item, "align-self");
    char buf[160];

    if (self.value == CSS_KEYWORD_AUTO) self = css_computed_align(container, "align-items");
    if (self.value == CSS_KEYWORD_BASELINE)
        DFAILF("%s is a grid item in baseline alignment, whose shared alignment baseline css-grid-1 §10.6 "
               "\"Grid Container Baselines\"' first two steps take. BUILD baseline self-alignment of grid items",
               box_subject(item, buf, sizeof buf));
}

bool grid_container_first_baseline_ask(lxb_dom_element_t *container, bool *has, CssPx *out)
{
    GridPlacement p;
    BoxTreeChild c;
    lxb_dom_element_t *first = NULL, *item;
    size_t k = 0, row = 0, col = 0;
    CssPx x, y, top, inner;
    bool inner_has;

    DCHECK(container != NULL && has != NULL && out != NULL,
           "a grid container's baseline was asked for with no container or nowhere to put it");
    /* css-contain-2 §3.3 "Layout Containment": "For the purpose of the vertical-align property, or any other
       property whose effects need to relate the position of the layout containment box's baseline to
       something other than its descendants, the containment box is treated as having no baseline." Every
       caller of this entry is such a reader. */
    if (containment_layout(container)) {
        *has = false;
        return true;
    }
    grid_track_placement(container, &p);
    /* The first (block-start-most) non-empty row, and its first item in grid order: the traversal of the grid's
       cells, which with one item per cell is the smallest column. */
    for (c = box_tree_first_child(container); box_tree_child_exists(c); c = box_tree_next_sibling(container, c)) {
        item = grid_item_of(container, c);
        if (item == NULL) continue;
        DCHECK(k < p.n, "a grid container's baseline walk met more items than its placement placed");
        if (first == NULL || p.cells[k].row < row || (p.cells[k].row == row && p.cells[k].col < col)) {
            first = item;
            row = p.cells[k].row;
            col = p.cells[k].col;
        }
        k++;
    }
    k = 0;
    for (c = box_tree_first_child(container); box_tree_child_exists(c); c = box_tree_next_sibling(container, c)) {
        item = grid_item_of(container, c);
        if (item == NULL) continue;
        if (p.cells[k].row == row) gb_require_no_baseline_alignment(container, item);
        k++;
    }
    grid_placement_free(&p);
    *has = first != NULL;
    if (first == NULL) return true;
    /* The item's border-box top, in the container's content box. */
    if (!grid_item_area_origin_ask(container, first, &x, &y)) return false;
    top = css_px_add(y, used_value_px(first, "margin-top"));
    /* §10.6's third step takes "the baseline set from the first such grid item in grid order" among the items
       that have one, and its fourth synthesizes from the first item. Chrome 155 takes the first item in grid
       order whether or not it has a baseline set, synthesizing from it when it has none, and is followed here:
       a first row of an empty item then an item with text puts the container's baseline at the empty item's
       border-box bottom in that browser. Grid items synthesize from their border edges (css-align-3 §9.1
       "Determining the Baselines of a Box"), the alphabetic baseline from the line-under edge. */
    if (!box_first_baseline_ask(first, &inner_has, &inner)) return false;
    *out = css_px_add(top, inner_has ? inner : used_value_border_edge_px(first, true));
    return true;
}

bool grid_container_first_baseline(lxb_dom_element_t *container, CssPx *out)
{
    LayoutJobDrive d;
    bool has = false;

    layout_job_drive_begin(&d);
    while (!grid_container_first_baseline_ask(container, &has, out)) (void)block_flow_drive_deferred(&d);
    layout_job_drive_end(&d);
    return has;
}
