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
#include "core/layout/box_tree.h"   /* the style ask placement makes of the items before it reads them */
#include "core/layout/grid_explicit_tracks.h"
#include "core/layout/grid_placement.h"
#include "core/layout/grid_track.h"
#include "core/layout/intrinsic_size.h"
#include "core/layout/layout_job.h"
#include "core/layout/replaced_element.h"
#include "core/layout/scroll_container.h"
#include "core/layout/used_value.h"
#include <lexbor/core/base.h>   /* lexbor_realloc: the allocator the engine gives its refusal edge */

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
    gt_require_initial(c, "grid-auto-columns", "auto",
                       "css-grid-1 §7.6 \"Implicit Track Sizing\"' sizing functions for the implicit columns");
    gt_require_initial(c, "grid-auto-flow", "row",
                       "css-grid-1 §8.5 \"Grid Item Placement Algorithm\" for column flow and `dense` packing");
}

/* The container's side for its rows. */
static void gt_require_supported_rows(lxb_dom_element_t *c)
{
    gt_require_initial(c, "grid-auto-rows", "auto",
                       "css-grid-1 §7.6 \"Implicit Track Sizing\"' sizing functions for the implicit rows");
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
   size, "clamped by the maximum size in the affected axis, if it's definite"). The suggestion is asked only
   where §6.6's track conditions hold (`gt_auto_minimum_applies`); elsewhere the automatic minimum is zero. */
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

static CssPx gt_minimum_contribution(lxb_dom_element_t *item, IntrinsicInlineSizes contributions,
                                     bool content_based)
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
        m = scroll_container_is(item) || !content_based ? css_px(0.0) : gt_content_size_suggestion(item);
    used.min_content = m;
    used.max_content = m;
    return intrinsic_outer_contribution(item, used).min_content;
}

/* One item's contributions to its row (§11.5), which are one outer size here. The item is a block container
   whose min-content and max-content block sizes are both its automatic block size (css-sizing-3 §5.1
   "Intrinsic Sizes"), measured at its used column width, and §6.6's content size suggestion is that same
   size, so the minimum contribution equals them. A percentage among the item's block-axis sizes is cyclic
   here, and css-sizing-3 §5.2.1 "Intrinsic Contributions of Percentage-Sized Boxes" treats a preferred or
   maximum one as its initial value and resolves a minimum one against zero: the row context this runs in
   gives the item's grid area no block size, so used_value.c's percentage arms answer both. A `calc()`
   minimum mixing a length is refused by name. False is "not yet". */
