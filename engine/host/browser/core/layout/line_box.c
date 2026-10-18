/* CSS 2.2 §9.4.2's inline formatting context and §10.8's line height calculation. See line_box.h for which box
   establishes this formatting context, for the input a line box's height does not need, and for why §9.4.2's
   zero-height line box is a second answer. */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <lexbor/dom/dom.h>

#include "check.h"
#include "core/css/css_computed_value.h"
#include "core/css/css_length.h"
#include "core/layout/block_flow.h"
#include "core/layout/flex_container_baseline.h"
#include "core/layout/grid_baseline.h"
#include "core/layout/box_subject.h"
#include "core/layout/box_tree.h"
#include "core/layout/inline_run_walk.h"
#include "core/layout/intrinsic_size.h"
#include "core/layout/layout_job.h"   /* the line box record of the job whose activation holds the thread */
#include "core/layout/line_box.h"
#include "core/layout/phrasing_break.h"
#include "core/layout/replaced_element.h"
#include "core/layout/text_run.h"
#include "core/layout/used_value.h"
#include "solver/metrics.h"   /* `_layoutFillRests` */

static char *lb_computed(lxb_dom_element_t *el, const char *name)
{
    char *v = css_computed_value(el, name);

    DCHECK(v != NULL, "the cascade produced no computed value for a property this engine models — every one of "
                      "them is in lexbor's registry with an initial value, so the last layer always answers");
    return v;
}

/* CSS 2 §8.1 "Box dimensions"' border width on one side, in CSS pixels, read as a computed value:
   css-backgrounds-3 §3.3's `Computed value:` line is "absolute length, snapped as a border width", so the
   computed value is the used one for the inline boxes this file reads it of. */
static CssPx lb_border_px(lxb_dom_element_t *el, const char *side)
{
    char name[32];
    CssLength b;

    snprintf(name, sizeof name, "border-%s-width", side);
    b = css_computed_length(el, name);
    DCHECK(b.kind == CSS_LENGTH_ABSOLUTE,
           "a `border-*-width` computed to something that is not an absolute length. css-backgrounds-3 §3.3's "
           "`Computed value:` line is `absolute length, snapped as a border width`, so every arm of that "
           "derivation produces one and a percentage or a keyword here is a rule that did not run");
    return b.px;
}

static char *lb_strdup(const char *s)
{
    char *out = strdup(s);

    CHECK(out != NULL, "out of memory copying a computed keyword while measuring CSS 2.2 §9.4.2's line boxes");
    return out;
}

/* CSS 2.2 §9.4.2's axes are physical ("boxes are laid out horizontally, one after the other, beginning at the
   top of a containing block"; "in general, the left edge of a line box touches the left edge of its containing
   block"), which are a `horizontal-tb` writing mode's, as is css-text-4 §7.1's note about `left` and `right`
   ("in vertical writing modes, this can be either the physical top or bottom, depending on writing-mode").
   This component reports offsets on those axes, so a vertical-mode box would get a distance on the wrong one.
   It is asked of the box and of the establishing container separately because css-writing-modes-4 §7.3
   "Orthogonal Flows" is where they differ. */
static void lb_require_horizontal_tb(lxb_dom_element_t *el)
{
    if (!css_computed_keyword_is(el, "writing-mode", CSS_KEYWORD_HORIZONTAL_TB))
        DFAIL("this box's computed `writing-mode` is not `horizontal-tb`, so CSS 2.2 §9.4.2's line boxes do not "
              "run along its physical horizontal axis and do not stack down its physical vertical one — "
              "css-writing-modes-4 §3.2 \"Block Flow Direction: the writing-mode property\" gives `vertical-rl` "
              "and `sideways-rl` a right-to-left block flow and `vertical-lr` and `sideways-lr` a left-to-right "
              "one. This file measures §9.4.2 and §10.8 PHYSICALLY, so placing a vertical-mode box by them "
              "would answer an offset on the wrong axis. BUILD css-writing-modes-4 §7.4 \"Flow-Relative "
              "Mappings\", which restates the layout over the block and inline axes, and §6.4 "
              "\"Abstract-to-Physical Mappings\" applied once at the end — the same subproblem "
              "core/layout/flow_position.c names for §9.4.1's stacking, and it lands with it. Where this box "
              "and its establishing container disagree it is additionally §7.3 \"Orthogonal Flows\"");
}

/* ---- §10.8's step 2, asked before step 1 ------------------------------------------------------------------
   "The inline-level boxes are aligned vertically according to their 'vertical-align' property." Step 3
   measures "the distance between the uppermost box top and the lowermost box bottom", and where each box's top
   and bottom are is step 2's decision, so an unread `vertical-align` would be a height over an alignment
   nobody chose. It is read as its three longhands (css-inline-3 §4.2 "Transverse Box Alignment: the
   vertical-align property": `[ first | last ] || <'alignment-baseline'> || <'baseline-shift'>`). This
   implements the one assignment under which every box shares one baseline: §4.2.2's initial `baseline` ("use
   the dominant baseline choice of the parent"), §4.2.1's initial `auto` source and §4.2.3's initial `0` shift.
   Under it §10.8.1's `A'` and `D'` are measured from one line, so step 3's maxima compare like numbers; any
   other value crashes. */
static void lb_require_baseline_alignment(lxb_dom_element_t *el)
{
    CssLength shift;

    if (!css_computed_keyword_is(el, "alignment-baseline", CSS_KEYWORD_BASELINE))
        DFAIL("CSS 2.2 §10.8's step 2 aligns this inline-level box \"according to their 'vertical-align' "
              "property\", and css-inline-3 §4.2.2 \"Alignment Baseline Type: the alignment-baseline "
              "longhand\" gives it a computed ALIGNMENT BASELINE that is not `baseline` — one of "
              "`text-bottom`, `alphabetic`, `ideographic`, `middle`, `central`, `mathematical` or `text-top`. "
              "Each names a DIFFERENT baseline of the box to align to the alignment context's, so §10.8.1's "
              "`A'` above the baseline and `D'` below it are no longer measured from the line's own baseline "
              "and step 3's \"distance between the uppermost box top and the lowermost box bottom\" cannot be "
              "two maxima over them. `middle` and `central` need the x-height and the central baseline, which "
              "css-values-4 §6.1.1's assumed x-height already answers (core/css/font_metrics.h), and `top` "
              "and `bottom` — which §10.8 states outright \"must be aligned so as to minimize the line box "
              "height\", warning that \"if such boxes are tall enough, there are multiple solutions and CSS "
              "2.2 does not define the position of the line box's baseline\" — are a FIXPOINT over the line "
              "rather than a per-box offset. BUILD css-inline-3 §4.2's baseline table for this box: the "
              "set of baselines a font has, which is what turns each keyword into an offset from the "
              "alphabetic one");
    if (!css_computed_keyword_is(el, "baseline-source", CSS_KEYWORD_AUTO))
        DFAIL("css-inline-3 §4.2.1 \"Alignment Baseline Source: the baseline-source longhand\" gives this "
              "inline-level box a source of `first` or `last`, which selects WHICH of its own baseline sets "
              "aligns to the line — \"when an inline-level box has more than one possible source for baseline "
              "information (such as for a multi-line inline block or inline flex container)\". A box with more "
              "than one baseline set is a box with more than one LINE inside it, and no box that reaches this "
              "check has a set to choose from at all. An `inline` box's own two baseline sets are the SAME set, "
              "which §4.2.1's `Applies to:` line still admits and which makes the source a choice between one "
              "thing. A REPLACED element reaches this check too and has NO baseline set — CSS 2.2 §10.8's "
              "`vertical-align` definition gives a baseline to an `inline-table` and to an `inline-block` and "
              "to nothing else, which is why `lb_atomic_extent` aligns its BOTTOM MARGIN EDGE to the line — so "
              "the source selects between none. THE THIRD BOX HERE IS AN ATOMIC INLINE AND IT IS THE ONE THAT "
              "GENUINELY HAS TWO SETS TO CHOOSE FROM. An `inline-block` whose `overflow` computes to `visible` "
              "and which has in-flow line boxes is measured by `lb_atomic_extent` through §10.8.1's MAIN arm, "
              "and a box with several line boxes has one baseline per line — so this longhand now names a real "
              "alternative and not an empty one. §4.2.1's own `auto` is \"last-baseline alignment for "
              "inline-block, first-baseline alignment for everything else\", which is exactly the LAST line box "
              "§10.8.1 states (\"the baseline of its last line box in the normal flow\") and is what "
              "`LbLines.last_baseline` carries. THE `first` ARM'S NUMBER IS NOW CARRIED and this line used to "
              "say it was not: `lb_reduce` keeps BOTH running positions, `line_box_content_height` reports the "
              "pair, and core/layout/block_flow.h's `block_flow_first_line_box_baseline` reduces §9.4.1's whole "
              "STACK to the FIRST box on it that has a line box — which is the distinction that made this more "
              "than one field, since a stack's first line is in a different box from its last. WHAT IS STILL "
              "NOT BUILT IS THE SELECTION: `lb_atomic_extent` reads the LAST-baseline entry unconditionally, so "
              "§4.2.1's `first` and `last` keywords choose nothing here. READ THIS LONGHAND at that call and "
              "route it to whichever of the two entries it names. An `inline-block` the "
              "exception sends to its bottom margin edge (a scroll container, or one with no in-flow line box) "
              "still has ONE source and is not a set this longhand indexes into. DECIDE which of the four is "
              "here, before reading a source that may select nothing");
    shift = css_computed_length(el, "baseline-shift");
    if (shift.kind != CSS_LENGTH_ABSOLUTE || shift.px.px != 0.0)
        DFAIL("css-inline-3 §4.2.3 \"Post-Alignment Shift: the baseline-shift longhand\" shifts this "
              "inline-level box off the position §10.8's step 2 aligned it to — `sub`, `super`, a "
              "`<length-percentage>` (whose percentage \"refer[s] to the used value of line-height\", which "
              "core/css/css_computed_value.h's `css_used_line_height_px` answers), or one of the "
              "LINE-RELATIVE values `top`, `center` and `bottom`. The first three are an offset added to this "
              "box's `A'` and subtracted from its `D'` before step 3's maxima, which is arithmetic this file "
              "already has the operands for; the last three are §10.8's own undefined-baseline case and are "
              "not an offset at all. BUILD the baseline-relative arm here — the shift resolved against the "
              "used `line-height` — and take the line-relative arm out through §10.8's step 2 with the top/"
              "bottom boxes it belongs with, so the two are never averaged into one number");
}

/* ---- §10.8's step 1 and §10.8.1's half-leading, for one inline box --------------------------------------- */

/* The two distances §10.8's step 3 takes a maximum of, for one box on the line: §10.8.1's `A' = A + L/2` and
   `D' = D + L/2`. They stay apart because step 3 is "the distance between the uppermost box top and the
   lowermost box bottom", two independent maxima that a per-box total cannot answer. */
typedef struct {
    CssPx above;
    CssPx below;
} LbExtent;

/* What one reduction over one fill answers; `lb_reduce` says why these are one pass. */
typedef struct {
    CssPx height;         /* §10.6.3's first bullet: to the bottom edge of the last line box that exists */
    CssPx first_baseline; /* css-inline-3 §4.2.1's `first` arm and CSS 2.1 §17.5.3's cell baseline: to the
                             baseline of the first line box that exists */
    CssPx last_baseline;  /* §10.8.1's `inline-block` arm: to the baseline of the last one */
    bool  any;            /* §9.4.2's other answer; false says the three distances are about no line box */
} LbLines;

/* Whether two sums of one real number, taken in different orders, agree to rounding: the tolerance of the
   identities `lb_strut_extent` and `line_box_inline_fragments` assert. It scales with the magnitude because a
   CSS pixel length is not fixed-point (a 1e6px `line-height` rounds at 1e-10, a 16px one at 1e-15). */
static bool lb_close(double a, double b)
{
    double scale = a < 0.0 ? -a : a;
    double other = b < 0.0 ? -b : b;
    double diff = a - b;

    if (other > scale) scale = other;
    if (diff < 0.0) diff = -diff;
    return diff <= 1e-9 * (1.0 + scale);
}

/* §10.8.1 for an inline box containing no glyphs, the case the section states itself: "if the inline box
   contains no glyphs at all, it is considered to contain a strut (an invisible glyph of zero width) with the
   `A` and `D` of the element's first available font". The box is measured as a glyph would be: "still for
   each glyph, determine the leading `L` to add, where `L = 'line-height' - AD`. Half the leading is added
   above `A` and the other half below `D`." Every inline box this component measures is measured this way.
   `A' + D'` is `line-height` by construction ("the height of the inline box … is thus exactly 'line-height'"),
   asserted rather than substituted: deriving the pair from the line height would lose where the baseline
   sits. `L` may be negative (§10.8.1's note), so `line-height: 0` contributes negative half-leading.
   The half above is floored to a whole pixel and the rest goes below, as real Chrome splits it (Chromium
   0842759f third_party/blink/renderer/core/layout/inline/line_utils.cc's CalculateLeadingSpace): a 24px line of
   16px Liberation Serif has its baseline 17px down, not 17.5px. */
static LbExtent lb_strut_extent(lxb_dom_element_t *el)
{
    CssPx a = css_font_ascent_px(el);
    CssPx d = css_font_descent_px(el);
    CssPx lh = css_used_line_height_px(el);
    CssPx lead = css_px_sub(lh, css_px_add(a, d));
    CssPx half = css_px_atom("Blink CalculateLeadingSpace: floor(leading / 2)", &lead, 1, floor(lead.px / 2.0));
    LbExtent out;

    out.above = css_px_add(a, half);
    out.below = css_px_add(d, css_px_sub(lead, half));
    /* §10.8.1's consequence, asserted: "the height of the inline box encloses all glyphs and their
       half-leading on each side and is thus exactly 'line-height'". It catches the three operands read for
       different elements, which is off by whole pixels; the tolerance is IEEE-754's for one real number
       reached by two orders of addition, never slack in the spec. */
    DCHECK(lb_close(css_px_add(out.above, out.below).px, lh.px),
           "CSS 2.2 §10.8.1's `A' + D'` came out different from the `line-height` it is built out of — the "
           "section's own \"the height of the inline box … is thus exactly 'line-height'\" is an identity over "
           "`A + L/2` and `D + L/2` with `L = 'line-height' - AD`, so a disagreement here is one of the three "
           "operands having been read for a different element than the other two");
    return out;
}

/* One disjunct of the exception in CSS 2.2 §10.8.1 "Leading and half-leading"'s `inline-block` sentence: "The
   baseline of an 'inline-block' is the baseline of its last line box in the normal flow, unless it has either
   no in-flow line boxes or if its 'overflow' property has a computed value other than 'visible', in which
   case the baseline is the bottom margin edge." CSS 2.2 writes the `line-height` and `vertical-align`
   definitions under the §10.8.1 heading, so §10.8.1 is the number that carries these words, though the
   parent §10.8 also contains them.
   This disjunct is a computed value, answerable without laying out the box, while "no in-flow line boxes"
   needs the inside flowed; `lb_atomic_extent` asks this first, so a scroll container's contents are never
   flowed for a baseline already stated, and the other disjunct is `LbLines.any` from the reduction that runs
   anyway. css-overflow-3 §3.1 "Managing Overflow: the overflow-x, overflow-y, and overflow properties" makes
   `overflow` a shorthand, so "computed value other than 'visible'" is the negation of both longhands being
   `visible`; §3.1's pairing rule ("if the other axis specifies a scrollable value, a specified value of
   visible computes to auto") can only turn `visible` into `auto`, never the reverse. */
static bool lb_inline_block_overflow_excepts_baseline(lxb_dom_element_t *el)
{
    return !(css_computed_keyword_is(el, "overflow-x", CSS_KEYWORD_VISIBLE) &&
             css_computed_keyword_is(el, "overflow-y", CSS_KEYWORD_VISIBLE));
}

/* CSS 2 §8.1 "Box dimensions"' three edges between a box's top margin edge and its top content edge: the
   frame conversion §10.8.1's inner baseline needs, since line boxes stack from the top content edge (§9.4.2)
   while §10.8's step 1 measures the box from its margin box ("for all other elements, the box used for
   alignment is the margin box"). No floor: §8.3 "Margin properties" allows a negative margin, so the content
   edge may sit above the margin edge. */
static CssPx lb_content_top_from_margin_edge(lxb_dom_element_t *el)
{
    return css_px_add(css_px_add(used_value_px(el, "margin-top"), lb_border_px(el, "top")),
                      used_value_px(el, "padding-top"));
}

/* §10.8's step 1 for an atomic inline: "The height of each inline-level box in the line box is calculated. For
   replaced elements, inline-block elements, and inline-table elements, this is the height of their margin
   box; for inline boxes, this is their 'line-height'." So the box is measured by its margin box
   (core/layout/used_value.h), not by `lb_strut_extent`.
   What this decides is where the baseline splits the margin box. §10.8's `vertical-align` definition: "for
   inline non-replaced elements, the box used for alignment is the box whose height is the 'line-height' …
   for all other elements, the box used for alignment is the margin box", and `baseline`: "align the baseline
   of the box with the baseline of the parent box. If the box does not have a baseline, align the bottom
   margin edge with the parent's baseline." §10.8 gives a baseline to an `inline-table` ("the baseline of the
   first row of the table") and an `inline-block` (§10.8.1's sentence) and nothing else.
   A replaced element (no baseline: a text descender shows below an image), an `inline-block` whose
   `overflow` is not `visible`, and one with no in-flow line boxes all split at the bottom margin edge:
   `above` is the whole margin box and `below` zero. */
/* The fourth box is the one this function looks inside: §10.8.1's main arm, "the baseline of its last line
   box in the normal flow", is a position in a context that must be flowed first. core/layout/block_flow.h
   flows it, because §9.2.1 says a block container "either contains only block-level boxes or establishes an
   inline formatting context": an `inline-block` holding a block-level box has its last line box inside
   whichever box §9.4.1's stack placed last that has one, at an offset only that walk knows. The same walk
   answers "no in-flow line boxes", so the main arm and that disjunct come from one reading, and both shapes
   of container go through the one entry.
   The alignment is `baseline` by assertion: `lb_require_baseline_alignment` runs over this element first.
   Neither number has a floor (§8.3 allows negative margins, and a declared `height` below the box's line
   boxes puts its last baseline under its bottom margin edge, so `below` is negative). `lb_take` is a maximum
   against the line's strut, non-negative for a non-negative `line-height`, so neither can pull a line box
   below zero. */
