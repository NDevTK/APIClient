/* css-grid-1 §11.3 "Track Sizing Algorithm" over a grid container's columns and rows. See grid_track.h. */
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

/* The container's side for its rows. */
static void gt_require_implicit_rows(lxb_dom_element_t *c)
{
    static const char EXPLICIT[] =
        "css-grid-1 §7.2 \"Explicit Track Sizing: the grid-template-rows and grid-template-columns properties\"' "
        "track list as this container's explicit rows";

    gt_require_initial(c, "grid-template-rows", "none", EXPLICIT);
    gt_require_initial(c, "grid-auto-rows", "auto",
                       "css-grid-1 §7.6 \"Implicit Track Sizing\"' sizing functions for the implicit rows");
    gt_require_initial(c, "row-gap", "normal",
                       "CSS Gaps 1 §2.1 \"Row and Column Gutters: the row-gap and column-gap properties\" as "
                       "fixed-size gutter tracks between the rows");
    gt_require_initial(c, "grid-row-gap", "normal",
                       "CSS Gaps 1 §2.4 \"Legacy Gap Properties: the grid-row-gap, grid-column-gap, and "
                       "grid-gap properties\" as the row gutters");
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

/* And in the row axis, where a line would leave rows without items or put two items in one row. */
static void gt_require_auto_row_placement(lxb_dom_element_t *item)
{
    static const char PLACEMENT[] =
        "css-grid-1 §8.3 \"Line-based Placement\" and §8.5 \"Grid Item Placement Algorithm\" for an item with "
        "a definite row position";

    gt_require_initial(item, "grid-row-start", "auto", PLACEMENT);
    gt_require_initial(item, "grid-row-end", "auto", PLACEMENT);
    gt_require_initial(item, "grid-row", "auto", PLACEMENT);
}

/* The grid item box-tree child `c` of `container` is, or NULL for a child that is none. css-grid-1 §6 "Grid
   Items" states css-flexbox-1 §4's rule word for word — "Each in-flow child of a grid container becomes a grid
   item, and each child text sequence is wrapped in an anonymous block container grid item", a white-space-only
   sequence not rendered — and §9 takes an absolutely positioned child out of the grid's layout, as §4.1 does a
   flex container's, so §4's classification answers for a grid. */
static lxb_dom_element_t *gt_item_of(lxb_dom_element_t *container, BoxTreeChild c)
{
    char buf[160];

    if (box_tree_child_is_pseudo(c))
        DFAILF("%s has a pseudo-element child box, which css-grid-1 §6 \"Grid Items\" makes a grid item. "
               "BUILD the pseudo-element as an item of the track sizing",
               box_subject(container, buf, sizeof buf));
    switch (flex_item_child_kind(container, c.node)) {
    case FLEX_ITEM_CHILD_NONE:
        return NULL;
    case FLEX_ITEM_CHILD_TEXT:
        DFAILF("%s has a child text sequence, which css-grid-1 §6 wraps in an anonymous block container grid "
               "item. BUILD that item's contributions (core/layout/intrinsic_size.h's run sizes)",
               box_subject(container, buf, sizeof buf));
        return NULL;
    case FLEX_ITEM_CHILD_ELEMENT:
        break;
    }
    return lxb_dom_interface_element(c.node);
}

/* css-grid-1 §11.5's minimum contribution: "if the item's computed preferred size behaves as auto or depends
   on the size of its containing block in the relevant axis, its minimum contribution is the outer size that
   would result from assuming the item's used minimum size as its preferred size; else the item's minimum
   contribution is its min-content contribution". The used minimum of `min-width: auto` is §6.6 "Automatic
   Minimum Size of Grid Items"': 0 for a scroll container, else the content size suggestion (the min-content
   size, "clamped by the maximum size in the affected axis, if it's definite"), since the one column has an
   `auto` minimum and is not flexible. */
static CssPx gt_content_size_suggestion(lxb_dom_element_t *item)
{
    CssPx m, mx;
    char buf[160];

    if (replaced_element_of(item).replaced)
        DFAILF("%s is a replaced grid item with an automatic minimum width. css-grid-1 §6.6's content-based "
               "minimum size takes its transferred size suggestion. BUILD that suggestion",
               box_subject(item, buf, sizeof buf));
    m = intrinsic_inline_sizes(item).min_content;
    if (intrinsic_declared_sizing_px(item, INTRINSIC_AXIS_HORIZONTAL, "max-width", CSS_KEYWORD_NONE, &mx))
        m = css_px_min(m, mx);
    return m;
}

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
    if (!intrinsic_declared_sizing_px(item, INTRINSIC_AXIS_HORIZONTAL, "min-width", CSS_KEYWORD_AUTO, &m))
        m = scroll_container_is(item) ? css_px(0.0) : gt_content_size_suggestion(item);
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
    lxb_dom_element_t *item;

    DCHECK(g != NULL && g->container != NULL && !g->sized, "css-grid-1 §11.3 was stepped after it ended");
    if (g->items_done) {
        gt_size(g);
        return true;
    }
    if (!box_tree_child_exists(g->cursor)) {
        g->items_done = true;
        return true;
    }
    item = gt_item_of(g->container, g->cursor);
    if (item != NULL) {
        gt_require_auto_column_placement(item);
        /* The one step that can stop: the item's measurement under a resting drive. The cursor stays. */
        if (!intrinsic_inline_sizes_ask(item)) return false;
        gt_fold_item(g, item);
    }
    g->cursor = box_tree_next_sibling(g->container, g->cursor);
    return true;
}

