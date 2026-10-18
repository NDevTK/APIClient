/* css-grid-1 §11.3 "Track Sizing Algorithm" over a grid container's columns and rows. See grid_track.h. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "core/css/css_computed_value.h"
#include "core/css/css_display.h"
#include "core/css/css_style_declaration.h"
#include "core/layout/block_flow.h"
#include "core/layout/box_subject.h"
#include "core/layout/grid_placement.h"
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

/* The container's side of the residual in grid_track.h: every property that would change its columns other
   than their explicit track list. */
static void gt_require_supported_columns(lxb_dom_element_t *c)
{
    static const char SHORTHAND[] =
        "the css-grid-1 §7.4 \"Explicit Grid Shorthand: the grid-template property\" and §7.8 \"Grid Definition "
        "Shorthand: the grid property\" expansions into the track lists";

    gt_require_initial(c, "grid-template-areas", "none",
                       "css-grid-1 §7.3 \"Named Areas: the grid-template-areas property\", whose areas count "
                       "explicit tracks");
    gt_require_initial(c, "grid-template", "none", SHORTHAND);
    gt_require_initial(c, "grid", "none", SHORTHAND);
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
static void gt_require_supported_rows(lxb_dom_element_t *c)
{
    gt_require_initial(c, "grid-auto-rows", "auto",
                       "css-grid-1 §7.6 \"Implicit Track Sizing\"' sizing functions for the implicit rows");
    gt_require_initial(c, "row-gap", "normal",
                       "CSS Gaps 1 §2.1 \"Row and Column Gutters: the row-gap and column-gap properties\" as "
                       "fixed-size gutter tracks between the rows");
    gt_require_initial(c, "grid-row-gap", "normal",
                       "CSS Gaps 1 §2.4 \"Legacy Gap Properties: the grid-row-gap, grid-column-gap, and "
                       "grid-gap properties\" as the row gutters");
}

/* `item`'s number among `container`'s grid items, in order-modified document order. */
static size_t gt_item_index(lxb_dom_element_t *container, lxb_dom_element_t *item)
{
    BoxTreeChild c;
    size_t k = 0;

    for (c = box_tree_first_child(container); box_tree_child_exists(c); c = box_tree_next_sibling(container, c)) {
        lxb_dom_element_t *it = grid_item_of(container, c);

        if (it == item) return k;
        if (it != NULL) k++;
    }
    DFAIL("a grid item's placement was asked for an element the container's grid items do not include");
    return 0;
}

/* css-grid-1 §11.5's minimum contribution: "if the item's computed preferred size behaves as auto or depends
   on the size of its containing block in the relevant axis, its minimum contribution is the outer size that
   would result from assuming the item's used minimum size as its preferred size; else the item's minimum
   contribution is its min-content contribution". The used minimum of `min-width: auto` is §6.6 "Automatic
   Minimum Size of Grid Items"': 0 for a scroll container, else the content size suggestion (the min-content
   size, "clamped by the maximum size in the affected axis, if it's definite"). Only an item in a track with an
   `auto` minimum is asked, which is §6.6's second condition. */
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

/* One item's contributions to its row (§11.5), which are one outer size here. The item is a block container
   whose min-content and max-content block sizes are both its automatic block size (css-sizing-3 §5.1
   "Intrinsic Sizes"), measured at its used column width, and §6.6's content size suggestion is that same
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

/* ---- one axis ------------------------------------------------------------------------------------------- */

static GridTrack *gt_push(GridAxisSizing *g, CssGridTrackKind kind, double flex, CssPx fixed)
{
    GridTrack *t;

    if (g->ntracks == g->capacity) {
        size_t c = g->capacity ? g->capacity * 2 : 4;
        GridTrack *grown = realloc(g->tracks, c * sizeof *grown);

        CHECK(grown != NULL, "css-grid-1 §11: out of memory holding a grid axis's tracks");
        g->tracks = grown;
        g->capacity = c;
    }
    t = &g->tracks[g->ntracks++];
    t->kind = kind;
    t->flex = flex;
    t->fixed = fixed;
    t->flex_maxc = css_px(0.0);
    t->has_items = false;
    /* §11.4 "Initialize Track Sizes": a fixed sizing function is both the base size and the growth limit; an
       intrinsic min (`auto`, and a bare `<flex>`'s implied `auto`) gives a base size of zero, and an intrinsic
       or flexible max a growth limit of infinity. */
    t->base = kind == CSS_GRID_TRACK_BREADTH ? fixed : css_px(0.0);
    t->growth = t->base;
    t->growth_inf = kind != CSS_GRID_TRACK_BREADTH;
    return t;
}

/* The number of explicit tracks on one axis of `container`. */
static size_t gt_explicit_count(lxb_dom_element_t *container, bool vertical)
{
    CssGridTrackList list;
    size_t n = 0;

    if (css_computed_grid_track_list(container, vertical ? "grid-template-rows" : "grid-template-columns", &list))
        n = list.n;
    css_grid_track_list_free(&list);
    return n;
}

/* The axis's tracks: the explicit ones, then the implicit ones §8.5's placement needs, which `grid-auto-rows`
   and `grid-auto-columns` size, both at their initial `auto` (§7.6). A percentage resolves against the
   available grid space; with none (an indefinite size, or a min-/max-content constraint) "the <percentage> must
   be treated as auto" (§7.2.1 "Track Sizes"). */
static void gt_tracks_init(GridAxisSizing *g)
{
    CssGridTrackList list;
    size_t i, total = g->vertical ? g->placement.rows : g->placement.columns;

    (void)css_computed_grid_track_list(g->container, g->vertical ? "grid-template-rows" : "grid-template-columns",
                                       &list);
    for (i = 0; i < list.n; i++) {
        const CssGridTrackSize *s = &list.tracks[i];

        if (s->kind == CSS_GRID_TRACK_FLEX) {
            gt_push(g, CSS_GRID_TRACK_FLEX, s->flex, css_px(0.0));
        } else if (s->kind == CSS_GRID_TRACK_BREADTH && s->length.kind == CSS_LENGTH_ABSOLUTE) {
            gt_push(g, CSS_GRID_TRACK_BREADTH, 0.0, s->length.px);
        } else if (s->kind == CSS_GRID_TRACK_BREADTH && g->constraint == GRID_SIZING_AVAILABLE) {
            gt_push(g, CSS_GRID_TRACK_BREADTH, 0.0,
                    css_px_max(css_length_resolve_pct(s->length, g->avail), css_px(0.0)));
        } else {
            gt_push(g, CSS_GRID_TRACK_AUTO, 0.0, css_px(0.0));
        }
    }
    css_grid_track_list_free(&list);
    while (g->ntracks < total) gt_push(g, CSS_GRID_TRACK_AUTO, 0.0, css_px(0.0));
}

void grid_axis_sizing_begin(GridAxisSizing *g, lxb_dom_element_t *container, lxb_dom_element_t *target,
                            bool vertical, GridSizingConstraint constraint)
{
    char buf[160];

    DCHECK(g != NULL && container != NULL, "css-grid-1 §11.3 was begun with no state or no grid container");
    DCHECKF(css_computed_display_is(container, CSS_KEYWORD_GRID) ||
                css_computed_display_is(container, CSS_KEYWORD_INLINE_GRID),
            "css-grid-1 §11.3 was begun over %s, which generates no grid container box",
            box_subject(container, buf, sizeof buf));
    DCHECK(target == NULL ||
               box_tree_parent(box_tree_child_of_node(lxb_dom_interface_node(target))) == container,
           "a grid track was asked for an element that is not a box-tree child of the grid container named");
    gt_require_supported_columns(container);
    gt_require_supported_rows(container);
    g->container = container;
    g->target = target;
    g->vertical = vertical;
    g->constraint = constraint;
    g->cursor = box_tree_first_child(container);
    g->items_done = g->sized = g->found = false;
    g->items = g->target_index = 0;
    g->tracks = NULL;
    g->ntracks = g->capacity = 0;
    g->avail = css_px(0.0);
    grid_placement_run(container, gt_explicit_count(container, false), gt_explicit_count(container, true),
                       &g->placement);
    /* §11.2 "Track Sizing Terminology"'s available grid space: "If the grid container's size is definite, then
       use the size of its content box." A container whose size depends on its tracks is sized under the
       max-content constraint its §5.2 auto size is defined by. */
    if (constraint == GRID_SIZING_AVAILABLE) {
        if (vertical && used_value_height_behaves_as_auto(container)) {
            CssLength m = css_computed_length(container, "min-height");

            g->constraint = GRID_SIZING_MAX_CONTENT;
            if (!(css_length_is_keyword(m, CSS_KEYWORD_AUTO) || (m.kind == CSS_LENGTH_ABSOLUTE && m.px.px == 0.0)))
                DFAILF("%s is a grid container with an indefinite height and a `min-height`, which css-grid-1 "
                       "§11.7 and §11.8 use for the free space. BUILD that free space",
                       box_subject(container, buf, sizeof buf));
        } else {
            g->avail = used_value_content_px(container, vertical);
        }
    }
    gt_tracks_init(g);
}

/* One item's contributions to the track it is in. Columns: §11.5's minimum contribution (in a min-/max-content
   constraint "limited min-content contribution", which for a track with no fixed max is the min-content
   contribution) and the max-content contribution. Rows: the one outer size. False is "not yet". */
static bool gt_contributions_ask(const GridAxisSizing *g, lxb_dom_element_t *item, CssPx *minc, CssPx *maxc)
{
    IntrinsicInlineSizes c;

    if (g->vertical) {
        if (!gt_row_contribution_ask(item, minc)) return false;
        *maxc = *minc;
        return true;
    }
    if (!intrinsic_inline_sizes_ask(item)) return false;
    c = intrinsic_inline_contributions(item);
    *minc = g->constraint == GRID_SIZING_AVAILABLE ? gt_minimum_contribution(item, c) : c.min_content;
    *maxc = c.max_content;
    return true;
}

/* §11.5 for one span-1 item in a track with an `auto` minimum: "set its base size to the maximum of its items'
   minimum contributions, floored at zero". An `auto` maximum's growth limit is "the maximum of the items'
   max-content contributions" ("auto max track sizing functions are treated the same as max-content"). A
   flexible track's base size takes the same minimum through §11.5's step for "items crossing flexible tracks"
   (distributing only to that track), and its items' max-content contributions are kept for §11.7. A fixed
   track takes nothing from its items. */
static void gt_fold(GridTrack *t, CssPx minc, CssPx maxc)
{
    t->base = css_px_max(t->base, css_px_max(minc, css_px(0.0)));
    if (t->kind == CSS_GRID_TRACK_AUTO) {
        t->growth = t->has_items ? css_px_max(t->growth, maxc) : maxc;
        t->growth_inf = false;
    } else {
        t->flex_maxc = t->has_items ? css_px_max(t->flex_maxc, maxc) : maxc;
    }
    t->has_items = true;
}

static CssPx gt_sum(const GridAxisSizing *g)
{
    CssPx s = css_px(0.0);
    size_t i;

    for (i = 0; i < g->ntracks; i++) s = css_px_add(s, g->tracks[i].base);
    return s;
}

/* §11.7.1 "Find the Size of an fr" over the tracks `[lo, hi)` and a space to fill. */
static CssPx gt_find_fr(const GridAxisSizing *g, size_t lo, size_t hi, CssPx space)
{
    bool *inflexible = calloc(hi - lo, sizeof *inflexible);
    CssPx hyp = css_px(0.0);
    bool restart = true;
    size_t i;

    CHECK(inflexible != NULL, "css-grid-1 §11.7.1: out of memory marking inflexible tracks");
    while (restart) {
        CssPx leftover = space;
        double sum = 0.0;

        /* "Let leftover space be the space to fill minus the base sizes of the non-flexible grid tracks. Let
           flex factor sum be the sum of the flex factors of the flexible tracks. If this value is less than 1,
           set it to 1 instead. Let the hypothetical fr size be the leftover space divided by the flex factor
           sum." */
        for (i = lo; i < hi; i++) {
            if (g->tracks[i].kind != CSS_GRID_TRACK_FLEX || inflexible[i - lo])
                leftover = css_px_sub(leftover, g->tracks[i].base);
            else
                sum += g->tracks[i].flex;
        }
        hyp = css_px_scale(leftover, 1.0 / (sum < 1.0 ? 1.0 : sum));
        /* "If the product of the hypothetical fr size and a flexible track's flex factor is less than the track's
           base size, restart this algorithm treating all such tracks as inflexible." */
        restart = false;
        for (i = lo; i < hi; i++)
            if (g->tracks[i].kind == CSS_GRID_TRACK_FLEX && !inflexible[i - lo] &&
                hyp.px * g->tracks[i].flex < g->tracks[i].base.px) {
                inflexible[i - lo] = true;
                restart = true;
            }
    }
    free(inflexible);
    return hyp;
}

/* §11.5's last sentence through §11.8, once every item is folded. */
static void gt_size(GridAxisSizing *g)
{
    CssPx free_space, fraction = css_px(0.0);
    bool any_flex = false;
    size_t i, nauto = 0;
    CssKeyword content;

    for (i = 0; i < g->ntracks; i++) {
        GridTrack *t = &g->tracks[i];

        /* "If any track still has an infinite growth limit (because, for example, it had no items placed in it
           or it is a flexible track), set its growth limit to its base size." */
        if (t->growth_inf) { t->growth = t->base; t->growth_inf = false; }
        t->growth = css_px_max(t->growth, t->base);
        if (t->kind == CSS_GRID_TRACK_FLEX) any_flex = true;
        if (t->kind == CSS_GRID_TRACK_AUTO) nauto++;
    }
    /* §11.6 "Maximize Tracks": "If the free space is positive, distribute it equally to the base sizes of all
       tracks, freezing tracks as they reach their growth limits (and continuing to grow the unfrozen tracks as
       needed). For the purpose of this step: if sizing the grid container under a max-content constraint, the
       free space is infinite; if sizing under a min-content constraint, the free space is zero." */
    if (g->constraint == GRID_SIZING_MAX_CONTENT) {
        for (i = 0; i < g->ntracks; i++) g->tracks[i].base = g->tracks[i].growth;
    } else if (g->constraint == GRID_SIZING_AVAILABLE) {
        bool froze = true;

        free_space = css_px_sub(g->avail, gt_sum(g));
        while (froze && free_space.px > 0.0) {
            size_t unfrozen = 0;
            CssPx share;

            for (i = 0; i < g->ntracks; i++)
                if (g->tracks[i].base.px < g->tracks[i].growth.px) unfrozen++;
            if (unfrozen == 0) break;
            share = css_px_scale(free_space, 1.0 / (double)unfrozen);
            froze = false;
            for (i = 0; i < g->ntracks; i++) {
                GridTrack *t = &g->tracks[i];
                CssPx room = css_px_sub(t->growth, t->base), inc;

                if (room.px <= 0.0) continue;
                inc = css_px_min(share, room);
                if (inc.px >= room.px) froze = true;
                t->base = css_px_add(t->base, inc);
                free_space = css_px_sub(free_space, inc);
            }
        }
    }
    /* §11.7 "Expand Flexible Tracks". */
    if (any_flex && g->constraint != GRID_SIZING_MIN_CONTENT) {
        if (g->constraint == GRID_SIZING_AVAILABLE) {
            /* "If the free space is zero …: The used flex fraction is zero. Otherwise, if the free space is a
               definite length: The used flex fraction is the result of finding the size of an fr using all of
               the grid tracks and a space to fill of the available grid space." */
            if (css_px_sub(g->avail, gt_sum(g)).px > 0.0) fraction = gt_find_fr(g, 0, g->ntracks, g->avail);
        } else {
            /* "Otherwise, if the free space is an indefinite length: The used flex fraction is the maximum of:
               For each flexible track, if the flexible track's flex factor is greater than one, the result of
               dividing the track's base size by its flex factor; otherwise, the track's base size. For each grid
               item that crosses a flexible track, the result of finding the size of an fr using all the grid
               tracks that the item crosses and a space to fill of the item's max-content contribution." Every
               item spans one track, and the largest contribution in a track gives that track's largest result. */
            for (i = 0; i < g->ntracks; i++) {
                const GridTrack *t = &g->tracks[i];

                if (t->kind != CSS_GRID_TRACK_FLEX) continue;
                fraction = css_px_max(fraction, t->flex > 1.0 ? css_px_scale(t->base, 1.0 / t->flex) : t->base);
                if (t->has_items) fraction = css_px_max(fraction, gt_find_fr(g, i, i + 1, t->flex_maxc));
            }
        }
        /* "For each flexible track, if the product of the used flex fraction and the track's flex factor is
           greater than the track's base size, set its base size to that product." */
        for (i = 0; i < g->ntracks; i++) {
            GridTrack *t = &g->tracks[i];

            if (t->kind == CSS_GRID_TRACK_FLEX && fraction.px * t->flex > t->base.px)
                t->base = css_px_scale(fraction, t->flex);
        }
    }
    /* §11.8 "Stretch auto Tracks": "When the content-distribution property of the grid container is normal or
       stretch in this axis, this step expands tracks that have an auto max track sizing function by dividing any
       remaining positive, definite free space equally amongst them." */
    content = css_computed_align(g->container, g->vertical ? "align-content" : "justify-content").value;
    if (g->constraint == GRID_SIZING_AVAILABLE && nauto > 0 &&
        (content == CSS_KEYWORD_NORMAL || content == CSS_KEYWORD_STRETCH)) {
        free_space = css_px_sub(g->avail, gt_sum(g));
        if (free_space.px > 0.0) {
            CssPx share = css_px_scale(free_space, 1.0 / (double)nauto);

            for (i = 0; i < g->ntracks; i++)
                if (g->tracks[i].kind == CSS_GRID_TRACK_AUTO) g->tracks[i].base = css_px_add(g->tracks[i].base, share);
        }
    }
    /* §11.6 and §11.7 redo their step against the container's inner size "as limited by its max-width/height"
       when the tracks would exceed it, which an indefinite height with a `max-height` can. */
    if (g->vertical && g->constraint == GRID_SIZING_MAX_CONTENT &&
        !css_computed_length_is(g->container, "max-height", CSS_KEYWORD_NONE)) {
        CssLength m = css_computed_length(g->container, "max-height");
        char buf[160];

        if (m.kind != CSS_LENGTH_ABSOLUTE || css_computed_keyword_is(g->container, "box-sizing", CSS_KEYWORD_BORDER_BOX) ||
            gt_sum(g).px > m.px.px)
            DFAILF("%s is a grid container with an indefinite height whose rows may exceed its `max-height`, "
                   "against which css-grid-1 §11.6 and §11.7 redo their step. BUILD that redo",
                   box_subject(g->container, buf, sizeof buf));
    }
    DCHECK(g->target == NULL || g->found,
           "a grid track was sized for an item the walk over the container's grid items never reached");
    g->sized = true;
}

bool grid_axis_sizing_step(GridAxisSizing *g)
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
    item = grid_item_of(g->container, g->cursor);
    if (item != NULL) {
        GridCell cell;
        size_t t;

        DCHECK(g->items < g->placement.n, "a grid axis met more items than its placement placed");
        cell = g->placement.cells[g->items];
        t = g->vertical ? cell.row : cell.col;
        DCHECK(t < g->ntracks, "a grid item was placed in a track the axis does not have");
        if (g->tracks[t].kind != CSS_GRID_TRACK_BREADTH) {
            CssPx minc, maxc;

            /* The one step that can stop: the item's measurement under a resting drive. The cursor stays. */
            if (!gt_contributions_ask(g, item, &minc, &maxc)) return false;
            gt_fold(&g->tracks[t], minc, maxc);
        }
        if (item == g->target) {
            g->found = true;
            g->target_index = g->items;
        }
        g->items++;
    }
    g->cursor = box_tree_next_sibling(g->container, g->cursor);
    return true;
}

