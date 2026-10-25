/* css-flexbox-1 §9.5 "Main-Axis Alignment" and §9.6 "Cross-Axis Alignment"; see flex_position.h. */
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include <lexbor/dom/dom.h>

#include "check.h"
#include "core/css/css_computed_value.h"
#include "core/css/css_keyword.h"
#include "core/css/css_length.h"
#include "core/css/css_logical.h"
#include "core/layout/box_subject.h"
#include "core/layout/box_tree.h"
#include "core/layout/flex_baseline.h"
#include "core/layout/flex_cross_size.h"
#include "core/layout/flex_item.h"
#include "core/layout/flex_line.h"
#include "core/layout/flex_position.h"
#include "core/layout/used_value.h"
#include <lexbor/core/base.h>   /* lexbor_malloc: the allocator the engine gives its refusal edge */

/* The container's two axes as physical sides, derived once per entry: whether the main axis is the vertical one,
   and the margin longhand on its main-start and cross-start sides (core/layout/flex_item.h). */
typedef struct {
    bool        main_vertical;
    const char *main_start;   /* the margin on the main-start side */
    const char *cross_start;  /* the margin on the cross-start side */
} FpAxes;

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
   so its border box is its used main size. */
static FpMain fp_main_terms(lxb_dom_element_t *container, const FlexLineMember *m, FpAxes a)
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
    t.border = used_value_border_edge_px(el, a.main_vertical);
    t.m_start = fp_margin(el, a.main_start, &t.auto_start);
    t.m_end = fp_margin(el, flex_margin_opposite(a.main_start), &t.auto_end);
    return t;
}

/* css-align-3 §4.1 "Positional Alignment: the center, start, end, self-start, self-end, flex-start, flex-end,
   left, and right keywords"' `justify-content` position `v` on §9.5's main axis, as the share of the free space
   before the first item: 0 at main-start, 1 at main-end. `start` and `end` are the container's own edges in the
   main axis and `left` and `right` physical ones, which in a `column` container "behaves as start" because the
   axis "is not parallel with either left↔right axis". */
static double fp_main_share(lxb_dom_element_t *container, CssKeyword v, FpAxes a)
{
    bool start_is_main_start = !css_computed_keyword_is(container, "flex-direction", CSS_KEYWORD_ROW_REVERSE) &&
                               !css_computed_keyword_is(container, "flex-direction", CSS_KEYWORD_COLUMN_REVERSE);

    if (a.main_vertical && (v == CSS_KEYWORD_LEFT || v == CSS_KEYWORD_RIGHT)) v = CSS_KEYWORD_START;
    switch (v) {
    case CSS_KEYWORD_FLEX_START: return 0.0;
    case CSS_KEYWORD_FLEX_END:   return 1.0;
    case CSS_KEYWORD_CENTER:     return 0.5;
    case CSS_KEYWORD_START:      return start_is_main_start ? 0.0 : 1.0;
    case CSS_KEYWORD_END:        return start_is_main_start ? 1.0 : 0.0;
    case CSS_KEYWORD_LEFT:       return flex_margin_is_low(a.main_start) ? 0.0 : 1.0;
    case CSS_KEYWORD_RIGHT:      return flex_margin_is_low(a.main_start) ? 1.0 : 0.0;
    default:
        DFAILF("`%s` reached the main-axis share and is no `justify-content` position", css_keyword_name(v));
        return 0.0;
    }
}

/* The members of the flex line holding `item`, in order, to a calloc'd `*out` the caller frees; returns how many.
   §9.5 and §9.6 run per line. */
static size_t fp_line_of(lxb_dom_element_t *container, lxb_dom_element_t *item, FlexLineMember **out)
{
    FlexLineMember *m = NULL;
    size_t n = flex_line_members(container, &m), i, k = 0, line = 0;
    bool found = false;
    char nbuf[160];

    for (i = 0; i < n; i++)
        if (!m[i].anonymous && m[i].first == lxb_dom_interface_node(item)) { line = m[i].line; found = true; }
    DCHECKF(found, "%s: §9.3's step 5 collected no flex item for this element of its container",
            box_subject(item, nbuf, sizeof nbuf));
    for (i = 0; i < n; i++)
        if (m[i].line == line) m[k++] = m[i];
    *out = m;
    return k;
}