static bool gt_row_contribution_ask(lxb_dom_element_t *item, CssPx *out)
{
    static const char *const SIZES[] = { "height", "min-height", "max-height" };
    CssPx content, unused;
    char buf[160];
    size_t i;

    for (i = 0; i < sizeof SIZES / sizeof SIZES[0]; i++) {
        CssLength l = css_computed_length(item, SIZES[i]);

        if (l.kind == CSS_LENGTH_CALCULATED && strcmp(SIZES[i], "min-height") == 0)
            DFAILF("%s is a grid item with a `calc()` `min-height`, which css-sizing-3 §5.2.1 resolves against "
                   "zero in its row's contribution. BUILD that resolution for a math function",
                   box_subject(item, buf, sizeof buf));
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

/* The track kinds §11 branches on. A min is intrinsic when it is `auto`, `min-content` or `max-content`; a max
   when it is one of those or `fit-content()`, which "auto max track sizing functions are treated the same as
   max-content except where explicitly specified otherwise" makes a max-content max. */
static bool gt_min_intrinsic(const GridTrack *t)
{
    return t->min != CSS_GRID_BREADTH_LENGTH;
}

static bool gt_max_intrinsic(const GridTrack *t)
{
    return t->max != CSS_GRID_BREADTH_LENGTH && t->max != CSS_GRID_BREADTH_FLEX;
}

static bool gt_max_max_content(const GridTrack *t)
{
    return t->max == CSS_GRID_BREADTH_MAX_CONTENT || t->max == CSS_GRID_BREADTH_AUTO ||
           t->max == CSS_GRID_BREADTH_FIT_CONTENT;
}

static bool gt_flexible(const GridTrack *t)
{
    return t->max == CSS_GRID_BREADTH_FLEX;
}

/* A track no item sizes: both sizing functions fixed. */
static bool gt_fixed(const GridTrack *t)
{
    return t->min == CSS_GRID_BREADTH_LENGTH && t->max == CSS_GRID_BREADTH_LENGTH;
}

/* The fixed limit §11.5's limited contributions read: a fixed max, or "the argument to a fit-content() track
   sizing function". False where the max is neither. */
static bool gt_max_limit(const GridTrack *t, CssPx *out)
{
    if (t->max != CSS_GRID_BREADTH_LENGTH && t->max != CSS_GRID_BREADTH_FIT_CONTENT) return false;
    *out = t->max_fixed;
    return true;
}

/* One sizing function with its length resolved: an absolute length as itself, a percentage against the
   available grid space where there is one, and otherwise "the <percentage> must be treated as auto" (§7.2.1),
   which for a `fit-content()` argument leaves the max-content max it is the limit of. */
static void gt_breadth_resolve(const GridAxisSizing *g, const CssGridBreadth *b, CssGridBreadthKind *kind,
                               CssPx *px)
{
    *kind = b->kind;
    *px = css_px(0.0);
    if (b->kind != CSS_GRID_BREADTH_LENGTH && b->kind != CSS_GRID_BREADTH_FIT_CONTENT) return;
    if (b->length.kind == CSS_LENGTH_ABSOLUTE) {
        *px = b->length.px;
    } else if (g->constraint == GRID_SIZING_AVAILABLE) {
        *px = css_px_max(css_length_resolve_pct(b->length, g->avail), css_px(0.0));
    } else {
        *kind = b->kind == CSS_GRID_BREADTH_LENGTH ? CSS_GRID_BREADTH_AUTO : CSS_GRID_BREADTH_MAX_CONTENT;
    }
}

static GridTrack *gt_push(GridAxisSizing *g, const CssGridTrackSize *size)
{
    GridTrack *t;

    if (g->ntracks == g->capacity) {
        size_t c = g->capacity ? g->capacity * 2 : 4;
        GridTrack *grown = lexbor_realloc(g->tracks, c * sizeof *grown);

        CHECK(grown != NULL, "css-grid-1 §11: out of memory holding a grid axis's tracks");
        g->tracks = grown;
        g->capacity = c;
    }
    t = &g->tracks[g->ntracks++];
    gt_breadth_resolve(g, &size->min, &t->min, &t->min_fixed);
    gt_breadth_resolve(g, &size->max, &t->max, &t->max_fixed);
    DCHECK(t->min != CSS_GRID_BREADTH_FLEX && t->min != CSS_GRID_BREADTH_FIT_CONTENT,
           "a track's min sizing function is a <flex> or fit-content(), which §7.2's grammar admits only as a max");
    t->flex = size->max.flex;
    t->growable = false;
    t->collapsed = false;
    /* §11.4 "Initialize Track Sizes": a fixed min is the base size and an intrinsic one gives zero; a fixed max is
       the growth limit and an intrinsic or flexible one gives infinity; "in all cases, if the growth limit is
       less than the base size, increase the growth limit to match the base size". */
    t->base = t->min == CSS_GRID_BREADTH_LENGTH ? t->min_fixed : css_px(0.0);
    t->growth_inf = t->max != CSS_GRID_BREADTH_LENGTH;
    t->growth = t->growth_inf ? t->base : css_px_max(t->max_fixed, t->base);
    return t;
}

/* Whether any item's area on this axis covers track `t`: §7.2.3.2's "An empty track is one with no in-flow
   grid items placed into or spanning across it". */
static bool gt_track_occupied(const GridAxisSizing *g, size_t t)
{
    size_t i;

    for (i = 0; i < g->placement.n; i++) {
        const GridCell *c = &g->placement.cells[i];
        size_t start = g->vertical ? c->row : c->col, span = g->vertical ? c->rows : c->cols;

        if (t >= start && t < start + span) return true;
    }
    return false;
}

/* The axis's tracks: the explicit ones (core/layout/grid_explicit_tracks.h), then the implicit ones §8.5's
   placement needs, which `grid-auto-rows` and `grid-auto-columns` size, both at their initial `auto` (§7.6).
   "The auto-fit keyword behaves the same as auto-fill, except that after grid item placement any empty repeated
   tracks are collapsed", and "a collapsed grid track is treated as having a fixed track sizing function of 0px,
   and the gutters on either side of it ... collapse". */
static void gt_tracks_init(GridAxisSizing *g)
{
    GridExplicitTracks ex;
    CssGridTrackSize implicit, collapsed;
    size_t i, total = g->vertical ? g->placement.rows : g->placement.columns;

    (void)grid_explicit_tracks(g->container, g->vertical, g->constraint != GRID_SIZING_AVAILABLE, &ex);
    memset(&collapsed, 0, sizeof collapsed);
    collapsed.min.kind = collapsed.max.kind = CSS_GRID_BREADTH_LENGTH;
    collapsed.min.length.kind = collapsed.max.length.kind = CSS_LENGTH_ABSOLUTE;
    collapsed.min.length.px = collapsed.max.length.px = css_px(0.0);
    for (i = 0; i < ex.list.n; i++) {
        bool collapse = ex.auto_fit && i >= ex.repeat_lo && i < ex.repeat_hi && !gt_track_occupied(g, i);

        gt_push(g, collapse ? &collapsed : &ex.list.tracks[i])->collapsed = collapse;
    }
    grid_explicit_tracks_free(&ex);
    memset(&implicit, 0, sizeof implicit);
    implicit.min.kind = CSS_GRID_BREADTH_AUTO;
    implicit.max.kind = CSS_GRID_BREADTH_AUTO;
    while (g->ntracks < total) gt_push(g, &implicit);
}

/* css-grid-1 §10.1 "Gutters: the row-gap, column-gap, and gap properties": "each gutter is treated as an extra,
   empty, fixed-size track of the specified size, which is spanned by any grid items that span across its
   corresponding grid line", and `normal` is "a used value of 0px" (CSS Gaps 1 §2.1). A percentage resolves
   against the content box (§2.3 "Percentages In gap Properties"), here the available grid space. */
static CssPx gt_used_gap(const GridAxisSizing *g)
{
    CssLength gap;
    char buf[160];

    if (!css_computed_gap(g->container, g->vertical, &gap)) return css_px(0.0);
    if (gap.kind == CSS_LENGTH_ABSOLUTE) return gap.px;
    if (g->constraint != GRID_SIZING_AVAILABLE)
        DFAILF("%s is a grid container with a percentage %s and no definite available grid space, which CSS Gaps 1 "
               "§2.3 resolves \"against zero for determining intrinsic size contributions, but ... against the "
               "box's content box when laying out the box's contents\". BUILD both resolutions",
               box_subject(g->container, buf, sizeof buf), g->vertical ? "`row-gap`" : "`column-gap`");
    return css_px_max(css_length_resolve_pct(gap, g->avail), css_px(0.0));
}

/* The gutters between the `n` adjacent tracks from `start`: "Gutters only appear between tracks of the implicit
   grid; there is no gutter before the first track or after the last track", and a collapsed track's gutters
   collapse (§7.2.3.2), so a gutter stands between each two consecutive tracks that are not collapsed. */
static CssPx gt_gutters(const GridAxisSizing *g, size_t start, size_t n)
{
    size_t t, kept = 0;

    DCHECK(start + n <= g->ntracks, "gutters were asked over tracks the axis does not have");
    for (t = start; t < start + n; t++)
        if (!g->tracks[t].collapsed) kept++;
    return kept > 1 ? css_px_scale(g->gap, (double)(kept - 1)) : css_px(0.0);
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
    g->contribs = NULL;
    g->ncontribs = g->contribs_cap = 0;
    g->avail = css_px(0.0);
    grid_placement_run(container, constraint != GRID_SIZING_AVAILABLE, &g->placement);
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
    g->gap = gt_used_gap(g);
    gt_tracks_init(g);
    if (vertical) g->outer_ctx = layout_job_grid_rows_enter(container);
}

/* One item's contributions on the axis. Columns: §11.5's minimum contribution (under a min-/max-content
   constraint "limited min-content contribution", which for tracks with no fixed max is the min-content
   contribution) and the min-content and max-content contributions. Rows: the one outer size for all three.
   False is "not yet". */
/* §6.6's track conditions over the item's spanned tracks `[start, start + span)`: "it spans at least one track
   in that axis whose min track sizing function is auto" (a bare `<flex>` track's min is `auto`), and "if it
   spans more than one track in that axis, none of those tracks are flexible". */
static bool gt_auto_minimum_applies(const GridAxisSizing *g, size_t start, size_t span)
{
    bool autoish = false, flex = false;
    size_t t;

    DCHECK(span >= 1 && start + span <= g->ntracks, "§6.6 was asked over tracks the axis does not have");
    for (t = start; t < start + span; t++) {
        if (g->tracks[t].min == CSS_GRID_BREADTH_AUTO) autoish = true;
        if (gt_flexible(&g->tracks[t])) flex = true;
    }
    return autoish && !(span > 1 && flex);
}

/* Whether every track in `[start, start + span)` has a fixed limit (`gt_max_limit`), which is when §11.5's
   limited contributions are limited: "the upper limit ... is the sum of the fixed max track sizing functions of
   any tracks it spans, and is applied if it only spans such tracks". */
static bool gt_span_limited(const GridAxisSizing *g, size_t start, size_t span)
{
    CssPx unused;
    size_t t;

    for (t = start; t < start + span; t++)
        if (!gt_max_limit(&g->tracks[t], &unused)) return false;
    return true;
}

static bool gt_contributions_ask(const GridAxisSizing *g, lxb_dom_element_t *item, GridContribution *out)
{
    IntrinsicInlineSizes c;
    char buf[160], cbuf[160];

    DCHECKF(!g->vertical || layout_job_grid_rows_sizing(g->container),
            "%s's row contribution was asked outside %s's grid row context, so a percentage cyclic through its "
            "grid area would resolve against the rows being sized",
            box_subject(item, buf, sizeof buf), box_subject(g->container, cbuf, sizeof cbuf));
    if (g->vertical) {
        if (!gt_row_contribution_ask(item, &out->minimum)) return false;
        out->min_content = out->max_content = out->minimum;
        return true;
    }
    if (!intrinsic_inline_sizes_ask(item)) return false;
    c = intrinsic_inline_contributions(item);
    out->minimum = g->constraint == GRID_SIZING_AVAILABLE || gt_span_limited(g, out->start, out->span)
                       ? gt_minimum_contribution(item, c, gt_auto_minimum_applies(g, out->start, out->span))
                       : c.min_content;
    out->min_content = c.min_content;
    out->max_content = c.max_content;
    return true;
}

static CssPx gt_sum(const GridAxisSizing *g)
{
    CssPx s = css_px(0.0);
    size_t i;

    for (i = 0; i < g->ntracks; i++) s = css_px_add(s, g->tracks[i].base);
    return css_px_add(s, gt_gutters(g, 0, g->ntracks));
}

/* §11.7.1 "Find the Size of an fr" over the tracks `[lo, hi)` and a space to fill, whose gutters are fixed
   tracks among them. */
static CssPx gt_find_fr(const GridAxisSizing *g, size_t lo, size_t hi, CssPx space)
{
    bool *inflexible = lexbor_calloc(hi - lo, sizeof *inflexible);
    CssPx hyp = css_px(0.0);
    bool restart = true;
    size_t i;

    CHECK(inflexible != NULL, "css-grid-1 §11.7.1: out of memory marking inflexible tracks");
    while (restart) {
        CssPx leftover = css_px_sub(space, gt_gutters(g, lo, hi - lo));
        double sum = 0.0;

        /* "Let leftover space be the space to fill minus the base sizes of the non-flexible grid tracks. Let
           flex factor sum be the sum of the flex factors of the flexible tracks. If this value is less than 1,
           set it to 1 instead. Let the hypothetical fr size be the leftover space divided by the flex factor
           sum." */
        for (i = lo; i < hi; i++) {
            if (!gt_flexible(&g->tracks[i]) || inflexible[i - lo])
                leftover = css_px_sub(leftover, g->tracks[i].base);
            else
                sum += g->tracks[i].flex;
        }
        hyp = css_px_scale(leftover, 1.0 / (sum < 1.0 ? 1.0 : sum));
        /* "If the product of the hypothetical fr size and a flexible track's flex factor is less than the track's
           base size, restart this algorithm treating all such tracks as inflexible." */
        restart = false;
        for (i = lo; i < hi; i++)
            if (gt_flexible(&g->tracks[i]) && !inflexible[i - lo] &&
                hyp.px * g->tracks[i].flex < g->tracks[i].base.px) {
                inflexible[i - lo] = true;
                restart = true;
            }
    }
    free(inflexible);
    return hyp;
}

/* Whether the item spans a track with a flexible sizing function. */
static bool gt_crosses_flex(const GridAxisSizing *g, const GridContribution *c)
{
    size_t t;

    for (t = c->start; t < c->start + c->span; t++)
        if (gt_flexible(&g->tracks[t])) return true;
    return false;
}

/* Which of a track's two sizes a distribution grows, and which contribution it accommodates: the minimum
   contribution (under a min-/max-content constraint, the limited min-content contribution), the min-content,
   the limited max-content or the max-content contribution. */
typedef enum { GT_BASE = 0, GT_GROWTH } GtSize;
typedef enum { GT_MINIMUM = 0, GT_MIN_CONTENT, GT_LIMITED_MAX_CONTENT, GT_MAX_CONTENT } GtKind;
/* Which tracks one sub-step of §11.5's spanning steps affects, by its own words: "an intrinsic min track sizing
   function", "a min track sizing function of min-content or max-content", "of auto or max-content", "of
   max-content", "intrinsic max track sizing function", and "a max track sizing function of max-content". */
typedef enum {
    GT_AFFECT_MIN_INTRINSIC = 0,
    GT_AFFECT_MIN_CONTENT_BASED,
    GT_AFFECT_MIN_AUTO_OR_MAX_CONTENT,
    GT_AFFECT_MIN_MAX_CONTENT,
    GT_AFFECT_MAX_INTRINSIC,
    GT_AFFECT_MAX_MAX_CONTENT
} GtAffect;

/* Whether sub-step `a` affects `t`. For the items crossing a flexible track (`flex_items`) only flexible
   tracks are affected, "treating all other tracks as having a fixed sizing function". */
static bool gt_affected(const GridTrack *t, GtAffect a, bool flex_items)
{
    if (flex_items && !gt_flexible(t)) return false;
    switch (a) {
    case GT_AFFECT_MIN_INTRINSIC: return gt_min_intrinsic(t);
    case GT_AFFECT_MIN_CONTENT_BASED:
        return t->min == CSS_GRID_BREADTH_MIN_CONTENT || t->min == CSS_GRID_BREADTH_MAX_CONTENT;
    case GT_AFFECT_MIN_AUTO_OR_MAX_CONTENT:
        return t->min == CSS_GRID_BREADTH_AUTO || t->min == CSS_GRID_BREADTH_MAX_CONTENT;
    case GT_AFFECT_MIN_MAX_CONTENT: return t->min == CSS_GRID_BREADTH_MAX_CONTENT;
    case GT_AFFECT_MAX_INTRINSIC: return gt_max_intrinsic(t);
    case GT_AFFECT_MAX_MAX_CONTENT: return gt_max_max_content(t);
    default:
        DFAILF("§11.5 sub-step %d is not one of the six this file enumerates", (int)a);
        return false;
    }
}

/* §11.5's limited min-/max-content contribution: the contribution "limited by the max track sizing function
   (which could be the argument to a fit-content() track sizing function) if that is fixed and ultimately floored
   by its minimum contribution"; for a spanning item the limit "is the sum of the fixed max track sizing functions
   of any tracks it spans, and is applied if it only spans such tracks". The gutters it crosses are fixed tracks
   (§11.4's Note). */
static CssPx gt_limited(const GridAxisSizing *g, const GridContribution *c, CssPx contribution)
{
    CssPx limit = gt_gutters(g, c->start, c->span), px;
    size_t t;

    for (t = c->start; t < c->start + c->span; t++) {
        if (!gt_max_limit(&g->tracks[t], &px)) return contribution;
        limit = css_px_add(limit, px);
    }
    return css_px_max(c->minimum, css_px_min(contribution, limit));
}

static CssPx gt_contribution(const GridAxisSizing *g, const GridContribution *c, GtKind k)
{
    switch (k) {
    case GT_MINIMUM:
        /* "If the grid container is being sized under a min- or max-content constraint, use the items' limited
           min-content contributions in place of their minimum contributions here." */
        return g->constraint == GRID_SIZING_AVAILABLE ? c->minimum : gt_limited(g, c, c->min_content);
    case GT_MIN_CONTENT: return c->min_content;
    case GT_LIMITED_MAX_CONTENT: return gt_limited(g, c, c->max_content);
    case GT_MAX_CONTENT: return c->max_content;
    default:
        DFAILF("§11.5 contribution %d is not one of the four this file enumerates", (int)k);
        return c->max_content;
    }
}

/* §11.5.1's affected size: "(For infinite growth limits, substitute the track's base size.)" */
static CssPx gt_affected_size(const GridTrack *t, GtSize which)
{
    return which == GT_BASE || t->growth_inf ? t->base : t->growth;
}

/* How far §11.5.1 lets a track's item-incurred increase go. Up to limits: "For base sizes, the limit is its
   growth limit, capped by its fit-content() argument if any. For growth limits, the limit is the growth limit if
   the growth limit is finite and the track is not infinitely growable, otherwise its fit-content() argument if it
   has a fit-content() track sizing function, and infinity otherwise." Beyond limits: "the max track sizing
   function of a fit-content() track is treated as max-content until the track reaches the limit specified as
   the fit-content() argument, after which its max track sizing function is treated as being a fixed sizing
   function of that argument", and no other track has a limit. Unlimited: none at all. False is infinity. */
typedef enum { GT_UP_TO_LIMITS = 0, GT_BEYOND_LIMITS, GT_UNLIMITED } GtLimits;

static bool gt_limit(const GridTrack *t, GtSize which, GtLimits mode, CssPx *out)
{
    bool fit = t->max == CSS_GRID_BREADTH_FIT_CONTENT;

    if (mode == GT_UNLIMITED) return false;
    if (mode == GT_BEYOND_LIMITS || (which == GT_GROWTH && (t->growth_inf || t->growable))) {
        if (fit) *out = t->max_fixed;
        return fit;
    }
    if (which == GT_GROWTH) {
        *out = t->growth;
        return true;
    }
    if (t->growth_inf) {
        if (fit) *out = t->max_fixed;
        return fit;
    }
    *out = fit ? css_px_min(t->growth, t->max_fixed) : t->growth;
    return true;
}

/* §11.5.1's "distributing the space equally among these tracks, freezing a track's item-incurred increase as
   its affected size + item-incurred increase reaches its limit (and continuing to grow the unfrozen tracks as
   needed)" over the tracks `set`, into `inc`, with `mode`'s limits. Answers the space left. */
static CssPx gt_share(GridAxisSizing *g, const size_t *set, size_t n, GtSize which, GtLimits mode, CssPx *inc,
                      CssPx space)
{
    bool froze = true;
    size_t i;

    while (froze && space.px > 0.0) {
        size_t unfrozen = 0;
        CssPx share;

        froze = false;
        for (i = 0; i < n; i++) {
            const GridTrack *t = &g->tracks[set[i]];
            CssPx lim;

            if (!gt_limit(t, which, mode, &lim) || css_px_add(gt_affected_size(t, which), inc[set[i]]).px < lim.px)
                unfrozen++;
        }
        if (unfrozen == 0) break;
        share = css_px_scale(space, 1.0 / (double)unfrozen);
        for (i = 0; i < n; i++) {
            const GridTrack *t = &g->tracks[set[i]];
            CssPx lim, d = share;

            if (gt_limit(t, which, mode, &lim)) {
                CssPx room = css_px_sub(lim, css_px_add(gt_affected_size(t, which), inc[set[i]]));

                if (room.px <= 0.0) continue;
                if (room.px < share.px) { d = room; froze = true; }
            }
            inc[set[i]] = css_px_add(inc[set[i]], d);
            space = css_px_sub(space, d);
        }
    }
    return space;
}

/* §11.5.1's "Distribute space beyond limits" set: "when accommodating minimum contributions or accommodating
   min-content contributions into base sizes: any affected track that happens to also have an intrinsic max track
   sizing function ...; when accommodating max-content contributions into base sizes: any affected track that
   happens to also have a max-content max track sizing function ...; when accommodating any contribution into
   growth limits: any affected track that has an intrinsic max track sizing function". */
static bool gt_beyond(const GridTrack *t, GtSize which, GtKind kind)
{
    if (which == GT_GROWTH || kind == GT_MINIMUM || kind == GT_MIN_CONTENT) return gt_max_intrinsic(t);
    return gt_max_max_content(t);
}

/* §11.5.1 "Distributing Extra Space Across Spanned Tracks" for one sub-step of §11.5 over the items `group`
   selects: span `span` among those crossing no flexible track, or (`span` 0) every item crossing one, in which
   case only flexible tracks are affected and the space goes "according to the ratios of their flexible sizing
   functions rather than distributing space equally; and if the sum is less than one, distributing that
   proportion of space according to the ratios of their flexible sizing functions and the rest equally". */
static void gt_distribute(GridAxisSizing *g, size_t span, GtAffect affect, GtSize which, GtKind kind, bool mark)
{
    size_t cap = g->ntracks ? g->ntracks : 1;
    CssPx *planned = lexbor_calloc(cap, sizeof *planned), *inc = lexbor_calloc(cap, sizeof *inc);
    bool *touched = lexbor_calloc(cap, sizeof *touched);
    size_t *aff = lexbor_calloc(cap, sizeof *aff), *rest = lexbor_calloc(cap, sizeof *rest);
    size_t *over = lexbor_calloc(cap, sizeof *over);
    size_t i, t;

    CHECK(planned && inc && touched && aff && rest && over,
          "css-grid-1 §11.5.1: out of memory distributing extra space");
    for (i = 0; i < g->ncontribs; i++) {
        const GridContribution *c = &g->contribs[i];
        bool flex = gt_crosses_flex(g, c);
        size_t na = 0, nr = 0, no = 0, end = c->start + c->span;
        CssPx space = css_px_sub(gt_contribution(g, c, kind), gt_gutters(g, c->start, c->span));

        if (span == 0 ? !flex : (flex || c->span != span)) continue;
        for (t = c->start; t < end; t++) {
            const GridTrack *tr = &g->tracks[t];

            /* "Subtract the affected size of every spanned track (not just the affected tracks) from the item's
               size contribution, flooring it at zero." */
            space = css_px_sub(space, gt_affected_size(tr, which));
            inc[t] = css_px(0.0);
            if (gt_affected(tr, affect, span == 0)) {
                aff[na++] = t;
                if (gt_beyond(tr, which, kind)) over[no++] = t;
            } else {
                rest[nr++] = t;
            }
        }
        space = css_px_max(space, css_px(0.0));
        if (na == 0) continue;
        if (span == 0) {
            double sum = 0.0;

            for (t = 0; t < na; t++) sum += g->tracks[aff[t]].flex;
            if (sum > 0.0) {
                double portion = sum < 1.0 ? sum : 1.0;

                for (t = 0; t < na; t++)
                    inc[aff[t]] = css_px_scale(space, portion * g->tracks[aff[t]].flex / sum);
                space = css_px_scale(space, 1.0 - portion);
            }
            space = gt_share(g, aff, na, which, GT_UNLIMITED, inc, space);
        } else {
            space = gt_share(g, aff, na, which, GT_UP_TO_LIMITS, inc, space);
            /* "If extra space remains at this point, and the item spans both affected tracks and non-affected
               tracks, distribute space as for the previous step, but into the non-affected tracks instead." */
            if (space.px > 0.0 && nr > 0) space = gt_share(g, rest, nr, which, GT_UP_TO_LIMITS, inc, space);
            /* "Distribute space beyond limits" into the set above; for a base size, "if there are no such tracks,
               then all affected tracks". */
            if (space.px > 0.0 && no > 0) space = gt_share(g, over, no, which, GT_BEYOND_LIMITS, inc, space);
            if (space.px > 0.0 && which == GT_BASE) space = gt_share(g, aff, na, which, GT_UNLIMITED, inc, space);
        }
        /* "For each affected track, if the track's item-incurred increase is larger than the track's planned
           increase set the track's planned increase to that value." */
        for (t = 0; t < na; t++) {
            if (!touched[aff[t]] || inc[aff[t]].px > planned[aff[t]].px) planned[aff[t]] = inc[aff[t]];
            touched[aff[t]] = true;
        }
    }
    /* "Update the tracks' affected sizes by adding in the planned increase … (If the affected size is an
       infinite growth limit, set it to the track's base size plus the planned increase.)" */
    for (t = 0; t < g->ntracks; t++) {
        GridTrack *tr = &g->tracks[t];

        if (!touched[t]) continue;
        if (which == GT_BASE) {
            tr->base = css_px_add(tr->base, planned[t]);
        } else if (tr->growth_inf) {
            tr->growth = css_px_add(tr->base, planned[t]);
            tr->growth_inf = false;
            /* "Mark any tracks whose growth limit changed from infinite to finite in this step as infinitely
               growable for the next step." */
            if (mark) tr->growable = true;
        } else {
            tr->growth = css_px_add(tr->growth, planned[t]);
        }
    }
    free(planned);
    free(inc);
    free(touched);
    free(aff);
    free(rest);
    free(over);
}

/* "If the track's growth limit is now less than its base size, increase the growth limit to match." */
static void gt_growth_floor(GridAxisSizing *g)
{
    size_t t;

    for (t = 0; t < g->ntracks; t++)
        if (!g->tracks[t].growth_inf) g->tracks[t].growth = css_px_max(g->tracks[t].growth, g->tracks[t].base);
}

/* §11.5's four base-size sub-steps for one group: "For intrinsic minimums", "For content-based minimums", "For
   max-content minimums" (its constraint arm and its "In all cases" arm), then the growth-limit floor. */
static void gt_base_substeps(GridAxisSizing *g, size_t span)
{
    gt_distribute(g, span, GT_AFFECT_MIN_INTRINSIC, GT_BASE, GT_MINIMUM, false);
    gt_distribute(g, span, GT_AFFECT_MIN_CONTENT_BASED, GT_BASE, GT_MIN_CONTENT, false);
    if (g->constraint == GRID_SIZING_MAX_CONTENT)
        gt_distribute(g, span, GT_AFFECT_MIN_AUTO_OR_MAX_CONTENT, GT_BASE, GT_LIMITED_MAX_CONTENT, false);
    gt_distribute(g, span, GT_AFFECT_MIN_MAX_CONTENT, GT_BASE, GT_MAX_CONTENT, false);
    gt_growth_floor(g);
}

/* §11.5 "Resolve Intrinsic Track Sizes" over the recorded contributions. */
static void gt_resolve_intrinsic(GridAxisSizing *g)
{
    size_t i, span, max_span = 1, t;

    /* "Size tracks to fit non-spanning items: For each track with an intrinsic track sizing function and not a
       flexible sizing function, consider the items in it with a span of 1", each sizing function by its own
       sentence; "floored at zero" for a base size. */
    for (i = 0; i < g->ncontribs; i++) {
        const GridContribution *c = &g->contribs[i];
        GridTrack *tr;
        CssPx b = css_px(0.0), lim = css_px(0.0);
        bool grows = true;

        if (c->span > max_span) max_span = c->span;
        if (c->span != 1) continue;
        tr = &g->tracks[c->start];
        if (gt_flexible(tr) || (!gt_min_intrinsic(tr) && !gt_max_intrinsic(tr))) continue;
        switch (tr->min) {
        case CSS_GRID_BREADTH_MIN_CONTENT: b = c->min_content; break;
        case CSS_GRID_BREADTH_MAX_CONTENT: b = c->max_content; break;
        case CSS_GRID_BREADTH_AUTO: b = gt_contribution(g, c, GT_MINIMUM); break;
        default: b = tr->base; break;
        }
        tr->base = css_px_max(tr->base, css_px_max(b, css_px(0.0)));
        switch (tr->max) {
        case CSS_GRID_BREADTH_MIN_CONTENT: lim = c->min_content; break;
        case CSS_GRID_BREADTH_MAX_CONTENT:
        case CSS_GRID_BREADTH_AUTO: lim = c->max_content; break;
        /* "For fit-content() maximums, furthermore clamp this growth limit by the fit-content() argument." */
        case CSS_GRID_BREADTH_FIT_CONTENT: lim = css_px_min(c->max_content, tr->max_fixed); break;
        default: grows = false; break;
        }
        if (!grows) continue;
        tr->growth = tr->growth_inf ? lim : css_px_max(tr->growth, lim);
        tr->growth_inf = false;
    }
    gt_growth_floor(g);
    /* "Increase sizes to accommodate spanning items crossing content-sized tracks: Next, consider the items with a
       span of 2 that do not span a track with a flexible sizing function", then each greater span. */
    for (span = 2; span <= max_span; span++) {
        gt_base_substeps(g, span);
        gt_distribute(g, span, GT_AFFECT_MAX_INTRINSIC, GT_GROWTH, GT_MIN_CONTENT, true);
        gt_distribute(g, span, GT_AFFECT_MAX_MAX_CONTENT, GT_GROWTH, GT_MAX_CONTENT, false);
        for (t = 0; t < g->ntracks; t++) g->tracks[t].growable = false;
    }
    /* "Increase sizes to accommodate spanning items crossing flexible tracks: Next, repeat the previous step
       instead considering (together, rather than grouped by span size) all items that do span a track with a
       flexible sizing function while distributing space only to flexible tracks". A flexible track's max is
       neither intrinsic nor max-content, so only the base-size sub-steps affect one. */
    gt_base_substeps(g, 0);
}

/* §11.5's last sentence through §11.8, once every item is folded. */
static void gt_size(GridAxisSizing *g)
{
    CssPx free_space, fraction = css_px(0.0);
    bool any_flex = false;
    size_t i, nauto = 0;
    CssKeyword content;

    gt_resolve_intrinsic(g);
    for (i = 0; i < g->ntracks; i++) {
        GridTrack *t = &g->tracks[i];

        /* "If any track still has an infinite growth limit (because, for example, it had no items placed in it
           or it is a flexible track), set its growth limit to its base size." */
        if (t->growth_inf) { t->growth = t->base; t->growth_inf = false; }
        t->growth = css_px_max(t->growth, t->base);
        if (gt_flexible(t)) any_flex = true;
        if (t->max == CSS_GRID_BREADTH_AUTO) nauto++;
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
               tracks that the item crosses and a space to fill of the item's max-content contribution." */
            for (i = 0; i < g->ntracks; i++) {
                const GridTrack *t = &g->tracks[i];

                if (!gt_flexible(t)) continue;
                fraction = css_px_max(fraction, t->flex > 1.0 ? css_px_scale(t->base, 1.0 / t->flex) : t->base);
            }
            for (i = 0; i < g->ncontribs; i++) {
                const GridContribution *c = &g->contribs[i];

                if (gt_crosses_flex(g, c))
                    fraction = css_px_max(fraction, gt_find_fr(g, c->start, c->start + c->span, c->max_content));
            }
        }
        /* "For each flexible track, if the product of the used flex fraction and the track's flex factor is
           greater than the track's base size, set its base size to that product." */
        for (i = 0; i < g->ntracks; i++) {
            GridTrack *t = &g->tracks[i];

            if (gt_flexible(t) && fraction.px * t->flex > t->base.px)
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
                if (g->tracks[i].max == CSS_GRID_BREADTH_AUTO)
                    g->tracks[i].base = css_px_add(g->tracks[i].base, share);
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
        GridContribution rec;
        size_t t;
        bool sized_by_items = false;

        DCHECK(g->items < g->placement.n, "a grid axis met more items than its placement placed");
        cell = g->placement.cells[g->items];
        rec.start = g->vertical ? cell.row : cell.col;
        rec.span = g->vertical ? cell.rows : cell.cols;
        DCHECK(rec.span >= 1 && rec.start + rec.span <= g->ntracks,
               "a grid item was placed across tracks the axis does not have");
        for (t = rec.start; t < rec.start + rec.span; t++)
            if (!gt_fixed(&g->tracks[t])) sized_by_items = true;
        /* A fixed track takes nothing from its items, so an item spanning only fixed tracks is not measured. */
        if (sized_by_items) {
            /* The one step that can stop: the item's measurement under a resting drive. The cursor stays. */
            if (!gt_contributions_ask(g, item, &rec)) return false;
            if (g->ncontribs == g->contribs_cap) {
                size_t cap = g->contribs_cap ? 2 * g->contribs_cap : 8;
                GridContribution *grown = lexbor_realloc(g->contribs, cap * sizeof *grown);

                CHECK(grown != NULL, "css-grid-1 §11.5: out of memory recording item contributions");
                g->contribs = grown;
                g->contribs_cap = cap;
            }
            g->contribs[g->ncontribs++] = rec;
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
    if (g->vertical) layout_job_grid_rows_leave(g->container, g->outer_ctx);
    free(g->tracks);
    g->tracks = NULL;
    g->ntracks = g->capacity = 0;
    free(g->contribs);
    g->contribs = NULL;
    g->ncontribs = g->contribs_cap = 0;
    grid_placement_free(&g->placement);
}

/* ---- readers -------------------------------------------------------------------------------------------- */

/* Every step in this activation; false, with the tracks released, at the first "not yet". */
static bool gt_run_ask(GridAxisSizing *g, lxb_dom_element_t *container, lxb_dom_element_t *target, bool vertical,
                       GridSizingConstraint constraint)
{
    /* css-grid-1 §8's placement, which the run begins with, reads every item's style. */
    if (!box_tree_children_style_ask(container)) return false;
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

/* The first track of the target's area and the number it spans, on the run's axis. */
static size_t gt_target_track(const GridAxisSizing *g, size_t *span)
{
    const GridCell *c;

    DCHECK(g->found, "a grid item's track was read from a run that never reached the item");
    c = &g->placement.cells[g->target_index];
    *span = g->vertical ? c->rows : c->cols;
    return g->vertical ? c->row : c->col;
}

static CssPx gt_track_offset(const GridAxisSizing *g, size_t t);

/* The target's area's extent on the run's axis: from its first track's start to its last track's end, so the
   gutters between them carry §10.5's distributed space ("the spanning item's grid area is increased to
   accommodate the extra space assigned to the gutters due to alignment"). */
static CssPx gt_target_extent(const GridAxisSizing *g)
{
    size_t span, start = gt_target_track(g, &span), last = start + span - 1;

    return css_px_sub(css_px_add(gt_track_offset(g, last), g->tracks[last].base), gt_track_offset(g, start));
}

void grid_track_placement(lxb_dom_element_t *container, GridPlacement *out)
{
    DCHECK(container != NULL && out != NULL, "a grid's placement was asked for with no container or no out");
    grid_placement_run(container, false, out);
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
    w = gt_target_extent(&g);
    grid_axis_sizing_end(&g);
    return w;
}

CssPx grid_item_area_block_size(lxb_dom_element_t *container, lxb_dom_element_t *item)
{
    GridAxisSizing g;
    CssPx h;
    char buf[160], cbuf[160];

    DCHECK(item != NULL, "a grid area's block size was asked for no item");
    DCHECKF(!layout_job_grid_rows_sizing(container),
            "%s's grid area block size was asked while %s's rows are being sized, which is the size those rows "
            "compute; css-sizing-3 §5.2.1 makes a percentage cyclic through it behave as auto there, which "
            "used_value.c's grid area arms answer before asking",
            box_subject(item, buf, sizeof buf), box_subject(container, cbuf, sizeof cbuf));
    gt_run_drive(&g, container, item, true);
    h = gt_target_extent(&g);
    grid_axis_sizing_end(&g);
    return h;
}

/* css-grid-1 §10.5 "Aligning the Grid: the justify-content and align-content properties" over the free space
   left after §11.8: the offset of the first track (`*lead`) and the extra space in each gutter (`*between`),
   which "is part of the gutter, and collapses with it" at a collapsed track. css-align-3 §4.3 "Distributed
   Alignment" spreads the space "between", "around" or "evenly" over the tracks that are not collapsed, and falls
   back to its default ("safe flow-start" for space-between, "safe center" for the other two) where it cannot;
   §4.4's `safe` aligns an overflowing grid "as if the alignment mode were flow-start", and with neither keyword
   the value is honoured. `normal` and `stretch` have stretched the `auto` tracks in §11.8 and keep the rest at
   the start. A content box this axis is not sized in (a min-/max-content constraint) has no free space. */
static void gt_alignment(const GridAxisSizing *g, CssPx *lead, CssPx *between)
{
    const char *name = g->vertical ? "align-content" : "justify-content";
    CssAlign a = css_computed_align(g->container, name);
    CssKeyword k = a.value;
    CssPx free_space;
    size_t t, kept = 0;
    char buf[160];

    *lead = *between = css_px(0.0);
    if (g->constraint != GRID_SIZING_AVAILABLE) return;
    free_space = css_px_sub(g->avail, gt_sum(g));
    for (t = 0; t < g->ntracks; t++)
        if (!g->tracks[t].collapsed) kept++;
    if (k == CSS_KEYWORD_LEFT || k == CSS_KEYWORD_RIGHT) {
        DCHECKF(!g->vertical, "`align-content: %s` is outside css-align-3 §5.2's grammar",
                k == CSS_KEYWORD_LEFT ? "left" : "right");
        if (!css_computed_keyword_is(g->container, "direction", CSS_KEYWORD_LTR))
            DFAILF("%s is a right-to-left grid container with `justify-content: left/right`, which css-align-3 "
                   "§4.2 maps onto its line-right or line-left edge. BUILD the right-to-left grid",
                   box_subject(g->container, buf, sizeof buf));
        k = k == CSS_KEYWORD_LEFT ? CSS_KEYWORD_START : CSS_KEYWORD_END;
    }
    if (k == CSS_KEYWORD_SPACE_BETWEEN) {
        if (kept > 1 && free_space.px > 0.0) { *between = css_px_scale(free_space, 1.0 / (double)(kept - 1)); return; }
        k = CSS_KEYWORD_START;
        a.overflow = CSS_KEYWORD_SAFE;
    } else if (k == CSS_KEYWORD_SPACE_AROUND || k == CSS_KEYWORD_SPACE_EVENLY) {
        if (kept > 0 && free_space.px > 0.0) {
            *between = css_px_scale(free_space, 1.0 / (double)(k == CSS_KEYWORD_SPACE_AROUND ? kept : kept + 1));
            *lead = k == CSS_KEYWORD_SPACE_AROUND ? css_px_scale(*between, 0.5) : *between;
            return;
        }
        k = CSS_KEYWORD_CENTER;
        a.overflow = CSS_KEYWORD_SAFE;
    }
    if (free_space.px < 0.0 && a.overflow == CSS_KEYWORD_SAFE) return;
    switch (k) {
    case CSS_KEYWORD_NORMAL:
    case CSS_KEYWORD_STRETCH:
    case CSS_KEYWORD_START:
    case CSS_KEYWORD_FLEX_START:
        return;
    case CSS_KEYWORD_END:
    case CSS_KEYWORD_FLEX_END:
        *lead = free_space;
        return;
    case CSS_KEYWORD_CENTER:
        *lead = css_px_scale(free_space, 0.5);
        return;
    default:
        DFAILF("%s is a grid container whose `%s` is a css-align-3 value §10.5 does not place here (a baseline "
               "content alignment). BUILD it", box_subject(g->container, buf, sizeof buf), name);
        return;
    }
}

/* The space from the grid's start to track `t` and to the end of the `span` tracks from `start`: the tracks
   ahead of it and, after each that is not collapsed, a gutter widened by §10.5's distributed space. */
static CssPx gt_track_offset(const GridAxisSizing *g, size_t t)
{
    CssPx lead, between, o;
    size_t i, kept = 0;

    gt_alignment(g, &lead, &between);
    o = lead;
    for (i = 0; i < t; i++) {
        o = css_px_add(o, g->tracks[i].base);
        if (!g->tracks[i].collapsed) kept++;
    }
    return css_px_add(o, css_px_scale(css_px_add(g->gap, between), (double)kept));
}

/* The target's track's start. */
static CssPx gt_target_offset(const GridAxisSizing *g)
{
    size_t span, t = gt_target_track(g, &span);

    return gt_track_offset(g, t);
}

bool grid_item_area_origin_ask(lxb_dom_element_t *container, lxb_dom_element_t *item, CssPx *x, CssPx *y)
{
    GridAxisSizing c, r;

    DCHECK(item != NULL && x != NULL && y != NULL, "a grid area's origin was asked for with nowhere to put it");
    if (!gt_run_ask(&c, container, item, false, GRID_SIZING_AVAILABLE)) return false;
    *x = gt_target_offset(&c);
    grid_axis_sizing_end(&c);
    if (!gt_run_ask(&r, container, item, true, GRID_SIZING_AVAILABLE)) return false;
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
    GridExplicitTracks ex;
    GridPlacement p;
    size_t k = gt_item_index(container, item), start, span, t;
    bool any, autoish = false, flex = false;

    grid_track_placement(container, &p);
    DCHECK(k < p.n, "a grid item's tracks were asked for an item its container's placement did not place");
    start = vertical ? p.cells[k].row : p.cells[k].col;
    span = vertical ? p.cells[k].rows : p.cells[k].cols;
    grid_placement_free(&p);
    any = grid_explicit_tracks(container, vertical, false, &ex);
    for (t = start; t < start + span; t++) {
        /* An implicit track, and every track of a `none` list, is `auto` (§7.6). */
        bool listed = any && t < ex.list.n;

        if (!listed || ex.list.tracks[t].min.kind == CSS_GRID_BREADTH_AUTO) autoish = true;
        if (listed && ex.list.tracks[t].max.kind == CSS_GRID_BREADTH_FLEX) flex = true;
    }
    grid_explicit_tracks_free(&ex);
    return autoish && !(span > 1 && flex);
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
