/* css-flexbox-1 §8.3's baseline alignment over one flex line; see flex_baseline.h. */
#include <stdbool.h>
#include <stdlib.h>

#include <lexbor/dom/dom.h>

#include "check.h"
#include "core/css/css_computed_value.h"
#include "core/layout/block_flow.h"
#include "core/layout/box_subject.h"
#include "core/layout/flex_baseline.h"
#include "core/layout/flex_cross_size.h"
#include "core/layout/flex_item.h"
#include "core/layout/flex_line.h"
#include "core/layout/replaced_element.h"
#include "core/layout/used_value.h"

/* The member's resolved `align-self` (css-flexbox-1 §8.3: "For anonymous flex items, align-self always matches
   the value of align-items on their associated flex container"). */
static CssAlign fb_align(lxb_dom_element_t *container, const FlexLineMember *m)
{
    return flex_cross_size_resolved_align(container, m->anonymous ? NULL : lxb_dom_interface_element(m->first));
}

/* Is the member baseline-aligned: css-flexbox-1 §8.3's `baseline` with both cross-axis margins non-auto ("If
   either of the flex item's cross-axis margins are auto, align-self has no effect")? §4's anonymous item has no
   margins. */
static bool fb_aligns(lxb_dom_element_t *container, const FlexLineMember *m)
{
    const char *start;
    lxb_dom_element_t *item;

    DCHECK(container != NULL && m != NULL && m->first != NULL, "baseline participation was asked of no member");
    if (fb_align(container, m).value != CSS_KEYWORD_BASELINE) return false;
    if (m->anonymous) return true;
    item = lxb_dom_interface_element(m->first);
    start = flex_container_start_margin(container, FLEX_AXIS_CROSS);
    return !css_computed_length_is(item, start, CSS_KEYWORD_AUTO) &&
           !css_computed_length_is(item, flex_margin_opposite(start), CSS_KEYWORD_AUTO);
}

/* Is the member's inline axis parallel to the main axis? The container is `horizontal-tb` (core/layout/
   flex_position.h), so for a `horizontal-tb` item exactly when the main axis is the inline one; §4's anonymous
   item has the container's writing mode. */
static bool fb_parallel(lxb_dom_element_t *container, const FlexLineMember *m)
{
    char nbuf[160];

    if (!m->anonymous && !css_computed_keyword_is(lxb_dom_interface_element(m->first), "writing-mode",
                                                  CSS_KEYWORD_HORIZONTAL_TB))
        DFAILF("%s: a baseline-aligned flex item whose writing mode is not its container's is a css-writing-modes-4 "
               "§7.3 \"Orthogonal Flows\" box; build its axes and its baseline in that mode together",
               box_subject_node(m->first, nbuf, sizeof nbuf));
    return flex_container_main_axis(container) == FLEX_MAIN_AXIS_INLINE;
}

bool flex_baseline_participates(lxb_dom_element_t *container, const FlexLineMember *m)
{
    return fb_aligns(container, m) && fb_parallel(container, m);
}

/* An element item's alignment baseline. css-align-3 §9.1: a block container's first/last baseline set "is taken
   from the first/last in-flow line box in the block container or the first/last in-flow block-level child in the
   block container that contributes a set of first/last baselines", which core/layout/block_flow.h's two walks
   answer as a distance from the content edge; with none, "If a box that participates in baseline alignment has
   no baseline set, then its alignment baseline is synthesized according to the rules of the formatting context
   in which it participates", which is §8.3's border box, its alphabetic baseline "from the line-under line" (the
   bottom border edge in `horizontal-tb`). A replaced element has no baseline set. */
static FlexBaselineExtent fb_element_extent(lxb_dom_element_t *item, bool last)
{
    FlexBaselineExtent e;
    CssPx b[4], mt = used_value_px(item, "margin-top"), mb = used_value_px(item, "margin-bottom");
    CssPx border_box = used_value_border_edge_px(item, true), inner = css_px(0.0);
    CssDisplay d = css_computed_display(item);
    bool has = false;
    char nbuf[160];

    if (!replaced_element_of(item).replaced) {
        if (!block_flow_display_is_block_container(d))
            DFAILF("%s: a baseline-aligned flex item that is not a block container takes its baseline set from "
                   "its own layout module (css-align-3 §9.1: tables, flex containers per css-flexbox-1 §8.5 "
                   "\"Flex Container Baselines\", grid containers); build that module's baseline",
                   box_subject(item, nbuf, sizeof nbuf));
        has = last ? block_flow_last_line_box_baseline(item, &inner) : block_flow_first_line_box_baseline(item, &inner);
    }
    if (has) {
        used_value_border_widths_px(item, b);
        e.before = css_px_add(css_px_add(mt, b[0]), css_px_add(used_value_px(item, "padding-top"), inner));
    } else {
        e.before = css_px_add(mt, border_box);
    }
    e.after = css_px_sub(css_px_add(css_px_add(mt, border_box), mb), e.before);
    return e;
}

/* A member whose inline axis is not parallel to the main axis (a `horizontal-tb` item of a `column`
   container) has no baseline in the cross axis, so css-flexbox-1 §8.3 synthesizes one from its border box.
   css-align-3 §9.1: "when that writing mode's block flow direction is parallel to the axis of the alignment
   context, an axis-compatible writing mode must be assumed: ... If the box's own writing mode is horizontal,
   assume vertical-lr if direction is ltr and vertical-rl if direction is rtl", and in both "line-over is on
   right, and line-under is on left" (css-writing-modes-4 §6.3 "Line-relative Directions"), so the alphabetic
   baseline is the item's left border edge. */
