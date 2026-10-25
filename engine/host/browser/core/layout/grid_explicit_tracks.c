/* css-grid-1 §7.1's explicit tracks with §7.2.3.2's repetitions counted. See grid_explicit_tracks.h. */
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "core/css/css_computed_value.h"
#include "core/css/css_display.h"
#include "core/layout/block_flow.h"
#include "core/layout/box_subject.h"
#include "core/layout/grid_explicit_tracks.h"
#include "core/layout/intrinsic_size.h"
#include "core/layout/used_value.h"
#include <lexbor/core/base.h>   /* lexbor_malloc: the allocator the engine gives its refusal edge */

/* §7.2.3.2's "definite preferred size" of the container's content box on the axis. A row's is a height that does
   not behave as auto. A column's is a `width` other than `auto`, or an `auto` one on a block-level box in a block
   container's flow, which CSS 2.1 §10.3.3 sizes from its containing block alone; a float, an absolutely
   positioned box and an `inline-grid` shrink to fit their content, which is indefinite here. Under a min-/max-
   content constraint the container is being sized, so only a declared length is definite. */
static bool get_preferred(lxb_dom_element_t *container, bool vertical, bool intrinsic, CssPx *out)
{
    CssDisplay d, parent;
    char buf[160];

    if (intrinsic)
        return intrinsic_declared_sizing_px(container, vertical ? INTRINSIC_AXIS_VERTICAL : INTRINSIC_AXIS_HORIZONTAL,
                                            vertical ? "height" : "width", CSS_KEYWORD_AUTO, out);
    if (vertical) {
        if (used_value_height_behaves_as_auto(container)) return false;
        *out = used_value_content_px(container, true);
        return true;
    }
    if (!css_computed_length_is(container, "width", CSS_KEYWORD_AUTO)) {
        *out = used_value_content_px(container, false);
        return true;
    }
    d = css_computed_display(container);
    if (block_flow_is_floated(container) || css_computed_keyword_is(container, "position", CSS_KEYWORD_ABSOLUTE) ||
        css_computed_keyword_is(container, "position", CSS_KEYWORD_FIXED) || d.kind != CSS_DISPLAY_PAIR ||
        d.outer != CSS_KEYWORD_BLOCK)
        return false;
    if (css_box_parent_display(lxb_dom_interface_node(container), &parent) &&
        !block_flow_display_is_block_container(parent))
        DFAILF("%s is a grid container with an `auto` width whose parent box is not a block container, and "
               "css-grid-1 §7.2.3.2 counts its auto repetitions from a definite preferred size. BUILD whether a "
               "stretched flex or grid item's inline size is definite (css-flexbox-1 §9.8, css-grid-1 §6.2)",
               box_subject(container, buf, sizeof buf));
    *out = used_value_content_px(container, false);
    return true;
}

/* One track's size for the count: "each track is treated as its max track sizing function if that is definite
   or else its min track sizing function if that is definite. If both are definite, floor the max track sizing
   function by the min track sizing function." A percentage is definite against a definite `space`. False when
   neither is. */
static bool get_track_size(const CssGridTrackSize *t, bool has_space, CssPx space, CssPx *out)
{
    bool hmin = false, hmax = false;
    CssPx vmin = css_px(0.0), vmax = css_px(0.0);

    if (t->min.kind == CSS_GRID_BREADTH_LENGTH && (t->min.length.kind == CSS_LENGTH_ABSOLUTE || has_space)) {
        vmin = t->min.length.kind == CSS_LENGTH_ABSOLUTE ? t->min.length.px
                                                         : css_length_resolve_pct(t->min.length, space);
        vmin = css_px_max(vmin, css_px(0.0));
        hmin = true;
    }
    if (t->max.kind == CSS_GRID_BREADTH_LENGTH && (t->max.length.kind == CSS_LENGTH_ABSOLUTE || has_space)) {
        vmax = t->max.length.kind == CSS_LENGTH_ABSOLUTE ? t->max.length.px
                                                         : css_length_resolve_pct(t->max.length, space);
        vmax = css_px_max(vmax, css_px(0.0));
        hmax = true;
    }
    if (!hmin && !hmax) return false;
    *out = hmax && hmin ? css_px_max(vmax, vmin) : hmax ? vmax : vmin;
    return true;
}

/* §7.2.3.2's number of repetitions of `list`'s `<auto-repeat>` in a content box of `space` (`has_space`), or in
   one at least `min_space` (`has_min`), with `gap` between adjacent tracks. */
