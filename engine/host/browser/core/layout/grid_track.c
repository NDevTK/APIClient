/* css-grid-1 §11.3 "Track Sizing Algorithm" over a grid container's columns. See grid_track.h. */
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "core/css/css_computed_value.h"
#include "core/css/css_display.h"
#include "core/css/css_style_declaration.h"
#include "core/layout/block_flow.h"
#include "core/layout/box_subject.h"
#include "core/layout/flex_item.h"
#include "core/layout/grid_track.h"
#include "core/layout/intrinsic_size.h"
#include "core/layout/layout_job.h"
#include "core/layout/replaced_element.h"
#include "core/layout/scroll_container.h"
#include "core/layout/used_value.h"

/* A grid property this sizing reads as its initial value: `name` must be undeclared on `el`, or declared as
   exactly `initial` (or the CSS-wide keywords that answer it for a non-inherited property). A declaration
   lexbor's registry does not parse still reaches the cascade by name (core/css/css_style_declaration.c's
   `__CUSTOM` route), so a declared value is seen here even though no computed value is derived for it. */
static void gt_require_initial(lxb_dom_element_t *el, const char *name, const char *initial, const char *build)
{
    char *v = cssom_cascaded_value(el, name);
    char buf[160];
    bool ok = v == NULL || strcmp(v, initial) == 0 || strcmp(v, "initial") == 0 || strcmp(v, "unset") == 0;

    if (!ok)
        DFAILF("%s declares `%s: %s`, and css-grid-1 §11 \"Grid Layout Algorithm\" is built here only for the "
               "property's initial value `%s`. BUILD %s",
               box_subject(el, buf, sizeof buf), name, v, initial, build);
    free(v);
}

/* The container's side of the residual in grid_track.h: every property that would change its columns. */
static void gt_require_implicit_columns(lxb_dom_element_t *c)
{
    static const char EXPLICIT[] =
        "css-grid-1 §7.2 \"Explicit Track Sizing: the grid-template-rows and grid-template-columns properties\"' "
        "track list as this container's explicit columns";

    gt_require_initial(c, "grid-template-columns", "none", EXPLICIT);
    gt_require_initial(c, "grid-template-areas", "none",
                       "css-grid-1 §7.3 \"Named Areas: the grid-template-areas property\", whose areas count "
                       "explicit columns");
    gt_require_initial(c, "grid-template", "none", EXPLICIT);
    gt_require_initial(c, "grid", "none", EXPLICIT);
    gt_require_initial(c, "grid-auto-columns", "auto",
                       "css-grid-1 §7.6 \"Implicit Track Sizing\"' sizing functions for the implicit columns");
    gt_require_initial(c, "grid-auto-flow", "row",
                       "css-grid-1 §8.5 \"Grid Item Placement Algorithm\" for column flow and `dense` packing");
    gt_require_initial(c, "column-gap", "normal",
                       "CSS Gaps 1 §2.1 \"Row and Column Gutters: the row-gap and column-gap properties\" as "
                       "fixed-size gutter tracks (css-grid-1 §11.2 \"Track Sizing Terminology\"' note)");
    gt_require_initial(c, "gap", "normal", "CSS Gaps 1 §2.2 \"Gap Shorthand: the gap property\"'s gutters");
    gt_require_initial(c, "grid-column-gap", "normal",
                       "CSS Gaps 1 §2.4 \"Legacy Gap Properties: the grid-row-gap, grid-column-gap, and "
                       "grid-gap properties\" as the column gutters");
    gt_require_initial(c, "grid-gap", "normal", "CSS Gaps 1 §2.4's legacy shorthand as the column gutters");
}

/* An item's side: an item placed by a line in the column axis is not auto-placed. */
static void gt_require_auto_column_placement(lxb_dom_element_t *item)
{
    static const char PLACEMENT[] =
        "css-grid-1 §8.3 \"Line-based Placement\" and §8.5 \"Grid Item Placement Algorithm\" for an item with "
        "a definite column position";

    gt_require_initial(item, "grid-column-start", "auto", PLACEMENT);
    gt_require_initial(item, "grid-column-end", "auto", PLACEMENT);
    gt_require_initial(item, "grid-column", "auto", PLACEMENT);
    gt_require_initial(item, "grid-area", "auto", PLACEMENT);
}