bool grid_axis_sizing_done(const GridAxisSizing *g)
{
    DCHECK(g != NULL, "css-grid-1 §11.3 was asked about with no state");
    return g->sized;
}

void grid_axis_sizing_end(GridAxisSizing *g)
{
    DCHECK(g != NULL, "css-grid-1 §11.3's tracks were released with no state");
    free(g->tracks);
    g->tracks = NULL;
    g->ntracks = g->capacity = 0;
    grid_placement_free(&g->placement);
}

/* ---- readers -------------------------------------------------------------------------------------------- */

/* Every step in this activation; false, with the tracks released, at the first "not yet". */
static bool gt_run_ask(GridAxisSizing *g, lxb_dom_element_t *container, lxb_dom_element_t *target, bool vertical,
                       GridSizingConstraint constraint)
{
    grid_axis_sizing_begin(g, container, target, vertical, constraint);
    while (!grid_axis_sizing_done(g))
        if (!grid_axis_sizing_step(g)) { grid_axis_sizing_end(g); return false; }
    return true;
}

/* Every step, driving deferred work between them. */
static void gt_run_drive(GridAxisSizing *g, lxb_dom_element_t *container, lxb_dom_element_t *target, bool vertical)
{
    LayoutJobDrive d;

    layout_job_drive_begin(&d);
    grid_axis_sizing_begin(g, container, target, vertical, GRID_SIZING_AVAILABLE);
    while (!grid_axis_sizing_done(g))
        if (!grid_axis_sizing_step(g)) (void)block_flow_drive_deferred(&d);
    layout_job_drive_end(&d);
}