/* §9.5 for the line holding `item`, as the distance from the line's main-start edge to `item`'s border box's main-start
   edge, with `*border` its main extent and `*ms`, `*me` its used main-start and main-end margins. Free space is
   decided on the examples, as css_length.h's extremes are. */
static CssPx fp_main_offset(lxb_dom_element_t *container, lxb_dom_element_t *item, FpAxes a, CssPx *border,
                            CssPx *ms, CssPx *me)
{
    FlexLineMember *m = NULL;
    size_t n = fp_line_of(container, item, &m), i, nauto = 0;
    FpMain *t;
    CssPx free_space = used_value_content_px(container, a.main_vertical), lead = css_px(0.0), gap = css_px(0.0);
    CssPx share = css_px(0.0), pos, at = css_px(0.0);
    bool found = false;
    CssAlign jc;
    char nbuf[160];

    t = n == 0 ? NULL : lexbor_calloc(n, sizeof *t);
    CHECK(n == 0 || t != NULL, "out of memory holding one flex line's main-axis terms");
    for (i = 0; i < n; i++) {
        t[i] = fp_main_terms(container, &m[i], a);
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
            lead = css_px_scale(free_space, fp_main_share(container, jc.value, a));
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

/* §9.6 step 13 for `item` on the one line, whose cross size is `line`: its used cross-start and cross-end
   margins, and whether either was `auto` (step 14 then does not align it). */
static bool fp_cross_margins(lxb_dom_element_t *item, FpAxes a, CssPx line, CssPx *mt, CssPx *mb)
{
    bool auto_top, auto_bottom;
    CssPx outer, diff;

    *mt = fp_margin(item, a.cross_start, &auto_top);
    *mb = fp_margin(item, flex_margin_opposite(a.cross_start), &auto_bottom);
    if (!auto_top && !auto_bottom) return false;
    outer = css_px_add(used_value_border_edge_px(item, !a.main_vertical), css_px_add(*mt, *mb));
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
   cross-start edge to the item's border box. */
static CssPx fp_cross_offset(lxb_dom_element_t *container, lxb_dom_element_t *item, FpAxes a, CssPx line)
{
    CssPx mt, mb, outer, diff;
    CssAlign align;
    double share = 0.0;
    char nbuf[160];

    if (fp_cross_margins(item, a, line, &mt, &mb)) return mt;
    outer = css_px_add(used_value_border_edge_px(item, !a.main_vertical), css_px_add(mt, mb));
    diff = css_px_sub(line, outer);
    /* Step 14, over css-align-3 §6.2's `align-self`: the margin box is what is aligned. The line's cross-start
       is the container's block-start (`row`) or inline-start (`column`) side: fp_require_built admits no
       `wrap-reverse`, so `start` and `flex-start` name one edge. */
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
    /* css-align-3 §4.1: "the edge of the alignment container corresponding to the alignment subject's start
       side in the appropriate axis", the item's own block-start (`row`) or inline-start (`column`) side, which
       its own writing mode and direction map. */
    case CSS_KEYWORD_SELF_START:
    case CSS_KEYWORD_SELF_END: {
        const char *self_start;

        if (!css_computed_keyword_is(item, "writing-mode", CSS_KEYWORD_HORIZONTAL_TB))
            DFAILF("%s: `align-self: %s` on an item whose writing mode is not its container's names the item's "
                   "own start side in the cross axis; build that side's mapping before placing it",
                   box_subject(item, nbuf, sizeof nbuf), css_keyword_name(align.value));
        self_start = css_logical_partner_of(item, a.main_vertical ? "margin-inline-start" : "margin-block-start");
        share = (strcmp(self_start, a.cross_start) == 0) == (align.value == CSS_KEYWORD_SELF_START) ? 0.0 : 1.0;
        break;
    }
    /* css-align-3 §9.3 places the item by its baseline-sharing group, fallback alignment included, so the
       share and the `safe` test below do not apply. */
    case CSS_KEYWORD_BASELINE:
        return css_px_add(flex_baseline_margin_offset(container, item, line), mt);
    default:
        DFAILF("%s: `%s` is no value css-align-3 §6.2's `align-self` or css-align-3 §7.2's `align-items` resolves to",
               box_subject(item, nbuf, sizeof nbuf), css_keyword_name(align.value));
        break;
    }
    /* css-align-3 §4.4's `safe`: an overflowing subject is aligned "as if the alignment mode were flex-start". */
    if (align.overflow == CSS_KEYWORD_SAFE && diff.px < 0.0) share = 0.0;
    return css_px_add(css_px_scale(diff, share), mt);
}

/* The checks every entry makes, and the container's axes. */
static FpAxes fp_require_built(lxb_dom_element_t *container, lxb_dom_element_t *item)
{
    FpAxes a;
    char nbuf[160];

    DCHECK(container != NULL && item != NULL, "a flex item's position was asked with no container or no item");
    DCHECKF(box_tree_parent(box_tree_child_of_node(lxb_dom_interface_node(item))) == container &&
                flex_item_display_is_flex_container(css_computed_display(container)),
            "%s: a flex item's position was asked against an element that is not its flex container",
            box_subject(item, nbuf, sizeof nbuf));
    if (!css_computed_keyword_is(container, "writing-mode", CSS_KEYWORD_HORIZONTAL_TB) ||
        css_computed_keyword_is(container, "flex-wrap", CSS_KEYWORD_WRAP_REVERSE))
        DFAILF("%s: css-flexbox-1 §9.5 and §9.6 are built for `nowrap` and `wrap` in `horizontal-tb`. Build "
               "the placement this container needs: §5.2's `wrap-reverse`, which swaps cross-start and "
               "cross-end and stacks the lines from the other edge, or the item axes of a vertical writing mode",
               box_subject(container, nbuf, sizeof nbuf));
    a.main_vertical = flex_container_axis_is_vertical(container, FLEX_AXIS_MAIN);
    a.main_start = flex_container_start_margin(container, FLEX_AXIS_MAIN);
    a.cross_start = flex_container_start_margin(container, FLEX_AXIS_CROSS);
    return a;
}

/* §9.6 step 15 for `item`'s line in a multi-line container: its cross size, and the distance from the
   container's cross-start content edge to the line's cross-start edge, over css-align-3 §5.1's `align-content`
   (§5.1.3: the flex lines are the alignment subjects in the container's content box, and `normal` "behaves as
   stretch", which §9.4's step 9 has already applied). A distribution with nothing to distribute takes css-align-3
   §4.3's default fallback ("safe flow-start" for space-between, "safe center" for the other two), and §4.4's
   `safe` aligns an overflowing subject as if `start`. A single-line container's one line is its inner cross
   size (§6 "Flex Lines"). */
static CssPx fp_line_place(lxb_dom_element_t *container, lxb_dom_element_t *item, FpAxes a, CssPx *start)
{
    FlexLineMember *m = NULL;
    CssPx *sizes = NULL, inner = used_value_content_px(container, !a.main_vertical), free_space, gap = css_px(0.0);
    CssPx lead = css_px(0.0), out;
    size_t n, lines, line = 0, i;
    CssAlign ac;
    char nbuf[160];

    *start = css_px(0.0);
    if (!flex_container_is_multi_line(container)) return inner;
    n = flex_line_members(container, &m);
    for (i = 0; i < n; i++)
        if (!m[i].anonymous && m[i].first == lxb_dom_interface_node(item)) line = m[i].line;
    free(m);
    lines = flex_cross_size_lines(container, &sizes);
    DCHECK(line < lines, "an item's §9.3 line is past the lines §9.4 measured over the same walk");
    free_space = inner;
    for (i = 0; i < lines; i++) free_space = css_px_sub(free_space, sizes[i]);
    ac = css_computed_align(container, "align-content");
    switch (ac.value) {
    case CSS_KEYWORD_NORMAL:
    case CSS_KEYWORD_STRETCH:
    case CSS_KEYWORD_FLEX_START:
    case CSS_KEYWORD_START:
        break;
    case CSS_KEYWORD_FLEX_END:
    case CSS_KEYWORD_END:
        if (ac.overflow != CSS_KEYWORD_SAFE || free_space.px >= 0.0) lead = free_space;
        break;
    case CSS_KEYWORD_CENTER:
        if (ac.overflow != CSS_KEYWORD_SAFE || free_space.px >= 0.0) lead = css_px_scale(free_space, 0.5);
        break;
    case CSS_KEYWORD_SPACE_BETWEEN:
        if (free_space.px > 0.0 && lines > 1) gap = css_px_scale(free_space, 1.0 / (double)(lines - 1));
        break;
    case CSS_KEYWORD_SPACE_AROUND:
        if (free_space.px > 0.0) {
            gap = css_px_scale(free_space, 1.0 / (double)lines);
            lead = css_px_scale(gap, 0.5);
        }
        break;
    case CSS_KEYWORD_SPACE_EVENLY:
        if (free_space.px > 0.0) {
            gap = css_px_scale(free_space, 1.0 / (double)(lines + 1));
            lead = gap;
        }
        break;
    default:
        DFAILF("%s: `align-content: %s` is no value css-align-3 §5.1 places flex lines by",
               box_subject(container, nbuf, sizeof nbuf), css_keyword_name(ac.value));
        break;
    }
    *start = lead;
    for (i = 0; i < line; i++) *start = css_px_add(*start, css_px_add(sizes[i], gap));
    out = sizes[line];
    free(sizes);
    return out;
}

/* The physical coordinate, from the container's low (left or top) content edge, of a box `offset` from the
   start edge on `start`'s axis whose extent there is `extent`, in a content box `size` long. */
static CssPx fp_physical(const char *start, CssPx offset, CssPx extent, CssPx size)
{
    return flex_margin_is_low(start) ? offset : css_px_sub(css_px_sub(size, offset), extent);
}

CssPx flex_position_item_margin(lxb_dom_element_t *container, lxb_dom_element_t *item, const char *name)
{
    FpAxes a = fp_require_built(container, item);
    CssPx border = css_px(0.0), ms = css_px(0.0), me = css_px(0.0), mt, mb, start;

    if (strcmp(name, a.main_start) == 0 || strcmp(name, flex_margin_opposite(a.main_start)) == 0) {
        (void)fp_main_offset(container, item, a, &border, &ms, &me);
        return strcmp(name, a.main_start) == 0 ? ms : me;
    }
    DCHECKF(strcmp(name, a.cross_start) == 0 || strcmp(name, flex_margin_opposite(a.cross_start)) == 0,
            "`%s`: a flex item's used margin was asked for a property that is not a margin", name);
    (void)fp_cross_margins(item, a, fp_line_place(container, item, a, &start), &mt, &mb);
    return strcmp(name, a.cross_start) == 0 ? mt : mb;
}

void flex_position_item_offset(lxb_dom_element_t *container, lxb_dom_element_t *item, CssPx *x, CssPx *y)
{
    FpAxes a;
    CssPx border = css_px(0.0), ms = css_px(0.0), me = css_px(0.0), main, cross, line, start;

    DCHECK(x != NULL && y != NULL, "a flex item's position was asked with nowhere to put it");
    a = fp_require_built(container, item);
    main = fp_main_offset(container, item, a, &border, &ms, &me);
    main = fp_physical(a.main_start, main, border, used_value_content_px(container, a.main_vertical));
    /* The item's line, and where it starts in the container's content box (`fp_line_place`). */
    line = fp_line_place(container, item, a, &start);
    cross = fp_physical(a.cross_start, css_px_add(start, fp_cross_offset(container, item, a, line)),
                        used_value_border_edge_px(item, !a.main_vertical),
                        used_value_content_px(container, !a.main_vertical));
    *x = a.main_vertical ? cross : main;
    *y = a.main_vertical ? main : cross;
}