static FlexBaselineExtent fb_synthesized_extent(lxb_dom_element_t *container, const FlexLineMember *m)
{
    FlexBaselineExtent e;
    lxb_dom_element_t *item;
    const char *start = flex_container_start_margin(container, FLEX_AXIS_CROSS);
    CssPx ml, mr, width;
    char nbuf[160];

    DCHECK(!flex_container_axis_is_vertical(container, FLEX_AXIS_CROSS),
           "a baseline was synthesized across the main axis of a container whose cross axis is the vertical one");
    if (m->anonymous)
        DFAILF("%s: §4's anonymous item of a `column` container synthesizes its baseline from its border box, "
               "whose width is its used cross size; build that size for the anonymous item first",
               box_subject_node(m->first, nbuf, sizeof nbuf));
    item = lxb_dom_interface_element(m->first);
    ml = used_value_px(item, "margin-left");
    mr = used_value_px(item, "margin-right");
    width = used_value_border_edge_px(item, false);
    e.before = flex_margin_is_low(start) ? ml : css_px_add(mr, width);
    e.after = css_px_sub(css_px_add(css_px_add(ml, width), mr), e.before);
    return e;
}

/* The member's alignment baseline as css-align-3 §4.2's preference names it. */
static FlexBaselineExtent fb_member_extent(lxb_dom_element_t *container, const FlexLineMember *m)
{
    FlexBaselineExtent e;
    CssPx first, last, inner;
    bool want_last;

    DCHECK(fb_aligns(container, m), "a baseline was measured for a member that is not baseline-aligned");
    if (!fb_parallel(container, m)) return fb_synthesized_extent(container, m);
    DCHECK(flex_container_axis_is_vertical(container, FLEX_AXIS_CROSS),
           "a member whose inline axis is parallel to the main axis was measured across a horizontal cross axis");
    want_last = fb_align(container, m).baseline == CSS_KEYWORD_LAST;
    if (!m->anonymous) return fb_element_extent(lxb_dom_interface_element(m->first), want_last);
    /* §4's anonymous item is an anonymous block container holding only line boxes, with no edges. */
    inner = flex_cross_size_anonymous_lines(container, m, &first, &last);
    e.before = want_last ? last : first;
    e.after = css_px_sub(inner, e.before);
    return e;
}

static void fb_take(FlexBaselineExtent *group, FlexBaselineExtent e, bool *any)
{
    group->before = *any ? css_px_max(group->before, e.before) : e.before;
    group->after = *any ? css_px_max(group->after, e.after) : e.after;
    *any = true;
}

/* The maxima over the members of `line` that step 8.1 collects (`pref` INVALID), or over the baseline-aligned
   members whose baseline preference (css-align-3 §4.2: "either “first” or “last”") is `pref`, which css-align-3
   §9.2 makes one baseline-sharing group: one line is one shared alignment context, and every item in it has the
   container's block flow direction. */
static bool fb_group_extent(lxb_dom_element_t *container, size_t line, CssKeyword pref, FlexBaselineExtent *out)
{
    FlexLineMember *m = NULL;
    size_t n = flex_line_members(container, &m), i;
    bool any = false;

    out->before = out->after = css_px(0.0);
    for (i = 0; i < n; i++) {
        if (m[i].line != line) continue;
        if (pref == CSS_KEYWORD_INVALID ? !flex_baseline_participates(container, &m[i])
                                        : !fb_aligns(container, &m[i]) || fb_align(container, &m[i]).baseline != pref)
            continue;
        fb_take(out, fb_member_extent(container, &m[i]), &any);
    }
    free(m);
    return any;
}

bool flex_baseline_line_extent(lxb_dom_element_t *container, size_t line, FlexBaselineExtent *out)
{
    return fb_group_extent(container, line, CSS_KEYWORD_INVALID, out);
}

CssPx flex_baseline_margin_offset(lxb_dom_element_t *container, lxb_dom_element_t *item, CssPx line_size)
{
    FlexLineMember *m = NULL;
    size_t n = flex_line_members(container, &m), i;
    const FlexLineMember *self = NULL;
    FlexBaselineExtent group, e;
    CssKeyword pref;
    CssPx at;
    char nbuf[160];

    for (i = 0; i < n && self == NULL; i++)
        if (!m[i].anonymous && m[i].first == lxb_dom_interface_node(item)) self = &m[i];
    DCHECKF(self != NULL, "%s: §9.3 collected no flex line member for this item of its container",
            box_subject(item, nbuf, sizeof nbuf));
    pref = fb_align(container, self).baseline;
    e = fb_member_extent(container, self);
    (void)fb_group_extent(container, self->line, pref, &group);
    free(m);
    /* css-align-3 §9.3 aligns the group's baselines; css-flexbox-1 §8.3 then puts "the item with the largest
       distance between its baseline and its cross-start margin edge" flush with the line's cross-start edge,
       which is the `safe self-start` fallback of `first baseline`. `last baseline`'s `safe self-end` puts the
       largest distance to the cross-end margin edge flush with the cross-end edge, unless the group overflows
       the line, where `safe` aligns it as `start` (css-align-3 §4.4). */
    if (pref == CSS_KEYWORD_LAST && css_px_add(group.before, group.after).px <= line_size.px)
        at = css_px_sub(css_px_sub(line_size, group.after), e.before);
    else
        at = css_px_sub(group.before, e.before);
    return at;
}