/* The track the target is in, on the run's axis. */
static size_t gt_target_track(const GridAxisSizing *g)
{
    DCHECK(g->found, "a grid item's track was read from a run that never reached the item");
    return g->vertical ? g->placement.cells[g->target_index].row : g->placement.cells[g->target_index].col;
}

void grid_track_placement(lxb_dom_element_t *container, GridPlacement *out)
{
    DCHECK(container != NULL && out != NULL, "a grid's placement was asked for with no container or no out");
    grid_placement_run(container, gt_explicit_count(container, false), gt_explicit_count(container, true), out);
}

bool grid_track_columns_ask(lxb_dom_element_t *container)
{
    GridAxisSizing g;

    if (!gt_run_ask(&g, container, NULL, false, GRID_SIZING_AVAILABLE)) return false;
    grid_axis_sizing_end(&g);
    return true;
}

CssPx grid_item_area_inline_size(lxb_dom_element_t *container, lxb_dom_element_t *item)
{
    GridAxisSizing g;
    CssPx w;

    DCHECK(item != NULL, "a grid area's inline size was asked for no item");
    gt_run_drive(&g, container, item, false);
    w = g.tracks[gt_target_track(&g)].base;
    grid_axis_sizing_end(&g);
    return w;
}

CssPx grid_item_area_block_size(lxb_dom_element_t *container, lxb_dom_element_t *item)
{
    GridAxisSizing g;
    CssPx h;

    DCHECK(item != NULL, "a grid area's block size was asked for no item");
    gt_run_drive(&g, container, item, true);
    h = g.tracks[gt_target_track(&g)].base;
    grid_axis_sizing_end(&g);
    return h;
}