static LbExtent lb_atomic_extent(lxb_dom_element_t *el)
{
    CssPx margin_box = used_value_margin_edge_px(el, true);
    bool inline_block = css_computed_display_is(el, CSS_KEYWORD_INLINE_BLOCK);
    bool inline_grid = css_computed_display_is(el, CSS_KEYWORD_INLINE_GRID);
    bool inline_flex = css_computed_display_is(el, CSS_KEYWORD_INLINE_FLEX);
    bool replaced = replaced_element_of(el).replaced;
    LbExtent out;
    CssPx inner_baseline = css_px(0.0);

    /* The other half of `lb_child`'s routing, asserted where the geometry is chosen: that walk emits an
       atomic item for a replaced element and for an `inline-block` of either kind (CSS 2.2 §10.3.10
       "'Inline-block', replaced elements in normal flow": "Exactly as inline replaced elements") and crashes for
       every other atomic inline-level box. A box outside that union here would be a silently wrong pair, since
       a too-small extent loses every maximum. The two facts are independent (HTML §15.4 "Replaced elements"'
       question and the cascade's `display`), so a box can be both; `replaced` is read first because §10.8.1
       gives a replaced element no baseline whatever its `display`. */
    DCHECK(replaced || inline_block || inline_grid || inline_flex,
           "CSS 2.2 §10.8's step 1 for an ATOMIC INLINE-LEVEL box was asked of a box that is neither a "
           "REPLACED element nor an `inline-block`. `lb_child` collects a run item of the atomic kind for "
           "exactly those — css-text-3 §5.5 \"Line Breaking Details\"' \"each replaced element or other atomic "
           "inline\" — an `inline-grid` and an `inline-flex` too, and crashes for an `inline-table`, so any "
           "other box reaching here is that walk and this measurement disagreeing about what was collected");
    out.above = margin_box;
    out.below = css_px(0.0);
    if (replaced) return out;
    /* An `inline-grid` or `inline-flex` aligns its first baseline (css-inline-3 §4.2.1's `auto`: first-baseline
       alignment for everything but an inline-block), css-grid-1 §10.6's or css-flexbox-1 §8.5's. With no item
       it has none, and an inline-level box synthesizes from its margin edges (css-align-3 §9.1), which is the
       bottom margin edge above. */
    if (inline_grid || inline_flex) {
        bool has = inline_grid ? grid_container_first_baseline(el, &inner_baseline)
                               : flex_container_first_baseline(el, &inner_baseline);

        if (!has) return out;
        out.above = css_px_add(lb_content_top_from_margin_edge(el), inner_baseline);
        out.below = css_px_sub(margin_box, out.above);
        return out;
    }
    if (lb_inline_block_overflow_excepts_baseline(el)) return out;
    /* §10.8.1's "no in-flow line boxes" disjunct, answered by the reading that runs anyway: a line box §9.4.2
       says "must be treated as not existing for any other purpose" is what `block_flow_last_line_box_baseline`
       reports false for, so an empty `inline-block`, one holding only empty inline boxes, and one whose
       block-level stack holds no line box all take this arm, as a scroll container does. */
    if (!block_flow_last_line_box_baseline(el, &inner_baseline)) return out;
    out.above = css_px_add(lb_content_top_from_margin_edge(el), inner_baseline);
    out.below = css_px_sub(margin_box, out.above);
    return out;
}

/* The walks `lb_atomic_extent` reads for `el`, asked without a drive: an `inline-block`'s content-based height
   where its `height` behaves as auto (css-sizing-3 §3.2.1) and, where §10.8.1 gives it a baseline, its last
   line box's. A replaced element reads neither. False ("not yet") when one was deferred. */
static bool lb_atomic_ask(lxb_dom_element_t *el)
{
    CssPx unused;
    bool has;

    if (replaced_element_of(el).replaced) return true;
    if (used_value_height_behaves_as_auto(el) && !block_flow_auto_height_ask(el, &unused)) return false;
    if (css_computed_display_is(el, CSS_KEYWORD_INLINE_GRID)) return grid_container_first_baseline_ask(el, &has, &unused);
    if (css_computed_display_is(el, CSS_KEYWORD_INLINE_FLEX)) return flex_container_first_baseline_ask(el, &has, &unused);
    if (lb_inline_block_overflow_excepts_baseline(el)) return true;
    return block_flow_last_line_box_baseline_ask(el, &unused, &has);
}

static void lb_take(LbExtent *line, LbExtent box)
{
    line->above = css_px_max(line->above, box.above);
    line->below = css_px_max(line->below, box.below);
}

/* ---- §9.4.2's zero-height line box ------------------------------------------------------------------------
   "…no inline elements with non-zero margins, padding, or borders…": the test is over all four sides because
   §9.4.2 names none. A non-replaced inline box's vertical margins do not affect the line box's height
   (§10.8.1: "although margins, borders, and padding of non-replaced elements do not enter into the line box
   calculation, they are still rendered around inline boxes"), but this sentence is about whether the line box
   exists, which a rendered border on any edge makes true. */
static bool lb_has_nonzero_box_edges(lxb_dom_element_t *el)
{
    static const char *const SIDE[] = { "top", "right", "bottom", "left" };
    unsigned i;

    for (i = 0; i < sizeof(SIDE) / sizeof(SIDE[0]); i++) {
        char name[32];
        CssLength border;

        snprintf(name, sizeof(name), "margin-%s", SIDE[i]);
        if (used_value_px(el, name).px != 0.0) return true;
        snprintf(name, sizeof(name), "padding-%s", SIDE[i]);
        if (used_value_px(el, name).px != 0.0) return true;
        snprintf(name, sizeof(name), "border-%s-width", SIDE[i]);
        border = css_computed_length(el, name);
        DCHECK(border.kind == CSS_LENGTH_ABSOLUTE,
               "a `border-*-width` computed to something that is not an absolute length. css-backgrounds-3 "
               "§3.3's `Computed value:` line is `absolute length, snapped as a border width` and every arm of "
               "that derivation produces one, so a percentage or a keyword here is a rule that did not run");
        if (border.px.px != 0.0) return true;
    }
    return false;
}


/* ---- the two elements that break the line without putting a glyph on it ------------------------------------
   css-text-3 §5.5 "Line Breaking Details": "out-of-flow boxes and inline box boundaries do not introduce a
   forced line break or soft wrap opportunity in the flow", so core/layout/text_run.h collects an inline box as
   two edge items invisible to [UAX14]. HTML §15.3.4 "Phrasing content" declares the two elements that are
   more than boundaries, and core/layout/phrasing_break.h answers which (the fact is HTML's, so no CSS property
   can be asked, and a name test here would duplicate core/layout/intrinsic_size.c's).
   The forced break is collected: text_run.h's forced-break item carries the U+000A css-text-3 §4 names, the
   fill divides the run there, and the `br` contributes its `line-height` to the line it ends. The opportunity
   (`wbr`) crashes, naming what it needs. The answer says whether the element was placed: a `br` emits no pair
   of edges and is not descended into, since §15.3.4's declaration replaces the box. */
static bool lb_phrasing_break(TextRunMeasure *m, lxb_dom_element_t *el)
{
    switch (phrasing_break_of(el)) {
    case PHRASING_BREAK_FORCED:
        text_run_measure_add_forced_break(m, el);
        return true;
    case PHRASING_BREAK_OPPORTUNITY:
        DFAIL("HTML §15.3.4 \"Phrasing content\" gives `wbr` the UA declaration `display-outside: "
              "break-opportunity`, which puts a SOFT WRAP OPPORTUNITY on this line — css-text-3 §5 says "
              "\"wrapping is only performed at an allowed break point, called a soft wrap opportunity\", and "
              "the UA \"must minimize the amount of content overflowing a line by wrapping the line at a soft "
              "wrap opportunity, if one exists\". THE ITEM KIND IS NO LONGER WHAT IS MISSING AND NEITHER IS "
              "THE BREAK MACHINERY: core/layout/text_run.h now carries a third kind for `br`'s FORCED break, "
              "collected with the U+000A css-text-3 §4 names as HTML's newline so that [UAX14] LB5 and LB6 "
              "decide its two boundaries, and the fill already weighs every soft wrap opportunity in a run "
              "against each line's available width. TWO THINGS ARE LEFT AND BOTH ARE ABOUT THE OPPORTUNITY "
              "RATHER THAN THE BREAK. (1) The code point: an opportunity item's is U+200B ZERO WIDTH SPACE, "
              "whose [UAX14] class is ZW, so LB7 `× ZW` forbids a break before it and LB8 `ZW SP* ÷` allows "
              "one after — a DIFFERENT pair of rules from the forced break's, reached by a different class. "
              "(2) WHOSE `white-space` DECIDES IT, which is the half a forced break never has to answer "
              "because §5.5 makes a forced break forced \"regardless of the white-space value\". "
              "`tr_opportunity_enabled` implements §5.5's two cases — the box DIRECTLY CONTAINING a character "
              "that disappears at the break, and otherwise the NEAREST COMMON ANCESTOR of the two characters "
              "— and a `wbr` is neither: the opportunity is created by the ELEMENT, so it is the `wbr`'s own "
              "computed `white-space` that governs. That is not a refinement, it is the case §15.3.4's own "
              "`nobr wbr { white-space: normal; }` rule exists to make work, and reading the nearest common "
              "ancestor instead would answer `nowrap` for exactly the document that rule is written about. "
              "THE ATOMIC INLINE IS NO LONGER THE THING THIS WAITS FOR: text_run.h's fourth item kind carries "
              "§5.5's \"soft wrap opportunity before and after each replaced element or other atomic inline\" "
              "as a U+FFFC of class CB, and this element needs a DIFFERENT code point under DIFFERENT rules, "
              "which is why the two were never one case. BUILD the two above");
        return true;
    case PHRASING_BREAK_NONE:
        return false;
    }
    DFAIL("HTML §15.3.4's line-breaking classification answered a value this walk has no arm for. "
          "core/layout/phrasing_break.h enumerates exactly what the section declares — a forced break, a soft "
          "wrap opportunity, and neither — so a fourth value is a keyword added there and not routed here");
    return false;
}

/* ---- the walk over the formatting context's content ------------------------------------------------------- */

/* What `lb_child` left for its caller: nothing, a text node whose characters are the next items, an inline
   box whose opening edge it emitted and whose box-tree children are next, with its closing edge after them, or
   "not yet" for an atomic inline whose intrinsic sizes are not ready, with nothing added. */
typedef enum { LB_CHILD_PLACED = 0, LB_CHILD_TEXT, LB_CHILD_INLINE_BOX, LB_CHILD_NOT_YET } LbChildNext;

/* One member of the inline formatting context; `*node` receives the text node or inline box the answer names.
   A wholly collapsible text node is not content: CSS 2.2 §9.2.2.1 "Anonymous inline boxes" says white space
   that would be collapsed away "does not generate any anonymous inline boxes", and core/layout/block_flow.h
   answers that once for every walk (core/layout/intrinsic_size.c asks the same), so this walk and §9.4.1's
   stack agree about white space. What survives goes in unchanged: css-text-3 §4.1.1 collapses over the
   formatting context, so the accumulator joins a run split across text nodes. */
static LbChildNext lb_child(TextRunMeasure *m, lxb_dom_element_t *parent, BoxTreeChild c, lxb_dom_node_t **node)
{
    lxb_dom_node_t *n;
    lxb_dom_element_t *el;
    CssDisplay d;
    bool atomic, inline_box, inline_block, inline_grid, inline_flex, on_the_line;

    /* css-lists-3 §3.5's outside marker box "is placed outside the principal block box", so it is on no line
       box of the list item's content; an inside one aborts at the classification.
       Named residual. §3.5 leaves undefined how "the size or contents of the marker box may affect … the
       height of its first line box, and in some cases may cause the creation of a new line box", and Chrome
       answers it (its line breaker keeps a line holding the marker non-empty, and an item with no line box
       gets the marker's block size). Not covered: that contribution. Next diff: the marker box laid out over
       §3.2's marker string, once css-counter-styles-3 generates it, with its extent taken into the first line
       box. Absence shows as an empty list item measuring zero tall where Chrome gives it one line. */
    if (box_tree_child_is_pseudo(c)) {
        DCHECK(block_flow_child_kind(parent, c) == BLOCK_FLOW_CHILD_OUTSIDE_MARKER,
               "a pseudo-element member of a list item's inline formatting context was classified as something "
               "other than css-lists-3 §3.5's outside marker, which is the one arm core/layout/block_flow.h "
               "answers for it");
        return LB_CHILD_PLACED;
    }
    n = c.node;
    switch (n->type) {
    case LXB_DOM_NODE_TYPE_TEXT:
        if (!block_flow_text_child_generates_box(n)) return LB_CHILD_PLACED;
        *node = n;
        return LB_CHILD_TEXT;
    case LXB_DOM_NODE_TYPE_COMMENT:
    case LXB_DOM_NODE_TYPE_PROCESSING_INSTRUCTION:
    case LXB_DOM_NODE_TYPE_DOCUMENT_TYPE:
        /* CSS 2.2 §9.2 generates boxes for elements and for text, and for neither of these three. */
        return LB_CHILD_PLACED;
    case LXB_DOM_NODE_TYPE_ELEMENT:
        break;
    default:
        DFAIL("a node type CSS 2.2 §9.2's box generation does not describe is inside a block container being "
              "laid out — the tree this walk iterates holds elements, text, comments, processing instructions "
              "and a doctype, and a CDATA section, a document or a fragment is not a child any parser this "
              "engine runs produces there. Find the writer that inserted it");
        return LB_CHILD_PLACED;
    }
    el = lxb_dom_interface_element(n);
    /* §9.4.2's line box holds in-flow inline-level boxes; an absolutely positioned box is out of flow and
       introduces no break (css-text-3 §5.5: "out-of-flow boxes and inline box boundaries do not introduce a
       forced line break or soft wrap opportunity in the flow"). Its position is §9.3.2's over a static
       position. */
    if (css_computed_keyword_is(el, "position", CSS_KEYWORD_ABSOLUTE) || css_computed_keyword_is(el, "position", CSS_KEYWORD_FIXED)) return LB_CHILD_PLACED;
    if (block_flow_is_floated(el))
        DFAIL("CSS 2.2 §9.5 \"Floats\" takes this box out of the line and puts it beside the line boxes, which "
              "changes the WIDTH available to every one of them: §9.4.2 says line boxes \"may vary in width if "
              "available horizontal space is reduced due to floats\", and §9.5's own rule shortens the line "
              "boxes beside a float rather than moving them. A float in this formatting context therefore "
              "makes the number of line boxes a function of the float's own width and height, which is the "
              "measurement this component exists to avoid needing. BUILD §9.5.1's float placement");
    d = css_computed_display(el);
    if (css_display_spells(d, CSS_KEYWORD_NONE)) { return LB_CHILD_PLACED; }
    if (css_display_spells(d, CSS_KEYWORD_CONTENTS)) {
        DFAIL("css-display-3 §2.5 \"Box Generation: the none and contents keywords\" gives this child "
              "`display: contents`: \"The element itself does not generate any boxes, but its children and "
              "pseudo-elements still generate boxes and text sequences as normal.\" So the boxes on this line "
              "are the child's children spliced in at its position. THE SPLICE IS BUILT — "
              "core/layout/box_tree.h is §2.5's \"the element must be treated as if it had been replaced in "
              "the element tree by its contents\" as a child sequence — so ROUTE this walk to it rather than "
              "descending here. WHAT THE ROUTING MUST NOT DISTURB IS THE `parent` THIS WALK THREADS: "
              "`lb_child` re-passes the element it descended into, so `parent` is already the node's OWN "
              "parent at every depth, and that is what §2.5's Note requires — \"any semantics based on the "
              "document tree, such as selector-matching, event handling, and property inheritance, are not "
              "affected\" — so only the CONTAINER's child enumeration moves to the box tree while every "
              "inherited property stays read off the node's document parent. AND THIS WALK DOES NOT LAND "
              "ALONE: the walk's `end` is a `BlockFlowRun`'s `end`, so this walk is BOUNDED by one of "
              "§9.2.1.1's anonymous block boxes, and that type reads its bounds as SIBLINGS of the container. "
              "core/layout/intrinsic_size.c's walk over the same content is bounded the same way. So the run "
              "type, core/layout/block_flow.c's five walks and both of these are ONE landing rather than "
              "four, and core/layout/table_box.c's array-materialising walk is the only one that is free");
        return LB_CHILD_PLACED;
    }
    inline_block = css_display_spells(d, CSS_KEYWORD_INLINE_BLOCK);
    atomic = inline_block || css_display_spells(d, CSS_KEYWORD_INLINE_FLEX) ||
             css_display_spells(d, CSS_KEYWORD_INLINE_GRID) || css_display_spells(d, CSS_KEYWORD_INLINE_TABLE);
    inline_box = css_display_spells(d, CSS_KEYWORD_INLINE);
    inline_grid = css_display_spells(d, CSS_KEYWORD_INLINE_GRID);
    inline_flex = css_display_spells(d, CSS_KEYWORD_INLINE_FLEX);
    /* Which atomic inline-level boxes this walk collects. §10.8.1's whole sentence (its main arm, its "no
       in-flow line boxes" disjunct and its `overflow` disjunct) is decided in `lb_atomic_extent`; what is left
       here is whether §10.8's step 1 and a used inline size exist for the box type at all.
       A replaced `inline-block` takes the replaced route: §10.3.9 "'Inline-block', non-replaced elements in
       normal flow" and §10.6.6 "Complicated cases" ("'Inline-block', non-replaced elements. … If 'height' is
       'auto', the height depends on the element's descendants per 10.6.7") are the non-replaced half, §10.3.10
       "'Inline-block', replaced elements in normal flow" is the other in one sentence ("Exactly as inline
       replaced elements."), and §10.6.2's title names "'inline-block' replaced elements in normal flow" beside
       "Inline replaced elements". core/layout/used_value.c reads `replaced_element_of` before `uv_box_kind`,
       so the sizes follow with nothing arranged here. Both halves emit `used_value_margin_edge_px` (§9.4.2:
       "Horizontal margins, borders, and padding are respected between these boxes."); the baseline differs,
       and `lb_atomic_extent` decides it. The replaced test runs at most once on any path, after the `display`
       test, so replaced_element.c's own crashes never pre-empt the classification crash below. */
    on_the_line = (inline_block || inline_grid || inline_flex) && !replaced_element_of(el).replaced;
    /* The crash is over the `display` alone, so a replaced `inline-block` falls through to the replaced arm
       below (§10.3.10's "exactly as inline replaced elements"). An `inline-table` crashes whether or not it is
       replaced. An `inline-grid` is collected like an `inline-block`: css-grid-1 §5.2 "Sizing Grid Containers"
       sizes it "as an atomic inline-level box (such as an inline-block)", and §10.6 gives it its baseline. An
       `inline-flex` is too: css-flexbox-1 §9.2 sizes it "using the rules of the formatting context in which it
       participates", and §8.5 gives it its baseline. */
    if (atomic && !inline_block && !inline_grid && !inline_flex)
        DFAIL("an `inline-table` on a line: CSS 2.2 §9.2.2 makes it an atomic inline-level box, collected like an "
              "`inline-grid` once its baseline exists. BUILD CSS 2.2 §10.8.1 \"Leading and half-leading\"'s "
              "\"baseline of the first row of the table\", a row baseline core/layout/table_height.h computes "
              "and does not export");
    /* §9.2.1.1's second paragraph is run by core/layout/block_flow.h's content-order box list: a block-level
       box inside an inline box is on the container's §9.4.1 stack, so it is this run's exclusive end and the
       walk stopped at it before this line. Neither origin can arrive here, which the one assert below says. */
    if (!inline_box && !atomic)
        DFAIL("a computed `display` inside an INLINE formatting context that is neither `inline`, nor an "
              "atomic inline-level box, nor `none`, nor `contents`, nor out of flow. §9.4.2's own condition is "
              "that the establishing block container \"contains no block-level boxes\", and "
              "core/layout/block_flow.c decides that over the SAME content before calling this component — so "
              "a block-level or table box reaching this walk is those two classifications having come apart, "
              "and the one to fix is whichever list this `display` is missing from. IT COVERS §9.2.1.1's OWN "
              "INVARIANT IN BOTH OF ITS SHAPES, and they are one sentence: \"then we force it to have only "
              "block-level boxes inside it\" puts a block-level box OUTSIDE the anonymous box as its SIBLING "
              "whether it is a child of the container or was reached through an inline box the section breaks "
              "around it — so no run this walk is ever handed may contain one, at any depth, and the run's "
              "exclusive end is where it stops. Whichever of `bf_content_is_inline` and `block_flow_next_block_box` "
              "stopped agreeing with the other is the fix");
    /* §10.8's step 2 for this box, asked at the walk because it is about the box (whether its `A'` and `D'`
       are measured from its neighbours' baseline) and the walk reaches each box once, while the per-line pass
       reaches it once per item. It is asked of a `br` too: the break is an inline-level box on the line it
       ends, so `lb_line_walk_item` takes its `A'` and `D'` (`<br style="line-height:100px">` makes a 100px line).
       It is asked of a replaced element before its item is collected, because `lb_atomic_extent` puts that box
       wholly above the baseline only under `baseline`'s "if the box does not have a baseline, align the bottom
       margin edge with the parent's baseline". */
    lb_require_baseline_alignment(el);
    /* CSS 2.2 §9.2.2's atomic inline-level box, collected as the one opaque box it is: css-text-3 §5.5's
       atomic item, a margin-box inline size carrying U+FFFC (class CB), identical to the replaced arm's. Where
       its baseline sits is asked once, at `lb_atomic_extent`. Its inline size is CSS 2.2 §10.3.9
       "'Inline-block', non-replaced elements in normal flow"'s shrink-to-fit and its block size §10.6.6
       "Complicated cases"' redirection to §10.6.7, both through core/layout/used_value.h; §10.6.6's last
       sentence ("For 'inline-block' elements, the margin box is used when calculating the height of the line
       box") is why the height is read at the line and the width here.
       The walk does not descend: §9.2.2 makes it "a single opaque box" in this context, so its contents are not
       this run's items. Its inside is flowed by its own questions (used_value.h for its height, and
       core/layout/block_flow.h's §9.4.1 walk for the baseline, which reaches this file again for the box
       holding the last line box). */
    if (on_the_line) {
        /* One used width, stated as both of css-sizing-3 §2.1's members: the line is filled at an available
           width its containing block has determined, so §10.3.9's shrink-to-fit is resolved and the box has one
           width here. `text_run_fill_step` asserts the atomics it is handed carry one number, which keeps
           this walk's items apart from core/layout/intrinsic_size.c's §5.2 contributions.
           Shrink-to-fit reads the box's intrinsic sizes, so they are asked first and the read is served from
           the job's record. */
        CssPx used;

        if (!used_value_width_ask(el)) return LB_CHILD_NOT_YET;
        used = used_value_margin_edge_px(el, false);

        text_run_measure_add_atomic(m, el, used, used);
        return LB_CHILD_PLACED;
    }
    /* HTML §15.4 "Replaced elements" makes this a replaced element: css-text-3 §5.5's "each replaced element
       or other atomic inline" reached through the element's nature rather than its `display` (an `img` is
       `display: inline` and is not an inline box). Its item carries both consequences: §5.5's opportunities
       before and after it, and CSS 2.2 §9.4.2's horizontal margins, borders and padding on the line. The width
       is core/layout/used_value.h's margin box, which runs §10.3.2 "Inline, replaced elements" over
       core/layout/replaced_element.h's natural dimensions. The walk does not descend and emits no edges: CSS
       2.1 §3.1 puts replaced content "outside the scope of the CSS formatting model", and §9.2.2 makes it "a
       single opaque box". Its height is read at the line (`lb_atomic_extent`). */
    if (replaced_element_of(el).replaced) {
        /* One used width, stated twice, for the `inline-block` arm's reason; css-sizing-3 §5.1 "Intrinsic
           Sizes" would also give a replaced box two equal numbers, but that is not the reason that governs. */
        CssPx used = used_value_margin_edge_px(el, false);

        text_run_measure_add_atomic(m, el, used, used);
        return LB_CHILD_PLACED;
    }
    /* The elements that change where the run breaks are placed before anything else is collected for them,
       since the fill is a function of the break positions. A phrasing break is not also an inline box. */
    if (lb_phrasing_break(m, el)) return LB_CHILD_PLACED;
    /* css-text-3 §5.5 puts this box's two edges at its boundaries and not at every break inside it ("inline
       box boundaries do not introduce a forced line break or soft wrap opportunity in the flow"), so they are
       emitted around the descent and the run's break mapping decides each one's line: a box spanning three
       lines has its opening edge on the first and its closing edge on the third, the fact CSSOM View §6's
       `getClientRects()` step 3 reports as a fragment count. They are emitted even when zero, because an
       edge's position says which line an otherwise-empty inline box is on, which §9.4.2's zero-height rule asks.
       The size is core/layout/intrinsic_size.h's `used_` entry: §9.4.2's "horizontal margins, borders, and
       padding are respected between these boxes" and css-sizing-3 §2.2's outer size are the same six lengths.
       It is the used entry, not the intrinsic one, because they differ in how a percentage resolves:
       css-sizing-3 §5.2.1 "Intrinsic Contributions of Percentage-Sized Boxes" resolves a cyclic one against
       zero while a contribution is measured, and otherwise "the percentage is resolved against the containing
       block's size". A line is filled at a determined width (the one `lb_fill` reads), so `margin-left: 5%`
       occupies 5% of it here and contributes zero to §5.2's measurement. */
    text_run_measure_add_box_edge(m, el, used_inline_box_edge_px(el, false));
    /* §10.8's step 1 is over every inline-level box on the line, nested ones included (§10.8.1's "boxes of
       child elements do not influence this height" is about the parent's own height), so the walk descends
       and each box's items carry it onto their lines. */
    *node = n;
    return LB_CHILD_INLINE_BOX;
}

