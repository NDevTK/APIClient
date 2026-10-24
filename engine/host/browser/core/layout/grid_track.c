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

    gt_require_initial(c, "grid-template", "none", SHORTHAND);
    gt_require_initial(c, "grid", "none", SHORTHAND);
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
    t->growable = false;
    /* §11.4 "Initialize Track Sizes": a fixed sizing function is both the base size and the growth limit; an
       intrinsic min (`auto`, and a bare `<flex>`'s implied `auto`) gives a base size of zero, and an intrinsic
       or flexible max a growth limit of infinity. */
    t->base = kind == CSS_GRID_TRACK_BREADTH ? fixed : css_px(0.0);
    t->growth = t->base;
    t->growth_inf = kind != CSS_GRID_TRACK_BREADTH;
    return t;
}

/* The number of explicit tracks on one axis of `container`. */
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

/* The gutters between `n` adjacent tracks: "Gutters only appear between tracks of the implicit grid; there is
   no gutter before the first track or after the last track." */
static CssPx gt_gutters(const GridAxisSizing *g, size_t n)
{
    return n > 1 ? css_px_scale(g->gap, (double)(n - 1)) : css_px(0.0);
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
    grid_placement_run(container, &g->placement);
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
   in that axis whose min track sizing function is auto" (a `<flex>` track's min is `auto`), and "if it spans
   more than one track in that axis, none of those tracks are flexible". */
static bool gt_auto_minimum_applies(const GridAxisSizing *g, size_t start, size_t span)
{
    bool autoish = false, flex = false;
    size_t t;

    DCHECK(span >= 1 && start + span <= g->ntracks, "§6.6 was asked over tracks the axis does not have");
    for (t = start; t < start + span; t++) {
        if (g->tracks[t].kind != CSS_GRID_TRACK_BREADTH) autoish = true;
        if (g->tracks[t].kind == CSS_GRID_TRACK_FLEX) flex = true;
    }
    return autoish && !(span > 1 && flex);
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
    out->minimum = g->constraint == GRID_SIZING_AVAILABLE
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
    return css_px_add(s, gt_gutters(g, g->ntracks));
}

/* §11.7.1 "Find the Size of an fr" over the tracks `[lo, hi)` and a space to fill, whose gutters are fixed
   tracks among them. */
static CssPx gt_find_fr(const GridAxisSizing *g, size_t lo, size_t hi, CssPx space)
{
    bool *inflexible = calloc(hi - lo, sizeof *inflexible);
    CssPx hyp = css_px(0.0);
    bool restart = true;
    size_t i;

    CHECK(inflexible != NULL, "css-grid-1 §11.7.1: out of memory marking inflexible tracks");
    while (restart) {
        CssPx leftover = css_px_sub(space, gt_gutters(g, hi - lo));
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

/* Whether the item spans a track with a flexible sizing function. */
static bool gt_crosses_flex(const GridAxisSizing *g, const GridContribution *c)
{
    size_t t;

    for (t = c->start; t < c->start + c->span; t++)
        if (g->tracks[t].kind == CSS_GRID_TRACK_FLEX) return true;
    return false;
}

/* Which of a track's two sizes a distribution grows, and which contribution it accommodates. */
typedef enum { GT_BASE = 0, GT_GROWTH } GtSize;
typedef enum { GT_MINIMUM = 0, GT_MIN_CONTENT, GT_MAX_CONTENT } GtKind;
/* Which tracks a sub-step affects: an intrinsic (`auto`) min or max, every flexible track, or none. */
typedef enum { GT_AFFECT_AUTO = 0, GT_AFFECT_FLEX } GtAffect;

static CssPx gt_contribution(const GridContribution *c, GtKind k)
{
    return k == GT_MINIMUM ? c->minimum : k == GT_MIN_CONTENT ? c->min_content : c->max_content;
}

/* §11.5.1's affected size: "(For infinite growth limits, substitute the track's base size.)" */
static CssPx gt_affected_size(const GridTrack *t, GtSize which)
{
    return which == GT_BASE || t->growth_inf ? t->base : t->growth;
}

/* §11.5.1's limit: for base sizes "the limit is its growth limit"; for growth limits "the growth limit if the
   growth limit is finite and the track is not infinitely growable … and infinity otherwise". False is
   infinity. */
static bool gt_limit(const GridTrack *t, GtSize which, CssPx *out)
{
    if (t->growth_inf || (which == GT_GROWTH && t->growable)) return false;
    *out = t->growth;
    return true;
}

/* §11.5.1's "distributing the space equally among these tracks, freezing a track's item-incurred increase as
   its affected size + item-incurred increase reaches its limit (and continuing to grow the unfrozen tracks as
   needed)" over the tracks `set`, into `inc`; `limited` false is the step "beyond limits". Answers the space
   left. */
static CssPx gt_share(const GridAxisSizing *g, const size_t *set, size_t n, GtSize which, bool limited, CssPx *inc,
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

            if (!limited || !gt_limit(t, which, &lim) ||
                css_px_add(gt_affected_size(t, which), inc[set[i]]).px < lim.px)
                unfrozen++;
        }
        if (unfrozen == 0) break;
        share = css_px_scale(space, 1.0 / (double)unfrozen);
        for (i = 0; i < n; i++) {
            const GridTrack *t = &g->tracks[set[i]];
            CssPx lim, d = share;

            if (limited && gt_limit(t, which, &lim)) {
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

/* §11.5.1 "Distributing Extra Space Across Spanned Tracks" for one sub-step of §11.5 over the items `group`
   selects: span `span` among those crossing no flexible track, or (`span` 0) every item crossing one, in which
   case only flexible tracks are affected and the space goes "according to the ratios of their flexible sizing
   functions rather than distributing space equally; and if the sum is less than one, distributing that
   proportion of space according to the ratios of their flexible sizing functions and the rest equally". */
static void gt_distribute(GridAxisSizing *g, size_t span, GtAffect affect, GtSize which, GtKind kind, bool mark)
{
    CssPx *planned = calloc(g->ntracks ? g->ntracks : 1, sizeof *planned);
    CssPx *inc = calloc(g->ntracks ? g->ntracks : 1, sizeof *inc);
    bool *touched = calloc(g->ntracks ? g->ntracks : 1, sizeof *touched);
    size_t *aff = calloc(g->ntracks ? g->ntracks : 1, sizeof *aff), *rest = calloc(g->ntracks ? g->ntracks : 1, sizeof *rest);
    size_t i, t;

    CHECK(planned && inc && touched && aff && rest, "css-grid-1 §11.5.1: out of memory distributing extra space");
    for (i = 0; i < g->ncontribs; i++) {
        const GridContribution *c = &g->contribs[i];
        bool flex = gt_crosses_flex(g, c);
        size_t na = 0, nr = 0, end = c->start + c->span;
        CssPx space = css_px_sub(gt_contribution(c, kind), gt_gutters(g, c->span));

        if (span == 0 ? !flex : (flex || c->span != span)) continue;
        for (t = c->start; t < end; t++) {
            const GridTrack *tr = &g->tracks[t];
            bool affected = affect == GT_AFFECT_FLEX ? tr->kind == CSS_GRID_TRACK_FLEX : tr->kind == CSS_GRID_TRACK_AUTO;

            /* "Subtract the affected size of every spanned track (not just the affected tracks) from the item's
               size contribution, flooring it at zero." */
            space = css_px_sub(space, gt_affected_size(tr, which));
            inc[t] = css_px(0.0);
            if (affected) aff[na++] = t;
            else rest[nr++] = t;
        }
        space = css_px_max(space, css_px(0.0));
        if (na == 0) continue;
        if (affect == GT_AFFECT_FLEX) {
            double sum = 0.0;

            for (t = 0; t < na; t++) sum += g->tracks[aff[t]].flex;
            if (sum > 0.0) {
                double portion = sum < 1.0 ? sum : 1.0;

                for (t = 0; t < na; t++)
                    inc[aff[t]] = css_px_scale(space, portion * g->tracks[aff[t]].flex / sum);
                space = css_px_scale(space, 1.0 - portion);
            }
            space = gt_share(g, aff, na, which, false, inc, space);
        } else {
            space = gt_share(g, aff, na, which, true, inc, space);
            /* "If space remains at this point, and the item spans both affected tracks and non-affected tracks,
               distribute space as for the previous step, but into the non-affected tracks instead." */
            if (space.px > 0.0 && nr > 0) space = gt_share(g, rest, nr, which, true, inc, space);
            /* "Distribute space beyond limits": to an affected track with an intrinsic (here `auto`, which is also
               the max-content kind) max, which every affected track here has. */
            if (space.px > 0.0) space = gt_share(g, aff, na, which, false, inc, space);
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
}

/* "If the track's growth limit is now less than its base size, increase the growth limit to match." */
static void gt_growth_floor(GridAxisSizing *g)
{
    size_t t;

    for (t = 0; t < g->ntracks; t++)
        if (!g->tracks[t].growth_inf) g->tracks[t].growth = css_px_max(g->tracks[t].growth, g->tracks[t].base);
}

/* §11.5 "Resolve Intrinsic Track Sizes" over the recorded contributions. */
static void gt_resolve_intrinsic(GridAxisSizing *g)
{
    size_t i, span, max_span = 1, t;

    /* "Size tracks to fit non-spanning items: For each track with an intrinsic track sizing function and not a
       flexible sizing function, consider the items in it with a span of 1": the base size takes the largest
       minimum contribution (under a min-/max-content constraint, the largest limited min-content
       contribution), "floored at zero", and an `auto` max (treated as max-content) the largest max-content
       contribution. */
    for (i = 0; i < g->ncontribs; i++) {
        const GridContribution *c = &g->contribs[i];
        GridTrack *tr;

        if (c->span > max_span) max_span = c->span;
        if (c->span != 1) continue;
        tr = &g->tracks[c->start];
        if (tr->kind != CSS_GRID_TRACK_AUTO) continue;
        tr->base = css_px_max(tr->base, css_px_max(c->minimum, css_px(0.0)));
        tr->growth = tr->growth_inf ? c->max_content : css_px_max(tr->growth, c->max_content);
        tr->growth_inf = false;
    }
    gt_growth_floor(g);
    /* "Increase sizes to accommodate spanning items crossing content-sized tracks: Next, consider the items with a
       span of 2 that do not span a track with a flexible sizing function", then each greater span. Every track
       with an intrinsic sizing function here is `auto`, which is the min of every sub-step's set and the max of
       both maximum sub-steps; the content-based minimums sub-step affects no track. */
    for (span = 2; span <= max_span; span++) {
        gt_distribute(g, span, GT_AFFECT_AUTO, GT_BASE, GT_MINIMUM, false);
        if (g->constraint == GRID_SIZING_MAX_CONTENT) gt_distribute(g, span, GT_AFFECT_AUTO, GT_BASE, GT_MAX_CONTENT, false);
        gt_growth_floor(g);
        gt_distribute(g, span, GT_AFFECT_AUTO, GT_GROWTH, GT_MIN_CONTENT, true);
        gt_distribute(g, span, GT_AFFECT_AUTO, GT_GROWTH, GT_MAX_CONTENT, false);
        for (t = 0; t < g->ntracks; t++) g->tracks[t].growable = false;
    }
    /* "Increase sizes to accommodate spanning items crossing flexible tracks: Next, repeat the previous step
       instead considering (together, rather than grouped by span size) all items that do span a track with a
       flexible sizing function while distributing space only to flexible tracks". A flexible track's min is
       `auto` and its max is neither intrinsic nor max-content, so only the minimum sub-steps affect it. */
    gt_distribute(g, 0, GT_AFFECT_FLEX, GT_BASE, GT_MINIMUM, false);
    if (g->constraint == GRID_SIZING_MAX_CONTENT) gt_distribute(g, 0, GT_AFFECT_FLEX, GT_BASE, GT_MAX_CONTENT, false);
    gt_growth_floor(g);
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
               tracks that the item crosses and a space to fill of the item's max-content contribution." */
            for (i = 0; i < g->ntracks; i++) {
                const GridTrack *t = &g->tracks[i];

                if (t->kind != CSS_GRID_TRACK_FLEX) continue;
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
            if (g->tracks[t].kind != CSS_GRID_TRACK_BREADTH) sized_by_items = true;
        /* A fixed track takes nothing from its items, so an item spanning only fixed tracks is not measured. */
        if (sized_by_items) {
            /* The one step that can stop: the item's measurement under a resting drive. The cursor stays. */
            if (!gt_contributions_ask(g, item, &rec)) return false;
            if (g->ncontribs == g->contribs_cap) {
                size_t cap = g->contribs_cap ? 2 * g->contribs_cap : 8;
                GridContribution *grown = realloc(g->contribs, cap * sizeof *grown);

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

/* The target's area's extent on the run's axis: the sum of the tracks it spans and the gutters between them. */
static CssPx gt_target_extent(const GridAxisSizing *g)
{
    size_t span, t, start = gt_target_track(g, &span);
    CssPx s = css_px(0.0);

    for (t = start; t < start + span; t++) s = css_px_add(s, g->tracks[t].base);
    return css_px_add(s, gt_gutters(g, span));
}

void grid_track_placement(lxb_dom_element_t *container, GridPlacement *out)
{
    DCHECK(container != NULL && out != NULL, "a grid's placement was asked for with no container or no out");
    grid_placement_run(container, out);
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

/* The target's track's start: the sizes of the tracks ahead of it and the gutter after each. */
static CssPx gt_target_offset(const GridAxisSizing *g)
{
    CssPx o = css_px(0.0);
    size_t span, i, t = gt_target_track(g, &span);

    for (i = 0; i < t; i++) o = css_px_add(o, g->tracks[i].base);
    return css_px_add(o, gt_gutters(g, t + 1));
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
    size_t k = gt_item_index(container, item), start, span, t;
    bool any, autoish = false, flex = false;

    grid_track_placement(container, &p);
    DCHECK(k < p.n, "a grid item's tracks were asked for an item its container's placement did not place");
    start = vertical ? p.cells[k].row : p.cells[k].col;
    span = vertical ? p.cells[k].rows : p.cells[k].cols;
    grid_placement_free(&p);
    any = css_computed_grid_track_list(container, vertical ? "grid-template-rows" : "grid-template-columns", &list);
    for (t = start; t < start + span; t++) {
        CssGridTrackKind kind = any && t < list.n ? list.tracks[t].kind : CSS_GRID_TRACK_AUTO;

        if (kind != CSS_GRID_TRACK_BREADTH) autoish = true;
        if (kind == CSS_GRID_TRACK_FLEX) flex = true;
    }
    css_grid_track_list_free(&list);
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
