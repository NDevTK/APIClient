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
#include "core/layout/flex_line.h"
#include "core/layout/replaced_element.h"
#include "core/layout/used_value.h"

/* The member's resolved `align-self` (css-flexbox-1 §8.3: "For anonymous flex items, align-self always matches
   the value of align-items on their associated flex container"). */
static CssAlign fb_align(lxb_dom_element_t *container, const FlexLineMember *m)
{
    return flex_cross_size_resolved_align(container, m->anonymous ? NULL : lxb_dom_interface_element(m->first));
}

bool flex_baseline_participates(lxb_dom_element_t *container, const FlexLineMember *m)
{
    lxb_dom_element_t *item;
    char nbuf[160];

    DCHECK(container != NULL && m != NULL && m->first != NULL, "baseline participation was asked of no member");
    if (fb_align(container, m).value != CSS_KEYWORD_BASELINE) return false;
    if (m->anonymous) return true;
    item = lxb_dom_interface_element(m->first);
    if (css_computed_length_is(item, "margin-top", CSS_KEYWORD_AUTO) ||
        css_computed_length_is(item, "margin-bottom", CSS_KEYWORD_AUTO))
        return false;
    /* The container is `horizontal-tb` and its main axis its inline one, so an item's inline axis is parallel
       to it exactly when the item is `horizontal-tb` too. */
    if (!css_computed_keyword_is(item, "writing-mode", CSS_KEYWORD_HORIZONTAL_TB))
        DFAILF("%s: a baseline-aligned flex item whose inline axis is not parallel to the main axis is left out "
               "of §9.4 step 8.1 and aligns by a baseline synthesized from its border box (§8.3); build that "
               "item's step 8.2 measurement and its synthesized baseline together",
               box_subject(item, nbuf, sizeof nbuf));
    return true;
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

FlexBaselineExtent flex_baseline_member_extent(lxb_dom_element_t *container, const FlexLineMember *m)
{
    FlexBaselineExtent e;
    CssPx first, last, inner;
    bool want_last;

    DCHECK(flex_baseline_participates(container, m), "a baseline was measured for a member that does not participate");
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

/* The maxima over the participating members of `line`; with `pref` other than INVALID, only those whose
   baseline preference (css-align-3 §4.2: "either “first” or “last”") is `pref`, which css-align-3 §9.2 makes
   one baseline-sharing group: one line is one shared alignment context, and every item in it has the
   container's block flow direction. */
static bool fb_group_extent(lxb_dom_element_t *container, size_t line, CssKeyword pref, FlexBaselineExtent *out)
{
    FlexLineMember *m = NULL;
    size_t n = flex_line_members(container, &m), i;
    bool any = false;

    out->before = out->after = css_px(0.0);
    for (i = 0; i < n; i++) {
        if (m[i].line != line || !flex_baseline_participates(container, &m[i])) continue;
        if (pref != CSS_KEYWORD_INVALID && fb_align(container, &m[i]).baseline != pref) continue;
        fb_take(out, flex_baseline_member_extent(container, &m[i]), &any);
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
    e = flex_baseline_member_extent(container, self);
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