/* ---- §10.8 over one of the fill's line boxes -------------------------------------------------------------
   §10.8's steps 1 to 3 for the line box holding items `[line.from, line.to)`, and §9.4.2's second question
   about it on the same pass. The strut is seeded per line, in §10.8's words: "exactly as if each line box
   starts with a zero-width inline box with the element's font and line height properties. We call that
   imaginary box a 'strut'." Step 3 counts it ("this includes the strut, as explained under 'line-height'
   below"), so a line with nothing else is the strut's height. For §9.2.1.1's anonymous block box the strut
   is the enclosing element's: "the font of the anonymous box is inherited from the DIV", as is `line-height`.
   A box is on this line exactly when one of its items is, which is why this walks items: an inline box whose
   text spans three lines contributes its `A'` and `D'` to all three. `lb_strut_extent` is idempotent under
   `lb_take`, so a box with several items on a line contributes once. */
/* `*exists` is §9.4.2's other answer, per line: "line boxes that contain no text, no preserved white space, no
   inline elements with non-zero margins, padding, or borders, and no other in-flow content … and do not end
   with a preserved newline must be treated as zero-height line boxes … and must be treated as not existing
   for any other purpose."
   A character item is text: the only character that renders nothing is §4.1.1's surviving collapsible space,
   and a run of only those never arrives (CSS 2.2 §9.2.2.1 "Anonymous inline boxes": "white space content that
   would subsequently be collapsed away … does not generate any anonymous inline boxes", decided at
   `lb_text`). Preserved white space crashes on the way here (`tr_wraps`). An atomic inline item is "other
   in-flow content", so `<div><img alt=""></div>` has a line box even though the image is 0 by 0. */
/* "Do not end with a preserved newline" is answered over the last item that renders. A forced break closes its
   line at the boundary after it, so it is its line's last item except at [UAX14] LB3's end of text, where the
   last line runs to the end of the collection and trailing box edges follow it; an edge renders nothing, so
   `<div>a<br><span></span></div>` is one line of text height. A line holds at most one forced break, asserted:
   LB5 `LF !` makes the boundary after every forced break mandatory. */
/* The walk holds one line's §10.8 between items, so a line is taken one item at a time: `begin` seeds the strut,
   `item` takes the next item and `end` closes the line. Each answers the box metrics it derived (a strut's or an
   atomic inline's extent). An inline box's strut is a function of its element alone, so a run of items in one
   box derives and takes it once: the line's extent already holds it, and taking it again would only nest the
   same extreme in a new opaque term wherever the comparison is undecided. */
typedef struct {
    TextRunLine line;
    size_t      i, breaks, last_rendering;
    LbExtent    out;
    bool        exists;
    lxb_dom_element_t *strut_of;   /* the element `strut` was derived for, or NULL */
    LbExtent    strut;
} LbLineWalk;

static size_t lb_line_walk_begin(LbLineWalk *w, lxb_dom_element_t *style, TextRunLine line)
{
    DCHECK(line.from < line.to,
           "CSS 2.2 §9.4.2's fill reported a line box holding NO items. \"Line boxes are created as needed to "
           "hold inline-level content\", so a line with nothing on it is not one the section creates — and its "
           "height would be the strut's, added to §10.6.3's total for content that is not there");
    w->line = line;
    w->i = line.from;
    w->breaks = 0;
    w->last_rendering = line.to;
    w->out = lb_strut_extent(style);
    w->exists = false;
    w->strut_of = NULL;
    return 1;
}

static bool lb_line_walk_done(const LbLineWalk *w)
{
    return w->i == w->line.to;
}

static size_t lb_line_walk_item(LbLineWalk *w, const TextRunMeasure *m)
{
    size_t i = w->i++, derived = 0;
    lxb_dom_element_t *box = text_run_measure_item_style(m, i);
    bool text = text_run_measure_item_is_text(m, i);
    bool brk = text_run_measure_item_is_forced_break(m, i);
    bool atom = text_run_measure_item_is_atomic(m, i);

    DCHECK(i < w->line.to, "CSS 2.2 §10.8's walk over one line box took an item past the line's end");
    /* §10.8's step 1 has a semicolon: "for replaced elements, inline-block elements, and inline-table
       elements, this is the height of their margin box; for inline boxes, this is their 'line-height'." An
       atomic item's box is on the first side, every other item's on the second. */
    if (atom) {
        lb_take(&w->out, lb_atomic_extent(box));
        derived++;
    } else if (w->strut_of != box) {
        w->strut = lb_strut_extent(box);
        w->strut_of = box;
        derived++;
        lb_take(&w->out, w->strut);
    }
    /* The box-edges test runs last because `lb_has_nonzero_box_edges` reads used values, which crash for box
       types this engine does not lay out; asking it of a box an earlier conjunct answered for would turn a
       measured line into an abort. */
    if (text || atom || lb_has_nonzero_box_edges(box)) w->exists = true;
    if (brk) w->breaks++;
    /* An atomic inline renders, so a line can end with one; that keeps the last conjunct from reading a forced
       break through a following image, though LB5 `LF !` already makes that unreachable. */
    if (text || brk || atom) w->last_rendering = i;
    return derived;
}

static LbExtent lb_line_walk_end(const LbLineWalk *w, const TextRunMeasure *m, bool *exists)
{
    size_t breaks = w->breaks, last_rendering = w->last_rendering;
    TextRunLine line = w->line;

    DCHECK(lb_line_walk_done(w), "CSS 2.2 §10.8's walk over one line box was closed before its last item");
    *exists = w->exists;
    DCHECK(breaks <= 1,
           "CSS 2.2 §9.4.2's fill put TWO forced line breaks on ONE line box. [UAX14] LB5 \"Treat CR followed "
           "by LF, as well as CR, LF, and NL, as hard line breaks\" ends with `LF !`, which makes the boundary "
           "after each of them MANDATORY — so the fill closes the line at the first and the second opens the "
           "next one, and two on a line is the fill and the break pass disagreeing about where a mandatory "
           "action is");
    /* §9.4.2's last conjunct. `last_rendering` is `line.to` exactly when the line holds nothing that renders,
       which is the empty-inline-box case the box-edges test above already answered. */
    if (last_rendering < line.to && text_run_measure_item_is_forced_break(m, last_rendering)) *exists = true;
    return w->out;
}

/* CSS 2.2 §9.4.2's "The width of a line box is determined by a containing block", stated or derived (see
   line_box.h's `LineBoxAvailableWidth`). Constructors rather than literals, so an unstated width's `px` is a
   zero no reader is meant to take, as `line_box_content_height`'s baselines are when no line box exists. */
LineBoxAvailableWidth line_box_available_width_derived(void)
{
    LineBoxAvailableWidth w;

    w.stated = false;
    w.px = css_px(0.0);
    return w;
}

LineBoxAvailableWidth line_box_available_width_stated(CssPx px)
{
    LineBoxAvailableWidth w;

    /* Every producer of this number has floored it (css-flexbox-1 §9.7 "Resolving Flexible Lengths" floors
       "its content-box size at zero", css-sizing-3 §3.3 "Box Edges for Sizing: the box-sizing property" floors
       the `border-box` conversion "(as the inner size of a box cannot be negative)"), so a negative is a lost
       operand. It is asserted here, at its origin, because the fill reads the value only for a run with a
       break position, so a check there would be silent for single-line runs and name the wrong site. */
    DCHECKF(px.px >= 0.0,
            "CSS 2.2 §9.4.2's line box width was stated as %g CSS pixels. \"The width of a line box is "
            "determined by a containing block and the presence of floats\", and a containing block's content "
            "box is an inner size that cannot be negative — css-sizing-3 §3.3 \"Box Edges for Sizing: the "
            "box-sizing property\" floors its own conversion \"(as the inner size of a box cannot be "
            "negative)\" and css-flexbox-1 §9.7 \"Resolving Flexible Lengths\" floors \"its content-box size "
            "at zero\" — so this is a derivation that lost an operand and not a declaration",
            px.px);
    w.stated = true;
    w.px = px;
    return w;
}

/* The one read of that value, apart from the fill so the derived arm stays lazy (line_box.h: CSS 2.1 §10.3
   is not run over `style` for a run with no break position in it). */
static CssPx lb_available_width(lxb_dom_element_t *style, LineBoxAvailableWidth avail)
{
    if (avail.stated) return avail.px;
    return used_value_content_px(style, false);
}

/* ---- §9.4.2's content, collected and distributed — the half every answer below shares ----------------------
   One collection and one fill per question, shared by every answer, for core/layout/text_run.h's reason: the
   fill is a function of where [UAX14] says the run may break, so two collections could disagree about which
   line a box is on. The collection is core/layout/inline_run_walk.h's walk held between steps and, inside a
   text node, its character position. The caller owes the measurement exactly one
   `text_run_measure_release`, because the collection must live as long as the `TextRunLine`s index it. */
typedef struct {
    TextRunMeasure *m;
    InlineRunWalk walk;
    bool in_text;
    TextRunText text;
} LbCollect;

static void lb_collect_begin(LbCollect *k, TextRunMeasure *m, lxb_dom_element_t *style, BlockFlowRun run)
{
    /* §9.4.2's content is collected before any of it is measured, because [UAX14]'s rules read past the
       boundary they decide (core/layout/text_run.h). */
    text_run_measure_init(m);
    k->m = m;
    k->in_text = false;
    inline_run_walk_begin(&k->walk, style, run);
}

static bool lb_collect_done(const LbCollect *k)
{
    return k->walk.done && !k->in_text;
}

/* One step of the walk: a character of the text node it is in, one child, or the end of a box. Answers the
   items it took, at least one, so a walk over content that adds no item still spends; or zero for a child that
   is not ready, which the walk still names. */
static size_t lb_collect_step(LbCollect *k)
{
    TextRunMeasure *m = k->m;
    size_t before = m->count;
    lxb_dom_element_t *parent;
    lxb_dom_node_t *node = NULL;
    BoxTreeChild c;

    DCHECK(!lb_collect_done(k), "an inline formatting context's collection was stepped after it ended");
    if (k->in_text) {
        text_run_measure_text_step(m, &k->text);
        k->in_text = !text_run_measure_text_done(&k->text);
        return m->count > before ? m->count - before : 1;
    }
    switch (inline_run_walk_next(&k->walk, &parent, &c)) {
    case INLINE_RUN_END:
        return 1;
    case INLINE_RUN_CLOSE:
        /* The box's closing edge is on this run's last line. */
        text_run_measure_add_box_edge(m, parent, used_inline_box_edge_px(parent, true));
        return 1;
    case INLINE_RUN_CHILD:
        break;
    }
    switch (lb_child(m, parent, c, &node)) {
    case LB_CHILD_NOT_YET:
        return 0;
    case LB_CHILD_PLACED:
        inline_run_walk_take(&k->walk, NULL);
        break;
    case LB_CHILD_TEXT:
        inline_run_walk_take(&k->walk, NULL);
        text_run_measure_text_begin(m, parent, node, &k->text);
        k->in_text = !text_run_measure_text_done(&k->text);
        break;
    case LB_CHILD_INLINE_BOX:
        inline_run_walk_take(&k->walk, lxb_dom_interface_element(node));
        break;
    }
    return m->count > before ? m->count - before : 1;
}

/* Close the collected content: the walk's end, and the checks over the whole of it. */
static void lb_collect_end(LbCollect *k, lxb_dom_element_t *style, BlockFlowRun run)
{
    DCHECK(lb_collect_done(k), "an inline formatting context's collection was closed before its walk ended");
    DCHECK(k->walk.end == run.end, "an inline formatting context's collection was closed over another run");
    inline_run_walk_end(&k->walk);
    text_run_measure_finish_begin(k->m);
    /* CSS 2.2 §9.5 "Floats" reaches every line box of a block formatting context, while the walk above sees
       only the run: "the current and subsequent line boxes created next to the float are shortened as
       necessary to make room for the margin box of the float", and §9.4.2 "Inline formatting contexts" says
       line boxes "may vary in width if available horizontal space is reduced due to floats". `lb_child`
       refuses a float in this run; a float in a preceding sibling's subtree or an ancestor shortens these line
       boxes too, and the fill would otherwise report the unshortened containing-block width, a plausible wrong
       number. This check is the only refusal for that population: §9.5 says a float changes no position on
       §9.4.1's stack ("as if the float did not exist"), and core/layout/block_flow.c refuses floats only under
       §9.5.2's and §10.6.7's own conditions. */
    /* Named residual. Not covered: this asks whether the context holds a float, where §9.5 asks whether a line
       box is next to one ("a line box is next to a float when there exists a vertical position that satisfies
       all of these four conditions: (a) at or below the top of the line box, (b) at or above the bottom of the
       line box, (c) below the top margin edge of the float, and (d) above the bottom margin edge of the
       float"), which needs a placed float. Next: §9.5's shortening, that test plus "if a shortened line box is
       too small to contain any content, then the line box is shifted downward (and its width recomputed) until
       either some content fits or there are no more floats present", over the placement
       core/layout/flow_position.c's §9.5.1 crash names; `LineBoxAvailableWidth` becomes per line box. How its
       absence shows: this abort firing for a context in which no line box and no float share a vertical
       position. */
    /* An `#if APICLIENT_DEV` block rather than a bare `DCHECKF`, because naming the float needs a local, and a
       local beside a `DCHECKF` survives in release: a subtree walk per fill feeding an abort that is not
       there. core/html/close_watcher.c makes the same trade. */
#if APICLIENT_DEV
    {
        lxb_dom_element_t *fl = block_flow_context_first_float(style);
        char sbuf[160], fbuf[160];

        DCHECKF(fl == NULL,
                "%s: CSS 2.2 §9.5 \"Floats\" puts %s in the same block formatting context as the line boxes "
                "this fill is about to measure, and §9.4.2 \"Inline formatting contexts\" makes that float an "
                "operand of their width: \"although line boxes in the same inline formatting context generally "
                "have the same width (that of the containing block), they may vary in width if available "
                "horizontal space is reduced due to floats\". The width this fill would use is the containing "
                "block's, which is the right number only where no float is next to any of these lines — and "
                "which line boxes those are is §9.5's own four-condition test over the float's MARGIN BOX, so "
                "the answer needs the float PLACED. BUILD §9.5's shortening: core/layout/flow_position.c "
                "crashes for §9.5.1 \"Positioning the float: the 'float' property\"' placement, and this is "
                "the consumer that turns that placement into a width. UNTIL THEN THE ABORT IS THE ANSWER — "
                "the alternative is the containing block's width reported for a line box that is not that "
                "wide, which no gate here can tell from a measurement",
                box_subject(style, sbuf, sizeof sbuf), box_subject(fl, fbuf, sizeof fbuf));
    }
#endif
}