/* A content alignment moves a track only where free space is left over (css-grid-1 §10.5 "Aligning the
   Grid"): `normal` and `stretch` leave none in an axis whose `auto` tracks §11.8 stretched, and `start` moves
   nothing. */
static void gt_require_unmoved_tracks(const GridAxisSizing *g)
{
    const char *name = g->vertical ? "align-content" : "justify-content";
    CssKeyword k = css_computed_align(g->container, name).value;
    CssPx leftover;
    char buf[160];

    if (g->constraint != GRID_SIZING_AVAILABLE) return;
    leftover = css_px_sub(g->avail, gt_sum(g));
    if (leftover.px > 0.0 && k != CSS_KEYWORD_START && k != CSS_KEYWORD_NORMAL && k != CSS_KEYWORD_STRETCH)
        DFAILF("%s is a grid container whose `%s` distributes %g px of free space around its tracks. BUILD "
               "css-grid-1 §10.5's track offsets",
               box_subject(g->container, buf, sizeof buf), name, leftover.px);
}

/* The target's track's start: the sizes of the tracks ahead of it. */
static CssPx gt_target_offset(const GridAxisSizing *g)
{
    CssPx o = css_px(0.0);
    size_t i, t = gt_target_track(g);

    for (i = 0; i < t; i++) o = css_px_add(o, g->tracks[i].base);
    return o;
}