/* css-grid-1 §11.5's minimum contribution: "if the item's computed preferred size behaves as auto or depends
   on the size of its containing block in the relevant axis, its minimum contribution is the outer size that
   would result from assuming the item's used minimum size as its preferred size; else the item's minimum
   contribution is its min-content contribution". The used minimum of `min-width: auto` is §6.6 "Automatic
   Minimum Size of Grid Items"': 0 for a scroll container, else the content size suggestion (the min-content
   size, "clamped by the maximum size in the affected axis, if it's definite"), since the one column has an
   `auto` minimum and is not flexible. */
static CssPx gt_minimum_contribution(lxb_dom_element_t *item, IntrinsicInlineSizes contributions)
{
    CssLength w = css_computed_length(item, "width");
    IntrinsicInlineSizes used;
    CssPx m;
    char buf[160];

    if (w.kind == CSS_LENGTH_ABSOLUTE) return contributions.min_content;
    if (w.kind == CSS_LENGTH_KEYWORD && w.keyword != CSS_KEYWORD_AUTO)
        DFAILF("%s has an intrinsic `width` keyword, whose css-grid-1 §11.5 minimum contribution is not built. "
               "BUILD css-sizing-3 §3.2's keyword as the item's preferred size in the contribution",
               box_subject(item, buf, sizeof buf));
    if (!intrinsic_declared_sizing_px(item, INTRINSIC_AXIS_HORIZONTAL, "min-width", CSS_KEYWORD_AUTO, &m)) {
        if (scroll_container_is(item)) {
            m = css_px(0.0);
        } else {
            if (replaced_element_of(item).replaced)
                DFAILF("%s is a replaced grid item with an automatic minimum width. css-grid-1 §6.6's content-based "
                       "minimum size takes its transferred size suggestion. BUILD that suggestion",
                       box_subject(item, buf, sizeof buf));
            m = intrinsic_inline_sizes(item).min_content;
            {
                CssPx mx;

                if (intrinsic_declared_sizing_px(item, INTRINSIC_AXIS_HORIZONTAL, "max-width", CSS_KEYWORD_NONE, &mx))
                    m = css_px_min(m, mx);
            }
        }
    }
    used.min_content = m;
    used.max_content = m;
    return intrinsic_outer_contribution(item, used).min_content;
}

void grid_track_sizing_begin(GridTrackSizing *g, lxb_dom_element_t *container)
{
    char buf[160];

    DCHECK(g != NULL && container != NULL, "css-grid-1 §11.3 was begun with no state or no grid container");
    DCHECKF(css_computed_display_is(container, CSS_KEYWORD_GRID) ||
                css_computed_display_is(container, CSS_KEYWORD_INLINE_GRID),
            "css-grid-1 §11.3 was begun over %s, which generates no grid container box",
            box_subject(container, buf, sizeof buf));
    gt_require_implicit_columns(container);
    g->container = container;
    g->cursor = box_tree_first_child(container);
    g->items_done = false;
    g->sized = false;
    /* §11.2 "Track Sizing Terminology"'s available grid space: "If the grid container's size is definite,
       then use the size of its content box." The used width this asks is that definite size. */
    g->avail = used_value_content_px(container, false);
    /* §11.4 "Initialize Track Sizes": an intrinsic min sizing function gives a base size of zero, an
       intrinsic max sizing function a growth limit of infinity. `auto` is both. */
    g->base = css_px(0.0);
    g->growth = css_px(0.0);
    g->growth_inf = true;
}

/* Fold one span-1 item into the column: §11.5 step 2's `auto` minimum ("set the track's base size to the
   maximum of its items' minimum contributions, floored at zero", the container being sized under no
   min-/max-content constraint) and its `auto` maximum, which "fit-content() and auto max track sizing
   functions are treated the same as max-content" makes "the maximum of the items' max-content
   contributions". */
static void gt_fold_item(GridTrackSizing *g, lxb_dom_element_t *item)
{
    IntrinsicInlineSizes c = intrinsic_inline_contributions(item);
    CssPx minc = css_px_max(gt_minimum_contribution(item, c), css_px(0.0));

    g->base = css_px_max(g->base, minc);
    g->growth = g->growth_inf ? c.max_content : css_px_max(g->growth, c.max_content);
    g->growth_inf = false;
}

/* §11.5's last span-1 sentence, §11.6 "Maximize Tracks", §11.7 (no flexible track exists here) and §11.8
   "Stretch auto Tracks", over the one column. */