/* The distribution begun at §9.4.2's width, once css-sizing-3 §2.1's walk has answered whether the run splits. */
static void lb_fill_distribute_begin(TextRunFill *d, const TextRunMeasure *m, lxb_dom_element_t *style,
                                     LineBoxAvailableWidth avail)
{
    CssPx available = css_px(0.0);

    /* §9.4.2: "The width of a line box is determined by a containing block and the presence of floats". With
       floats refused above (in this run by `lb_child`, elsewhere in the context by the check just made), every
       line box is as wide as the establishing container's content box, which `avail` names: `style` answers
       for §9.2.1.1's anonymous block box, whose non-inherited properties "have their initial value" so
       §10.3.3's equation with six zero terms gives it the parent's content width, and a caller states it for
       css-flexbox-1 §4 "Flex Items"' anonymous block container flex item, whose width is css-flexbox-1 §9.7
       "Resolving Flexible Lengths"'.
       It is derived only where it is an operand: §9.4.2's overflow sentence makes a run with no break position
       one line box at every width, and for many containers (`<div><span></span></div>`) the layout that
       derivation runs is one `used_value.c` may crash for. The fill asserts the theorem. */
    if (text_run_measure_splits(m)) available = lb_available_width(style, avail);
    text_run_fill_begin(d, m, available);
}

/* ---- one fill per layout job -------------------------------------------------------------------------------
   A fill, with the per-line facts every reduction reads, is held by the current layout job's record
   (`layout_job_line_boxes`) for the job's lifetime, so a container's line boxes are laid out once per job however
   many of its descendants, passes and reductions ask. The key is every operand the fill reads besides the tree
   and the cascade: `style`, the run's two bounds, and the stated width (a derived width is `style`'s own used
   content width, so it adds nothing). No baseline pass is an operand: both baselines come out of one reduction.
   The tree and the cascade are constant for the job's lifetime (core/layout/layout_job.h's validity argument).
   Every ask and reader is made under a drive or an entered job, so a record is always held (asserted). The
   asks take the fill and its extents; a reader asserts they did. */
typedef struct {
    LbExtent e;
    bool     exists;
    CssPx    top;   /* §9.4.2's stacking position at this line box's top edge */
} LbLineFacts;

/* Where a fill stands. Its lines are complete at LB_FILL_LINES; the extents and the block-axis span are walks over
   them, taken when a reader asks. */
typedef enum {
    LB_FILL_COLLECT = 0,  /* the content's collection */
    LB_FILL_WALK,         /* [UAX14]'s pass and css-sizing-3 §2.1's walk over the code point positions */
    LB_FILL_DISTRIBUTE,   /* CSS 2.2 §9.4.2's distribution */
    LB_FILL_CHECK,        /* a dev build's second derivation of the lines, compared line for line */
    LB_FILL_LINES
} LbFillPhase;

/* Whether the block-axis span of `line_box_content_span_ask` is untaken, part-taken or held. */
typedef enum { LB_SPAN_NONE = 0, LB_SPAN_PART, LB_SPAN_DONE } LbSpanState;

/* A fill and the walks over it that a resting drive takes one item at a time, each holding its place: the
   distribution in `dist`, the extents (`extents` lines written into `partial`, the open one in `walk`, `top` the
   stacking position at it) and the span (`span_line`, `span_item` and the extreme so far). The key is held so a
   step needs no caller to restate it. */
typedef struct LbFill {
    TextRunMeasure m;
    LbCollect      col;        /* live while `phase` is LB_FILL_COLLECT */
    bool           stepping;   /* a collection step is on the C stack */
    TextRunFill    dist;       /* live while `phase` is LB_FILL_DISTRIBUTE */
    LbFillPhase    phase;
    lxb_dom_element_t *style;
    BlockFlowRun   run;
    LineBoxAvailableWidth avail;
    TextRunLine   *lines;
    size_t         n;
    LbLineFacts   *facts;      /* one per line, published once every one is written */
    LbLineFacts   *partial;
    size_t         extents;
    size_t         atomics;    /* items below this have had their atomic inline's walks answered */
    bool           walking;
    LbLineWalk     walk;
    CssPx          top;
    LbSpanState    span;
    size_t         span_line, span_item;
    CssPx          span_lo, span_hi;
    lxb_dom_element_t *span_strut_of;   /* the element `span_strut` was derived and folded for, or NULL */
    size_t         span_strut_line;     /* the line it was folded on */
    LbExtent       span_strut;
    CssPx         *align;      /* one per line, read where `aligned` */
    bool          *aligned;
    bool           checks;     /* a dev build re-derives this fill; false for that re-derivation */
    struct LbFill *check;      /* the re-derivation, from LB_FILL_CHECK until its extents are compared */
} LbFill;

typedef struct LbEntry {
    lxb_dom_element_t *style;   /* NULL is empty; nothing is removed, so there are no tombstones */
    lxb_dom_node_t    *after, *end;
    bool               stated;
    double             px;      /* the stated width; 0 when not `stated` */
    LbFill            *fill;    /* owned, allocated apart so a consumer's pointer survives the table's growth */
} LbEntry;

/* A fill at the start of its collection; every step after this is `lb_fill_lines_pull`'s. */
static void lb_fill_init(LbFill *f, lxb_dom_element_t *style, BlockFlowRun run, LineBoxAvailableWidth avail,
                         bool checks)
{
    f->style = style;
    f->run = run;
    f->avail = avail;
    f->lines = NULL;
    f->n = 0;
    f->facts = NULL;
    f->partial = NULL;
    f->extents = 0;
    f->atomics = 0;
    f->walking = false;
    f->top = css_px(0.0);
    f->span = LB_SPAN_NONE;
    f->span_line = 0;
    f->span_item = 0;
    f->span_strut_of = NULL;
    f->span_lo = css_px(0.0);
    f->span_hi = css_px(0.0);
    f->align = NULL;
    f->aligned = NULL;
    f->checks = checks;
    f->check = NULL;
    f->stepping = false;
    lb_collect_begin(&f->col, &f->m, style, run);
    f->phase = LB_FILL_COLLECT;
}

static void lb_fill_release(LbFill *f)
{
    if (f->phase == LB_FILL_COLLECT) {
        /* An abandoned collection is closed so the measurement can be released. */
        inline_run_walk_release(&f->col.walk);
        text_run_measure_finish_begin(&f->m);
    }
    if (f->phase == LB_FILL_DISTRIBUTE) text_run_fill_release(&f->dist);
    free(f->lines);
    free(f->facts);
    free(f->partial);
    free(f->align);
    free(f->aligned);
    text_run_measure_release(&f->m);
    if (f->check != NULL) {
        lb_fill_release(f->check);
        free(f->check);
        f->check = NULL;
    }
}

static CssPx lb_align_offset(lxb_dom_element_t *style, const TextRunMeasure *m, TextRunLine line,
                             size_t index, size_t n);

/* css-text-4 §7.1's offset of line `i`, derived on first read: `lb_align_offset` reads a used width only where
   it is an operand, and a line no caller places never asks. */
static CssPx lb_fill_align(LbFill *f, lxb_dom_element_t *style, size_t i)
{
    DCHECK(i < f->n, "a line box's alignment was asked for a line the fill does not have");
    if (f->align == NULL) {
        f->align = malloc(f->n * sizeof *f->align);
        f->aligned = calloc(f->n, sizeof *f->aligned);
        CHECK(f->align != NULL && f->aligned != NULL,
              "out of memory holding css-text-4 §7.1's line offsets for one inline formatting context");
    }
    if (!f->aligned[i]) {
        f->align[i] = lb_align_offset(style, &f->m, f->lines[i], i, f->n);
        f->aligned[i] = true;
    }
    return f->align[i];
}

static size_t lb_key_slot(lxb_dom_element_t *style, BlockFlowRun run, double px, size_t cap)
{
    uint64_t h = (uint64_t)(uintptr_t)style * 0x9E3779B97F4A7C15ull, bits;

    memcpy(&bits, &px, sizeof bits);
    h ^= (uint64_t)(uintptr_t)run.after * 0xC2B2AE3D27D4EB4Full;
    h ^= (uint64_t)(uintptr_t)run.end * 0x165667B19E3779F9ull;
    h ^= bits * 0x27D4EB2F165667C5ull;
    h ^= h >> 31;
    h *= 0xbf58476d1ce4e5b9ull;
    h ^= h >> 29;
    return (size_t)h & (cap - 1);
}

/* The slot `style`'s run at `avail` occupies, or the empty slot it would be inserted at. A stated width is
   compared by its bits, so the key is the number the fill was handed. */
static size_t lb_key_probe(const LbEntry *tab, size_t cap, lxb_dom_element_t *style, BlockFlowRun run,
                           bool stated, double px)
{
    size_t i = lb_key_slot(style, run, px, cap);

    while (tab[i].style != NULL &&
           (tab[i].style != style || tab[i].after != run.after || tab[i].end != run.end ||
            tab[i].stated != stated || memcmp(&tab[i].px, &px, sizeof px) != 0))
        i = (i + 1) & (cap - 1);
    return i;
}

static void lb_record_grow(LineBoxRecord *r)
{
    size_t cap = r->cap != 0 ? r->cap * 2 : 64, i;
    LbEntry *tab = calloc(cap, sizeof *tab);

    CHECK(tab != NULL, "the layout job's line box record could not grow");
    for (i = 0; i < r->cap; i++) {
        BlockFlowRun run;

        if (r->tab[i].style == NULL) continue;
        run.after = r->tab[i].after;
        run.end = r->tab[i].end;
        tab[lb_key_probe(tab, cap, r->tab[i].style, run, r->tab[i].stated, r->tab[i].px)] = r->tab[i];
    }
    free(r->tab);
    r->tab = tab;
    r->cap = cap;
}

void line_box_record_release(LineBoxRecord *r)
{
    size_t i;

    DCHECKF(r->asks == r->served + r->built + r->resumed + r->deferred,
            "a layout job's line box record does not close: %lld asks against %lld served, %lld fills begun, "
            "%lld continued and %lld deferred; every ask has exactly one of the four outcomes, so a gap is a "
            "return between the ask and its outcome", r->asks, r->served, r->built, r->resumed, r->deferred);
    DCHECKF(r->built >= 0 && (size_t)r->built == r->used,
            "a layout job's line box record holds %zu entries after %lld fills; each fill records exactly one "
            "new key", r->used, r->built);
    for (i = 0; i < r->cap; i++) {
        if (r->tab[i].style == NULL) continue;
        lb_fill_release(r->tab[i].fill);
        free(r->tab[i].fill);
    }
    free(r->tab);
    memset(r, 0, sizeof *r);
}

#if APICLIENT_DEV
/* NaN equals itself here: one derivation run twice over one document agrees with itself. */
static bool lb_same_px(CssPx a, CssPx b)
{
    return a.px == b.px || (a.px != a.px && b.px != b.px);
}

/* A held fill against its second derivation from the same key, which a dev build takes through the same steps
   and rests: a key that omits an operand of the fill answers a second asker with the first asker's lines. */
static void lb_fill_compare_lines(const LbFill *held, const LbFill *fresh)
{
    char sbuf[160];
    size_t i;

    DCHECKF(fresh->n == held->n,
            "%s: a layout job holds a fill of %zu line boxes for a context that fills to %zu now, so the line "
            "box record's key omits an operand of the fill", box_subject(held->style, sbuf, sizeof sbuf),
            held->n, fresh->n);
    for (i = 0; i < fresh->n; i++)
        DCHECKF(fresh->lines[i].from == held->lines[i].from && fresh->lines[i].to == held->lines[i].to &&
                    lb_same_px(fresh->lines[i].size, held->lines[i].size),
                "%s, line box %zu: a layout job holds items [%zu, %zu) at %g px where a fresh fill puts "
                "[%zu, %zu) at %g px, so the line box record's key omits an operand of the fill",
                box_subject(held->style, sbuf, sizeof sbuf), i, held->lines[i].from, held->lines[i].to,
                held->lines[i].size.px, fresh->lines[i].from, fresh->lines[i].to, fresh->lines[i].size.px);
}

static void lb_fill_compare_facts(const LbFill *held, const LbFill *fresh)
{
    char sbuf[160];
    size_t i;

    for (i = 0; i < fresh->n; i++)
        DCHECKF(fresh->facts[i].exists == held->facts[i].exists &&
                    lb_same_px(fresh->facts[i].e.above, held->facts[i].e.above) &&
                    lb_same_px(fresh->facts[i].e.below, held->facts[i].e.below) &&
                    lb_same_px(fresh->facts[i].top, held->facts[i].top),
                "%s, line box %zu: a layout job holds CSS 2.2 §10.8's extent (%g above, %g below, top %g) "
                "where a fresh derivation gives (%g, %g, %g)", box_subject(held->style, sbuf, sizeof sbuf), i,
                held->facts[i].e.above.px, held->facts[i].e.below.px, held->facts[i].top.px,
                fresh->facts[i].e.above.px, fresh->facts[i].e.below.px, fresh->facts[i].top.px);
}
#endif

/* Whether an ask's next step fits the resting drive's line box items; outside one, or under a drive nested in a
   body, it always does. */
static bool lb_left(void)
{
    return layout_job_line_items_left();
}

/* The fill's lines: css-sizing-3 §2.1's walk, then §9.4.2's distribution, then a dev build's second derivation.
   False when the allowance ran out first, with every walk holding its place. */
static bool lb_fill_lines_pull(LbFill *f)
{
    DCHECK(!f->stepping,
           "an inline formatting context's fill was asked from inside its own collection, so its line boxes are "
           "an operand of themselves");
    if (f->phase == LB_FILL_COLLECT) {
        while (!lb_collect_done(&f->col)) {
            size_t n;

            if (!lb_left()) return false;
            f->stepping = true;
            n = lb_collect_step(&f->col);
            f->stepping = false;
            if (n == 0) {
                DCHECK(!lb_left(),
                       "an atomic inline's intrinsic sizes answered \"not yet\" with the line box items unspent; "
                       "that ask defers nothing else, so its only \"not yet\" is the allowance");
                return false;
            }
            layout_job_line_items_spend(n);
        }
        lb_collect_end(&f->col, f->style, f->run);
        f->phase = LB_FILL_WALK;
    }
    if (f->phase == LB_FILL_WALK) {
        while (!text_run_measure_finish_done(&f->m)) {
            if (!lb_left()) return false;
            layout_job_line_items_spend(text_run_measure_finish_step(&f->m));
        }
        /* A derived width is `style`'s own used width, which may shrink-to-fit over its intrinsic sizes. */
        if (!f->avail.stated && text_run_measure_splits(&f->m) && !used_value_width_ask(f->style)) return false;
        lb_fill_distribute_begin(&f->dist, &f->m, f->style, f->avail);
        f->phase = LB_FILL_DISTRIBUTE;
    }
    if (f->phase == LB_FILL_DISTRIBUTE) {
        while (!text_run_fill_done(&f->dist)) {
            if (!lb_left()) return false;
            layout_job_line_items_spend(text_run_fill_step(&f->dist, &f->m));
        }
        f->n = text_run_fill_end(&f->dist, &f->m, &f->lines);
        DCHECK(f->n == 0 || f->lines != NULL, "CSS 2.2 §9.4.2's fill reported line boxes and handed back none");
        f->phase = APICLIENT_DEV && f->checks ? LB_FILL_CHECK : LB_FILL_LINES;
    }
#if APICLIENT_DEV
    if (f->phase == LB_FILL_CHECK) {
        if (f->check == NULL) {
            if (!lb_left()) return false;
            f->check = malloc(sizeof *f->check);
            CHECK(f->check != NULL, "out of memory re-deriving one inline formatting context's line boxes");
            lb_fill_init(f->check, f->style, f->run, f->avail, false);
        }
        if (!lb_fill_lines_pull(f->check)) return false;
        lb_fill_compare_lines(f, f->check);
        f->phase = LB_FILL_LINES;
    }
#endif
    return true;
}

/* §10.8's extent of every line box and §9.4.2's stacking position of each ("line boxes are stacked with no
   vertical separation"; a zero-height line box advances by nothing), one item per step. Published only once
   complete, because every reduction that reads one line's reads all of them. A dev build's second derivation
   is walked to its own extents and compared, then released. */
static bool lb_fill_extents_pull(LbFill *f)
{
    DCHECK(f->phase == LB_FILL_LINES, "CSS 2.2 §10.8's line box extents were taken before the fill's lines");
    if (f->facts == NULL && f->n != 0) {
        if (f->partial == NULL) {
            f->partial = malloc(f->n * sizeof *f->partial);
            CHECK(f->partial != NULL,
                  "out of memory holding CSS 2.2 §10.8's line box extents for one inline formatting context");
        }
        while (f->extents < f->n) {
            LbLineFacts *v = &f->partial[f->extents];

            if (!f->walking) {
                if (!lb_left()) return false;
                layout_job_line_metrics_spend(lb_line_walk_begin(&f->walk, f->style, f->lines[f->extents]));
                f->walking = true;
                layout_job_line_items_spend(1);
            }
            while (!lb_line_walk_done(&f->walk)) {
                if (!lb_left()) return false;
                layout_job_line_metrics_spend(lb_line_walk_item(&f->walk, &f->m));
                layout_job_line_items_spend(1);
            }
            v->e = lb_line_walk_end(&f->walk, &f->m, &v->exists);
            v->top = f->top;
            if (v->exists) f->top = css_px_add(f->top, css_px_add(v->e.above, v->e.below));
            f->walking = false;
            f->extents++;
        }
        f->facts = f->partial;
        f->partial = NULL;
    }
#if APICLIENT_DEV
    if (f->check != NULL) {
        if (!lb_fill_extents_pull(f->check)) return false;
        lb_fill_compare_facts(f, f->check);
        lb_fill_release(f->check);
        free(f->check);
        f->check = NULL;
    }
#endif
    return true;
}

/* The fill of `style`'s run at `avail`, held by the current job's record and created on the first ask. NULL
   when it would be begun with the resting drive's line box items spent. */
static LbFill *lb_fill_of(lxb_dom_element_t *style, BlockFlowRun run, LineBoxAvailableWidth avail)
{
    LineBoxRecord *r = layout_job_line_boxes();
    double px = avail.stated ? avail.px.px : 0.0;
    LbFill *f;
    size_t i;

    DCHECK(style != NULL, "CSS 2.2 §9.4.2's line boxes were asked for with no element");
    DCHECK(r != NULL,
           "CSS 2.2 §9.4.2's line boxes were asked with no layout job holding a record — no drive is open and no "
           "job is entered, and every ask and every reader is made under one (core/layout/layout_job.h)");
    r->asks++;
    if (r->cap != 0) {
        i = lb_key_probe(r->tab, r->cap, style, run, avail.stated, px);
        if (r->tab[i].style != NULL) {
            f = r->tab[i].fill;
            if (f->phase == LB_FILL_LINES) r->served++;
            else                           r->resumed++;
            return f;
        }
    }
    if (!lb_left()) {
        r->deferred++;
        return NULL;
    }
    f = malloc(sizeof *f);
    CHECK(f != NULL, "out of memory holding one inline formatting context's line boxes");
    lb_fill_init(f, style, run, avail, true);
    /* Recorded before its collection is taken, so a later ask continues it; an atomic inline's own line boxes
       recorded inside a step grow the table under it, and the fill asking for itself is `lb_fill_lines_pull`'s
       assert. */
    if (r->used * 2 >= r->cap) lb_record_grow(r);
    i = lb_key_probe(r->tab, r->cap, style, run, avail.stated, px);
    DCHECK(r->tab[i].style == NULL, "a line box record found its own key empty and then occupied in one ask");
    r->tab[i].style = style;
    r->tab[i].after = run.after;
    r->tab[i].end = run.end;
    r->tab[i].stated = avail.stated;
    r->tab[i].px = px;
    r->tab[i].fill = f;
    r->used++;
    r->built++;
    return f;
}

