/* css-flexbox-1 §9.5 "Main-Axis Alignment" and §9.6 "Cross-Axis Alignment"; see flex_position.h. */
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include <lexbor/dom/dom.h>

#include "check.h"
#include "core/css/css_computed_value.h"
#include "core/css/css_keyword.h"
#include "core/css/css_length.h"
#include "core/layout/box_subject.h"
#include "core/layout/box_tree.h"
#include "core/layout/flex_cross_size.h"
#include "core/layout/flex_item.h"
#include "core/layout/flex_line.h"
#include "core/layout/flex_position.h"
#include "core/layout/used_value.h"

/* One item's main-axis terms: its border-box extent, and its two margins with `auto` ones zero and flagged. */
typedef struct {
    CssPx border, m_start, m_end;
    bool  auto_start, auto_end;
} FpMain;

/* A margin on one side of `el`, `auto` read as computed (used_value.c refuses an `auto` flex-item margin). */
static CssPx fp_margin(lxb_dom_element_t *el, const char *name, bool *is_auto)
{
    *is_auto = css_computed_length_is(el, name, CSS_KEYWORD_AUTO);
    return *is_auto ? css_px(0.0) : used_value_px(el, name);
}

/* §9.5 step 1's operand for one member. An anonymous item (§4) has the initial margins, border and padding,
   so its border box is its used main size. `start_left` names which physical side is main-start. */
static FpMain fp_main_terms(lxb_dom_element_t *container, const FlexLineMember *m, bool start_left)
{
    FpMain t = { css_px(0.0), css_px(0.0), css_px(0.0), false, false };
    lxb_dom_element_t *el;
    char nbuf[160];

    if (m->anonymous) {
        t.border = flex_line_used_main_size(container, m->first);
        return t;
    }
    el = lxb_dom_interface_element(m->first);
    if (flex_item_is_collapsed(el))
        DFAILF("%s: css-flexbox-1 §4.4 \"Collapsed Items\" leaves a collapsed item a strut on its line, which "
               "§9.5 places with zero main size; build the strut here before positioning its siblings",
               box_subject(el, nbuf, sizeof nbuf));
    t.border = used_value_border_edge_px(el, false);
    t.m_start = fp_margin(el, start_left ? "margin-left" : "margin-right", &t.auto_start);
    t.m_end = fp_margin(el, start_left ? "margin-right" : "margin-left", &t.auto_end);
    return t;
}

/* css-align-3 §4.1 "Positional Alignment: the center, start, end, self-start, self-end, flex-start, flex-end,
   left, and right keywords"' `justify-content` position `v` on §9.5's main axis, as the share of the free space
   before the first item: 0 at main-start, 1 at main-end. `start` and `end` are the container's inline edges and
   `left` and `right` physical ones; `start_left` says whether main-start is the left edge. */
static double fp_main_share(lxb_dom_element_t *container, CssKeyword v, bool start_left)
{
    bool start_is_main_start = start_left == !css_computed_keyword_is(container, "direction", CSS_KEYWORD_RTL);

    switch (v) {
    case CSS_KEYWORD_FLEX_START: return 0.0;
    case CSS_KEYWORD_FLEX_END:   return 1.0;
    case CSS_KEYWORD_CENTER:     return 0.5;
    case CSS_KEYWORD_START:      return start_is_main_start ? 0.0 : 1.0;
    case CSS_KEYWORD_END:        return start_is_main_start ? 1.0 : 0.0;
    case CSS_KEYWORD_LEFT:       return start_left ? 0.0 : 1.0;
    case CSS_KEYWORD_RIGHT:      return start_left ? 1.0 : 0.0;
    default:
        DFAILF("`%s` reached the main-axis share and is no `justify-content` position", css_keyword_name(v));
        return 0.0;
    }
}