static void gt_size(GridTrackSizing *g)
{
    CssPx free_space;
    CssKeyword jc;

    /* "If the track's growth limit is now less than its base size, increase the growth limit to match." */
    if (!g->growth_inf) g->growth = css_px_max(g->growth, g->base);
    /* §11.2's free space: the available grid space minus the base sizes, floored at zero. §11.6: "If the free
       space is positive, distribute it equally to the base sizes of all tracks, freezing tracks as they reach
       their growth limits". */
    free_space = css_px_max(css_px_sub(g->avail, g->base), css_px(0.0));
    g->base = g->growth_inf ? css_px_add(g->base, free_space)
                            : css_px_min(g->growth, css_px_add(g->base, free_space));
    /* §11.8: "When the content-distribution property of the grid container is normal or stretch in this axis,
       this step expands tracks that have an auto max track sizing function by dividing any remaining positive,
       definite free space equally amongst them." */
    jc = css_computed_align(g->container, "justify-content").value;
    if (jc == CSS_KEYWORD_NORMAL || jc == CSS_KEYWORD_STRETCH)
        g->base = css_px_add(g->base, css_px_max(css_px_sub(g->avail, g->base), css_px(0.0)));
    g->sized = true;
}

bool grid_track_sizing_step(GridTrackSizing *g)
{
    char buf[160];

    DCHECK(g != NULL && g->container != NULL && !g->sized, "css-grid-1 §11.3 was stepped after it ended");
    if (g->items_done) {
        gt_size(g);
        return true;
    }
    if (!box_tree_child_exists(g->cursor)) {
        g->items_done = true;
        return true;
    }
    if (box_tree_child_is_pseudo(g->cursor))
        DFAILF("%s has a pseudo-element child box, which css-grid-1 §6 \"Grid Items\" makes a grid item. "
               "BUILD the pseudo-element as an item of the track sizing",
               box_subject(g->container, buf, sizeof buf));
    /* css-grid-1 §6 "Grid Items" states css-flexbox-1 §4's rule word for word — "Each in-flow child of a grid
       container becomes a grid item, and each child text sequence is wrapped in an anonymous block container
       grid item", a white-space-only sequence not rendered — and §9 takes an absolutely positioned child out of
       the grid's layout, as §4.1 does a flex container's, so §4's classification answers for a grid. */
    switch (flex_item_child_kind(g->container, g->cursor.node)) {
    case FLEX_ITEM_CHILD_NONE:
        break;
    case FLEX_ITEM_CHILD_TEXT:
        DFAILF("%s has a child text sequence, which css-grid-1 §6 wraps in an anonymous block container grid "
               "item. BUILD that item's contributions (core/layout/intrinsic_size.h's run sizes)",
               box_subject(g->container, buf, sizeof buf));
        break;
    case FLEX_ITEM_CHILD_ELEMENT: {
        lxb_dom_element_t *item = lxb_dom_interface_element(g->cursor.node);

        gt_require_auto_column_placement(item);
        /* The one step that can stop: the item's measurement under a resting drive. The cursor stays. */
        if (!intrinsic_inline_sizes_ask(item)) return false;
        gt_fold_item(g, item);
        break;
    }
    }
    g->cursor = box_tree_next_sibling(g->container, g->cursor);
    return true;
}

bool grid_track_sizing_done(const GridTrackSizing *g)
{
    DCHECK(g != NULL, "css-grid-1 §11.3 was asked about with no state");
    return g->sized;
}

CssPx grid_item_area_inline_size(lxb_dom_element_t *container, lxb_dom_element_t *item)
{
    LayoutJobDrive d;
    GridTrackSizing g;
    CssPx w;

    DCHECK(item != NULL && box_tree_parent(box_tree_child_of_node(lxb_dom_interface_node(item))) == container,
           "a grid area was asked for an element that is not a box-tree child of the grid container named");
    /* §8.5 in row flow with no explicit column places every item in the one implicit column, so the area's
       inline size is that column's used size. The reader is a drive over the steps. */
    layout_job_drive_begin(&d);
    grid_track_sizing_begin(&g, container);
    while (!grid_track_sizing_done(&g))
        if (!grid_track_sizing_step(&g)) (void)block_flow_drive_deferred(&d);
    w = g.base;
    layout_job_drive_end(&d);
    return w;
}