/* A reader's fill, whose lines `line_box_atomics_ask` took for the same key. */
static LbFill *lb_layout(lxb_dom_element_t *style, BlockFlowRun run, LineBoxAvailableWidth avail)
{
    LbFill *f = lb_fill_of(style, run, avail);

    DCHECK(f != NULL && f->phase == LB_FILL_LINES,
           "a fill was read before `line_box_atomics_ask` took its lines for the same key");
    return f;
}

/* A reader's extents, which `line_box_extents_ask` took. */
static const LbLineFacts *lb_fill_facts(LbFill *f, lxb_dom_element_t *style)
{
    DCHECK(f->style == style, "a fill's extents were read under an element that is not the fill's own");
    DCHECK(f->facts != NULL || f->n == 0, "a fill's extents were read before `line_box_extents_ask` took them");
    return f->facts;
}

/* ---- §9.4.2's line boxes, reduced once ---------------------------------------------------------------------
   The two distances CSS 2.2 measures down one inline formatting context, and whether it has a line box at
   all, on one pass over one fill. §10.6.3's first bullet wants the bottom edge of the last line box and
   §10.8.1's `inline-block` arm that line box's baseline, a position inside it, so both are one running
   position read at two points of one loop; a second pass could put the baseline on a line the height did not
   count.
   §9.4.2's zero-height line box is skipped by both readings: a line that "must be treated as zero-height line
   boxes for the purposes of determining the positions of any elements inside of them, and must be treated as
   not existing for any other purpose" advances the stack by nothing and is not "the last line box", so `any`
   is §10.8.1's "no in-flow line boxes" answer, the fact §8.3.1 asks from the other side ("does not contain a
   line box"). The baselines are distances only when `any` is true; they are written on every path, and both
   callers read `any` first. */
static LbLines lb_reduce(lxb_dom_element_t *style, BlockFlowRun run, LineBoxAvailableWidth avail)
{
    LbFill *f = lb_layout(style, run, avail);
    const LbLineFacts *facts = lb_fill_facts(f, style);
    LbLines out;
    size_t i;

    out.height = css_px(0.0);
    out.first_baseline = css_px(0.0);
    out.last_baseline = css_px(0.0);
    out.any = false;
    for (i = 0; i < f->n; i++) {
        LbExtent e = facts[i].e;
        CssPx lh;

        if (!facts[i].exists) continue;
        lh = css_px_add(e.above, e.below);
        DCHECK(lh.px >= 0.0,
               "CSS 2.2 §10.8's step 3 produced a NEGATIVE height for one line box. It is \"the distance "
               "between the uppermost box top and the lowermost box bottom\", and every box on the line "
               "contributes `A' + D'` = its own `line-height`, which css-inline-3 §5.1 makes "
               "`<number [0,∞]>` / `<length-percentage [0,∞]>` and says outright that \"negative values are "
               "illegal\" — so a negative maximum is a declaration that should have been dropped by "
               "css-values-4 §5.1's range restriction reaching the arithmetic");
        /* §10.8's step 3 measures a line box "between the uppermost box top and the lowermost box bottom", and
           `e.above` is the maximum `A'` across it, so its baseline is that far below its top, and the stack has
           reached the heights of the lines above. Read before the advance and overwritten by each existing line,
           so the last one's survives: §10.8.1's words, and the coordinate `line_box_inline_fragments` uses. */
        out.last_baseline = css_px_add(out.height, e.above);
        /* The same coordinate kept for the first existing line: css-inline-3 §4.2.1's `first` arm and CSS 2.1
           §17.5.3 Table height algorithms' "the baseline of a cell is the baseline of the first in-flow line box
           in the cell". The test is `out.any`, not an index, because zero-height lines are `continue`d above, so
           line 0 of the fill need not exist. */
        if (!out.any) out.first_baseline = out.last_baseline;
        out.any = true;
        /* §9.4.2: "line boxes are stacked with no vertical separation (except as specified elsewhere) and they
           never overlap", so §10.6.3's distance to "the bottom edge of the last line box" is the sum of the
           heights, with no gap term. */
        out.height = css_px_add(out.height, lh);
    }
    return out;
}

bool line_box_atomics_ask(lxb_dom_element_t *style, BlockFlowRun run, LineBoxAvailableWidth avail)
{
    LbFill *f;
    size_t items;

#if APICLIENT_DEV
    /* The fill's float check (lb_fill) searches the fill's whole formatting context. It is asked here first,
       where "not yet" propagates, so a resting drive divides the search; the fill then reads the held answer. */
    {
        lxb_dom_element_t *fl;

        if (!block_flow_context_first_float_ask(style, &fl)) return false;
    }
#endif
    f = lb_fill_of(style, run, avail);
    if (f == NULL || !lb_fill_lines_pull(f)) {
        METRIC_ADD(fill_rests, 1);
        return false;
    }
    items = f->n == 0 ? 0 : f->lines[f->n - 1].to;
    /* The item count is the partition's, which `text_run_fill_step` closes at the end of the collection. The
       scan holds its place in the fill, so each item is looked at once whichever caller asks and however often
       it rests; an atomic whose walk was deferred stays at the cursor and is asked again. */
    while (f->atomics < items) {
        size_t i = f->atomics;

        if (!lb_left()) {
            METRIC_ADD(fill_rests, 1);
            return false;
        }
        layout_job_line_items_spend(1);
        if (text_run_measure_item_is_atomic(&f->m, i) && !lb_atomic_ask(text_run_measure_item_style(&f->m, i)))
            return false;
        f->atomics++;
    }
    return true;
}

bool line_box_extents_ask(lxb_dom_element_t *style, BlockFlowRun run, LineBoxAvailableWidth avail)
{
    LbFill *f = lb_fill_of(style, run, avail);
    bool ready;

    DCHECK(f != NULL && f->phase == LB_FILL_LINES,
           "CSS 2.2 §10.8's line box extents were asked before `line_box_atomics_ask` answered for the same key");
    ready = lb_fill_extents_pull(f);
    if (!ready) METRIC_ADD(fill_rests, 1);
    return ready;
}

CssPx line_box_content_height(lxb_dom_element_t *style, BlockFlowRun run, LineBoxAvailableWidth avail,
                              bool *any_line_box, CssPx *first_baseline, CssPx *last_baseline)
{
    LbLines r;

    DCHECK(first_baseline != NULL,
           "CSS 2.2 §9.4.2's line boxes were asked for with nowhere to report the FIRST one's baseline. It is "
           "not an optional out-parameter either: css-inline-3 §4.2.1 \"Alignment Baseline Source: the "
           "baseline-source longhand\" and CSS 2.1 §17.5.3 \"Table height algorithms\" both ask for it by "
           "name, and the reduction below computes it out of the same running position the height ends at — so "
           "declining it buys nothing and a caller holding a nullable one would be a second walk waiting to be "
           "written");
    DCHECK(any_line_box != NULL,
           "CSS 2.2 §9.4.2's line boxes were asked for with nowhere to report whether any of them exists — "
           "that is not an optional out-parameter, it is §8.3.1's own question");
    DCHECK(last_baseline != NULL,
           "CSS 2.2 §9.4.2's line boxes were asked for with nowhere to report the LAST one's baseline — that "
           "is not an optional out-parameter either, it is CSS 2.2 §10.8.1 \"Leading and half-leading\"'s own "
           "question about the same lines. The reduction below computes it whether or not anyone reads it, "
           "because it is the same running position §10.6.3's height ends at, so declining it buys nothing and "
           "a caller holding a nullable one would be a second walk waiting to be written");
    r = lb_reduce(style, run, avail);
    *any_line_box = r.any;
    /* With no line box, §10.6.3 has no last line box and the height is the zero the reduction started at; the
       caller learns from `any_line_box` that it is an absent line box rather than a measured zero, the
       distinction §8.3.1 needs. Both baselines are the same zero for the same reason. */
    *first_baseline = r.first_baseline;
    *last_baseline = r.last_baseline;
    /* Asserted: the first existing line's baseline is not below the last's. Each is the running position at its
       line plus that line's maximum `A'`, and the position advances by the previous line's maximum `A'` plus
       maximum `D'`. A `D'` may be negative (a `line-height` under the font's extent), so the order rests on
       §10.8.1's strut, which every line carries: the next baseline exceeds this one by at least the strut's
       `A' + D'`, its non-negative `line-height`. */
    DCHECK(!r.any || r.first_baseline.px <= r.last_baseline.px + 1e-9 * (1.0 + (r.last_baseline.px < 0.0 ?
                                                                               -r.last_baseline.px :
                                                                               r.last_baseline.px)),
           "CSS 2.2 §9.4.2 stacks line boxes \"with no vertical separation\" and they \"never overlap\", so the "
           "FIRST existing line box's baseline cannot sit BELOW the last one's — the two were read off one "
           "running position that only advances. A first above the last means the loop recorded them out of "
           "order, which would put css-inline-3 §4.2.1's `first` arm and CSS 2.1 §17.5.3's cell baseline on a "
           "line the height had not counted");
    return r.height;
}

/* ---- where the boxes on those line boxes reach ------------------------------------------------------------
   §9.4.2's distribution again, reduced to a span (line_box.h says why each axis is exact without a per-item
   position, and what the numbers may be read as).
   The inline axis needs no `lb_line_walk_item`, so it asks no used value of any box on the line: a line box
   §9.4.2 treats as not existing contains "no text, no preserved white space, no inline elements with non-zero
   margins, padding, or borders, and no other in-flow content", so its `TextRunLine.size` is zero and a
   maximum over every line equals one over the existing lines. Calling `lb_line_walk_item` would read used values
   this component crashes for. */
static CssPx lb_widest_line(const TextRunLine *lines, size_t n)
{
    CssPx widest = css_px(0.0);
    size_t i;

    for (i = 0; i < n; i++) widest = css_px_max(widest, lines[i].size);
    return widest;
}

/* §9.4.2's block axis is a maximum over the boxes, not the stack's bottom: a line box's edges include the strut,
   and CSSOM View §2's extreme is over "the element's descendants' boxes", of which the strut ("we call that
   imaginary box a 'strut'") is not one. A container whose `line-height` exceeds its content's has line boxes
   taller than anything in them. The walk is over the items, holds its place in the fill and is seeded at the
   content box's beginning corner, for the inline axis's reason. It reads the extents `line_box_extents_ask`
   took, which `lb_fill_facts` asserts. */
static bool lb_span_pull(LbFill *f)
{
    const LbLineFacts *facts;

    if (f->span == LB_SPAN_DONE) return true;
    facts = lb_fill_facts(f, f->style);
    if (f->span == LB_SPAN_NONE) {
        f->span_lo = css_px(0.0);
        f->span_hi = css_px(0.0);
        f->span_line = 0;
        f->span_item = 0;
        f->span_strut_of = NULL;
        f->span = LB_SPAN_PART;
    }
    while (f->span_line < f->n) {
        size_t i = f->span_line;
        LbExtent e = facts[i].e;
        CssPx top = facts[i].top;

        /* A not-existing line box is "treated as zero-height … for the purposes of determining the positions of
           any elements inside of them", so it advances the stack by nothing (`LbLineFacts.top`). */
        if (!facts[i].exists) { f->span_line++; continue; }
        if (f->span_item < f->lines[i].from) f->span_item = f->lines[i].from;
        while (f->span_item < f->lines[i].to) {
            /* The baseline is `top + e.above`: `e.above` is the line's maximum `A'`, and every box hangs its own
               `A'` above and `D'` below that one line. */
            LbExtent b;
            size_t j;

            if (!lb_left()) return false;
            j = f->span_item++;
            layout_job_line_items_spend(1);
            /* Only a character item's box is taken: the anonymous inline box around a text run has no element to
               be reached through, so it is this walk's. An edge, a forced break and an atomic inline belong to
               elements CSSOM View §2's element walk places (core/layout/flow_position.h) and composes directly:
               a non-replaced inline box's vertical margins, padding and borders do not enter §10.8's line box,
               and an atomic inline has its own used width and height (CSS 2.1 §10.3.2;
               core/layout/scrolling_area.c). Deriving them from `A'` and `D'` would give a rectangle smaller
               than the rendered one. */
            if (!text_run_measure_item_is_text(&f->m, j)) continue;
            /* A run of items in one box on one line reaches one extent, so it is folded once, as the line walk
               takes it once. */
            if (f->span_strut_of == text_run_measure_item_style(&f->m, j) && f->span_strut_line == i) continue;
            f->span_strut_of = text_run_measure_item_style(&f->m, j);
            f->span_strut_line = i;
            f->span_strut = lb_strut_extent(f->span_strut_of);
            layout_job_line_metrics_spend(1);
            b = f->span_strut;
            f->span_lo = css_px_min(f->span_lo, css_px_sub(css_px_add(top, e.above), b.above));
            f->span_hi = css_px_max(f->span_hi, css_px_add(css_px_add(top, e.above), b.below));
        }
        f->span_line++;
    }
    f->span = LB_SPAN_DONE;
    return true;
}

bool line_box_content_span_ask(lxb_dom_element_t *style, BlockFlowRun run,
                               bool vertical, CssPx *lo, CssPx *hi)
{
    LbFill *f;
    bool ready;

    DCHECK(lo != NULL && hi != NULL,
           "CSS 2.2 §9.4.2's line boxes were asked where their boxes reach with nowhere to report one of the "
           "two edges. They are two answers and not one: css-writing-modes-4 §6.2 \"Flow-relative Directions\" "
           "puts the ENDING edge of an axis at the lower coordinate for a `rtl` inline axis and at the higher "
           "one otherwise, and a caller that received only the far edge would have the wrong one half the time");
    f = lb_layout(style, run, line_box_available_width_derived());
    if (!vertical) {
        /* §9.4.2's inline axis: "In general, the left edge of a line box touches the left edge of its
           containing block and the right edge touches the right edge of its containing block", and css-text-4
           §7.1 "Text Alignment: the text-align shorthand": "if (after justification, if any) the inline contents
           of a line box are too long to fit within it, then the contents are start-aligned: any content that
           doesn't fit overflows the line box's end edge." So the one operand is the content box's start edge
           (line_box.h states the theorem). The content box's beginning corner seeds both edges, so a context
           with no line box reports a degenerate span there. It is inside the padding box (CSS 2 §8.1), CSSOM View
           §2's other operand, so the seed cannot invent an overflow. */
        CssPx widest = lb_widest_line(f->lines, f->n);

        *lo = css_px(0.0);
        *hi = css_px(0.0);
        if (css_computed_keyword_is(style, "direction", CSS_KEYWORD_RTL)) {
            /* The start edge is line-right, so content grows toward the lower coordinate and the line box's
               right edge is the far one. Only this arm needs the line box's width, so the used content width is
               read here rather than beside the fill, where it would run CSS 2.1 §10.3 for every unbreakable run. */
            CssPx width = used_value_content_px(style, false);

            *lo = css_px_min(*lo, css_px_sub(width, widest));
            *hi = css_px_max(*hi, width);
        } else {
            DCHECK(css_computed_keyword_is(style, "direction", CSS_KEYWORD_LTR),
                   "a computed `direction` is neither `ltr` nor `rtl`. css-writing-modes-4 §2.1 \"Specifying "
                   "Directionality: the direction property\" gives the property the `Value:` line "
                   "`ltr | rtl` and nothing else, so a third spelling is a declaration the cascade should have "
                   "refused — asserted rather than read as \"not rtl\", which would quietly mean `ltr`");
            *hi = css_px_max(*hi, widest);
        }
        return true;
    }
    ready = lb_span_pull(f);
    if (ready) {
        *lo = f->span_lo;
        *hi = f->span_hi;
    } else {
        METRIC_ADD(fill_rests, 1);
    }
    return ready;
}

/* ---- css-text-4 §7.1's alignment of one line box's content --------------------------------------------------
   CSS 2.2 §9.4.2 hands the question over: "when the total width of the inline-level boxes on a line is less
   than the width of the line box containing them, their horizontal distribution within the line box is
   determined by the 'text-align' property", which css-text-4 §7.1 defines as a shorthand ("see individual
   properties"), so this reads its longhands, css-text-4 §7.3 "Default Text Alignment: the text-align-all
   property" and css-text-4 §7.4 "Last Line Alignment: the text-align-last property". Every alignment
   citation here names css-text-4 explicitly, since the file's other citations are CSS 2.2's.
   css-text-4 §7.4's `auto` is a redirection: "if auto is specified, content on the affected line is aligned
   per text-align-all unless text-align-all is set to justify, in which case it is start-aligned." */

/* Which longhand aligns this line: css-text-4 §7.4's property "describes how the last line of a block or a
   line right before a forced line break is aligned", so the fill's last line and any line holding a forced
   break (LB5's `LF !` closes a line at one, and `lb_line_walk_item` asserts at most one per line). */
static bool lb_line_is_last(const TextRunMeasure *m, TextRunLine line, size_t index, size_t n)
{
    size_t i;

    if (index + 1 == n) return true;
    for (i = line.from; i < line.to; i++)
        if (text_run_measure_item_is_forced_break(m, i)) return true;
    return false;
}

/* The alignment keyword for one line, resolved through css-text-4 §7.4's redirection; the caller frees it. */
static char *lb_line_alignment(lxb_dom_element_t *style, bool last)
{
    char *all = lb_computed(style, "text-align-all");

    if (!last) return all;
    {
        char *lastv = lb_computed(style, "text-align-last");

        if (strcmp(lastv, "auto") != 0) { free(all); return lastv; }
        free(lastv);
        /* css-text-4 §7.4's `auto`, second half: "unless text-align-all is set to justify, in which case it is
           start-aligned", so a justified block does not justify its last line (`justify-all` exists for that). */
        if (strcmp(all, "justify") == 0) { free(all); return lb_strdup("start"); }
        return all;
    }
}

/* Where `line`'s content begins inside its line box: the distance from the line box's left edge to the
   content's left edge, in CSS pixels. §9.4.2 gives the line box the containing block's width ("in general, the
   left edge of a line box touches the left edge of its containing block and the right edge touches the right
   edge of its containing block"), which is `style`'s content box, for §9.2.1.1's anonymous block box too (as
   `lb_fill` derives). The width is derived only where it is an operand: a `start`-aligned `ltr` line begins at
   the left edge whatever the width, and the layout that derivation runs may crash in `used_value.c`.
   §7.1's overflow sentence applies before the alignment: "if (after justification, if any) the inline contents
   of a line box are too long to fit within it, then the contents are start-aligned: any content that doesn't
   fit overflows the line box's end edge", which is also what makes the skip sound. */