/* §9.5 for the one line, as the distance from the line's main-start edge to `item`'s border box's main-start
   edge, with `*border` its main extent and `*ms`, `*me` its used main-start and main-end margins. Free space is
   decided on the examples, as css_length.h's extremes are. */
static CssPx fp_main_offset(lxb_dom_element_t *container, lxb_dom_element_t *item, bool start_left, CssPx *border,
                            CssPx *ms, CssPx *me)
{
    FlexLineMember *m = NULL;
    size_t n = flex_line_members(container, &m), i, nauto = 0;
    FpMain *t;
    CssPx free_space = used_value_content_px(container, false), lead = css_px(0.0), gap = css_px(0.0);
    CssPx share = css_px(0.0), pos, at = css_px(0.0);
    bool found = false;
    CssAlign jc;
    char nbuf[160];

    t = n == 0 ? NULL : calloc(n, sizeof *t);
    CHECK(n == 0 || t != NULL, "out of memory holding one flex line's main-axis terms");
    for (i = 0; i < n; i++) {
        t[i] = fp_main_terms(container, &m[i], start_left);
        free_space = css_px_sub(free_space, css_px_add(t[i].border, css_px_add(t[i].m_start, t[i].m_end)));
        nauto += (size_t)t[i].auto_start + (size_t)t[i].auto_end;
    }
    /* Step 1: "If the remaining free space is positive and at least one main-axis margin on this line is auto,
       distribute the free space equally among these margins. Otherwise, set all auto margins to zero." */
    if (free_space.px > 0.0 && nauto > 0) {
        share = css_px_scale(free_space, 1.0 / (double)nauto);
        free_space = css_px(0.0);
    }
    /* Step 2, over css-align-3 §5.1's `justify-content`. A distribution that cannot distribute takes
       css-align-3 §4.3's fallback, `safe flex-start` or `safe center`, and css-align-3 §4.4's `safe` aligns an
       overflowing subject "as if the alignment mode were flex-start": with no positive free space all three
       lead with nothing. */
    jc = css_computed_align(container, "justify-content");
    switch (jc.value) {
    /* css-align-3 §5.1.3 "Flex Containers": `normal` "behaves as stretch", and `stretch` "behaves as
       flex-start" because flex controls main-axis stretching. */
    case CSS_KEYWORD_NORMAL:
    case CSS_KEYWORD_STRETCH:
        break;
    case CSS_KEYWORD_SPACE_BETWEEN:
        if (free_space.px > 0.0 && n > 1) gap = css_px_scale(free_space, 1.0 / (double)(n - 1));
        break;
    case CSS_KEYWORD_SPACE_AROUND:
        if (free_space.px > 0.0) {
            gap = css_px_scale(free_space, 1.0 / (double)n);
            lead = css_px_scale(gap, 0.5);
        }
        break;
    case CSS_KEYWORD_SPACE_EVENLY:
        if (free_space.px > 0.0) {
            gap = css_px_scale(free_space, 1.0 / (double)(n + 1));
            lead = gap;
        }
        break;
    default:
        if (jc.overflow != CSS_KEYWORD_SAFE || free_space.px >= 0.0)
            lead = css_px_scale(free_space, fp_main_share(container, jc.value, start_left));
        break;
    }
    pos = lead;
    for (i = 0; i < n; i++) {
        pos = css_px_add(pos, t[i].auto_start ? share : t[i].m_start);
        if (!m[i].anonymous && m[i].first == lxb_dom_interface_node(item)) {
            at = pos;
            *border = t[i].border;
            *ms = t[i].auto_start ? share : t[i].m_start;
            *me = t[i].auto_end ? share : t[i].m_end;
            found = true;
        }
        pos = css_px_add(css_px_add(pos, t[i].border), css_px_add(t[i].auto_end ? share : t[i].m_end, gap));
    }
    DCHECKF(found, "%s: §9.3's step 5 collected no flex item for this element of its container",
            box_subject(item, nbuf, sizeof nbuf));
    free(t);
    free(m);
    return at;
}