bool grid_track_sizing_done(const GridTrackSizing *g)
{
    DCHECK(g != NULL, "css-grid-1 §11.3 was asked about with no state");
    return g->sized;
}

bool grid_track_columns_ask(lxb_dom_element_t *container)
{
    GridTrackSizing g;

    grid_track_sizing_begin(&g, container);
    while (!grid_track_sizing_done(&g))
        if (!grid_track_sizing_step(&g)) return false;
    return true;
}

CssPx grid_item_content_size_suggestion_inline(lxb_dom_element_t *container, lxb_dom_element_t *item)
{
    DCHECK(item != NULL && box_tree_parent(box_tree_child_of_node(lxb_dom_interface_node(item))) == container,
           "a content size suggestion was asked for an element that is not a box-tree child of the grid "
           "container named");
    DCHECK(!scroll_container_is(item), "css-grid-1 §6.6 gives a scroll container no content-based minimum");
    gt_require_implicit_columns(container);
    gt_require_auto_column_placement(item);
    return gt_content_size_suggestion(item);
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

/* ---- the rows ------------------------------------------------------------------------------------------- */

/* One item's contributions to its `auto` row (§11.5), which are one outer size here. The item is a block
   container whose min-content and max-content block sizes are both its automatic block size (css-sizing-3
   §5.1 "Intrinsic Sizes"), measured at its used column width, and §6.6's content size suggestion is that same
   size, so the minimum contribution equals them. A percentage among the item's block-axis sizes is cyclic
   here (css-sizing-3 §5.2.1 "Intrinsic Contributions of Percentage-Sized Boxes") and refused by name. False
   is "not yet". */
static bool gt_row_contribution_ask(lxb_dom_element_t *item, CssPx *out)
{
    static const char *const SIZES[] = { "height", "min-height", "max-height" };
    CssPx content, unused;
    char buf[160];
    size_t i;

    for (i = 0; i < sizeof SIZES / sizeof SIZES[0]; i++) {
        CssLength l = css_computed_length(item, SIZES[i]);

        if (l.kind == CSS_LENGTH_PERCENTAGE || l.kind == CSS_LENGTH_CALCULATED)
            DFAILF("%s is a grid item with a percentage `%s`, which css-sizing-3 §5.2.1 makes cyclic in its row's "
                   "contribution. BUILD that section's substitution in the row sizing",
                   box_subject(item, buf, sizeof buf), SIZES[i]);
        if (l.kind == CSS_LENGTH_KEYWORD && l.keyword != CSS_KEYWORD_AUTO && l.keyword != CSS_KEYWORD_NONE)
            DFAILF("%s is a grid item with an intrinsic `%s` keyword, whose row contribution is not built. BUILD "
                   "css-sizing-3 §3.2's keyword in the row sizing",
                   box_subject(item, buf, sizeof buf), SIZES[i]);
    }
    if (css_computed_length_is(item, "margin-top", CSS_KEYWORD_AUTO) ||
        css_computed_length_is(item, "margin-bottom", CSS_KEYWORD_AUTO))
        DFAILF("%s is a grid item with an `auto` block-axis margin, which css-grid-1 §10.2 \"Aligning with auto "
               "margins\" resolves over its grid area. BUILD that alignment",
               box_subject(item, buf, sizeof buf));
    if (replaced_element_of(item).replaced)
        DFAILF("%s is a replaced grid item, whose row contribution is css-sizing-3 §5.1's natural size. BUILD it",
               box_subject(item, buf, sizeof buf));
    /* The one ask that can stop: the item's own content-based block size, held once answered. */
    if (used_value_height_behaves_as_auto(item) && !block_flow_auto_height_ask(item, &unused)) return false;
    content = used_value_block_level_content_px(item, true);
    *out = css_px_add(used_value_border_edge_from_content_px(item, content, true),
                      css_px_add(used_value_px(item, "margin-top"), used_value_px(item, "margin-bottom")));
    return true;
}

void grid_row_sizing_begin(GridRowSizing *g, lxb_dom_element_t *container, lxb_dom_element_t *target)
{
    char buf[160];

    DCHECK(g != NULL && container != NULL, "css-grid-1 §11.3's rows were begun with no state or no container");
    DCHECKF(css_computed_display_is(container, CSS_KEYWORD_GRID) ||
                css_computed_display_is(container, CSS_KEYWORD_INLINE_GRID),
            "css-grid-1 §11.3's rows were begun over %s, which generates no grid container box",
            box_subject(container, buf, sizeof buf));
    DCHECK(target == NULL ||
               box_tree_parent(box_tree_child_of_node(lxb_dom_interface_node(target))) == container,
           "a grid row was asked for an element that is not a box-tree child of the grid container named");
    /* One item per row holds only while every item shares the one column. */
    gt_require_implicit_columns(container);
    gt_require_implicit_rows(container);
    g->container = container;
    g->target = target;
    g->cursor = box_tree_first_child(container);
    g->items_done = g->sized = g->found = false;
    g->rows = g->target_index = 0;
    g->sum = g->before = g->target_base = g->share = css_px(0.0);
    /* §11.2's available grid space: "If the grid container's size is definite, then use the size of its content
       box." Otherwise the free space is indefinite, and §11.8's own fallback for that case — "if the grid
       container has a definite min-width/height, use that size" — is refused by name where it would apply. */
    g->definite = !used_value_height_behaves_as_auto(container);
    g->avail = g->definite ? used_value_content_px(container, true) : css_px(0.0);
    if (!g->definite) {
        CssLength m = css_computed_length(container, "min-height");

        if (!(css_length_is_keyword(m, CSS_KEYWORD_AUTO) || (m.kind == CSS_LENGTH_ABSOLUTE && m.px.px == 0.0)))
            DFAILF("%s is a grid container with an indefinite height and a `min-height`, which css-grid-1 §11.8 "
                   "\"Stretch auto Tracks\" uses for the free space. BUILD that free space",
                   box_subject(container, buf, sizeof buf));
    }
}

/* §11.4 through §11.8 over the rows. A row's base size is its one item's contribution, floored at zero, and so
   is its growth limit (§11.5: "If the track's growth limit is now less than its base size, increase the growth
   limit to match"), so §11.6 "Maximize Tracks" grows nothing and there is no flexible track for §11.7. §11.8:
   "When the content-distribution property of the grid container is normal or stretch in this axis, this step
   expands tracks that have an auto max track sizing function by dividing any remaining positive, definite free
   space equally amongst them." */
static void gt_rows_size(GridRowSizing *g)
{
    CssKeyword ac = css_computed_align(g->container, "align-content").value;

    if (g->definite && g->rows > 0 && (ac == CSS_KEYWORD_NORMAL || ac == CSS_KEYWORD_STRETCH))
        g->share = css_px_scale(css_px_max(css_px_sub(g->avail, g->sum), css_px(0.0)), 1.0 / (double)g->rows);
    DCHECK(g->target == NULL || g->found,
           "a grid row was sized for an item the walk over the container's grid items never reached");
    g->sized = true;
}

bool grid_row_sizing_step(GridRowSizing *g)
{
    lxb_dom_element_t *item;
    CssPx c;

    DCHECK(g != NULL && g->container != NULL && !g->sized, "css-grid-1 §11.3's rows were stepped after they ended");
    if (g->items_done) {
        gt_rows_size(g);
        return true;
    }
    if (!box_tree_child_exists(g->cursor)) {
        g->items_done = true;
        return true;
    }
    item = gt_item_of(g->container, g->cursor);
    if (item != NULL) {
        gt_require_auto_column_placement(item);
        gt_require_auto_row_placement(item);
        /* The one step that can stop. The cursor stays. */
        if (!gt_row_contribution_ask(item, &c)) return false;
        c = css_px_max(c, css_px(0.0));
        if (item == g->target) {
            g->found = true;
            g->target_index = g->rows;
            g->before = g->sum;
            g->target_base = c;
        }
        g->sum = css_px_add(g->sum, c);
        g->rows++;
    }
    g->cursor = box_tree_next_sibling(g->container, g->cursor);
    return true;
}

bool grid_row_sizing_done(const GridRowSizing *g)
{
    DCHECK(g != NULL, "css-grid-1 §11.3's rows were asked about with no state");
    return g->sized;
}

CssPx grid_item_area_block_size(lxb_dom_element_t *container, lxb_dom_element_t *item)
{
    LayoutJobDrive d;
    GridRowSizing g;
    CssPx h;

    DCHECK(item != NULL, "a grid area's block size was asked for no item");
    layout_job_drive_begin(&d);
    grid_row_sizing_begin(&g, container, item);
    while (!grid_row_sizing_done(&g))
        if (!grid_row_sizing_step(&g)) (void)block_flow_drive_deferred(&d);
    h = css_px_add(g.target_base, g.share);
    layout_job_drive_end(&d);
    return h;
}

/* A content alignment moves a track only where free space is left over (css-grid-1 §10.5 "Aligning the
   Grid"): `normal` and `stretch` leave none in an axis whose `auto` tracks §11.8 stretched, and `start` moves
   nothing. */
static void gt_require_unmoved_tracks(lxb_dom_element_t *container, const char *name, CssPx leftover)
{
    CssKeyword k = css_computed_align(container, name).value;
    char buf[160];

    if (leftover.px > 0.0 && k != CSS_KEYWORD_NORMAL && k != CSS_KEYWORD_STRETCH && k != CSS_KEYWORD_START)
        DFAILF("%s is a grid container whose `%s` distributes %g px of free space around its tracks. BUILD "
               "css-grid-1 §10.5's track offsets",
               box_subject(container, buf, sizeof buf), name, leftover.px);
}

bool grid_item_area_origin_ask(lxb_dom_element_t *container, lxb_dom_element_t *item, CssPx *x, CssPx *y)
{
    GridTrackSizing c;
    GridRowSizing r;

    DCHECK(item != NULL && x != NULL && y != NULL, "a grid area's origin was asked for with nowhere to put it");
    grid_track_sizing_begin(&c, container);
    while (!grid_track_sizing_done(&c))
        if (!grid_track_sizing_step(&c)) return false;
    gt_require_unmoved_tracks(container, "justify-content", css_px_sub(c.avail, c.base));
    grid_row_sizing_begin(&r, container, item);
    while (!grid_row_sizing_done(&r))
        if (!grid_row_sizing_step(&r)) return false;
    if (r.definite)
        gt_require_unmoved_tracks(container, "align-content",
                                  css_px_sub(r.avail, css_px_add(r.sum, css_px_scale(r.share, (double)r.rows))));
    /* §8.5 puts every item in the one column, whose start line is the content box's start edge, and in the row
       of its own order, which starts after the rows ahead of it. */
    *x = css_px(0.0);
    *y = css_px_add(r.before, css_px_scale(r.share, (double)r.target_index));
    return true;
}

bool grid_container_max_content_block_size_ask(lxb_dom_element_t *container, CssPx *out)
{
    GridRowSizing g;

    DCHECK(out != NULL, "a grid container's max-content block size was asked for with nowhere to put it");
    grid_row_sizing_begin(&g, container, NULL);
    while (!grid_row_sizing_done(&g))
        if (!grid_row_sizing_step(&g)) return false;
    /* Under a max-content constraint §11.6's free space is infinite and §11.8 has none to divide, so each row
       is its base size: §5.2's sum is of those. */
    *out = g.sum;
    return true;
}