static CssPx lb_align_offset(lxb_dom_element_t *style, const TextRunMeasure *m, TextRunLine line,
                             size_t index, size_t n)
{
    char *kw = lb_line_alignment(style, lb_line_is_last(m, line, index, n));
    bool rtl = css_computed_keyword_is(style, "direction", CSS_KEYWORD_RTL);
    bool to_left, centered;
    CssPx width, slack;

    if (!rtl)
        DCHECK(css_computed_keyword_is(style, "direction", CSS_KEYWORD_LTR),
               "a computed `direction` is neither `ltr` nor `rtl`. css-writing-modes-4 §2.1 \"Specifying "
               "Directionality: the direction property\" gives the property the `Value:` line `ltr | rtl` and "
               "nothing else, so a third spelling is a declaration the cascade should have refused — asserted "
               "rather than read as \"not rtl\", which would quietly mean `ltr` and put this line's content at "
               "the wrong edge of its line box");
    if (strcmp(kw, "justify") == 0) {
        free(kw);
        DFAIL("css-text-4 §7.1 \"Text Alignment: the text-align shorthand\"'s `justify`: \"text is justified "
              "according to the method specified by the text-justify property, IN ORDER TO EXACTLY FILL THE "
              "LINE BOX.\" That is not an offset applied to the line's content — it CHANGES THE CONTENT'S OWN "
              "WIDTHS, by expanding the justification opportunities inside it, so a fragment's inline extent is "
              "no longer the sum core/layout/text_run.h measured and every item after an expanded opportunity "
              "moves. TWO THINGS ARE MISSING. (1) §7.5 \"Justification Method: the text-justify property\", "
              "whose computed value core/css/css_computed_value.c does not derive and whose `auto` leaves the "
              "method to the UA — \"the UA determines the justification algorithm to follow\" — with §7.5.1 "
              "\"Expanding and Compressing Text\" naming what may be expanded. (2) The EXPANSION ITSELF as a "
              "per-item term beside `text_run_measure_line_offset`, since §7.1's own overflow sentence is "
              "stated \"after justification, if any\" and therefore runs over the expanded widths. BUILD §7.5's "
              "computed value, then the per-opportunity expansion in core/layout/text_run.c, and this arm "
              "becomes a distribution of `slack` over the opportunities rather than a single offset");
        return css_px(0.0);
    }
    /* css-text-4 §7.1 "Text Alignment: the text-align shorthand"'s distributing values. `left` and `right` are
       line-relative ("in vertical writing modes, this can be either the physical top or bottom, depending on
       writing-mode"), and both callers run `lb_require_horizontal_tb` before their fill: `line_box_glyphs` on
       the establishing container, `line_box_inline_fragments` on that container and on the box asked about
       (css-writing-modes-4 §7.3 "Orthogonal Flows"' pair). */
    centered = strcmp(kw, "center") == 0;
    if (strcmp(kw, "left") == 0) to_left = true;
    else if (strcmp(kw, "right") == 0) to_left = false;
    /* css-writing-modes-4 §6.2 "Flow-relative Directions": the inline-start side of an `ltr` inline axis is its
       line-left one and of an `rtl` one its line-right, so `start` is the left edge exactly when the direction
       is `ltr` and `end` is its mirror. */
    else if (strcmp(kw, "start") == 0) to_left = !rtl;
    else if (strcmp(kw, "end") == 0) to_left = rtl;
    else {
        DCHECK(centered,
               "css-text-4 §7.3 \"Default Text Alignment: the text-align-all property\" answered a computed "
               "value outside its own `Value:` line. Its keywords are `start | end | left | right | center | "
               "justify | match-parent`, §7.3's `Computed value:` line resolves `match-parent` away, and §7.4 "
               "adds only `auto`, which lb_line_alignment redirects — so a sixth spelling here is a value the "
               "cascade admitted and this partition has no arm for, and taking it as `center` would silently "
               "move every line of the document");
        to_left = false;
    }
    free(kw);
    /* The line-left edge of an `ltr` context needs no width, so the initial `start` places without running
       §10.3 over the establishing box. An `rtl` context aligning line-left is not skipped: §7.1's overflow
       sentence start-aligns a too-long line, which under `rtl` is the line-right edge. */
    if (to_left && !centered && !rtl) return css_px(0.0);
    width = used_value_content_px(style, false);
    slack = css_px_sub(width, line.size);
    /* §7.1's overflow sentence. A negative slack is a line "too long to fit within it", and its contents are
       start-aligned whatever the property said. */
    if (slack.px < 0.0) return rtl ? slack : css_px(0.0);
    if (centered) return css_px_scale(slack, 0.5);
    return to_left ? css_px(0.0) : slack;
}

/* ---- CSS 2.1 §E.2's "the text" — the placed characters of one formatting context ---------------------------
   line_box.h gives the frame, why a baseline rather than a top edge, and why advances ride as positions. This
   is a third reduction over `lb_fill`'s one pass, written beside the other two rather than in the painter,
   since a second collection of one context could disagree about where the run breaks. */

/* The array grows by doubling and an allocation failure is a `CHECK` (the out-of-memory floor): a dropped
   character is a word rendered without it, indistinguishable downstream from collapsed white space. */
static LineBoxGlyph *lb_glyphs_room(LineBoxGlyph *v, size_t n, size_t *cap)
{
    LineBoxGlyph *grown;

    if (n < *cap) return v;
    *cap = *cap ? *cap * 2 : 16;
    grown = realloc(v, *cap * sizeof *grown);
    CHECK(grown != NULL, "out of memory placing CSS 2.1 §E.2 \"Painting order\"'s step 7.2.1 text. There is "
                         "one entry per character css-text-3 §4.1.1's Phase I left on the line boxes of one "
                         "inline formatting context, so a failure here is the physical floor");
    return grown;
}

size_t line_box_glyphs(lxb_dom_element_t *style, BlockFlowRun run, LineBoxGlyph **out)
{
    LayoutJobDrive d;
    LbFill *f;
    const LbLineFacts *facts;
    LineBoxGlyph *v = NULL;
    size_t i, j, ng = 0, cap = 0;

    DCHECK(style != NULL && out != NULL,
           "CSS 2.1 §E.2 \"Painting order\"'s step 7.2.1 text was asked for with no element whose properties "
           "the establishing box has, or with nowhere to report the characters");
    /* §9.4.2's physical axes are asserted at this entry, not inside `lb_align_offset`, because both
       coordinates are physical and only `origin_x` goes through that helper; `baseline_y` comes from the
       `top`/`e.above` stack with no alignment in it, and css-writing-modes-4 §3.2 "Block Flow Direction: the
       writing-mode property" makes the block axis the one that moves. One call, not
       `line_box_inline_fragments`' two: this entry is handed the establishing container itself, so
       css-writing-modes-4 §7.3 "Orthogonal Flows" has no second element to compare here. */
    lb_require_horizontal_tb(style);
    /* A paint answers synchronously, so this entry is a drive over the two asks, as
       `line_box_inline_fragments` is over its own. */
    {
        LineBoxAvailableWidth avail = line_box_available_width_derived();

        layout_job_drive_begin(&d);
        while (!line_box_atomics_ask(style, run, avail) || !line_box_extents_ask(style, run, avail))
            (void)block_flow_drive_deferred(&d);
    }
    f = lb_layout(style, run, line_box_available_width_derived());
    facts = lb_fill_facts(f, style);
    for (i = 0; i < f->n; i++) {
        /* The line's baseline, `e.above` (its maximum `A'`) below its top edge: the coordinate
           `line_box_inline_fragments` and `lb_reduce` use, read at a third point of the same stack. */
        CssPx baseline = css_px_add(facts[i].top, facts[i].e.above);

        for (j = f->lines[i].from; j < f->lines[i].to; j++) {
            CssPx align;

            /* The other three kinds are refused by `text_run_measure_item_cp`, whose message says what a forced
               break's U+000A and an atomic's U+FFFC are for; this is the test every line walk makes. */
            if (!text_run_measure_item_is_text(&f->m, j)) continue;
            /* css-text-3 §4.1.2 renders a preserved tab "as a horizontal shift", not as a glyph. */
            if (text_run_measure_item_cp(&f->m, j) == 0x0009) continue;
            /* css-text-4 §7.3's distribution, read only where a character is placed, so a context whose lines
               all collapsed away never asks `lb_align_offset` for a computed `direction` and `text-align` (a
               crash surface, as core/layout/scrolling_area.c's origin read is). */
            align = lb_fill_align(f, style, i);
            v = lb_glyphs_room(v, ng, &cap);
            v[ng].cp = text_run_measure_item_cp(&f->m, j);
            v[ng].style = text_run_measure_item_style(&f->m, j);
            v[ng].face = text_run_measure_item_face(&f->m, j);
            /* The offset is `text_run_measure_line_offset` at this item, the walk `TextRunLine.size` is taken by,
               so a glyph's position and its line's width describe one text; no advance is accumulated here. */
            v[ng].origin_x = css_px_add(align, text_run_measure_line_offset(&f->m, f->lines[i], j));
            v[ng].baseline_y = baseline;
            ng++;
        }
    }
    layout_job_drive_end(&d);
    *out = v;
    return ng;
}

/* ---- CSSOM View §6's box fragments of one inline box -------------------------------------------------------
   line_box.h gives the frame, why the block axis is the box's own content area, and why this entry finds the
   formatting context itself where the two above are handed a run. */

/* §9.4.2's context `el` is on, expressed as line_box.h's other entries take one: the element whose
   properties the box has, the run of its children the box holds, and where the box's content box origin sits
   inside that element's content box. The offsets are fields, not a flag, so the composition adds them without
   asking which shape it got: a block container "that contains no block-level boxes" establishes the context
   over its whole child list with both offsets zero, while a mixed container's inline-level children sit in
   CSS 2.2 §9.2.1.1 "Anonymous block boxes"' boxes, one per maximal run, placed by CSS 2 §9.4.1 "Block
   formatting contexts"' stack among their block-level siblings. */
/* `style` is the container in both shapes, by §9.2.1.1: "the properties of anonymous boxes are inherited from
   the enclosing non-anonymous box …. Non-inherited properties have their initial value. For example, the font
   of the anonymous box is inherited from the DIV, but the margins will be 0." So it supplies every property
   the fill and §10.8 read, and it is the box line_box.h promises coordinates from; the origin offset keeps a
   mixed container's fragments in an unmixed one's frame.
   The inline-axis offset is zero (block_flow.h: `width: auto` and zero margins give the anonymous box its
   container's content edges under CSS 2.1 §10.3.3, which is also why `lb_align_offset` reads the container's
   width) and is still added, since the enumeration reports it. */
/* A collection, not one box, because of §9.2.1.1's split: "When an inline box contains an in-flow block-level
   box, the inline box (and its inline ancestors within the same line box) is broken around the block-level
   box …, splitting the inline box into two boxes (even if either side is empty), one on each side of the
   block-level box(es). The line boxes before the break and after the break are enclosed in anonymous block
   boxes." A broken box has fragments on several, which CSSOM View §6 "Extensions to the Element Interface"
   reports as `getClientRects()`'s "DOMRect objects in content order, one for each box fragment". */
typedef struct {
    BlockFlowRun run;        /* the content of one of §9.2.1.1's boxes — block_flow.h's own range */
    CssPx origin_x, origin_y;/* that box's content origin inside the container, block_flow.h's own offsets */
} LbBox;

typedef struct {
    lxb_dom_element_t *style;
    LbBox *box;              /* owned, in tree order; the caller owes exactly one `free` */
    size_t n;                /* never zero — §9.4.2's unmixed shape is the single entry `{NULL, NULL}` */
} LbContext;

/* The subject of every crash below: an element, not a line. This walk is reached from every CSSOM View §6
   member asking for a position or extent, from §7's scroll algorithms, a Range's client rects,
   IntersectionObserver's update and the rendering step, so an abort names its box rather than a call site: a
   caller's `__FILE__`/`__LINE__` would arrive through forwarding functions (`sa_inline_box_edge`,
   `flow_inline_fragment_rects`) and still not say which box has no rule, while every remedy below names a
   `display` and a container, as core/layout/used_value.c's `DFAILF`s do.
   It writes into a caller-owned buffer because a local name is a length and pointer with no terminator. A
   NULL element and a nameless one are different answers. It is total, as core/layout/box_subject.h explains:
   an assert during message composition replaces the first defect instead of reporting a second, so `cap`
   under 2 is reported in the string rather than asserted. */
static const char *lb_el_name(lxb_dom_element_t *el, char *buf, size_t cap)
{
    size_t len = 0;
    const lxb_char_t *tag = el == NULL ? NULL : lxb_dom_element_local_name(el, &len);

    if (buf == NULL || cap < 2) return "(nowhere to write an element name)";
    if (el == NULL) return "(no element)";
    if (tag == NULL) return "(no local name)";
    if (len > cap - 1) len = cap - 1;
    memcpy(buf, tag, len);
    buf[len] = '\0';
    return buf;
}

/* The same, for a node that may not be an element: §9.2.1.1 delimits runs over a container's whole child
   list, so the child a lookup fails on may be a text node, and the node kind is what a reader needs. */
static const char *lb_node_name(lxb_dom_node_t *n, char *buf, size_t cap)
{
    if (n == NULL) return "(no node)";
    if (n->type == LXB_DOM_NODE_TYPE_ELEMENT) return lb_el_name(lxb_dom_interface_element(n), buf, cap);
    if (n->type == LXB_DOM_NODE_TYPE_TEXT) return "(text node)";
    if (n->type == LXB_DOM_NODE_TYPE_COMMENT) return "(comment node)";
    return "(non-element node)";
}

/* The computed `display` of the same subject, serialized, the second half of the address. It leaks on the
   aborting path, one `abort()` away; in release the call sits inside `DFAILF`'s `sizeof` and is never
   evaluated. */
static const char *lb_el_display(lxb_dom_element_t *el)
{
    return el == NULL ? "(no element)" : css_display_serialize(css_computed_display(el));
}

/* Which of `container`'s anonymous block boxes hold `child`, and where each is. `child` is `container`'s own
   child on the path to the inline box being measured, one of the nodes §9.2.1.1's forcing classified, so this
   is a lookup in core/layout/block_flow.h's enumeration, not a second delimitation: a run's boundaries and its
   box's position are one derivation over §9.4.1's stack (core/layout/scrolling_area.c gives the same reason).
   The four asserts are four defects with four fixes: a container the section does not apply to, a per-child
   classification answered two ways, an unbroken child on several boxes, and a broken child not on the two the
   split's parenthesis guarantees. It answers a collection because a broken child has a fragment on several
   boxes and CSSOM View §6's `getClientRects()` wants "one for each box fragment". */
/* The container's own child that `n` is inside, or `n` when it is one. A `BlockFlowRun`'s bounds are
   positions in the container's content, so either may be inside an inline box the run splits; this projects
   one onto the child list the lookup is stated over. */
static lxb_dom_node_t *lb_container_child_of(lxb_dom_element_t *container, lxb_dom_node_t *n)
{
    /* The member of `container`'s box-tree child sequence that `n` is inside, which differs from the DOM
       child where css-display-3 §2.5 "Box Generation: the none and contents keywords"' splice has run: a
       `contents` element is in no sequence, so projecting onto it would order it against members it is not
       among. */
    /* The ascent is box parent to box parent, never `->parent`: `box_tree_parent` refuses a node that
       generates no box, and stepping by boxes keeps the result a member of `container`'s sequence. */
    while (n != NULL) {
        lxb_dom_element_t *p = box_tree_parent(box_tree_child_of_node(n));

        if (p == container) break;
        n = p == NULL ? NULL : lxb_dom_interface_node(p);
    }
    DCHECK(n != NULL,
           "CSS 2.2 §9.2.1.1's run is bounded by a node that is not a descendant of the container whose box "
           "list it came from, so the two came from different trees");
    return n;
}

/* Whether `n` is strictly inside `el`'s content (`el` itself is false): §9.2.1.1's question about a run
   bound. A bound inside an inline box is a break the box was broken around, so the fragment on that side
   carries one of its two edges; a bound at the box itself is a sibling boundary with the box wholly on one
   side. */
static bool lb_inside(lxb_dom_element_t *el, lxb_dom_node_t *n)
{
    lxb_dom_node_t *box = lxb_dom_interface_node(el);

    for (n = n == NULL ? NULL : n->parent; n != NULL; n = n->parent)
        if (n == box) return true;
    return false;
}

/* Whether `child`, one of `container`'s own children, is inside `run`. §9.2.1.1's runs partition the
   container's content, not its child list, so this is a range intersection. Each exclusive bound, projected
   onto the child list (`lb_container_child_of`), has three cases:
     - NULL: the run reaches the start or end of the content, so that side constrains nothing;
     - a different child: a sibling boundary, and `child` is inside only on the run's side of it;
     - `child` itself: the bound is inside `child`, so the run begins or ends part way through it, which a
       sibling scan cannot express (`[after->next, end)` would exclude `child` from every run).
   In the third case a run's `end` on `child` is §9.2.1.1's "C1 (open at the end of the line)", its `after`
   on `child` is "C2 (open at the start of the line)", and both is a fragment between two block-level boxes
   that owns text and neither edge. For a child no run splits, this is the sibling scan. */
static bool lb_run_holds_child(lxb_dom_element_t *container, BlockFlowRun run, lxb_dom_node_t *child)
{
    BoxTreeChild c;
    lxb_dom_node_t *a, *e;

    if (run.after != NULL) {
        a = lb_container_child_of(container, run.after);
        if (a != child) {
            /* `child` must be strictly after the projected bound (`after` is exclusive). The scan is over the
               container's box-tree sequence, which both are members of; a `->next` scan could step onto a
               `contents` element and out of the list. */
            for (c = box_tree_next_sibling(container, box_tree_child_of_node(a));
                 box_tree_child_exists(c) && !box_tree_child_is_node(c, child);
                 c = box_tree_next_sibling(container, c))
                ;
            if (!box_tree_child_exists(c)) return false;
        }
    }
    if (run.end != NULL) {
        e = lb_container_child_of(container, run.end);
        if (e != child) {
            /* … and strictly before the other (`end` is exclusive). */
            for (c = box_tree_child_of_node(child); box_tree_child_exists(c) && !box_tree_child_is_node(c, e);
                 c = box_tree_next_sibling(container, c))
                ;
            if (!box_tree_child_exists(c)) return false;
        }
    }
    return true;
}