static size_t get_count(const CssGridTrackList *list, bool has_space, CssPx space, bool has_min, CssPx min_space,
                        CssPx gap)
{
    CssPx fixed = css_px(0.0), per = css_px(0.0), against = has_space ? space : min_space, sz;
    size_t i, k, others = list->n - list->auto_n;

    if (!has_space && !has_min) return 1;
    for (i = 0; i < list->n; i++) {
        bool repeated = i >= list->auto_at && i < list->auto_at + list->auto_n;

        if (!get_track_size(&list->tracks[i], true, against, &sz)) return 1;
        /* "For the purpose of finding the number of auto-repeated tracks, the UA must floor the track size to a
           UA-specified value to avoid division by zero. It is suggested that this floor be 1px." */
        if (repeated) per = css_px_add(per, css_px_max(sz, css_px(1.0)));
        else fixed = css_px_add(fixed, sz);
    }
    for (k = 1;; k++) {
        size_t tracks = others + k * list->auto_n;
        CssPx total = css_px_add(css_px_add(fixed, css_px_scale(per, (double)k)),
                                 css_px_scale(gap, (double)(tracks - 1)));

        /* "the largest possible positive integer that does not cause the grid to overflow the content box of its
           grid container taking gap into account; if any number of repetitions would overflow, then 1
           repetition", or under a definite minimum "the smallest possible positive integer that fulfills that
           minimum requirement". The total grows by at least 1px a repetition, so either test ends the walk. */
        if (has_space && total.px > space.px) return k > 1 ? k - 1 : 1;
        if (!has_space && total.px >= min_space.px) return k;
    }
}

bool grid_explicit_tracks(lxb_dom_element_t *container, bool vertical, bool intrinsic, GridExplicitTracks *out)
{
    CssGridTrackList list;
    CssLength gap_len;
    CssPx space = css_px(0.0), mx, mn = css_px(0.0), gap = css_px(0.0);
    bool has_space, has_max, has_min;
    size_t count, i, k, cap;
    char buf[160];

    DCHECK(container != NULL && out != NULL, "a grid's explicit tracks were asked with no container or nowhere");
    memset(out, 0, sizeof *out);
    if (!css_computed_grid_track_list(container, vertical ? "grid-template-rows" : "grid-template-columns", &list))
        return false;
    if (list.auto_n == 0) {
        out->list = list;
        return true;
    }
    /* "if the grid container has a definite preferred size or maximum size in the relevant axis" — both, the
       smaller, since the maximum limits the preferred size — "Otherwise, if the grid container has a definite
       minimum size". */
    has_space = get_preferred(container, vertical, intrinsic, &space);
    has_max = intrinsic_declared_sizing_px(container, vertical ? INTRINSIC_AXIS_VERTICAL : INTRINSIC_AXIS_HORIZONTAL,
                                           vertical ? "max-height" : "max-width", CSS_KEYWORD_NONE, &mx);
    if (has_max) space = has_space ? css_px_min(space, mx) : mx;
    has_space = has_space || has_max;
    has_min = intrinsic_declared_sizing_px(container, vertical ? INTRINSIC_AXIS_VERTICAL : INTRINSIC_AXIS_HORIZONTAL,
                                           vertical ? "min-height" : "min-width", CSS_KEYWORD_AUTO, &mn);
    if (css_computed_gap(container, vertical, &gap_len)) {
        if (gap_len.kind == CSS_LENGTH_ABSOLUTE) gap = gap_len.px;
        else if (has_space) gap = css_px_max(css_length_resolve_pct(gap_len, space), css_px(0.0));
        else
            DFAILF("%s is a grid container with an auto-repeated track list and a percentage gap but no definite "
                   "size to resolve it against. BUILD CSS Gaps 1 §2.3's resolution against zero for the count",
                   box_subject(container, buf, sizeof buf));
    }
    count = get_count(&list, has_space, space, has_min, mn, gap);
    cap = list.n - list.auto_n + count * list.auto_n;
    out->list.tracks = lexbor_malloc(cap * sizeof *out->list.tracks);
    CHECK(out->list.tracks != NULL, "css-grid-1 §7.2.3.2: out of memory expanding an auto-repeated track list");
    for (i = 0; i < list.auto_at; i++) out->list.tracks[out->list.n++] = list.tracks[i];
    out->repeat_lo = out->list.n;
    for (k = 0; k < count; k++)
        for (i = 0; i < list.auto_n; i++) out->list.tracks[out->list.n++] = list.tracks[list.auto_at + i];
    out->repeat_hi = out->list.n;
    for (i = list.auto_at + list.auto_n; i < list.n; i++) out->list.tracks[out->list.n++] = list.tracks[i];
    out->auto_fit = list.auto_fit;
    css_grid_track_list_free(&list);
    return true;
}

void grid_explicit_tracks_free(GridExplicitTracks *t)
{
    DCHECK(t != NULL, "a grid's explicit tracks were released with no record");
    css_grid_track_list_free(&t->list);
    t->repeat_lo = t->repeat_hi = 0;
    t->auto_fit = false;
}