/* §9.6 step 13 for `item` on the one line, whose cross size is `line`: its used top and bottom margins, and
   whether either was `auto` (step 14 then does not align it). */
static bool fp_cross_margins(lxb_dom_element_t *item, CssPx line, CssPx *mt, CssPx *mb)
{
    bool auto_top, auto_bottom;
    CssPx outer, diff;

    *mt = fp_margin(item, "margin-top", &auto_top);
    *mb = fp_margin(item, "margin-bottom", &auto_bottom);
    if (!auto_top && !auto_bottom) return false;
    outer = css_px_add(used_value_border_edge_px(item, true), css_px_add(*mt, *mb));
    diff = css_px_sub(line, outer);
    /* "If its outer cross size (treating those auto margins as zero) is less than the cross size of its flex
       line, distribute the difference in those sizes equally to the auto margins. Otherwise, if the
       block-start or inline-start margin (whichever is in the cross axis) is auto, set it to zero. Set the
       opposite margin so that the outer cross size of the item equals the cross size of its flex line." */
    if (diff.px > 0.0) {
        CssPx each = css_px_scale(diff, auto_top && auto_bottom ? 0.5 : 1.0);

        if (auto_top) *mt = each;
        if (auto_bottom) *mb = each;
        return true;
    }
    *mb = css_px_add(*mb, diff);
    return true;
}

/* §9.6 steps 13 and 14 for `item` on the one line, whose cross size is `line`: the distance from the line's
   cross-start edge (the top, in `horizontal-tb`) to the item's border box. */
static CssPx fp_cross_offset(lxb_dom_element_t *container, lxb_dom_element_t *item, CssPx line)
{
    CssPx mt, mb, outer, diff;
    CssAlign align;
    double share = 0.0;
    char nbuf[160];

    if (fp_cross_margins(item, line, &mt, &mb)) return mt;
    outer = css_px_add(used_value_border_edge_px(item, true), css_px_add(mt, mb));
    diff = css_px_sub(line, outer);
    /* Step 14, over css-align-3 §6.2's `align-self`: the margin box is what is aligned. The line's cross-start
       is its top, which is also the container's block-start: fp_require_built admits no `wrap-reverse`. */
    align = flex_cross_size_resolved_align(container, item);
    switch (align.value) {
    /* css-align-3 §6.2.4 "Flex Items": `normal` "Behaves as stretch". A stretched item's used cross size
       already fills the line (§9.4 step 11), and one that is not stretched takes css-align-3 §4.3's
       `flex-start` fallback. */
    case CSS_KEYWORD_NORMAL:
    case CSS_KEYWORD_STRETCH:
    case CSS_KEYWORD_FLEX_START:
    case CSS_KEYWORD_START:
        break;
    case CSS_KEYWORD_FLEX_END:
    case CSS_KEYWORD_END:
        share = 1.0;
        break;
    case CSS_KEYWORD_CENTER:
        share = 0.5;
        break;
    /* css-align-3 §4.1: the item's own start or end side in the cross axis, which is its block-start only for an item
       whose writing mode is the container's. */
    case CSS_KEYWORD_SELF_START:
    case CSS_KEYWORD_SELF_END:
        if (!css_computed_keyword_is(item, "writing-mode", CSS_KEYWORD_HORIZONTAL_TB))
            DFAILF("%s: `align-self: %s` on an item whose writing mode is not its container's names the item's "
                   "own start side in the cross axis; build that side's mapping before placing it",
                   box_subject(item, nbuf, sizeof nbuf), css_keyword_name(align.value));
        share = align.value == CSS_KEYWORD_SELF_END ? 1.0 : 0.0;
        break;
    case CSS_KEYWORD_BASELINE:
        DFAILF("%s: `align-self: %s baseline` aligns the item's baseline with the line's baseline group (§9.4 "
               "step 8.1); build that alignment over core/layout/block_flow.h's first-line-box baseline before "
               "placing it",
               box_subject(item, nbuf, sizeof nbuf), css_keyword_name(align.baseline));
        break;
    default:
        DFAILF("%s: `%s` is no value css-align-3 §6.2's `align-self` or css-align-3 §7.2's `align-items` resolves to",
               box_subject(item, nbuf, sizeof nbuf), css_keyword_name(align.value));
        break;
    }
    /* css-align-3 §4.4's `safe`: an overflowing subject is aligned "as if the alignment mode were flex-start". */
    if (align.overflow == CSS_KEYWORD_SAFE && diff.px < 0.0) share = 0.0;
    return css_px_add(css_px_scale(diff, share), mt);
}