static bool lb_anon_run(lxb_dom_element_t *container, lxb_dom_node_t *child, LbContext *ctx)
{
    BlockFlowAnonBox *v = NULL;
    size_t n = 0, i, found = 0;
    /* §9.2.1.1's own predicate, read only to assert with: the collection below does not branch on it, so it
       is a second derivation of one fact the closing asserts compare. */
    bool breaks = block_flow_child_breaks_inline_box(container, box_tree_child_of_node(child));
    char cbuf[64], nbuf[64];

    ctx->box = NULL;
    ctx->n = 0;
    if (!block_flow_anonymous_boxes_ask(container, &v, &n)) return false;
    DCHECKF(n > 0,
           "<%s> (display `%s`), child <%s>: "
           "CSS 2.2 §9.2.1.1 \"Anonymous block boxes\" generated NO box inside a block container that answered "
           "FALSE to CSS 2.2 §9.4.2 \"Inline formatting contexts\"' establishing condition, while an "
           "inline-level box inside that container is being measured. Those two answers cannot both be right. "
           "§9.4.2's condition is that \"an inline formatting context is established by a block container box "
           "that contains no block-level boxes\", so a FALSE says this container holds one — and §9.2.1.1 then "
           "wraps every maximal run of inline-level children, of which the run holding this box is one. A zero "
           "is core/layout/block_flow.h's positive statement that this container has NO inline-level content "
           "at all, which the box being measured contradicts, so the two are its one per-child predicate asked "
           "twice over one child list and answered differently",
           lb_el_name(container, cbuf, sizeof cbuf), lb_el_display(container),
           lb_node_name(child, nbuf, sizeof nbuf));
    /* Every one of §9.2.1.1's boxes this child has a fragment on. The predicate is a range intersection over
       the content, so a child the section breaks answers true for every run its break separates ("splitting
       the inline box into two boxes (even if either side is empty), one on each side of the block-level
       box(es)") and an unbroken child for exactly one. The array is sized at `n`, the container's box count,
       rather than by a second pass over block_flow.h's enumeration. */
    if (n > 0) {
        ctx->box = malloc(n * sizeof *ctx->box);
        CHECK(ctx->box != NULL,
              "out of memory naming CSS 2.2 §9.2.1.1's anonymous block boxes an inline box has a fragment on. "
              "There is at most one entry per box of one block container, so a failure here is the physical "
              "floor");
        for (i = 0; i < n; i++) {
            if (!lb_run_holds_child(container, v[i].run, child)) continue;
            ctx->box[found].run = v[i].run;
            ctx->box[found].origin_x = v[i].content_x;
            ctx->box[found].origin_y = v[i].content_y;
            found++;
        }
    }
    free(v);
    ctx->n = found;
    DCHECKF(found != 0,
           "<%s> (display `%s`), child <%s>: "
           "the block container's child holding this inline box is in NONE of CSS 2.2 §9.2.1.1 \"Anonymous "
           "block boxes\"' runs. Every child that generates an inline-level box is inside at least one of them, "
           "because the section forces the container \"to have only block-level boxes inside it\" — so a child "
           "is either a block-level box, or generates no box, or has a fragment on the box wrapping each run "
           "its content meets — and the caller has already established that an inline box hangs below this "
           "child. So core/layout/block_flow.c classified it as block-level or as generating no box at all "
           "while this walk reached a box on a line through it, and filling the container's whole child list "
           "instead would partition a run this box is not in",
           lb_el_name(container, cbuf, sizeof cbuf), lb_el_display(container),
           lb_node_name(child, nbuf, sizeof nbuf));
    /* The count and §9.2.1.1's predicate answer one question and are asserted against each other: a child
       `block_flow_child_breaks_inline_box` answers false for lies wholly inside one run, and one it answers
       true for is broken around at least one in-flow block-level box and so has a fragment on the box before
       that break and the box after it. The runs partition the content, not the child list. */
    DCHECKF(breaks || found == 1,
           "<%s> (display `%s`), child <%s>: "
           "one child of a block container is inside SEVERAL of CSS 2.2 §9.2.1.1 \"Anonymous block boxes\"' "
           "runs while the section's own breaking predicate says it is not split. A child that holds no "
           "in-flow block-level box has no break for the section to wrap around — \"when an inline box "
           "contains an in-flow block-level box, the inline box … is broken around the block-level box\" is "
           "the only sentence that puts one child on two boxes — so it lies wholly inside one run. Two hits "
           "are core/layout/block_flow.c's stack and its breaking predicate disagreeing about which of this "
           "container's children hold a block-level box",
           lb_el_name(container, cbuf, sizeof cbuf), lb_el_display(container),
           lb_node_name(child, nbuf, sizeof nbuf));
    DCHECKF(!breaks || found >= 2,
           "<%s> (display `%s`), child <%s>: "
           "CSS 2.2 §9.2.1.1 \"Anonymous block boxes\" BREAKS this child and yet it has a fragment on FEWER "
           "than two of the container's boxes. The section splits a broken inline box \"into two boxes (even "
           "if either side is empty), one on each side of the block-level box(es)\" and encloses "
           "\"the line boxes before the break and after the break\" in anonymous block boxes, so a broken "
           "child is on at least the one before its first break and the one after it. A lower count is "
           "core/layout/block_flow.c's enumeration not having emitted a box at a break its own predicate "
           "reports, or `block_flow_run_generates_box` having dropped one of the two EMPTY sides the "
           "parenthesis above says the section generates anyway",
           lb_el_name(container, cbuf, sizeof cbuf), lb_el_display(container),
           lb_node_name(child, nbuf, sizeof nbuf));
    return true;
}

/* The inline formatting context `el` is on: the nearest ancestor that generates a block container box,
   reached by walking past the inline boxes between, then which of §9.4.2's two shapes holds this box. §9.4.2's
   condition is asked over the container's whole child list, the one shape with an element to name it
   (core/layout/block_flow.h); a mixed container's runs are §9.2.1.1's anonymous block boxes, and the one
   holding this box supplies the run and origin `lb_anon_run` looks up. False ("not yet") when that lookup's
   §9.4.1 walk deferred a contribution; `*out` is then unwritten. */
static bool lb_establishing_context(lxb_dom_element_t *el, LbContext *out)
{
    lxb_dom_node_t *a, *child = lxb_dom_interface_node(el);
    char ebuf[64], abuf[64];
    LbContext ctx;

    ctx.style = NULL;
    ctx.box = NULL;
    ctx.n = 0;
    /* The two kinds of ancestor this walk steps over are stepped over differently, which decides the node
       named as the container's child. An `inline` ancestor generates a box, so it is a member of the
       container's box list and becomes the lookup's key. A `contents` ancestor generates none (css-display-3
       §2.5 "Box Generation: the none and contents keywords"), so the member is whatever is already in
       `child`, and advancing past it would key the lookup on a node in no sequence. So the advance is
       conditional, not in the increment clause. */
    for (a = child->parent; a != NULL && a->type == LXB_DOM_NODE_TYPE_ELEMENT; a = a->parent) {
        lxb_dom_element_t *anc = lxb_dom_interface_element(a);
        CssDisplay d = css_computed_display(anc);
        bool container = block_flow_display_is_block_container(d);
        bool inline_box = css_display_spells(d, CSS_KEYWORD_INLINE);
        bool step_over = inline_box || css_display_spells(d, CSS_KEYWORD_CONTENTS);

        /* The `container` arm below returns, so this test is reached only with `container` false. */
        if (!container && !step_over)
            DFAILF("<%s> (display `%s`), ancestor <%s> (display `%s`): "
                  "CSS 2.2 §9.4.2's inline formatting context was asked for an inline box whose nearest "
                  "box-generating ancestor is neither a BLOCK CONTAINER nor an inline box it can be reached "
                  "through. Its computed `display` makes it a table box, a table row or row group, a flex or "
                  "grid container, or a box that generates none at all — and each of those puts this box in a "
                  "formatting context a DIFFERENT module owns: CSS 2.1 §17.5 \"Visual layout of table "
                  "contents\" for the first three, css-flexbox-1 §4 \"Flex Items\" and "
                  "css-grid-1 §6.1 \"Grid Item Display\" for the next two, which "
                  "BLOCKIFY their children so an `inline` child of one is not an inline box at all. Fix the "
                  "blockification where the child's `display` is computed (css-display §2.7's "
                  "blockification), or BUILD the module that owns the container",
                  lb_el_name(el, ebuf, sizeof ebuf), lb_el_display(el),
                  lb_el_name(anc, abuf, sizeof abuf), lb_el_display(anc));
        if (container) {
            ctx.style = anc;
            /* §9.4.2's condition over the whole child list picks the shape, asked once through the component that
               owns it, so this branch and `lb_anon_run`'s enumeration are one classification. A container that
               establishes the context itself is the run with no bound on either side, spelled as the one-entry
               collection so the fill below has one loop over both shapes. */
            if (block_flow_establishes_inline_context(anc)) {
                ctx.box = malloc(sizeof *ctx.box);
                CHECK(ctx.box != NULL,
                      "out of memory naming CSS 2.2 §9.4.2's inline formatting context. It is ONE entry for a "
                      "container that establishes the context itself, so a failure here is the physical floor");
                ctx.box[0].run.after = NULL;
                ctx.box[0].run.end = NULL;
                ctx.box[0].origin_x = css_px(0.0);
                ctx.box[0].origin_y = css_px(0.0);
                ctx.n = 1;
                *out = ctx;
                return true;
            }
            if (!lb_anon_run(anc, child, &ctx)) return false;
            *out = ctx;
            return true;
        }
        if (inline_box) child = a;
    }
    /* The one abort on this walk whose fix is the caller's, not the box's, so it names connectedness rather
       than a `display`: some entry asked for a position without the has-a-box predicate, and the highest
       ancestor reached says whether the tree is detached or rootless. */
    DFAILF("<%s> (display `%s`), highest ancestor reached <%s>: "
          "the inline formatting context walk ran out of ancestors without finding a block container box. "
          "CSS Display §2.8 makes the ROOT ELEMENT a block container whatever it declares (\"a display of "
          "contents computes to block on the root element\"), so every element inside a document has one above "
          "it — an element that reached this line is in a tree with no root element, which is a caller that "
          "asked where a DETACHED box is. core/dom/element_view.h's has-a-box predicate answers that before "
          "any position is asked for, so the two have come apart",
          lb_el_name(el, ebuf, sizeof ebuf), lb_el_display(el),
          lb_node_name(child, abuf, sizeof abuf));
    *out = ctx;
    return true;
}

/* This inline box's items in one of §9.2.1.1's boxes: the half-open range `[*lo, *hi)` of the fill just
   collected, or false when the box has no fragment here. `lb_child` emits the opening edge, descends, and
   emits the closing edge (css-text-3 §5.5 puts them at the box's boundaries), so a box's items are contiguous
   and "which lines is this box on" is a range intersection.
   §9.2.1.1's split means the range is not always two edges: "The line boxes before the break and after the
   break are enclosed in anonymous block boxes", so the fragment before the first break has the opening edge
   alone ("C1 (open at the end of the line)"), the one after the last break the closing edge alone ("C2 (open
   at the start of the line)"), and one between two block-level boxes neither. Which edges to expect is read
   off the run's bounds (`lb_inside`): `run.after` inside the box means the opening edge is on an earlier box,
   `run.end` inside it that the closing edge is on a later one, and the count must be 2 less one for each; a
   mismatch is `lb_fill`'s step-out and block_flow.h's enumeration describing two splits of one box. */
/* False is a positive answer for a descendant: the collection is keyed on the container's child, and `el` may
   sit on one side of that child's break (`<a>t<b>x</b><div/>u</a>` puts `<b>` in the first run only), so the
   other run's fill holds none of its items. It is told from a defect because a box open on either side is
   present by construction, so only a box open on neither can be absent. */
static bool lb_box_items(const TextRunMeasure *m, lxb_dom_element_t *el, size_t count, BlockFlowRun run,
                         size_t *lo, size_t *hi, bool *has_open, bool *has_close)
{
    size_t i, found = 0, first = 0, last = 0, want;
    bool open_at_start = lb_inside(el, run.after);
    bool open_at_end = lb_inside(el, run.end);

    for (i = 0; i < count; i++) {
        if (text_run_measure_item_style(m, i) != el) continue;
        /* The three non-edge kinds are excluded by name: a character or forced break carries an element that is
           not this box's boundary, and an atomic inline is "a single opaque box" with one item. None can belong
           to `el` here (it is a non-replaced inline box and the walk does not descend into atomics); the test
           makes that a statement, so no such kind delimits a fragment. */
        if (text_run_measure_item_is_text(m, i) || text_run_measure_item_is_forced_break(m, i) ||
            text_run_measure_item_is_atomic(m, i))
            continue;
        if (found == 0) first = i;
        last = i;
        found++;
    }
    if (!open_at_start && !open_at_end && found == 0) return false;
    want = (size_t) 2 - (open_at_start ? 1 : 0) - (open_at_end ? 1 : 0);
    DCHECK(found == want,
           "an inline box does not carry the box-edge items CSS 2.2 §9.2.1.1's split leaves it in the run its "
           "formatting context collected. core/layout/line_box.c emits one edge before descending into the box "
           "and one after (`text_run_measure_add_box_edge`, which css-text-3 §5.5 \"Line Breaking Details\" "
           "places at the box's two boundaries), and it emits them even when both are ZERO because an edge "
           "occupies a POSITION — so a fragment the section leaves open at the start carries the closing edge "
           "alone, one open at the end carries the opening edge alone, and one between two block-level boxes "
           "carries neither. Which of those this run is was read from the run's OWN bounds, so a count other "
           "than the one they imply is core/layout/block_flow.c's enumeration and `lb_fill`'s step-out "
           "disagreeing about where this box was broken, and every fragment below would be delimited by "
           "another box's boundary");
    DCHECK(found < 2 || first < last,
           "an inline box's CLOSING edge item is not after its opening one, so the range that is supposed to "
           "hold its content holds none of it");
    /* A fragment open at the start begins at the fill's first item and one open at the end runs to its last:
       the run begins (or ends) at the break inside this box, so everything from index 0 (or to the end) is
       inside it. */
    *lo = open_at_start ? 0 : first;
    *hi = open_at_end ? count : last + 1;
    *has_open = !open_at_start;
    *has_close = !open_at_end;
    return true;
}

/* The one run item of an atomic inline-level box. CSS 2.2 §9.2.2 "Inline-level elements and inline boxes": it
   "participate[s] in [its] inline formatting context as a single opaque box", so `lb_child` emits one item
   and does not descend; the range is that item's index. Other kinds carrying this element are asserted
   against, not skipped, because this box owns nothing but its item (unlike `lb_box_items`, whose box owns its
   descendants' characters). False is "not in this run", as for `lb_box_items`: an atomic inside a child
   §9.2.1.1 breaks sits on one side of the break, and since it is never split it is on exactly one box, which
   the caller's closing count asserts. */
static bool lb_atomic_item(const TextRunMeasure *m, lxb_dom_element_t *el, size_t count, size_t *at)
{
    size_t i, found = 0;

    for (i = 0; i < count; i++) {
        if (text_run_measure_item_style(m, i) != el) continue;
        DCHECK(text_run_measure_item_is_atomic(m, i),
               "an ATOMIC INLINE-LEVEL box owns a run item that is not the atomic one — a character, a forced "
               "break, or one of the two box EDGES core/layout/line_box.c brackets an inline box's content "
               "with. CSS 2.2 §9.2.2 makes this box \"a single opaque box\" and CSS 2.1 §3.1 puts a replaced "
               "element's content \"outside the scope of the CSS formatting model\", so `lb_child` emits its "
               "one item and does not descend — an item of any other kind carrying this element is that walk "
               "having collected an inside this box does not have");
        *at = i;
        found++;
    }
    DCHECK(found <= 1,
           "an ATOMIC INLINE-LEVEL box has MORE THAN ONE run item in the formatting context that collected "
           "it. `lb_child` emits one (`text_run_measure_add_atomic`, css-text-3 §5.5 \"Line Breaking "
           "Details\"' \"each replaced element or other atomic inline\") and returns without descending, so a "
           "count above one is two walks through one accumulator. A count of ZERO is not a defect here and is "
           "not asserted against: it is this box being on another of CSS 2.2 §9.2.1.1's boxes, which the "
           "caller's own count over all of them settles");
    return found == 1;
}