bool grid_item_area_origin_ask(lxb_dom_element_t *container, lxb_dom_element_t *item, CssPx *x, CssPx *y)
{
    GridAxisSizing c, r;

    DCHECK(item != NULL && x != NULL && y != NULL, "a grid area's origin was asked for with nowhere to put it");
    if (!gt_run_ask(&c, container, item, false, GRID_SIZING_AVAILABLE)) return false;
    gt_require_unmoved_tracks(&c);
    *x = gt_target_offset(&c);
    grid_axis_sizing_end(&c);
    if (!gt_run_ask(&r, container, item, true, GRID_SIZING_AVAILABLE)) return false;
    gt_require_unmoved_tracks(&r);
    *y = gt_target_offset(&r);
    grid_axis_sizing_end(&r);
    return true;
}

bool grid_container_max_content_block_size_ask(lxb_dom_element_t *container, CssPx *out)
{
    GridAxisSizing g;

    DCHECK(out != NULL, "a grid container's max-content block size was asked for with nowhere to put it");
    if (!gt_run_ask(&g, container, NULL, true, GRID_SIZING_MAX_CONTENT)) return false;
    *out = gt_sum(&g);
    grid_axis_sizing_end(&g);
    return true;
}

bool grid_container_inline_sizes_ask(lxb_dom_element_t *container, CssPx *min_content, CssPx *max_content)
{
    GridAxisSizing g;

    DCHECK(min_content != NULL && max_content != NULL,
           "a grid container's intrinsic inline sizes were asked for with nowhere to put them");
    if (!gt_run_ask(&g, container, NULL, false, GRID_SIZING_MIN_CONTENT)) return false;
    *min_content = gt_sum(&g);
    grid_axis_sizing_end(&g);
    if (!gt_run_ask(&g, container, NULL, false, GRID_SIZING_MAX_CONTENT)) return false;
    *max_content = gt_sum(&g);
    grid_axis_sizing_end(&g);
    return true;
}

