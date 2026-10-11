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

/* §9.5 for the one line, as the distance from the line's main-start edge to `item`'s border box's main-start
   edge, with `*border` its main extent. Free space is decided on the examples, as css_length.h's extremes are. */
static CssPx fp_main_offset(lxb_dom_element_t *container, lxb_dom_element_t *item, bool start_left, CssPx *border)
{
    FlexLineMember *m = NULL;
    size_t n = flex_line_members(container, &m), i, nauto = 0;
    FpMain *t;
    CssPx free_space = used_value_content_px(container, false), lead = css_px(0.0), gap = css_px(0.0);
    CssPx share = css_px(0.0), pos, at = css_px(0.0);
    bool found = false;
    char *jc;
    char nbuf[160], vbuf[64];

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
    /* Step 2, §8.2's five values (its `Value:` line is the cascade's whole set). */
    jc = css_computed_value(container, "justify-content");
    DCHECK(jc != NULL, "the cascade produced no computed `justify-content`; lexbor's registry has an initial value");
    if (strcmp(jc, "flex-end") == 0) {
        lead = free_space;
    } else if (strcmp(jc, "center") == 0) {
        lead = css_px_scale(free_space, 0.5);
    } else if (strcmp(jc, "space-between") == 0) {
        /* "If the leftover free-space is negative or there is only a single flex item on the line, this value
           falls back to safe flex-start." */
        if (free_space.px > 0.0 && n > 1) gap = css_px_scale(free_space, 1.0 / (double)(n - 1));
    } else if (strcmp(jc, "space-around") == 0) {
        /* "If the leftover free-space is negative or there is only a single flex item on the line, this value
           falls back to safe center": safe alignment of an overflowing line is flex-start's. */
        if (free_space.px > 0.0) {
            gap = css_px_scale(free_space, 1.0 / (double)n);
            lead = css_px_scale(gap, 0.5);
        }
    } else {
        DCHECKF(strcmp(jc, "flex-start") == 0, "%s: computed `justify-content` `%s` is outside §8.2's `Value:` line",
                box_subject(container, nbuf, sizeof nbuf), box_subject_computed(container, "justify-content", vbuf,
                                                                                 sizeof vbuf));
    }
    free(jc);
    pos = lead;
    for (i = 0; i < n; i++) {
        pos = css_px_add(pos, t[i].auto_start ? share : t[i].m_start);
        if (!m[i].anonymous && m[i].first == lxb_dom_interface_node(item)) {
            at = pos;
            *border = t[i].border;
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

/* §9.6 steps 13 and 14 for `item` on the one line, whose cross size is `line`: the distance from the line's
   cross-start edge (the top, in `horizontal-tb`) to the item's border box. */
static CssPx fp_cross_offset(lxb_dom_element_t *container, lxb_dom_element_t *item, CssPx line)
{
    bool auto_top, auto_bottom;
    CssPx mt = fp_margin(item, "margin-top", &auto_top), mb = fp_margin(item, "margin-bottom", &auto_bottom);
    CssPx outer = css_px_add(used_value_border_edge_px(item, true), css_px_add(mt, mb));
    CssPx diff = css_px_sub(line, outer);
    char *align;
    CssPx at;
    char nbuf[160];

    /* Step 13: "If its outer cross size (treating those auto margins as zero) is less than the cross size of its
       flex line, distribute the difference in those sizes equally to the auto margins. Otherwise, if the
       block-start or inline-start margin (whichever is in the cross axis) is auto, set it to zero." */
    if (auto_top || auto_bottom) {
        if (diff.px > 0.0 && auto_top) return css_px_scale(diff, auto_bottom ? 0.5 : 1.0);
        return mt;
    }
    /* Step 14, per §8.3: the margin box is what is aligned. */
    align = flex_cross_size_resolved_align(container, item);
    if (strcmp(align, "flex-end") == 0) {
        at = css_px_add(diff, mt);
    } else if (strcmp(align, "center") == 0) {
        at = css_px_add(css_px_scale(diff, 0.5), mt);
    } else if (strcmp(align, "flex-start") == 0 || strcmp(align, "stretch") == 0) {
        /* A stretched item's used cross size already fills the line (§9.4 step 11), so it starts there. */
        at = mt;
    } else {
        DFAILF("%s: §8.3's `%s` aligns the item's baseline with the line's baseline group (§9.4 step 8.1); build "
               "that alignment over core/layout/block_flow.h's first-line-box baseline before placing it",
               box_subject(item, nbuf, sizeof nbuf), align);
        at = mt;
    }
    free(align);
    return at;
}

void flex_position_item_offset(lxb_dom_element_t *container, lxb_dom_element_t *item, CssPx *x, CssPx *y)
{
    bool reverse, rtl, start_left;
    CssPx border = css_px(0.0), main;
    char nbuf[160];

    DCHECK(container != NULL && item != NULL && x != NULL && y != NULL,
           "a flex item's position was asked with no container, no item or nowhere to put it");
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
    /* §5.1: `row` main-start is the inline-start edge, the left in `ltr`; `row-reverse` swaps it. */
    reverse = css_computed_keyword_is(container, "flex-direction", CSS_KEYWORD_ROW_REVERSE);
    rtl = css_computed_keyword_is(container, "direction", CSS_KEYWORD_RTL);
    start_left = reverse == rtl;
    main = fp_main_offset(container, item, start_left, &border);
    *x = start_left ? main : css_px_sub(css_px_sub(used_value_content_px(container, false), main), border);
    /* §8.4 "Packing Flex Lines: the align-content property": "In a single-line flex container, the cross size of
       the line is the cross size of the flex container", its inner (content-box) one. */
    *y = fp_cross_offset(container, item, used_value_content_px(container, true));
}