bool line_box_inline_fragments_ask(lxb_dom_element_t *el, lxb_dom_element_t **establishing,
                                   LineBoxFragment **out, size_t *count)
{
    LbFill *f;
    const LbLineFacts *facts;
    const TextRunLine *lines;
    /* NULL so the first growth is the same call as every later one (`realloc(NULL, n)` is `malloc(n)`). */
    LineBoxFragment *frags = NULL;
    LbContext ctx;
    lxb_dom_element_t *style;
    CssPx lead, trail;
    CssPx before = css_px(0.0), after = css_px(0.0), drop = css_px(0.0), extent = css_px(0.0);
    /* §10.8.1's split of this box's margin box at its own baseline, zero for a box the section gives no
       baseline, so the atomic composition below is one arithmetic over both shapes. */
    CssPx below = css_px(0.0);
    size_t n, i, b, nf = 0;
    bool atomic;

    DCHECK(el != NULL && establishing != NULL && out != NULL && count != NULL,
           "CSSOM VIEW §6's box fragments were asked for with no element or nowhere to report them");
    DCHECK(css_computed_display_is(el, CSS_KEYWORD_INLINE) || css_computed_display_is(el, CSS_KEYWORD_INLINE_BLOCK) ||
               css_computed_display_is(el, CSS_KEYWORD_INLINE_GRID) ||
               css_computed_display_is(el, CSS_KEYWORD_INLINE_FLEX),
           "CSSOM VIEW §6's box fragments were asked for a box that `lb_child` does not put on a line: neither "
           "`inline`, `inline-block`, `inline-grid` nor `inline-flex`. The caller's own step (core/dom/"
           "element_view.h's fragment kind, core/layout/flow_position.c's placement) classifies first, so the "
           "two lists have come apart");
    /* Which delimitation this box's fragments take; `display` decides only half. CSS 2.2 §9.2.2:
       "Inline-level boxes that are not inline boxes (such as replaced inline-level elements, inline-block
       elements, and inline-table elements) are called atomic inline-level boxes because they participate in
       their inline formatting context as a single opaque box". HTML §15.4 "Replaced elements" makes an `img`,
       `iframe`, `video` or `input` replaced while its `display` stays `inline`; an `inline-block` is atomic by
       `display` alone, replaced or not (§10.3.10: "Exactly as inline replaced elements."). css-text-3 §5.5
       "Line Breaking Details" treats the class alike ("a soft wrap opportunity before and after each replaced
       element or other atomic inline"), so `lb_child` collects one item and the fragment is delimited by its
       index. A single opaque box is never the box §9.4.2 "split[s] into several boxes", so it is exactly one
       fragment, which the closing assert checks. */
    atomic = replaced_element_of(el).replaced || css_computed_display_is(el, CSS_KEYWORD_INLINE_BLOCK) ||
             css_computed_display_is(el, CSS_KEYWORD_INLINE_GRID) ||
             css_computed_display_is(el, CSS_KEYWORD_INLINE_FLEX);
    /* HTML §15.3.4 "Phrasing content"'s `br { display-outside: newline; }` and `wbr { display-outside:
       break-opportunity; }` reach here as `display: inline` boxes (core/layout/phrasing_break.h), and
       `lb_child` puts them on the line as a forced break or an opportunity rather than as a box with two
       boundaries, so they have no edge items for the range below. */
    if (phrasing_break_of(el) != PHRASING_BREAK_NONE)
        DFAIL("CSSOM VIEW §6's box fragments were asked for an element HTML §15.3.4 \"Phrasing content\" gives "
              "a `display-outside` of `newline` or `break-opportunity` — a `br` or a `wbr`. CSS 2.2 §9.4.2's "
              "run holds it as the BREAK it is and not as an inline box with two boundaries, so it has no edge "
              "items to delimit a fragment with. THE SINGLE-ITEM DELIMITATION IS BUILT AND IS NOT WHAT THIS "
              "WAITS FOR: `lb_atomic_item` below delimits a fragment by an item's OWN INDEX and the loop after "
              "it composes the rectangle, so what is left is one ROUTE and one DERIVATION, and each names a "
              "different absence. THE ROUTE: that lookup asserts the item is text_run.h's ATOMIC kind, which "
              "is the kind css-text-3 §5.5 \"Line Breaking Details\" gives \"each replaced element or other "
              "atomic inline\" and is NOT the FORCED-BREAK kind a `br` is collected as — widen it to the "
              "item's own element and let each caller state the kind it expects. THE DERIVATION: an atomic "
              "inline's block axis hangs its MARGIN BOX from the line's baseline (CSS 2.2 §10.8's "
              "`vertical-align` `baseline`: \"if the box does not have a baseline, align the bottom margin "
              "edge with the parent's baseline\"), and a `br` has no margin box to hang — the box it generates "
              "is one no CSS module sizes, because no module defines `display-outside: newline` at all "
              "(core/layout/phrasing_break.h states that in full), so there is no `Applies to:` line to read a "
              "width or a height off. Its border area is a ZERO-WIDTH rectangle at its item's position with "
              "§10.6.1's content area out of its own first available font, which `lb_line_walk_item` already "
              "reads for it (`<br style=\"line-height:100px\">` makes a line 100px tall). A `wbr` does not "
              "reach even that: `lb_phrasing_break` crashes for the soft wrap opportunity itself, one walk "
              "earlier, so it is that crash and not this one that gates it");
    lb_require_horizontal_tb(el);
    if (!lb_establishing_context(el, &ctx)) return false;
    style = ctx.style;
    lb_require_horizontal_tb(style);
    *establishing = style;
    /* §6's step 3 asks for the border area and the run carries the margin one (an edge item is css-sizing-3
       §2.2's outer size, "based on the outer size of the box"), so the two margins come off, and which fragment
       each comes off is CSS 2.2 §9.4.2's sentence: "when an inline box is split, margins, borders, and padding
       have no visual effect where the split occurs (or at any split, when there are several)". The leading
       margin is on the fragment with the opening edge and the trailing one on the fragment with the closing
       edge; middle fragments run edge to edge. §9.2.1.1's split is read the same way, through `lb_box_items`'
       report of which fragments carry which edge. An atomic inline's one item is its margin box
       (`used_value_margin_edge_px`; §9.4.2: "horizontal margins, borders, and padding are respected between
       these boxes"), and it is unsplit, so both margins come off its one fragment. */
    lead = used_value_px(el, "margin-left");
    trail = used_value_px(el, "margin-right");
    if (atomic) {
        /* CSS 2.2 §10.8.1's `vertical-align` definition decides an atomic inline's block axis, not §10.6.1: "For
           all other elements, the box used for alignment is the margin box", and `baseline`: "Align the
           baseline of the box with the baseline of the parent box. If the box does not have a baseline, align
           the bottom margin edge with the parent's baseline." `lb_atomic_extent` returns the margin box split
           at the box's own baseline. A replaced element has none, so `below` is zero and its bottom margin edge
           is on the line's baseline; a non-replaced `inline-block` has the one §10.8.1 puts inside it ("The
           baseline of an 'inline-block' is the baseline of its last line box in the normal flow, unless it has
           either no in-flow line boxes or if its 'overflow' property has a computed value other than 'visible',
           in which case the baseline is the bottom margin edge"), so its margin box hangs `below` further down.
           CSS 2.2 writes the `line-height` and `vertical-align` definitions under the §10.8.1 heading, the last
           in the chapter, so all three sentences lie inside §10.8.1. */
        /* The read is outside the loop because the split is a fact about the box, and a single opaque box has
           one line. It is the call `lb_line_walk_item` makes for this box, so the line and the fragment come from
           one reading of §10.8's step 1. §8.1's nesting is the rest: the border area's far edge is the bottom
           margin edge less `margin-bottom`, and its near edge one used border-edge extent further up (§10.6.2
           "Inline replaced elements, block-level replaced elements in normal flow, 'inline-block' replaced
           elements in normal flow and floating replaced elements" for a replaced box, §10.6.6 "Complicated
           cases"' redirection to §10.6.7 for a non-replaced `inline-block`, through core/layout/used_value.h). */
        below = lb_atomic_extent(el).below;
        drop = used_value_px(el, "margin-bottom");
        extent = used_value_border_edge_px(el, true);
    } else {
        /* CSS 2 §8.1 "Box dimensions"' two nestings between this box's content area and its border area on the
           block axis, stated over the content area by §10.6.1 ("the vertical padding, border and margin of an
           inline, non-replaced box start at the top and bottom of the content area"). Read once: §9.4.2's split
           ("no visual effect where the split occurs") cuts the inline axis and leaves both block-axis edges on
           every fragment. */
        before = css_px_add(lb_border_px(el, "top"), used_value_px(el, "padding-top"));
        after = css_px_add(lb_border_px(el, "bottom"), used_value_px(el, "padding-bottom"));
    }
    /* One pass per §9.2.1.1 box this element has a fragment on; the concatenation is content order because
       block_flow.h enumerates its boxes in tree order and each fill partitions its box's content into lines in
       order, which is CSSOM View §6's "DOMRect objects in content order, one for each box fragment". For an
       element the section does not break the loop runs once.
       The run is filled, not the child list: in a mixed container a fill over the whole child list would flow
       this box's items with every other run's, on line boxes that do not exist, and two boxes separated by a
       block-level box are filled separately for the same reason. The stack is seeded at each box's own top
       content edge, keeping coordinates in line_box.h's frame (offsets from the container's content box) while
       lines are measured inside their box; the seed is zero for the unmixed shape. */
    for (b = 0; b < ctx.n; b++) {
        if (!line_box_atomics_ask(style, ctx.box[b].run, line_box_available_width_derived()) ||
            !line_box_extents_ask(style, ctx.box[b].run, line_box_available_width_derived())) {
            free(ctx.box);
            return false;
        }
    }
    for (b = 0; b < ctx.n; b++) {
        size_t lo_item, hi_item;
        bool has_open, has_close, present;

        f = lb_layout(style, ctx.box[b].run, line_box_available_width_derived());
        n = f->n;
        lines = f->lines;
        DCHECK(n >= 1,
               "CSS 2.2 §9.4.2's fill produced NO line box for one of CSS 2.2 §9.2.1.1 \"Anonymous block boxes\"' "
               "boxes. \"Line boxes are created as needed to hold inline-level content\" and the section generates "
               "one of these boxes only to wrap such content — `block_flow_run_generates_box` is the predicate that "
               "says so, and an empty answer is that predicate and this fill disagreeing about whether this run "
               "holds any");
        /* The item count is `lines[n - 1].to`, the partition's, because `text_run_fill_step` asserts its
           last line closes at the end of the collection. Both delimitations produce one half-open item range and
           the loop reads nothing else: an inline box's range is the boundaries §9.2.1.1's split left in this box
           plus its descendants' items between, an atomic's the one item of §9.2.2's "single opaque box". Either
           may answer "not on this box", a positive statement for an element on one side of a broken child. */
        lo_item = hi_item = 0;
        if (atomic) {
            /* `lb_atomic_item` leaves `lo_item` untouched when there is no item here, so the `+ 1` is inside the
               arm that established it. */
            present = lb_atomic_item(&f->m, el, lines[n - 1].to, &lo_item);
            if (present) hi_item = lo_item + 1;
            has_open = has_close = true;
        } else {
            present = lb_box_items(&f->m, el, lines[n - 1].to, ctx.box[b].run,
                                   &lo_item, &hi_item, &has_open, &has_close);
        }
        if (!present) continue;
        /* One entry per line of this box, appended to the earlier boxes' fragments; `nf + n` is an upper
           bound, so the array is grown to it rather than counted twice. */
        frags = realloc(frags, (nf + n) * sizeof *frags);
        CHECK(frags != NULL, "out of memory reporting CSSOM VIEW §6's box fragments. There is one entry per line "
                             "box of one formatting context and the fragments are a subset of them, so a failure "
                             "here is the physical floor");
        facts = lb_fill_facts(f, style);
        for (i = 0; i < n; i++) {
            LbExtent e = facts[i].e;
            /* §9.4.2's stacking position, seeded at this box's own top content edge. */
            CssPx top = css_px_add(ctx.box[b].origin_y, facts[i].top);

            if (lines[i].from < hi_item && lines[i].to > lo_item) {
                size_t lo = lines[i].from > lo_item ? lines[i].from : lo_item;
                size_t hi = lines[i].to < hi_item ? lines[i].to : hi_item;
                CssPx align = lb_fill_align(f, style, i);
                /* The offset along the line is inside the box holding it, and `ctx.box[b].origin_x` is that box's
                   inline-start content edge inside the container: zero by §9.2.1.1's initial values under
                   §10.3.3, added for the reason `LbContext` gives, and read per box because a split element's
                   fragments are on several boxes. */
                CssPx start = css_px_add(ctx.box[b].origin_x,
                                         css_px_add(align, text_run_measure_line_offset(&f->m, lines[i], lo)));
                CssPx end = css_px_add(ctx.box[b].origin_x,
                                       css_px_add(align, text_run_measure_line_offset(&f->m, lines[i], hi)));
                /* The line's baseline, its maximum `A'` (`e.above`) below its top, from which every box on it
                   hangs its content area. */
                CssPx baseline = css_px_add(top, e.above);

                if (has_open && lo == lo_item) start = css_px_add(start, lead);
                if (has_close && hi == hi_item) end = css_px_sub(end, trail);
                frags[nf].inline_start = start;
                frags[nf].inline_end = end;
                DCHECK(frags[nf].inline_end.px >= frags[nf].inline_start.px,
                       "a box fragment's END is before its START along the line. Both are "
                       "`text_run_measure_line_offset` over the SAME line at two bounds and that walk is a sum of "
                       "non-negative advances and non-negative box edges, so a decreasing prefix is an advance "
                       "measure or an edge that lost its sign");
                if (atomic) {
                    /* §10.8.1's `vertical-align` `baseline` over this box's own baseline: its bottom margin edge is
                       `below` beneath the line's baseline (zero for a box with no baseline), and CSS 2 §8.1 nests
                       the border area inside it, ending one `margin-bottom` above that edge and beginning one used
                       border-edge extent further up. */
                    CssPx margin_bottom_edge = css_px_add(baseline, below);

                    frags[nf].block_end = css_px_sub(margin_bottom_edge, drop);
                    frags[nf].block_start = css_px_sub(frags[nf].block_end, extent);
                    /* The one place this rectangle and core/layout/used_value.h can disagree, asserted. The block
                       axis is that component's extent by construction; the inline axis is
                       `text_run_measure_line_offset` at two adjacent item bounds less the two horizontal margins,
                       and must equal the used border-edge extent CSSOM View §6's step 3 reports for this element
                       through core/dom/element_view.c's one-fragment arm. It catches the item sized off another
                       element, an index naming the wrong item, or css-text-3 §4.1.2's trimming reaching between
                       adjacent bounds; the tolerance is IEEE-754's, as in `lb_strut_extent`. Which section sized
                       the box (§10.3.2 "Inline, replaced elements", §10.3.9 "'Inline-block', non-replaced
                       elements in normal flow", or §10.3.10's delegation to §10.3.2) is not its question: both
                       sides read used_value.h, so it tests that the item and the margins describe one element. */
                    DCHECK(lb_close(css_px_sub(frags[nf].inline_end, frags[nf].inline_start).px,
                                    used_value_border_edge_px(el, false).px),
                           "an ATOMIC INLINE-LEVEL box's fragment is a different width along the line than CSS 2.1 "
                           "§10.3's used BORDER EDGE for it — §10.3.2 \"Inline, replaced elements\" for a replaced "
                           "box, §10.3.9 \"'Inline-block', non-replaced elements in normal flow\" for a "
                           "non-replaced `inline-block`, and §10.3.10 \"'Inline-block', replaced elements in "
                           "normal flow\" (\"Exactly as inline replaced elements.\") for the third. The run item "
                           "carries "
                           "`used_value_margin_edge_px` — CSS 2.2 §9.4.2's \"horizontal margins, borders, and "
                           "padding are respected between these boxes\" — so its span at two ADJACENT item bounds "
                           "less the box's own two horizontal margins is that same nesting read back through CSS 2 "
                           "§8.1, and a disagreement is the item having been sized for a different element than "
                           "the margins were read for");
                } else {
                    /* §10.6.1's content area from the first available font's `A` and `D`, with CSS 2 §8.1's
                       padding and border outside it: "the vertical padding, border and margin of an inline,
                       non-replaced box start at the top and bottom of the content area, and has nothing to do with
                       the 'line-height'." The line's height is not read for this rectangle. */
                    frags[nf].block_start = css_px_sub(css_px_sub(baseline, css_font_ascent_px(el)), before);
                    frags[nf].block_end = css_px_add(css_px_add(baseline, css_font_descent_px(el)), after);
                }
                nf++;
            }
        }
    }
    free(ctx.box);
    DCHECK(nf >= 1,
           "an inline-level box landed on NO line box of ANY of the boxes its formatting context was collected "
           "into. Its items are inside `[0, count)` of one of those fills and CSS 2.2 §9.4.2's fill PARTITIONS "
           "those items across its lines, so every item is on exactly one line — and `lb_anon_run` has already "
           "asserted that at least one of CSS 2.2 §9.2.1.1 \"Anonymous block boxes\"' boxes holds the child "
           "this box hangs below. An empty total is therefore either that partition having lost an item, which "
           "`text_run_fill_step` asserts it does not, or every one of those boxes having answered NOT "
           "PRESENT — which cannot be, since the child is in each of their runs and this box is inside the "
           "child");
    /* §9.2.2's "single opaque box" is the count, asserted: an atomic's item range is one index and the fill's
       lines partition the items, so it meets one line; and `lb_atomic_item` answers not present on every
       §9.2.1.1 box but its own, since the section breaks only inline boxes. A count above one is the fill
       putting one item on two lines, or this element collected into two anonymous block boxes. */
    DCHECK(!atomic || nf == 1,
           "an ATOMIC INLINE-LEVEL box was reported as MORE THAN ONE box fragment. CSS 2.2 §9.2.2 "
           "\"Inline-level elements and inline boxes\" makes it \"a single opaque box\", so it is never the "
           "box §9.4.2 \"SPLIT[s] into several boxes … distributed across several line boxes\", and it is not "
           "one CSS 2.2 §9.2.1.1 \"Anonymous block boxes\" splits either — that section breaks an INLINE box "
           "around an in-flow block-level box, and an atomic participates \"as a single opaque box\". So a "
           "second fragment is §9.4.2's fill having put one item on two lines, or this element having been "
           "collected into two of the container's anonymous block boxes");
    *out = frags;
    *count = nf;
    return true;
}

size_t line_box_inline_fragments(lxb_dom_element_t *el, lxb_dom_element_t **establishing,
                                 LineBoxFragment **out)
{
    LayoutJobDrive d;
    size_t n = 0;

    layout_job_drive_begin(&d);
    while (!line_box_inline_fragments_ask(el, establishing, out, &n)) (void)block_flow_drive_deferred(&d);
    layout_job_drive_end(&d);
    return n;
}

bool line_box_inline_margin_span_ask(lxb_dom_element_t *el, lxb_dom_element_t **establishing,
                                     bool vertical, CssPx *lo, CssPx *hi)
{
    LineBoxFragment *frags = NULL;
    size_t n, i;

    DCHECK(el != NULL && establishing != NULL && lo != NULL && hi != NULL,
           "CSS 2.2 §9.4.2's margin span of an inline box was asked for with no element, nowhere to report the "
           "formatting context it is in, or nowhere to report one of the two edges");
    /* The replacedness half of "non-replaced inline box", asked here because this entry's block axis is what
       divides them and `line_box_inline_fragments` admits replaced elements (one opaque item). The vertical
       arm reports the border area as the margin edge on CSS 2.2 §8.3 "Margin properties"' exception
       ("vertical margins will not have any effect on non-replaced inline elements"); a replaced element's
       vertical margins do take effect, and its caller composes an origin plus an extent
       (core/layout/scrolling_area.c; §10.3.2 and §10.6.2), so this is an assert, not an arm. */
    DCHECK(!replaced_element_of(el).replaced,
           "CSS 2.2 §9.4.2's margin span was asked for a REPLACED inline element. §8.3 \"Margin properties\"' "
           "exception — \"these properties apply to all elements, but VERTICAL MARGINS WILL NOT HAVE ANY "
           "EFFECT ON NON-REPLACED INLINE ELEMENTS\" — is what lets the block arm below report a border area "
           "as a margin edge, and it does not cover this box: HTML §15.4 \"Replaced elements\" makes it "
           "replaced, so `margin-top` and `margin-bottom` are used values its margin edge is outside of. It "
           "also does not need this entry at all — CSS 2.1 §10.3.2 and §10.6.2 give it both extents and "
           "core/layout/flow_position.h gives it one origin, which is the composition "
           "core/layout/scrolling_area.c's own non-replaced-inline predicate routes it to");
    /* And the `display` half, which `line_box_inline_fragments`' own precondition (`inline` or
       `inline-block`) does not supply. An `inline-block` is §9.2.2's atomic box with a used `width` (§10.3.9
       "'Inline-block', non-replaced elements in normal flow") and `height` (§10.6.6 "Complicated cases": "for
       'inline-block' elements, the margin box is used when calculating the height of the line box"), so the
       block arm would drop both vertical margins. It is a routing invariant: core/layout/scrolling_area.c's
       `sa_is_non_replaced_inline` sends every other box to the origin-plus-extent composition first. */
    DCHECK(css_computed_display_is(el, CSS_KEYWORD_INLINE),
           "CSS 2.2 §9.4.2's margin span was asked for a box whose computed `display` is not `inline`. §8.3 "
           "\"Margin properties\"' exception — \"these properties apply to all elements, but VERTICAL MARGINS "
           "WILL NOT HAVE ANY EFFECT ON NON-REPLACED INLINE ELEMENTS\" — is what lets the block arm below "
           "report §10.6.1 \"Inline, non-replaced elements\"' border area AS a margin edge, and an "
           "`inline-block` is not that box: §9.2.2 \"Inline-level elements and inline boxes\" makes it an "
           "ATOMIC inline-level box, §10.3.9 \"'Inline-block', non-replaced elements in normal flow\" gives it "
           "a used `width` and §10.6.6 \"Complicated cases\" a used `height` — which states the effect this "
           "arm would drop in its own words, \"for 'inline-block' elements, the margin box is used when "
           "calculating the height of the line box\". It needs ONE ORIGIN PLUS ONE EXTENT instead, which is "
           "what core/layout/scrolling_area.c routes every box its `sa_is_non_replaced_inline` refuses to — so "
           "reaching here is that classification and this entry's precondition having come apart");
    if (!line_box_inline_fragments_ask(el, establishing, &frags, &n)) return false;
    DCHECK(n >= 1 && frags != NULL,
           "CSS 2.2 §9.4.2's fragments were reported as NONE for an inline box that generates one. That entry's "
           "own closing assert makes a zero count impossible — the box's two edge items are content the fill "
           "partitions and every item is on exactly one line — so this is that contract having been broken "
           "between two functions of one file");
    if (vertical) {
        /* CSS 2.2 §8.3: "vertical margins will not have any effect on non-replaced inline elements", so the
           margin edge on this axis is §10.6.1's border area and every fragment carries its own. */
        *lo = frags[0].block_start;
        *hi = frags[0].block_end;
        for (i = 1; i < n; i++) {
            *lo = css_px_min(*lo, frags[i].block_start);
            *hi = css_px_max(*hi, frags[i].block_end);
        }
    } else {
        /* §9.4.2's split sentence decides which coordinate each fragment contributes: the first fragment's
           start and the last's end are the box's boundaries, every other edge a split where "margins, borders,
           and padding have no visual effect". The two ends are the seed, not loop operands, because of negative
           margins: with `margin-left: -20px` the first fragment's margin edge is inside its border edge, and
           that border edge is a coordinate no margin edge of this box occupies. */
        *lo = css_px_sub(frags[0].inline_start, used_value_px(el, "margin-left"));
        *hi = css_px_add(frags[n - 1].inline_end, used_value_px(el, "margin-right"));
        for (i = 1; i < n; i++) *lo = css_px_min(*lo, frags[i].inline_start);
        for (i = 0; i + 1 < n; i++) *hi = css_px_max(*hi, frags[i].inline_end);
    }
    free(frags);
    DCHECK(css_px_sub(*hi, *lo).px >= 0.0,
           "an inline box's margin span reported an ENDING edge before its BEGINNING edge. Both are extremes "
           "over the SAME fragment array, each of whose rectangles `line_box_inline_fragments` asserts is "
           "non-inverted on the inline axis and derives on the block axis from one baseline plus a "
           "non-negative ascent and descent — so an inverted pair is the two edges having been taken over "
           "different fragment sets");
    return true;
}