bool grid_item_track_min_is_auto(lxb_dom_element_t *container, lxb_dom_element_t *item, bool vertical)
{
    CssGridTrackList list;
    GridPlacement p;
    size_t k = gt_item_index(container, item), t;
    bool any, autoish;

    grid_placement_run(container, gt_explicit_count(container, false), gt_explicit_count(container, true), &p);
    DCHECK(k < p.n, "a grid item's track was asked for an item its container's placement did not place");
    t = vertical ? p.cells[k].row : p.cells[k].col;
    grid_placement_free(&p);
    any = css_computed_grid_track_list(container, vertical ? "grid-template-rows" : "grid-template-columns", &list);
    autoish = !any || t >= list.n || list.tracks[t].kind != CSS_GRID_TRACK_BREADTH;
    css_grid_track_list_free(&list);
    return autoish;
}

CssPx grid_item_content_size_suggestion_inline(lxb_dom_element_t *container, lxb_dom_element_t *item)
{
    DCHECK(item != NULL && box_tree_parent(box_tree_child_of_node(lxb_dom_interface_node(item))) == container,
           "a content size suggestion was asked for an element that is not a box-tree child of the grid "
           "container named");
    DCHECK(!scroll_container_is(item), "css-grid-1 §6.6 gives a scroll container no content-based minimum");
    gt_require_supported_columns(container);
    return gt_content_size_suggestion(item);
}
