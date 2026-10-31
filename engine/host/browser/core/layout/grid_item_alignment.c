/* A grid item's self-alignment in its grid area. See grid_item_alignment.h. */
#include <string.h>

#include "check.h"
#include "core/css/css_align.h"
#include "core/css/css_computed_value.h"
#include "core/css/css_keyword.h"
#include "core/layout/box_subject.h"
#include "core/layout/grid_item_alignment.h"
#include "core/layout/used_value.h"

/* css-align-3 §6.1's `auto`: "It behaves as the computed justify-items value of the parent box (minus any
   legacy keywords) otherwise", and `align-self`'s twin over `align-items`; a bare `legacy` is `normal`. */
static CssAlign gia_self(lxb_dom_element_t *container, lxb_dom_element_t *item, bool vertical)
{
    CssAlign a = css_computed_align(item, vertical ? "align-self" : "justify-self");

    if (a.value != CSS_KEYWORD_AUTO) return a;
    a = css_computed_align(container, vertical ? "align-items" : "justify-items");
    if (a.value == CSS_KEYWORD_LEGACY && !a.legacy) a.value = CSS_KEYWORD_NORMAL;
    return a;
}

/* Named residual: css-align-3 §4.4.1.3 "All Other Alignment"'s second rule — an overflowing subject that would
   extend into its nearest scroll container's unscrollable region has that overflow biased to the other side —
   is not covered; the default overflow alignment is taken as unsafe, as core/layout/flex_position.c takes it.
   The next diff builds the bias over the scroll container's scrollable overflow area for both. Its absence
   shows as a centred item wider than its area reaching past a scroll container's start edge where a browser
   keeps it scrollable. */
CssPx grid_item_self_alignment_offset(lxb_dom_element_t *container, lxb_dom_element_t *item, CssPx area,
                                      bool vertical)
{
    CssAlign a = gia_self(container, item, vertical);
    CssPx free_space = css_px_sub(area, used_value_margin_edge_px(item, vertical));
    bool ltr = vertical || css_computed_keyword_is(container, "direction", CSS_KEYWORD_LTR);
    double share = 0.0;
    char buf[160];

    if (!css_computed_keyword_is(container, "writing-mode", CSS_KEYWORD_HORIZONTAL_TB))
        DFAILF("%s is a grid item of a container whose `writing-mode` is not `horizontal-tb`; css-grid-1 §10.3 "
               "and §10.4 align in that mode's flow-relative axes. BUILD the vertical-mode grid",
               box_subject(item, buf, sizeof buf));
    switch (a.value) {
    /* css-align-3 §6.1.5 "Grid Items": `normal` "Sizes as either stretch (typical non-replaced elements) or
       start (typical replaced elements) ... The resulting box is then start-aligned"; `stretch` "falls back to
       flow-start generally"; `flex-start` is "A legacy value alias for flow-start", which outside a flex or
       grid lanes container "behaves as start", the edge §4.1's `start` is "flush with". A stretched box has no
       free space, so the share it takes moves nothing. */
    case CSS_KEYWORD_NORMAL:
    case CSS_KEYWORD_STRETCH:
    case CSS_KEYWORD_FLEX_START:
    case CSS_KEYWORD_START:
        share = ltr ? 0.0 : 1.0;
        break;
    case CSS_KEYWORD_END:
    case CSS_KEYWORD_FLEX_END:
        share = ltr ? 1.0 : 0.0;
        break;
    case CSS_KEYWORD_CENTER:
        share = 0.5;
        break;
    /* §4.1: the "line-left or physical left edge", which in a horizontal-tb container is the physical one. */
    case CSS_KEYWORD_LEFT:
    case CSS_KEYWORD_RIGHT:
        DCHECKF(!vertical, "`align-self: %s` is outside css-align-3 §6.2's grammar", css_keyword_name(a.value));
        share = a.value == CSS_KEYWORD_LEFT ? 0.0 : 1.0;
        break;
    case CSS_KEYWORD_SELF_START:
    case CSS_KEYWORD_SELF_END:
        if (!css_computed_keyword_is(item, "writing-mode", CSS_KEYWORD_HORIZONTAL_TB) ||
            (!vertical && css_computed_keyword_is(item, "direction", CSS_KEYWORD_LTR) != ltr))
            DFAILF("%s: `%s` on a grid item whose writing mode or direction is not its container's names the "
                   "item's own start side; BUILD that side's mapping before placing it",
                   box_subject(item, buf, sizeof buf), css_keyword_name(a.value));
        share = (a.value == CSS_KEYWORD_SELF_START) == ltr ? 0.0 : 1.0;
        break;
    case CSS_KEYWORD_BASELINE:
        DFAILF("%s is a grid item in baseline self-alignment, which css-grid-1 §10.3 and §10.4 hand to "
               "css-align-3 §9.3's baseline-sharing groups. BUILD them over the grid's rows and columns",
               box_subject(item, buf, sizeof buf));
        break;
    default:
        DFAILF("%s: `%s` is no value css-align-3 §6.1's `justify-self` or §6.2's `align-self` resolves to",
               box_subject(item, buf, sizeof buf), css_keyword_name(a.value));
        break;
    }
    /* css-align-3 §4.4's `safe`: an overflowing subject is aligned "as if the alignment mode were flow-start",
       which is `start` here. */
    if (a.overflow == CSS_KEYWORD_SAFE && free_space.px < 0.0) share = ltr ? 0.0 : 1.0;
    return css_px_scale(free_space, share);
}