/* The checks every entry makes, and which physical side is main-start (§5.1: `row` main-start is the inline-start
   edge, the left in `ltr`; `row-reverse` swaps it). */
static bool fp_require_built(lxb_dom_element_t *container, lxb_dom_element_t *item)
{
    char nbuf[160];

    DCHECK(container != NULL && item != NULL, "a flex item's position was asked with no container or no item");
    DCHECKF(box_tree_parent(box_tree_child_of_node(lxb_dom_interface_node(item))) == container &&
                flex_item_display_is_flex_container(css_computed_display(container)),
            "%s: a flex item's position was asked against an element that is not its flex container",
            box_subject(item, nbuf, sizeof nbuf));
    if (!css_computed_keyword_is(container, "writing-mode", CSS_KEYWORD_HORIZONTAL_TB) ||
        flex_container_main_axis(container) != FLEX_MAIN_AXIS_INLINE || flex_container_is_multi_line(container))
        DFAILF("%s: css-flexbox-1 §9.5 and §9.6 are built for a single-line `row` container in `horizontal-tb`. "
               "Build the line placement this container needs: §9.4's per-line cross sizes and `align-content` "
               "for `wrap`, §9.5 over the block axis for `column`",
               box_subject(container, nbuf, sizeof nbuf));
    return css_computed_keyword_is(container, "flex-direction", CSS_KEYWORD_ROW_REVERSE) ==
           css_computed_keyword_is(container, "direction", CSS_KEYWORD_RTL);
}

CssPx flex_position_item_margin(lxb_dom_element_t *container, lxb_dom_element_t *item, const char *name)
{
    bool start_left = fp_require_built(container, item);
    CssPx border = css_px(0.0), ms = css_px(0.0), me = css_px(0.0), mt, mb;
    bool left = strcmp(name, "margin-left") == 0, right = strcmp(name, "margin-right") == 0;

    if (left || right) {
        (void)fp_main_offset(container, item, start_left, &border, &ms, &me);
        return left == start_left ? ms : me;
    }
    DCHECKF(strcmp(name, "margin-top") == 0 || strcmp(name, "margin-bottom") == 0,
            "`%s`: a flex item's used margin was asked for a property that is not a margin", name);
    (void)fp_cross_margins(item, used_value_content_px(container, true), &mt, &mb);
    return strcmp(name, "margin-top") == 0 ? mt : mb;
}

void flex_position_item_offset(lxb_dom_element_t *container, lxb_dom_element_t *item, CssPx *x, CssPx *y)
{
    bool start_left;
    CssPx border = css_px(0.0), ms = css_px(0.0), me = css_px(0.0), main;

    DCHECK(x != NULL && y != NULL, "a flex item's position was asked with nowhere to put it");
    start_left = fp_require_built(container, item);
    main = fp_main_offset(container, item, start_left, &border, &ms, &me);
    *x = start_left ? main : css_px_sub(css_px_sub(used_value_content_px(container, false), main), border);
    /* §8.4 "Packing Flex Lines: the align-content property": "In a single-line flex container, the cross size of
       the line is the cross size of the flex container", its inner (content-box) one. */
    *y = fp_cross_offset(container, item, used_value_content_px(container, true));
}
