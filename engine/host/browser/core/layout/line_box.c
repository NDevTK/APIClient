/* CSS 2.2 §9.4.2's inline formatting context and §10.8's line height calculation. See line_box.h for which
   box establishes this formatting context, for the one input a line box's HEIGHT does not need, and for why
   §9.4.2's zero-height line box is a second answer rather than a small first one. */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <lexbor/dom/dom.h>

#include "check.h"
#include "core/css/css_computed_value.h"
#include "core/css/css_length.h"
#include "core/layout/block_flow.h"
#include "core/layout/intrinsic_size.h"
#include "core/layout/line_box.h"
#include "core/layout/phrasing_break.h"
#include "core/layout/replaced_element.h"
#include "core/layout/text_run.h"
#include "core/layout/used_value.h"

static char *lb_computed(lxb_dom_element_t *el, const char *name)
{
    char *v = css_computed_value(el, name);

    DCHECK(v != NULL, "the cascade produced no computed value for a property this engine models — every one of "
                      "them is in lexbor's registry with an initial value, so the last layer always answers");
    return v;
}

static bool lb_computed_is(lxb_dom_element_t *el, const char *name, const char *kw)
{
    char *v = lb_computed(el, name);
    bool same = strcmp(v, kw) == 0;

    free(v);
    return same;
}

/* CSS 2 §8.1 "Box dimensions"' BORDER WIDTH on one side, in CSS pixels. It is read as a COMPUTED value and
   that is not a shortcut: css-backgrounds-3 §3.3's `Computed value:` line is "absolute length, snapped as a
   border width", so the computed value already IS the used one and there is nothing for §10 to do to it —
   which is why core/layout/used_value.h's entry does not carry the four border widths at all. */
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

/* CSS 2.2 §9.4.2's OWN AXES ARE PHYSICAL — "boxes are laid out HORIZONTALLY, one after the other, beginning at
   the TOP of a containing block", and "in general, the LEFT edge of a line box touches the left edge of its
   containing block" — and those are the physical axes of a `horizontal-tb` writing mode and of no other. So is
   css-text-4 §7.1's own note about `left` and `right` ("in vertical writing modes, this can be either the
   physical top or bottom, depending on writing-mode"). This component reports OFFSETS on those two axes, so a
   vertical-mode box would be given a distance along the wrong one rather than fail.
   IT IS ASKED OF THE BOX AND OF THE ESTABLISHING CONTAINER SEPARATELY, because css-writing-modes-4 §7.3
   "Orthogonal Flows" is exactly the case where they differ and it is a layout of its own. */
static void lb_require_horizontal_tb(lxb_dom_element_t *el)
{
    if (!lb_computed_is(el, "writing-mode", "horizontal-tb"))
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

/* ---- §10.8's step 2, asked BEFORE step 1 ------------------------------------------------------------------
   "The inline-level boxes are aligned vertically according to their 'vertical-align' property." Step 3 then
   measures "the distance between the uppermost box top and the lowermost box bottom", and WHERE each box's top
   and bottom are is what step 2 decided — so an unread `vertical-align` is not a detail omitted from the
   height, it is the height computed against an alignment nobody chose.
   THE PROPERTY IS READ AS ITS THREE LONGHANDS because css-inline-3 §4.2 "Transverse Box Alignment: the
   vertical-align property" defines it as `[ first | last ] || <'alignment-baseline'> || <'baseline-shift'>`
   and core/css/css_computed_value.c derives each separately. What this component implements is the ONE
   assignment under which every box on the line shares one baseline — §4.2.2's initial `baseline`, "use the
   dominant baseline choice of the parent", with §4.2.1's initial `auto` source and §4.2.3's initial `0` shift.
   Under it §10.8.1's `A'` and `D'` are measured from the SAME line, so step 3's two maxima are taken over
   comparable numbers; under any other value they are not, and this file crashes rather than taking a maximum
   over distances measured from different origins. */
static void lb_require_baseline_alignment(lxb_dom_element_t *el)
{
    CssLength shift;

    if (!lb_computed_is(el, "alignment-baseline", "baseline"))
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
    if (!lb_computed_is(el, "baseline-source", "auto"))
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
              "the source selects between none. THE THIRD BOX HERE IS AN ATOMIC INLINE AND IT IS THE ONE CASE "
              "THAT LOOKS LIKE AN EXCEPTION AND IS NOT: an `inline-block` whose `overflow` computes to other "
              "than `visible` is routed onto the line by `lb_child`, and it may well hold many lines — but "
              "§10.8.1's own exception makes NONE of them its baseline (\"in which case the baseline is the "
              "bottom margin edge\"), so there is again one source and it is not a set this longhand indexes "
              "into. The box that genuinely has two is the `inline-block` whose `overflow` computes to "
              "`visible`, which this component still crashes for, and building its inner baseline is where this "
              "longhand acquires a meaning. DECIDE which of the four is here, before reading a source that "
              "selects nothing");
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

/* ---- §10.8's step 1 and §10.8.1's half-leading, for ONE inline box --------------------------------------- */

/* THE TWO DISTANCES §10.8's step 3 TAKES A MAXIMUM OF, for one box on the line: §10.8.1's `A' = A + L/2` and
   `D' = D + L/2`. They are kept apart rather than summed because step 3 is "the distance between the UPPERMOST
   box top and the LOWERMOST box bottom" — two independent maxima across the line, which a per-box total
   height cannot answer (a tall box above the baseline and a deep one below it make a line box taller than
   either). */
typedef struct {
    CssPx above;
    CssPx below;
} LbExtent;

/* TWO SUMS OF THE SAME REAL NUMBER, taken in different orders — the only comparison this file makes between
   doubles, and it exists for the assert below. The neighbourhood is scaled by the magnitude because a CSS
   pixel length is not a fixed-point quantity: a `line-height` of 1e6px carries its rounding at 1e-10 and one
   of 16px carries it at 1e-15, so a fixed epsilon would be simultaneously too tight for the first and too
   loose for the second. */
static bool lb_close(double a, double b)
{
    double scale = a < 0.0 ? -a : a;
    double other = b < 0.0 ? -b : b;
    double diff = a - b;

    if (other > scale) scale = other;
    if (diff < 0.0) diff = -diff;
    return diff <= 1e-9 * (1.0 + scale);
}

/* §10.8.1 FOR AN INLINE BOX CONTAINING NO GLYPHS, which is every box this component measures. The section
   states this case itself rather than leaving it to a caller: "if the inline box contains no glyphs at all, it
   is considered to contain a STRUT (an invisible glyph of zero width) with the `A` and `D` of THE ELEMENT'S
   FIRST AVAILABLE FONT". So the box is measured exactly as a glyph would be — "still for each glyph, determine
   the leading `L` to add, where `L = 'line-height' - AD`. Half the leading is added above `A` and the other
   half below `D`" — and the arithmetic below is those two sentences.
   THE SUM `A' + D'` IS `line-height` BY CONSTRUCTION, which §10.8.1 states as a consequence ("the height of
   the inline box … is thus exactly 'line-height'"), and it is asserted rather than substituted: deriving the
   pair FROM the line height would lose where the baseline sits inside it, and deriving the line height from
   the pair would make the identity unfalsifiable. `L` MAY BE NEGATIVE — §10.8.1 says so in its own note — so
   there is no floor here and a box with `line-height: 0` correctly contributes negative half-leading. */
static LbExtent lb_strut_extent(lxb_dom_element_t *el)
{
    CssPx a = css_font_ascent_px(el);
    CssPx d = css_font_descent_px(el);
    CssPx lh = css_used_line_height_px(el);
    CssPx half = css_px_scale(css_px_sub(lh, css_px_add(a, d)), 0.5);
    LbExtent out;

    out.above = css_px_add(a, half);
    out.below = css_px_add(d, half);
    /* §10.8.1's OWN CONSEQUENCE, ASSERTED: "the height of the inline box encloses all glyphs and their
       half-leading on each side and is thus exactly 'line-height'". It is a check on the three OPERANDS having
       been read for one element — the defect it catches is `A` from one box and `line-height` from another,
       which is off by whole pixels — and the comparison is therefore stated with the neighbourhood IEEE-754
       leaves around one real number reached by two different orders of addition. That tolerance is about
       BINARY FLOATING POINT and never about the spec: §10.8.1 admits no slack at all, and a mismatch larger
       than a rounding is a wrong element. */
    DCHECK(lb_close(css_px_add(out.above, out.below).px, lh.px),
           "CSS 2.2 §10.8.1's `A' + D'` came out different from the `line-height` it is built out of — the "
           "section's own \"the height of the inline box … is thus exactly 'line-height'\" is an identity over "
           "`A + L/2` and `D + L/2` with `L = 'line-height' - AD`, so a disagreement here is one of the three "
           "operands having been read for a different element than the other two");
    return out;
}

/* §10.8's STEP 1 FOR AN ATOMIC INLINE, WHICH IS THE OTHER HALF OF ITS OWN SENTENCE. "The height of each
   inline-level box in the line box is calculated. For REPLACED ELEMENTS, INLINE-BLOCK elements, and
   INLINE-TABLE elements, this is the HEIGHT OF THEIR MARGIN BOX; for inline boxes, this is their
   'line-height'." So this box is measured by CSS 2 §8.1's outer edge (core/layout/used_value.h) and NOT by
   `lb_strut_extent`, which is §10.8.1's strut for a box containing no glyphs and would answer the second half
   of that semicolon for a box on the first.
   ALL OF IT IS ABOVE THE BASELINE, AND THAT IS §10.8's `vertical-align` DEFINITION RATHER THAN AN ESTIMATE.
   Two sentences of it compose: "in the following definitions, for inline non-replaced elements, the box used
   for alignment is the box whose height is the 'line-height' … FOR ALL OTHER ELEMENTS, THE BOX USED FOR
   ALIGNMENT IS THE MARGIN BOX", and `baseline`'s own definition — "align the baseline of the box with the
   baseline of the parent box. IF THE BOX DOES NOT HAVE A BASELINE, ALIGN THE BOTTOM MARGIN EDGE with the
   parent's baseline." A REPLACED ELEMENT HAS NO BASELINE: §10.8's definition gives one to an `inline-table`
   ("The baseline of an 'inline-table' is the baseline of the first row of the table") and to an `inline-block`
   ("The baseline of an 'inline-block' is the baseline of its last line box in the normal flow, unless it has
   either no in-flow line boxes or if its 'overflow' property has a computed value other than 'visible', in
   which case the baseline is the bottom margin edge"), and to nothing else — so the bottom margin edge sits ON
   the line's baseline, `D'` is zero and `A'` is the whole margin box. That is why an image on a line of text
   leaves the font's descender visible below it, which is the single most recognisable thing about inline
   replaced content.
   AND THE SECOND OF THOSE TWO SENTENCES SENDS A BOX BACK HERE, WHICH IS WHY "NO BASELINE" IS NOT THE ONLY
   SUBJECT. Its exception names the BOTTOM MARGIN EDGE outright, so an `inline-block` whose `overflow` computes
   to other than `visible` is answered by the identical arithmetic — not because it has no baseline, but
   because §10.8.1 places the baseline it does have exactly where this function puts one. `lb_child` routes
   that box to the run and this function is what measures it; the two ways to arrive are two sentences of one
   definition and there is nothing here to branch on.
   THE ALIGNMENT IS `baseline` BY ASSERTION AND NOT BY ASSUMPTION: `lb_require_baseline_alignment` runs over
   this element before it reaches the run, so any other css-inline-3 §4.2 value has already crashed. A box
   whose baseline is INSIDE it is the arm that is missing here rather than the arm that is wrong — see the
   atomic-inline crash in `lb_child`, whose remaining work is exactly that inner baseline.
   NO FLOOR ON THE HEIGHT, because CSS 2.2 §8.3 allows a negative margin and §8.1's nesting is unconditional.
   `lb_take` is a maximum against the line's strut, which §10.8.1 makes non-negative for a non-negative
   `line-height`, so a negative margin box cannot pull a line box below zero — it simply does not win. */
static LbExtent lb_atomic_extent(lxb_dom_element_t *el)
{
    LbExtent out;

    out.above = used_value_margin_edge_px(el, true);
    out.below = css_px(0.0);
    return out;
}

static void lb_take(LbExtent *line, LbExtent box)
{
    line->above = css_px_max(line->above, box.above);
    line->below = css_px_max(line->below, box.below);
}

/* §10.8's `vertical-align` DEFINITION GIVES AN `inline-block` A BASELINE IN A SENTENCE THAT HAS AN EXCEPTION IN
   IT, AND THIS PREDICATE IS ONE HALF OF THE EXCEPTION. CSS 2.2 §10.8.1 "Leading and half-leading" states it
   whole: "The baseline of an 'inline-block' is the baseline of its last line box in the normal flow, unless it
   has either no in-flow line boxes or if its 'overflow' property has a computed value other than 'visible', in
   which case the baseline is the bottom margin edge."
   §10.8.1 IS WHERE THE STANDARD'S OWN DOCUMENT PUTS THAT SENTENCE, and it is cited here rather than the §10.8
   the sites around it name for the same rule: CSS 2.2 writes the `line-height` and `vertical-align` property
   definitions BENEATH the §10.8.1 heading, so the number that carries these words is the subsection and the
   number the neighbours write is its parent. Both resolve; only one of them can be opened and read.
   THE EXCEPTION IS A DISJUNCTION AND ONLY ONE DISJUNCT IS ASKED HERE. "No in-flow line boxes" is a question
   about the formatting context INSIDE this box, which has to be laid out before anything can answer it;
   "'overflow' … has a computed value other than 'visible'" is a COMPUTED VALUE, which
   core/css/css_computed_value.h answers for a box whose inside has never been looked at. So TRUE means the
   exception FIRES and §10.8.1 states the baseline outright, while FALSE means only that THIS disjunct did not
   fire and says nothing whatever about the other one — a box it answers FALSE for may still have no in-flow
   line boxes, and deciding that is the other half.
   IT READS THE TWO LONGHANDS BECAUSE THE SHORTHAND HAS NO COMPUTED VALUE OF ITS OWN. css-overflow-3 §3.1
   "Managing Overflow: the overflow-x, overflow-y, and overflow properties" defines `overflow` as a shorthand
   over `overflow-x` and `overflow-y`, so what CSS 2.2 calls "the 'overflow' property"'s computed value is
   `visible` exactly when BOTH longhands compute to it — which is why this is the negation of a CONJUNCTION and
   not a pair of independent tests. §3.1's own pairing rule ("if the other axis specifies a scrollable value, a
   specified value of visible computes to auto") runs inside css_computed_value.c and can only turn a `visible`
   into an `auto`, never the reverse, so it cannot make a pair this answers TRUE for read `visible` later. */
static bool lb_inline_block_overflow_excepts_baseline(lxb_dom_element_t *el)
{
    return !(lb_computed_is(el, "overflow-x", "visible") && lb_computed_is(el, "overflow-y", "visible"));
}

/* ---- §9.4.2's zero-height line box ------------------------------------------------------------------------
   "…no inline elements with non-zero margins, padding, or borders…" — the one conjunct of that list this
   component can still be reached with, since text, preserved white space and every other kind of in-flow
   content crash on the way here. The test is over ALL FOUR SIDES because §9.4.2 states it over none: the
   vertical margins of a non-replaced inline box have no effect on the line box's height (§10.8.1: "although
   margins, borders, and padding of non-replaced elements do not enter into the line box calculation, they are
   still rendered around inline boxes"), and this sentence is nevertheless not about the height — it is about
   whether the line box EXISTS, which a rendered border makes true whichever edge it is on. */
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

/* ---- THE TWO ELEMENTS THAT BREAK THE LINE WITHOUT PUTTING A GLYPH ON IT ------------------------------------
   css-text-3 §5.5 "Line Breaking Details" says "out-of-flow boxes and inline box boundaries do not introduce a
   forced line break or soft wrap opportunity in the flow", and core/layout/text_run.h collects an inline box
   as exactly that — two EDGE items carrying a width and no code point, invisible to [UAX14]. HTML §15.3.4
   "Phrasing content" declares the two elements that are inline boxes and are NOT merely boundaries, and
   core/layout/phrasing_break.h is the one place that answers which — the fact is HTML's and no CSS module
   defines either keyword, so the cascade cannot be asked and a name test inside this walk would be the same
   fact answered again inside core/layout/intrinsic_size.c's walk over the same children.
   THE FORCED BREAK IS COLLECTED AND THE OPPORTUNITY IS NOT YET, which is why one arm adds an item and the
   other crashes. Adding the break to the run is the whole of `br`: text_run.h holds a FORCED-BREAK item kind
   carrying the U+000A css-text-3 §4 names, so the fill divides the run at it and §10.8 below measures each
   piece, with the break's own element contributing its `line-height` to the line it ends.
   IT ANSWERS WHETHER IT PLACED THE ELEMENT, because a `br` is not additionally an inline BOX: it emits no
   pair of edges and the walk does not descend into it. §15.3.4's declaration replaces what the box would
   otherwise have been, so a caller that added the break AND then bracketed it with edges would put two
   zero-width boundaries around a break that has no inside. */
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

static void lb_walk(TextRunMeasure *m, lxb_dom_element_t *el);

/* ONE TEXT NODE of the formatting context, added to the run CSS 2.2 §9.4.2 distributes.
   A WHOLLY-COLLAPSIBLE RUN IS NOT SKIPPED HERE — IT IS NOT CONTENT. CSS 2.2 §9.2.2.1 "Anonymous inline boxes"
   is the sentence, and core/layout/block_flow.h answers it once for every walk over a block container's
   children: white space that would be collapsed away "does not generate any anonymous inline boxes", so there
   is no box to put on a line and no character for css-text-3 §4.1 to process. Asking block_flow.h rather than
   re-deriving it is what keeps this walk and §9.4.1's stack agreeing about what a document's white space is,
   and it is the same call core/layout/intrinsic_size.c makes at the same arm of the same classification.
   WHAT SURVIVES THAT TEST GOES INTO THE RUN UNCHANGED, because css-text-3 §4.1.1's collapsing is stated over
   the FORMATTING CONTEXT and not over a node — a space either side of an inline box boundary is one run — so
   the accumulator and not this walk is where a run split across two text nodes is joined. */
static void lb_text(TextRunMeasure *m, lxb_dom_element_t *parent, lxb_dom_node_t *n)
{
    if (!block_flow_text_child_generates_box(parent, n)) return;
    text_run_measure_add_text(m, parent, n);
}

/* ONE CHILD NODE of the inline formatting context. */
static void lb_child(TextRunMeasure *m, lxb_dom_element_t *parent, lxb_dom_node_t *n)
{
    lxb_dom_element_t *el;
    char *d;
    bool atomic, inline_box, inline_block, bottom_margin_edge;

    switch (n->type) {
    case LXB_DOM_NODE_TYPE_TEXT:
        lb_text(m, parent, n);
        return;
    case LXB_DOM_NODE_TYPE_COMMENT:
    case LXB_DOM_NODE_TYPE_PROCESSING_INSTRUCTION:
    case LXB_DOM_NODE_TYPE_DOCUMENT_TYPE:
        /* CSS 2.2 §9.2 generates boxes for elements and for text, and for neither of these three. */
        return;
    case LXB_DOM_NODE_TYPE_ELEMENT:
        break;
    default:
        DFAIL("a node type CSS 2.2 §9.2's box generation does not describe is inside a block container being "
              "laid out — the tree this walk iterates holds elements, text, comments, processing instructions "
              "and a doctype, and a CDATA section, a document or a fragment is not a child any parser this "
              "engine runs produces there. Find the writer that inserted it");
        return;
    }
    el = lxb_dom_interface_element(n);
    /* §9.4.2's line box holds the IN-FLOW inline-level boxes; an absolutely positioned box is out of flow, so
       it is not on the line at all and css-text-3 §5.5 confirms it introduces no break either ("out-of-flow
       boxes and inline box boundaries do not introduce a forced line break or soft wrap opportunity in the
       flow"). Its own position is §9.3.2's over a static position, which is a different question. */
    if (lb_computed_is(el, "position", "absolute") || lb_computed_is(el, "position", "fixed")) return;
    if (!lb_computed_is(el, "float", "none"))
        DFAIL("CSS 2.2 §9.5 \"Floats\" takes this box out of the line and puts it beside the line boxes, which "
              "changes the WIDTH available to every one of them: §9.4.2 says line boxes \"may vary in width if "
              "available horizontal space is reduced due to floats\", and §9.5's own rule shortens the line "
              "boxes beside a float rather than moving them. A float in this formatting context therefore "
              "makes the number of line boxes a function of the float's own width and height, which is the "
              "measurement this component exists to avoid needing. BUILD §9.5.1's float placement");
    d = lb_computed(el, "display");
    if (strcmp(d, "none") == 0) { free(d); return; }
    if (strcmp(d, "contents") == 0) {
        free(d);
        DFAIL("css-display-3 §2.5 \"Box Generation: the none and contents keywords\" gives this child "
              "`display: contents`: \"The element itself does not generate any boxes, but its children and "
              "pseudo-elements still generate boxes and text sequences as normal.\" So the boxes on this line "
              "are the child's children spliced in at its position. That splice is a BOX-TREE construction "
              "step belonging to every walk over children rather than to this one, and "
              "core/layout/block_flow.c's own child walk names the same absence. BUILD THE SPLICE §2.5 STATES "
              "— \"For the purposes of box generation and layout, the element must be treated as if it had "
              "been replaced in the element tree by its contents (including both its source-document children "
              "and its pseudo-elements, such as ::before and ::after pseudo-elements, which are generated "
              "before/after the element's children as normal).\" — as the thing both walks iterate");
        return;
    }
    inline_block = strcmp(d, "inline-block") == 0;
    atomic = inline_block || strcmp(d, "inline-flex") == 0 ||
             strcmp(d, "inline-grid") == 0 || strcmp(d, "inline-table") == 0;
    inline_box = strcmp(d, "inline") == 0;
    free(d);
    /* §10.8.1's INLINE-BLOCK EXCEPTION, DECIDED HERE BECAUSE IT DECIDES WHETHER THE CRASH BELOW IS TRUE. The
       sentence `lb_inline_block_overflow_excepts_baseline` quotes ends "in which case the baseline is the
       BOTTOM MARGIN EDGE", and a box whose bottom margin edge is its baseline is a box `lb_atomic_extent`
       already answers exactly — `above` the whole margin box, `below` zero — so for this box §10.8's step 1 is
       not unbuilt and the crash below would be naming an absence that is not there.
       THE THIRD CONJUNCT IS §10.3.9's AND §10.6.6's OWN WORD. Both of the used-value sections that answer this
       box's margin box say "non-replaced" in their own titles and lists — CSS 2.2 §10.3.9 "'Inline-block',
       non-replaced elements in normal flow" for the inline size and §10.6.6 "Complicated cases" for the block
       one ("'Inline-block', non-replaced elements. … If 'height' is 'auto', the height depends on the
       element's descendants per 10.6.7") — and a REPLACED element with `display: inline-block` is §10.3.10's
       and §10.6.2's box instead. Its baseline is the bottom margin edge too, but by the REPLACED half of
       `baseline`'s definition rather than by this exception, so routing it on an `overflow` value that decides
       nothing about it would be one predicate answering two questions. It keeps the crash below.
       THE CLASSIFICATION IS ASKED AT TWO SITES AND AT MOST ONCE ON ANY PATH, which is the ordering rather than
       an oversight: a routed box returns at its own arm below and never reaches the replaced test, and a box
       whose `display` is not `inline-block` short-circuits before asking here. Merging the two into one earlier
       call would put replaced_element.c's OWN crashes (it names an `img`, an `embed`, a `video`, a `canvas`, an
       `object`, an `audio` and an `input` whose rule it cannot decide) ahead of the two classification crashes
       below, which report an entirely different defect and would then be reported a step late. */
    bottom_margin_edge = inline_block && !replaced_element_of(el).replaced &&
                         lb_inline_block_overflow_excepts_baseline(el);
    if (atomic && !bottom_margin_edge)
        DFAIL("CSS 2.2 §9.2.2 makes this an ATOMIC INLINE-LEVEL box, which \"participate[s] in [its] inline "
              "formatting context as a single opaque box\". THE RUN ITEM IS BUILT AND IS NO LONGER PART OF WHAT "
              "THIS NAMES: core/layout/text_run.h carries css-text-3 §5.5's atomic inline as a kind of its own "
              "— a MARGIN BOX inline size at a position plus the U+FFFC whose [UAX14] class CB is \"a soft wrap "
              "opportunity before and after each replaced element or other atomic inline\" — and the REPLACED "
              "arm below already emits one, so the fill, the per-line sum and §10.8's step 1 over a margin box "
              "(`lb_atomic_extent`) all run for this shape of box today. WHAT IS LEFT IS TWO THINGS AND BOTH "
              "ARE FACTS ABOUT THIS BOX RATHER THAN ABOUT THE LINE. (1) THE BASELINE, WHOSE `overflow` ARM IS "
              "NO LONGER PART OF WHAT THIS NAMES. CSS 2.2 §10.8.1 \"Leading and half-leading\" — the section "
              "the standard's own document writes the `vertical-align` definition under — states an "
              "`inline-block`'s baseline as one sentence with an exception in it: \"The baseline of an "
              "'inline-block' is the baseline of its last line box in the normal flow, unless it has either no "
              "in-flow line boxes or if its 'overflow' property has a computed value other than 'visible', in "
              "which case the baseline is the bottom margin edge.\" The `overflow` disjunct is ROUTED above and "
              "reaches the run: it names the BOTTOM MARGIN EDGE, which is the pair `lb_atomic_extent` already "
              "returns. THREE SHAPES STILL ARRIVE HERE AND THEY DO NOT WANT THE SAME WORK. An `inline-block` "
              "whose `overflow` computes to `visible` wants the sentence's MAIN arm — the last line box of the "
              "formatting context inside this box, and its `e.above`, which is this component's own answer one "
              "level down — together with the \"no in-flow line boxes\" test that chooses between that and the "
              "bottom margin edge; those two are what must exist afterward, and neither is the `overflow` read, "
              "which is done. An `inline-table` wants a DIFFERENT sentence of the same definition, \"The "
              "baseline of an 'inline-table' is the baseline of the first row of the table\", which is CSS 2.1 "
              "§17.2 \"The CSS table model\"'s box structure before it is a baseline at all. AN `inline-block` "
              "THAT HTML §15.4 \"Replaced elements\" MAKES REPLACED WANTS NO BASELINE BUILT AND IS HERE FOR A "
              "ROUTING DECISION INSTEAD: it is §10.3.10 \"'Inline-block', replaced elements in normal flow\"'s "
              "and §10.6.2's box, core/layout/used_value.c sizes it through those, and `baseline`'s own "
              "replaced arm (\"If the box does not have a baseline, align the bottom margin edge with the "
              "parent's baseline\") already puts its margin box where `lb_atomic_extent` puts one — so what is "
              "undecided is whether it takes the REPLACED arm below or the routed one above, and the two "
              "answers are the same geometry reached through two different sentences. DECIDE that, do not "
              "build a baseline for it. (2) THE USED INLINE SIZE for the box types CSS 2.1 §10 does not own: "
              "§10.3.9 \"'Inline-block', non-replaced elements in normal flow\" answers "
              "an `inline-block` (core/layout/used_value.c's shrink-to-fit), and an `inline-flex` and an "
              "`inline-grid` are CLASSIFIED — `uv_box_kind`'s `UV_BOX_INLINE_FLEX_GRID` — so an `auto` width "
              "on one now CRASHES by its own module's name rather than answering with §10.3.3's constraint "
              "equation over a box no part of §10.3 describes, and this line no longer asks for that fix. "
              "WHAT THE CLASSIFICATION LEFT IS THE TWO INTRINSIC TERMS §10.3.9's shrink-to-fit reads: "
              "css-flexbox-1 §9.9.1 \"Flex Container Intrinsic Main Sizes\" and css-grid-1 §5.2 \"Sizing Grid "
              "Containers\" (\"the sum of the grid container's track sizes (including gutters)\") each define "
              "their own, and core/layout/intrinsic_size.c measures a BLOCK CONTAINER's by laying out its line "
              "boxes — which is why it crashes for one of these rather than answering. BUILD the module's own "
              "intrinsic main sizes as a second producer of `IntrinsicInlineSizes`; an `inline-table` is "
              "§17.5.2's and is the same shape one module over");
    /* CSS 2.2 §9.2.1.1's SECOND PARAGRAPH, which is a DIFFERENT box structure from its first and is reached
       from here rather than from block_flow.c — the block-level box is not a child of the block container at
       all, so the classification that delimits the anonymous runs never sees it. It is named apart from the
       came-apart crash below because the two ask for opposite work: that one says a classifier is wrong, this
       one says a construction is missing, and a reader who took this for that would go looking in a list that
       is right. */
    if (!inline_box && !atomic && lb_computed_is(parent, "display", "inline"))
        DFAIL("CSS 2.2 §9.2.1.1 \"Anonymous block boxes\": an INLINE BOX contains an in-flow BLOCK-LEVEL box. "
              "\"When an inline box contains an in-flow block-level box, the inline box (and its inline "
              "ancestors within the same line box) is BROKEN AROUND the block-level box (and any block-level "
              "siblings that are consecutive or separated only by collapsible whitespace and/or out-of-flow "
              "elements), SPLITTING THE INLINE BOX INTO TWO BOXES (even if either side is empty), one on each "
              "side of the block-level box(es). The line boxes before the break and after the break are "
              "enclosed in anonymous block boxes, and the block-level box becomes a SIBLING of those anonymous "
              "boxes.\" So this is not the container's child list needing anonymous boxes — that generation "
              "runs in core/layout/block_flow.c and produced the box this walk is measuring — it is the "
              "INLINE box needing to be cut in two, which moves a block-level box UP the tree past every "
              "inline ancestor it has inside this line box and re-splits the runs around it. Two further "
              "sentences of the same paragraph are part of the same build: \"properties set on elements that "
              "cause anonymous block boxes to be generated still apply to the boxes and content of that "
              "element\" (so the split halves keep the inline box's own border, \"open at the end of the line\" "
              "and \"open at the start\"), and \"when such an inline box is affected by relative positioning, "
              "any resulting translation ALSO AFFECTS the block-level box contained in the inline box\". BUILD "
              "the split where the runs are delimited (block_flow.c's `bf_anon_run_end`), so the box list a "
              "container has is computed over its inline DESCENDANTS' block-level boxes and not only over its "
              "own children — this walk is then unreachable for a block-level box for the reason below, which "
              "is the state that crash already describes");
    /* `!atomic` IS THIS CONDITION'S OWN MESSAGE READ BACK — it says "neither `inline`, nor an atomic
       inline-level box, nor …", and an atomic box was excluded from it only by the crash above firing first.
       That is a distinction with no consequence while every atomic aborts, and the routed `inline-block` above
       is the box that makes it one: it reaches this line, it is exactly what the message excludes, and the
       classification it accuses (block_flow.c's list of what is block-level) is not in question for it. */
    if (!inline_box && !atomic)
        DFAIL("a computed `display` inside an INLINE formatting context that is neither `inline`, nor an "
              "atomic inline-level box, nor `none`, nor `contents`, nor out of flow. §9.4.2's own condition is "
              "that the establishing block container \"contains no block-level boxes\", and "
              "core/layout/block_flow.c decides that over the SAME child list before calling this component — "
              "so a block-level or table box reaching this walk is those two classifications having come "
              "apart, and the one to fix is whichever list this `display` is missing from. THIS IS ALSO WHERE "
              "§9.2.1.1's OWN INVARIANT IS CLOSED for an anonymous block box, and it is the same sentence: "
              "\"then we force it to have only block-level boxes inside it\" puts the block-level box OUTSIDE "
              "the anonymous box as its SIBLING, so a run block_flow.c delimited for one must contain none — "
              "and the run is delimited by the very classifier this crash contradicts, which is why one assert "
              "at this consumer covers both callers and a second copy inside the generation would not");
    /* §10.8's step 2 for this box. It is asked at the WALK and not at the per-line pass below even though the
       height is measured there, because it is a question about the BOX — whether §10.8.1's `A'` and `D'` are
       measured from the same baseline as its neighbours' — and every box in this context is reached exactly
       once here, while the per-line pass reaches a box once per ITEM it owns.
       IT IS ASKED OF A `br` TOO, and that is §10.8's step 1 rather than a wide net: the break is an
       inline-level box ON the line it ends, so `lb_line_extent` takes its `A'` and `D'` like any other box's —
       `<br style="line-height:100px">` makes a line 100px tall — and a box whose extents enter step 3's maxima
       is a box step 2 has to have aligned.
       IT IS ASKED OF A REPLACED ELEMENT BEFORE ITS ITEM IS COLLECTED, because `lb_atomic_extent` puts the whole
       of that box ABOVE the baseline and §10.8's `vertical-align` definition is the only reason it may: "if the
       box does not have a baseline, align the BOTTOM MARGIN EDGE with the parent's baseline" is the `baseline`
       arm, and any other alignment moves the box somewhere this file does not compute. */
    lb_require_baseline_alignment(el);
    /* §10.8.1's INLINE-BLOCK EXCEPTION, WHICH IS THE ONE ATOMIC INLINE THIS COMPONENT PLACES ON A LINE WITHOUT
       LOOKING INSIDE IT. The sentence ends "in which case the baseline IS THE BOTTOM MARGIN EDGE", and that is
       a complete answer rather than a condition on one: the box's margin box hangs entirely above the line's
       baseline, which is the pair `lb_atomic_extent` returns and is the same geometry `baseline`'s own
       no-baseline arm gives a replaced element below. So the two arms are one derivation reached through two
       sentences, and the item they emit is identical — css-text-3 §5.5's atomic inline, a MARGIN BOX inline
       size carrying the U+FFFC of [UAX14] class CB.
       THE INLINE SIZE IS CSS 2.2 §10.3.9 "'Inline-block', non-replaced elements in normal flow"'s SHRINK-TO-FIT
       AND ITS BLOCK SIZE IS §10.6.6 "Complicated cases"' REDIRECTION TO §10.6.7, both through
       core/layout/used_value.h, and §10.6.6 states this box's own place on the line in its last sentence: "For
       'inline-block' elements, the margin box is used when calculating the height of the line box." That is
       §10.8's step 1 said twice, once from each side, which is why the height is read at the line
       (`lb_atomic_extent`) and the width is read here.
       THE WALK DOES NOT DESCEND, for the reason it does not descend into a replaced element: §9.2.2 makes this
       "a single opaque box" in THIS formatting context, and the inline formatting context INSIDE it belongs to
       the box itself — used_value.h's block-axis answer is what runs it, one level down, through the same
       component. Emitting edges around it would put two boundaries around content this run does not hold. */
    if (bottom_margin_edge) {
        text_run_measure_add_atomic(m, el, used_value_margin_edge_px(el, false));
        return;
    }
    /* HTML §15.4 "Replaced elements" MAKES THIS A REPLACED ELEMENT, which is css-text-3 §5.5's "each replaced
       element or other atomic inline" reached through the element's own nature rather than through its
       `display` — an `img` is `display: inline` and is not an inline BOX. Its two consequences are the atomic
       inline's, and both are the item: §5.5 puts a soft wrap opportunity before and after it, and CSS 2.2
       §9.4.2 puts its horizontal margins, borders and padding on the line between its neighbours.
       THE WIDTH IS THE MARGIN BOX'S AND IS core/layout/used_value.h's, WHICH ALREADY RUNS §10.3.2 "Inline,
       replaced elements" over core/layout/replaced_element.h's natural dimensions — so nothing is derived here
       and there is no second answer to how wide an image is. The walk does NOT descend into it and emits no
       edges around it: CSS 2.1 §3.1 puts a replaced element's content "outside the scope of the CSS formatting
       model", and §9.2.2 makes the box "a single opaque box", so there is no inside for this run to hold.
       ITS HEIGHT IS READ AT THE LINE AND NOT HERE, because §10.8's step 1 is a question about the LINE the item
       lands on — `lb_atomic_extent` is where the same margin box is asked for on the other axis. */
    if (replaced_element_of(el).replaced) {
        text_run_measure_add_atomic(m, el, used_value_margin_edge_px(el, false));
        return;
    }
    /* THE ELEMENTS THAT CHANGE WHERE THE RUN BREAKS ARE PLACED BEFORE ANYTHING ELSE IS COLLECTED FOR THEM,
       because the fill's whole answer is a function of the break positions and a run collected without one of
       them is a partition of different text. A phrasing break is not additionally an inline box. */
    if (lb_phrasing_break(m, el)) return;
    /* css-text-3 §5.5 places this box's two edges AT ITS BOUNDARIES and not at every break inside it — "inline
       box boundaries do not introduce a forced line break or soft wrap opportunity in the flow" — so they are
       emitted in call order around the descent and it is the run's own break-position mapping that decides
       which line each of them lands on. That is what makes an inline box spanning three lines put its opening
       edge on the first and its closing edge on the third, which is the same fact CSSOM VIEW §6's
       `getClientRects()` step 3 reports as a fragment count. They are emitted even when both are ZERO, because
       an edge occupies a POSITION and that position is what says which line an otherwise-empty inline box is
       on — which is precisely what §9.4.2's zero-height rule is then asked about.
       THE SIZE IS core/layout/intrinsic_size.h's, NOT A SECOND COPY OF IT: §9.4.2's "horizontal margins,
       borders, and padding are respected between these boxes" and css-sizing-3 §2.2's outer size are the same
       six lengths, and two derivations of them would be two answers to how wide this line is. */
    text_run_measure_add_box_edge(m, el, intrinsic_inline_box_edge_px(el, false));
    /* §10.8's step 1 is over EVERY inline-level box on the line, and a nested inline box is one — §10.8.1's
       "boxes of child elements do not influence this height" is about the PARENT box's own height, not about
       whether the child is on the line. So the walk descends and each box's own items carry it onto whichever
       lines the fill puts them on. */
    lb_walk(m, el);
    text_run_measure_add_box_edge(m, el, intrinsic_inline_box_edge_px(el, true));
}

static void lb_walk(TextRunMeasure *m, lxb_dom_element_t *el)
{
    lxb_dom_node_t *n = lxb_dom_interface_node(el), *c;

    for (c = n->first_child; c != NULL; c = c->next) lb_child(m, el, c);
}

/* ---- §10.8 OVER ONE OF THE FILL'S LINE BOXES -------------------------------------------------------------
   §10.8's steps 1 to 3 for the line box holding the items `[line.from, line.to)`, and §9.4.2's SECOND question
   about that same line answered on the same pass.
   THE STRUT IS SEEDED PER LINE AND NOT ONCE PER FORMATTING CONTEXT, which is §10.8's own wording: "the minimum
   height consists of a minimum height above the baseline and a minimum depth below it, exactly as if EACH LINE
   BOX starts with a zero-width inline box with the element's font and line height properties. We call that
   imaginary box a 'strut'." A strut taken once for the whole context would make a second line box as tall as
   the first only by accident. §10.8's step 3 counts it ("this includes the strut, as explained under
   'line-height' below"), which is why a line with nothing else on it is still exactly as tall as the strut.
   FOR §9.2.1.1's ANONYMOUS BLOCK BOX THE STRUT IS STILL THE ENCLOSING ELEMENT'S, and that is the section's own
   sentence rather than an approximation of one: "the font of the anonymous box is inherited from the DIV", and
   `line-height` is inherited too — so the imaginary zero-width box §10.8 starts each line with has exactly the
   properties `style` computes, whether or not the box wrapping the run has an element of its own.
   A BOX IS ON THIS LINE EXACTLY WHEN ONE OF ITS ITEMS IS, which is what the fill's partition means and is why
   this walks items rather than the tree. An inline box whose text spans three lines contributes its `A'` and
   `D'` to all three — §10.8's step 1 is over the inline-level boxes on the line, and each of its fragments is
   one — while an inline box on one line contributes to that line alone, which a walk over the ELEMENT TREE
   could not distinguish. `lb_strut_extent` is idempotent under `lb_take`, so a box owning several items on one
   line contributes once however many that is.
   `*exists` IS §9.4.2's OTHER ANSWER and it is per LINE, because the section states it per line: "line boxes
   that contain no text, no preserved white space, no inline elements with non-zero margins, padding, or
   borders, and no other in-flow content … and do not end with a preserved newline must be treated as
   ZERO-HEIGHT line boxes … and must be treated as NOT EXISTING for any other purpose."
   A CHARACTER ITEM IS "TEXT" FOR THAT LIST, AND THAT IS A DERIVATION OVER WHAT REACHES HERE rather than a
   reading of the word. The only character a line could hold that renders nothing is css-text-3 §4.1.1's one
   surviving collapsible space, and a run consisting only of those never arrives: CSS 2.2 §9.2.2.1 "Anonymous
   inline boxes" removes it before the walk ("white space content that would subsequently be collapsed away …
   does not generate any anonymous inline boxes"), which core/layout/block_flow.h decides at `lb_text`. So a
   line holding a character item holds a character the document contains. One of the other three conjuncts still
   crashes on the way here — preserved white space, in `tr_wraps` — and "NO OTHER IN-FLOW CONTENT" is now a live
   question this function answers rather than one the walk aborted for: an ATOMIC INLINE item is exactly that
   content, so a line holding a replaced element EXISTS however empty the rest of it is, and
   `<div><img alt=""></div>` is a box whose margins do not collapse through it even though the image is 0 by 0.
   That leaves the box-edges conjunct and the PRESERVED NEWLINE for this function to decide alongside it.
   "DO NOT END WITH A PRESERVED NEWLINE" IS ANSWERED OVER THE LAST ITEM THAT RENDERS, and the qualification is
   forced by the fill's own mapping rather than chosen. A forced break closes its line at the boundary
   immediately after it, so it is the last item on its line everywhere except at [UAX14] LB3's end of text,
   where the last line runs to the end of the collection and any inline box EDGE collected after the final code
   point trails it. An edge renders nothing and adds no content — §9.4.2's list already exempts inline elements
   whose margins, padding and borders are zero — so it is not something a line can be said to END WITH, and
   scanning past it is what makes `<div>a<br><span></span></div>` one line of text height rather than none.
   A LINE HOLDS AT MOST ONE FORCED BREAK, and that is a theorem asserted rather than a shape assumed: [UAX14]
   LB5 `LF !` makes the boundary after every forced break MANDATORY, so the fill closes the line there and a
   second break cannot join it. */
static LbExtent lb_line_extent(lxb_dom_element_t *style, const TextRunMeasure *m, TextRunLine line, bool *exists)
{
    LbExtent out = lb_strut_extent(style);
    size_t i, breaks = 0, last_rendering = line.to;

    DCHECK(line.from < line.to,
           "CSS 2.2 §9.4.2's fill reported a line box holding NO items. \"Line boxes are created as needed to "
           "hold inline-level content\", so a line with nothing on it is not one the section creates — and its "
           "height would be the strut's, added to §10.6.3's total for content that is not there");
    *exists = false;
    for (i = line.from; i < line.to; i++) {
        lxb_dom_element_t *box = text_run_measure_item_style(m, i);
        bool text = text_run_measure_item_is_text(m, i);
        bool brk = text_run_measure_item_is_forced_break(m, i);
        bool atom = text_run_measure_item_is_atomic(m, i);

        /* §10.8's STEP 1 IS ONE SENTENCE WITH A SEMICOLON IN IT AND THIS IS THE SEMICOLON: "for replaced
           elements, inline-block elements, and inline-table elements, this is the height of their MARGIN BOX;
           for inline boxes, this is their 'line-height'." An atomic inline's item is on the first side and
           every other item's box is on the second, so the two extents come from two functions and a walk that
           took the strut for both would report an image's height as its `line-height`. */
        lb_take(&out, atom ? lb_atomic_extent(box) : lb_strut_extent(box));
        /* THE BOX-EDGES TEST IS STILL SHORT-CIRCUITED BEHIND THE OTHERS, and that is not a saving:
           `lb_has_nonzero_box_edges` reads used values, which is a derivation that crashes for box types this
           engine does not lay out, so asking it about a box an earlier conjunct already answered for would turn
           a measured line into an abort. */
        if (text || atom || lb_has_nonzero_box_edges(box)) *exists = true;
        if (brk) breaks++;
        /* AN ATOMIC INLINE RENDERS, so it is one of the things a line can be said to END WITH — which is what
           keeps §9.4.2's last conjunct from reading a forced break through an image that follows it. That
           configuration is unreachable ([UAX14] LB5 `LF !` closes the line at the break), and the point of
           including it here is that this test does not depend on that being true. */
        if (text || brk || atom) last_rendering = i;
    }
    DCHECK(breaks <= 1,
           "CSS 2.2 §9.4.2's fill put TWO forced line breaks on ONE line box. [UAX14] LB5 \"Treat CR followed "
           "by LF, as well as CR, LF, and NL, as hard line breaks\" ends with `LF !`, which makes the boundary "
           "after each of them MANDATORY — so the fill closes the line at the first and the second opens the "
           "next one, and two on a line is the fill and the break pass disagreeing about where a mandatory "
           "action is");
    /* §9.4.2's last conjunct. `last_rendering` is `line.to` exactly when the line holds nothing that renders,
       which is the empty-inline-box case the box-edges test above already answered. */
    if (last_rendering < line.to && text_run_measure_item_is_forced_break(m, last_rendering)) *exists = true;
    return out;
}

/* ---- §9.4.2's CONTENT, COLLECTED AND DISTRIBUTED — the half every answer below shares ----------------------
   ONE COLLECTION AND ONE FILL PER QUESTION, and the two answers share this rather than each running their own,
   for the reason core/layout/text_run.h gives about its three partitions: the fill's whole result is a function
   of where [UAX14] says this run may break, so two collections of one formatting context would be two chances
   for the height and the scrolling area to disagree about which line a box is on.
   THE CALLER OWNS `*lines` AND OWES `m` EXACTLY ONE `text_run_measure_release`, which is why this is a static
   with two call sites and not an entry: the collection and the [UAX14] pass stay alive for as long as the
   `TextRunLine`s index them, and that lifetime is a property of the loop the caller writes. */
static size_t lb_fill(TextRunMeasure *m, lxb_dom_element_t *style, lxb_dom_node_t *first,
                      lxb_dom_node_t *end, TextRunLine **lines)
{
    CssPx available = css_px(0.0);
    lxb_dom_node_t *c;
    size_t n;

    DCHECK(style != NULL, "CSS 2.2 §9.4.2's line boxes were asked for with no element");
    DCHECK(first == NULL || first->parent == lxb_dom_interface_node(style),
           "the run handed to CSS 2.2 §9.4.2's inline formatting context does not start at a CHILD of the "
           "element whose properties the box has. Those two arguments are the two halves of §9.2.1.1's "
           "anonymous block box — the content is one run of the container's children and the style is the "
           "enclosing non-anonymous box's — so a run from somewhere else would measure one document's line "
           "boxes against another element's font and `line-height`");
    /* §9.4.2's CONTENT, COLLECTED BEFORE ANY OF IT IS MEASURED, which [UAX14] forces rather than anyone
       choosing: its rules read forward past the boundary they decide, so no per-character state can settle a
       break as the character arrives. core/layout/text_run.h states it in full. */
    text_run_measure_init(m);
    for (c = first; c != NULL && c != end; c = c->next) lb_child(m, style, c);
    DCHECK(c == end,
           "the half-open run handed to CSS 2.2 §9.4.2's inline formatting context ran off the end of the "
           "child list without ever meeting its exclusive end, so the two came from different lists or the "
           "tree changed under the walk — and the boxes just collected are some prefix of a formatting context "
           "no box has");
    text_run_measure_finish(m);
    /* §9.4.2: "THE WIDTH OF A LINE BOX IS DETERMINED BY A CONTAINING BLOCK and the presence of floats", and
       the float half is refused at the walk above — so every line box here is as wide as the content box of
       the block container that establishes this formatting context, which is `style`.
       FOR §9.2.1.1's ANONYMOUS BLOCK BOX THAT IS STILL `style`'s CONTENT WIDTH and not a second derivation:
       the anonymous box's non-inherited properties "have their initial value", so it has no margin, border or
       padding, and it is a block-level box in its parent's block formatting context — §10.3.3's constraint
       equation with six zero terms makes its width the containing block's, which is the parent's content
       width. The same number, reached without giving an element-less box a `used_value` query it cannot
       answer.
       IT IS DERIVED ONLY WHERE IT IS AN OPERAND. §9.4.2's overflow sentence makes a run with no break position
       inside it ONE line box at every width, so asking for one there would run CSS 2.1 §10.3 over this box to
       throw the answer away — and for a great many block containers (`<div><span></span></div>`, an empty
       one) that layout is not merely wasted but is the earlier subproblem `used_value.c` still crashes for.
       The fill asserts the theorem this skip rests on rather than trusting it. */
    if (text_run_measure_splits(m)) available = used_value_content_px(style, false);
    n = text_run_measure_fill(m, available, lines);
    DCHECK(n == 0 || *lines != NULL, "CSS 2.2 §9.4.2's fill reported line boxes and handed back none of them");
    return n;
}

CssPx line_box_content_height(lxb_dom_element_t *style, lxb_dom_node_t *first, lxb_dom_node_t *end,
                              bool *any_line_box)
{
    TextRunMeasure m;
    TextRunLine *lines = NULL;
    CssPx height = css_px(0.0);
    size_t n, i;

    DCHECK(any_line_box != NULL,
           "CSS 2.2 §9.4.2's line boxes were asked for with nowhere to report whether any of them exists — "
           "that is not an optional out-parameter, it is §8.3.1's own question");
    n = lb_fill(&m, style, first, end, &lines);
    *any_line_box = false;
    for (i = 0; i < n; i++) {
        bool exists = false;
        LbExtent e = lb_line_extent(style, &m, lines[i], &exists);
        CssPx lh;

        /* §9.4.2: a line box the section says "must be treated as ZERO-HEIGHT … and must be treated as NOT
           EXISTING for any other purpose" adds nothing to §10.6.3's distance, and §8.3.1 asks the caller's
           other question over the same fact — its adjoining test excepts such line boxes by name and its
           collapse-through note requires that the box "does not contain a line box". So the two answers are
           produced by one pass and cannot come to describe different lines. */
        if (!exists) continue;
        *any_line_box = true;
        lh = css_px_add(e.above, e.below);
        DCHECK(lh.px >= 0.0,
               "CSS 2.2 §10.8's step 3 produced a NEGATIVE height for one line box. It is \"the distance "
               "between the uppermost box top and the lowermost box bottom\", and every box on the line "
               "contributes `A' + D'` = its own `line-height`, which css-inline-3 §5.1 makes "
               "`<number [0,∞]>` / `<length-percentage [0,∞]>` and says outright that \"negative values are "
               "illegal\" — so a negative maximum is a declaration that should have been dropped by "
               "css-values-4 §5.1's range restriction reaching the arithmetic");
        /* §9.4.2: "line boxes are stacked with NO VERTICAL SEPARATION (except as specified elsewhere) and they
           never overlap", so §10.6.3's "distance from its top content edge to the bottom edge of the last line
           box" is the SUM of the heights above it — no gap term, and no need to carry a running position. */
        height = css_px_add(height, lh);
    }
    free(lines);
    /* THE MEASUREMENT ENDS HERE AND NOT BEFORE: every line above named its boxes by an index into the
       collection, so `lines` and the items it indexes are one object with two owners for the length of that
       loop. core/layout/text_run.h asserts a read after this. */
    text_run_measure_release(&m);
    /* When no line box exists, §10.6.3 has no last line box for its bottom edge to be and the accumulated
       height is exactly the zero this started at — the caller learns through `any_line_box` that this is an
       ABSENT line box rather than a measured zero, which is the distinction §8.3.1 needs and a single number
       cannot carry. */
    return height;
}

/* ---- WHERE THE BOXES ON THOSE LINE BOXES REACH ------------------------------------------------------------
   §9.4.2's distribution again, reduced to a SPAN instead of to a height. See line_box.h for the two facts that
   make each axis exact without a per-item position, and for what a caller may and may not read the numbers as.

   THE INLINE AXIS NEEDS NO `lb_line_extent` AND THEREFORE ASKS NO USED VALUE OF ANY BOX ON THE LINE, and that
   is a derivation rather than a saving. A line box §9.4.2 says "must be treated as NOT EXISTING for any other
   purpose" contains "no text, no preserved white space, no inline elements with non-zero margins, padding, or
   borders, and no other in-flow content" — no character advances, and every box edge on it zero — so its
   `TextRunLine.size` IS ZERO and a maximum over every line is the same number as a maximum over the existing
   ones. Calling `lb_line_extent` to find that out would read used values of boxes this component crashes for
   and turn a measured span into an abort, which is the same short-circuit its own box-edges test relies on. */
static CssPx lb_widest_line(const TextRunLine *lines, size_t n)
{
    CssPx widest = css_px(0.0);
    size_t i;

    for (i = 0; i < n; i++) widest = css_px_max(widest, lines[i].size);
    return widest;
}

void line_box_content_span(lxb_dom_element_t *style, lxb_dom_node_t *first, lxb_dom_node_t *end,
                           bool vertical, CssPx *lo, CssPx *hi)
{
    TextRunMeasure m;
    TextRunLine *lines = NULL;
    CssPx top = css_px(0.0);
    size_t n, i, j;

    DCHECK(lo != NULL && hi != NULL,
           "CSS 2.2 §9.4.2's line boxes were asked where their boxes reach with nowhere to report one of the "
           "two edges. They are two answers and not one: css-writing-modes-4 §6.2 \"Flow-relative Directions\" "
           "puts the ENDING edge of an axis at the lower coordinate for a `rtl` inline axis and at the higher "
           "one otherwise, and a caller that received only the far edge would have the wrong one half the time");
    /* THE CONTENT BOX'S OWN BEGINNING CORNER SEEDS BOTH EDGES, so a formatting context with no line box at all
       reports the degenerate span AT that corner rather than an uninitialised one. It is inside the padding
       box on both axes (CSS 2 §8.1 nests the two and a padding is non-negative), which is CSSOM VIEW §2's own
       other operand — so the seed is invisible to the extreme the caller takes and cannot invent an overflow. */
    *lo = css_px(0.0);
    *hi = css_px(0.0);
    n = lb_fill(&m, style, first, end, &lines);
    if (!vertical) {
        /* §9.4.2's INLINE AXIS. "In general, the left edge of a line box touches the left edge of its
           containing block and the right edge touches the right edge of its containing block", and css-text-4
           §7.1 "Text Alignment: the text-align shorthand" settles where the content sits inside that: "if
           (after justification, if any) the inline contents of a line box are too long to fit within it, then
           the contents are START-ALIGNED: any content that doesn't fit overflows the line box's end edge."
           SO THE START EDGE IS THE ONE OPERAND, AND IT IS THE CONTENT BOX'S OWN — for a line that overflows
           because §7.1 says so, and for a line that fits because the reported edge is then inside the line box
           whatever §7.1's distribution did with it. line_box.h states that as the theorem it is. */
        CssPx widest = lb_widest_line(lines, n);

        if (lb_computed_is(style, "direction", "rtl")) {
            /* THE START EDGE IS THE LINE-RIGHT ONE, so the content grows toward the lower coordinate and the
               line box's own right edge is the far one. This is the ONE arm that needs the line box's WIDTH —
               a `ltr` context measures its overflow from a corner it already has — which is why the used
               content width is read HERE and not beside the fill, where it would run CSS 2.1 §10.3 over every
               box whose run has no break position inside it. */
            CssPx width = used_value_content_px(style, false);

            *lo = css_px_min(*lo, css_px_sub(width, widest));
            *hi = css_px_max(*hi, width);
        } else {
            DCHECK(lb_computed_is(style, "direction", "ltr"),
                   "a computed `direction` is neither `ltr` nor `rtl`. css-writing-modes-4 §2.1 \"Specifying "
                   "Directionality: the direction property\" gives the property the `Value:` line "
                   "`ltr | rtl` and nothing else, so a third spelling is a declaration the cascade should have "
                   "refused — asserted rather than read as \"not rtl\", which would quietly mean `ltr`");
            *hi = css_px_max(*hi, widest);
        }
        free(lines);
        text_run_measure_release(&m);
        return;
    }
    /* §9.4.2's BLOCK AXIS, WHICH IS A MAXIMUM OVER THE BOXES AND NOT THE STACK'S OWN BOTTOM. §10.8's step 3
       makes the line box "the distance between the uppermost box top and the lowermost box bottom", so the
       line box's own edges are the extremes of the boxes ON it PLUS the STRUT — and CSSOM VIEW §2's extreme is
       over "the element's DESCENDANTS' BOXES", of which the strut is not one ("exactly as if each line box
       starts with a zero-width inline box … we call that IMAGINARY box a 'strut'"). A container whose own
       `line-height` exceeds its content's therefore has line boxes taller than anything in them, and taking
       the stack's bottom would report an overflow no box makes. */
    for (i = 0; i < n; i++) {
        bool exists = false;
        LbExtent e = lb_line_extent(style, &m, lines[i], &exists);

        /* §9.4.2's not-existing line box is "treated as ZERO-HEIGHT … for the purposes of determining the
           positions of any elements inside of them", so it advances the stack by nothing and the boxes on it
           are at the position the next line starts from — which the next iteration reports for them. */
        if (!exists) continue;
        for (j = lines[i].from; j < lines[i].to; j++) {
            /* THE BASELINE IS `top + e.above`: §10.8's step 3 measures the line box from the uppermost box
               top, and `e.above` is the maximum `A'` across the line, so the line's baseline sits exactly that
               far below its top edge and every box on it hangs its own `A'` above and `D'` below that one
               line. */
            LbExtent b;

            /* ONLY A CHARACTER ITEM'S BOX IS TAKEN HERE, and the other three kinds are not omitted — they are
               ANOTHER WALK'S. An EDGE, a FORCED BREAK and an ATOMIC INLINE all belong to a real ELEMENT, whose
               own box CSSOM VIEW §2's walk over the element tree reaches and places
               (core/layout/flow_position.h), and each has a margin edge that walk composes DIRECTLY rather than
               out of this line: a non-replaced inline box's vertical margins, padding and borders do not enter
               §10.8's line box calculation at all, and an atomic inline HAS a used width and height of its own
               (core/layout/scrolling_area.c takes exactly that path for a replaced inline, and CSS 2.1 §10.3.2
               is where its extent comes from). Deriving either from `A'` and `D'` here would be a rectangle
               smaller than the one it renders. The ANONYMOUS INLINE BOX around a text run is the box with no
               element to be reached through, which is why it alone is this walk's. */
            if (!text_run_measure_item_is_text(&m, j)) continue;
            b = lb_strut_extent(text_run_measure_item_style(&m, j));
            *lo = css_px_min(*lo, css_px_sub(css_px_add(top, e.above), b.above));
            *hi = css_px_max(*hi, css_px_add(css_px_add(top, e.above), b.below));
        }
        /* §9.4.2: "line boxes are stacked with NO VERTICAL SEPARATION (except as specified elsewhere) and they
           never overlap" — the same sum §10.6.3's distance is, carried here as a running position because a
           span needs where each line STARTS and a height only needs the total. */
        top = css_px_add(top, css_px_add(e.above, e.below));
    }
    free(lines);
    text_run_measure_release(&m);
}

/* ---- css-text-4 §7.1's ALIGNMENT OF ONE LINE BOX'S CONTENT --------------------------------------------------
   CSS 2.2 §9.4.2 hands the question over by name — "when the total width of the inline-level boxes on a line is
   LESS than the width of the line box containing them, their horizontal distribution within the line box is
   determined by the 'text-align' property" — and css-text-4 §7.1 is where that property now lives. It is a SHORTHAND whose
   `Computed value:` line reads "see individual properties", so there is no computed `text-align` to ask for and
   the two properties this reads are its longhands: css-text-4 §7.3 "Default Text Alignment: the text-align-all
   property" and css-text-4 §7.4 "Last Line Alignment: the text-align-last property". EVERY BARE § IN THIS
   COMPONENT'S ALIGNMENT PROSE IS css-text-4's, and each one states that standard rather than inheriting it from
   this banner: an unanchored number is placed by whatever the file cites most, and this file's own dominant
   anchor is neither of the two documents that own alignment.
   css-text-4 §7.4's `auto` IS A REDIRECTION AND NOT A VALUE: "if auto is specified, content on the affected line is
   aligned per text-align-all unless text-align-all is set to justify, in which case it is START-aligned." Both
   halves are here, and the second is the reason `auto` cannot simply fall through to the other longhand. */

/* WHICH LONGHAND'S VALUE ALIGNS THIS LINE. css-text-4 §7.4's property "describes how the LAST LINE of a block
   OR A LINE RIGHT BEFORE A FORCED LINE BREAK is aligned", which is two cases and one answer: the fill's last line, and
   any line the fill closed at a forced break — which is a line holding one, since [UAX14] LB5's `LF !` makes
   the boundary after a forced break mandatory and `lb_line_extent` asserts a line holds at most one. */
static bool lb_line_is_last(const TextRunMeasure *m, TextRunLine line, size_t index, size_t n)
{
    size_t i;

    if (index + 1 == n) return true;
    for (i = line.from; i < line.to; i++)
        if (text_run_measure_item_is_forced_break(m, i)) return true;
    return false;
}

/* THE ALIGNMENT KEYWORD FOR ONE LINE, resolved through css-text-4 §7.4's redirection. OWNED: the caller frees. */
static char *lb_line_alignment(lxb_dom_element_t *style, bool last)
{
    char *all = lb_computed(style, "text-align-all");

    if (!last) return all;
    {
        char *lastv = lb_computed(style, "text-align-last");

        if (strcmp(lastv, "auto") != 0) { free(all); return lastv; }
        free(lastv);
        /* css-text-4 §7.4's `auto`, second half: "unless text-align-all is set to justify, in which case it is
           start-aligned". A justified block therefore does NOT justify its last line, which is what makes
           `justify-all` a separate value of the shorthand at all. */
        if (strcmp(all, "justify") == 0) { free(all); return lb_strdup("start"); }
        return all;
    }
}

/* WHERE `line`'s CONTENT BEGINS INSIDE ITS LINE BOX — the distance from the line box's LEFT edge to its
   content's left edge, in CSS pixels. §9.4.2 gives the line box the containing block's width ("in general, the
   left edge of a line box touches the left edge of its containing block and the right edge touches the right
   edge of its containing block"), and `style`'s content box IS that rectangle: for §9.2.1.1's anonymous block
   box too, whose non-inherited properties "have their initial value" so its own width is its parent's content
   width, exactly as `lb_fill` derives the available width from the same number.
   THE LINE BOX'S WIDTH IS DERIVED ONLY WHERE IT IS AN OPERAND, which is the same discipline `lb_fill` follows
   and for the same reason: a `start`-aligned `ltr` line begins at the content box's own left edge whatever the
   line box is, so asking for the width there would run CSS 2.1 §10.3 over this box to discard the answer — and
   for a great many block containers that layout is the earlier subproblem `used_value.c` still crashes for.
   §7.1's OVERFLOW SENTENCE IS APPLIED BEFORE THE ALIGNMENT AND NOT AFTER IT: "if (after justification, if any)
   the inline contents of a line box are TOO LONG to fit within it, then the contents are START-aligned: any
   content that doesn't fit overflows the line box's end edge." So a line wider than its box takes the start
   answer regardless of what the property says, which is also what makes the skip above sound — the case the
   width would have told us about resolves to the start edge either way. */
static CssPx lb_align_offset(lxb_dom_element_t *style, const TextRunMeasure *m, TextRunLine line,
                             size_t index, size_t n)
{
    char *kw = lb_line_alignment(style, lb_line_is_last(m, line, index, n));
    bool rtl = lb_computed_is(style, "direction", "rtl");
    bool to_left, centered;
    CssPx width, slack;

    if (!rtl)
        DCHECK(lb_computed_is(style, "direction", "ltr"),
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
    /* §7.1's five distributing values. `left` and `right` are the LINE-RELATIVE edges — "in vertical writing "
       modes, this can be either the physical top or bottom" — and the entry below has already
       established that this formatting context's line-left IS its physical left. */
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
    /* THE LINE-LEFT EDGE OF AN `ltr` CONTEXT NEEDS NO WIDTH, which is what lets a `text-align: start` document
       — the initial value of both longhands — be placed without running §10.3 over the establishing box. The
       skip is NOT extended to an `rtl` context that also aligns line-left, because §7.1's overflow sentence
       moves that case: a line too long to fit is START-aligned, which under `rtl` is the line-RIGHT edge and
       therefore a different number. */
    if (to_left && !centered && !rtl) return css_px(0.0);
    width = used_value_content_px(style, false);
    slack = css_px_sub(width, line.size);
    /* §7.1's overflow sentence. A negative slack is a line "too long to fit within it", and its contents are
       start-aligned whatever the property said. */
    if (slack.px < 0.0) return rtl ? slack : css_px(0.0);
    if (centered) return css_px_scale(slack, 0.5);
    return to_left ? css_px(0.0) : slack;
}

/* ---- CSSOM VIEW §6's BOX FRAGMENTS OF ONE INLINE BOX -------------------------------------------------------
   See line_box.h for the frame the four numbers are in, for why the block axis is the LINE BOX's, and for why
   this entry finds the formatting context itself where the two above are handed a run. */

/* §9.4.2's CONTEXT `el` IS ON, EXPRESSED THE WAY core/layout/line_box.h's TWO OTHER ENTRIES ALREADY TAKE ONE —
   the ELEMENT whose properties the box has, the half-open RUN of that element's children the box holds, and
   where the box's own content box origin sits inside that element's content box.
   THE TWO OFFSETS ARE THE WHOLE DIFFERENCE BETWEEN §9.4.2's TWO SHAPES OF ONE CONTEXT, and they are fields
   rather than a flag because the composition below must add them without asking which shape it got. A block
   container "that contains no block-level boxes" (§9.4.2's own condition) establishes the context over its
   WHOLE child list — the box IS that element's, so the run is that list and both offsets are zero because
   there is no box between the two frames — while a MIXED container's inline-level children sit inside CSS 2.2
   §9.2.1.1 "Anonymous block boxes"' boxes, one per maximal run, each of which no element names: its run is its
   own and its origin is where CSS 2 §9.4.1 "Block formatting contexts"' stack put it among its block-level
   siblings.
   `style` IS THE CONTAINER IN BOTH SHAPES, AND §9.2.1.1 IS WHY RATHER THAN A CONVENIENCE: "the properties of
   anonymous boxes are inherited from the enclosing non-anonymous box …. Non-inherited properties have their
   initial value. For example, the font of the anonymous box is inherited from the DIV, but the margins will be
   0." So the container supplies every property the fill and §10.8's measurement read — there is no other
   element to read them off — and it is also the box core/layout/line_box.h promises the caller its coordinates
   are measured from. That promise is what the origin offset KEEPS: a mixed container's fragments come out in
   the same frame an unmixed one's do, so no caller of that header learns that this box's lines were on a box
   the element tree does not contain, and none of them changes.
   THE INLINE-AXIS OFFSET IS A ZERO AND IS STILL ADDED. block_flow.h derives it: `width` has the initial value
   `auto` and both margins are 0, so CSS 2.1 §10.3.3's constraint equation leaves the whole of the container's
   content width to `width` and the anonymous box's two inline margin edges are exactly its container's two
   CONTENT edges. That is also why `lb_align_offset` reads the line box's width off the container for both
   shapes — §9.4.2 gives the line box the containing block's width, and the two boxes have the same one. The
   offset is added rather than assumed away because the enumeration REPORTS it, and a consumer that dropped a
   reported field would be reading one of two numbers and trusting the other. */
typedef struct {
    lxb_dom_element_t *style;
    lxb_dom_node_t *first;
    lxb_dom_node_t *end;
    CssPx origin_x, origin_y;
} LbContext;

/* THE SUBJECT OF EVERY CRASH BELOW, WHICH IS AN ELEMENT AND NOT A LINE.
   §AN-ASSERT-THAT-NAMES-A-REMEDY's test is to count the call sites that can reach an abort and to make the
   ADDRESS part of the assert once that number is larger than one would read by hand. This walk is reached from
   every CSSOM VIEW §6 member that asks for a position or an extent, from §7's scroll algorithms, from a Range's
   client rects, from IntersectionObserver's update and from the rendering step — so it is.
   WHICH ADDRESS IS THE QUESTION, and the answer here is the one core/idl_indexed.c already reasoned out for its
   own shape: a `__FILE__`/`__LINE__` threaded from those callers arrives through `sa_inline_box_edge` and
   `flow_inline_fragment_rects`, which is a handful of forwarding functions for the whole tree — the form that
   rule names as the WRONG capture point — and it would still not say which BOX has no rule. Every remedy the
   aborts below state names a `display` value and a container rather than a place, so the address their reader
   is standing in front of is the ELEMENT and the computed `display` beside it. That is also what the rest of
   this directory already carries: core/layout/used_value.c's `DFAILF`s name the `display` and nothing else.
   MEASURED, AND IT IS WHY THIS IS NOT DECORATION. An abort on this walk was diagnosed from a wasm frame list,
   which named the ASKER — `scrollWidth` — and carried no subject at all; the subject was then reasoned out
   from the fixture's stylesheet, and the conclusion was that its `<div>`s are `display: none` so the walk could
   not have reached them. That was true of the divs and false of the crash, because the walk that aborts is the
   VIEWPORT's, taken over the whole document, and its subject is an element the reasoning never considered. The
   half that had an instrument was the half nobody got wrong.
   IT WRITES INTO A CALLER-OWNED BUFFER rather than handing back lexbor's pointer, because a local name is a
   length-and-pointer pair with no promise of a terminator and a crash path must not read past one. A NULL
   element and a nameless one are DIFFERENT answers and are spelled differently: the first says the caller had
   nothing to name, the second says it had a node neither parser nor `createElement` minted.
   THE NOWHERE-TO-WRITE CASE IS A THIRD ANSWER AND NOT AN ASSERT, for the reason core/layout/box_subject.h
   states in full: a helper a `DFAILF` calls must be TOTAL, because an assert firing during message composition
   does not report a SECOND defect — it REPLACES the first, and the reader loses the box, the container and the
   remedy. This one guarded a caller mistake, so it could only ever have fired while composing the diagnostic it
   is part of, which is the one moment an abort here is worth nothing. The invariant it stood for is unchanged:
   `cap` under 2 leaves no room for a name and a terminator, and this says so instead of aborting over it. */
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

/* THE SAME, FOR A NODE THAT MAY NOT BE AN ELEMENT. §9.2.1.1 delimits its runs over a container's WHOLE child
   list, so the child a run lookup fails on is as likely to be a text node as an element and "(no element)"
   would be a wrong answer rather than a missing one — the node kind IS the fact a reader needs there. */
static const char *lb_node_name(lxb_dom_node_t *n, char *buf, size_t cap)
{
    if (n == NULL) return "(no node)";
    if (n->type == LXB_DOM_NODE_TYPE_ELEMENT) return lb_el_name(lxb_dom_interface_element(n), buf, cap);
    if (n->type == LXB_DOM_NODE_TYPE_TEXT) return "(text node)";
    if (n->type == LXB_DOM_NODE_TYPE_COMMENT) return "(comment node)";
    return "(non-element node)";
}

/* THE COMPUTED `display` OF THE SAME SUBJECT, which is the second half of the address. It LEAKS on the
   aborting path and that is deliberate: the process is one `abort()` away, and the alternative is a free every
   crash arm would have to reach. In release the whole call sits inside `DFAILF`'s `sizeof` and is never
   evaluated, so nothing is allocated there at all.
   IT GOES TO `css_computed_value` DIRECTLY AND NOT THROUGH `lb_computed`, WHICH IS THE ONE INTERESTING LINE IN
   THIS FILE'S TWO NAMING HELPERS. `lb_computed` DCHECKs its result non-NULL, and that assert is correct where
   this file READS a property to compute geometry with. Here the value is being read to COMPOSE A CRASH
   MESSAGE, and an assert on that path does not report a second defect — it REPLACES the first one, so the
   reader loses the box, the container and the remedy and is handed the cascade's invariant instead. A helper
   that a `DFAILF` calls must therefore be TOTAL: every guard in these two functions is there because its
   failure mode is destroying the diagnostic it is part of, which is the opposite of a `?:` past a broken
   invariant — the invariant still has its own crash, at the site that depends on the value. */
static const char *lb_el_display(lxb_dom_element_t *el)
{
    char *d = el == NULL ? NULL : css_computed_value(el, "display");

    return d != NULL ? d : "(no computed display)";
}

/* WHICH of `container`'s anonymous block boxes holds `child`, and WHERE that box is. `child` is `container`'s
   OWN child on the path down to the inline box being measured, so it is one of the nodes §9.2.1.1's forcing
   classified when it delimited the runs — which makes this a LOOKUP in core/layout/block_flow.h's enumeration
   and not a second delimitation. It is reached that way for the reason core/layout/scrolling_area.c gives at
   its own caller: a run's boundaries and its box's POSITION are two halves of ONE derivation, since the
   position is a distance down §9.4.1's stack and that stack is the walk that generated the run, so a second
   copy here could disagree with it about where a margin collapsed.
   THE THREE ASSERTS ARE THREE DIFFERENT DEFECTS AND ARE DELIBERATELY NOT ONE COUNT, because each names a
   different thing to fix — a container the section's sentence does not apply to, a per-child classification
   asked twice and answered differently, and a walk that emitted a run it did not advance past. */
static void lb_anon_run(lxb_dom_element_t *container, lxb_dom_node_t *child, LbContext *ctx)
{
    BlockFlowAnonBox *v = NULL;
    size_t n = block_flow_anonymous_boxes(container, &v), i, found = 0;
    char cbuf[64], nbuf[64];

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
    for (i = 0; i < n; i++) {
        lxb_dom_node_t *c;

        for (c = v[i].first; c != NULL && c != v[i].end; c = c->next) {
            if (c != child) continue;
            ctx->first = v[i].first;
            ctx->end = v[i].end;
            ctx->origin_x = v[i].content_x;
            ctx->origin_y = v[i].content_y;
            found++;
        }
    }
    free(v);
    DCHECKF(found != 0,
           "<%s> (display `%s`), child <%s>: "
           "the block container's child holding this inline box is in NONE of CSS 2.2 §9.2.1.1 \"Anonymous "
           "block boxes\"' runs. Every child that generates an inline-level box is inside exactly one of them, "
           "because the section forces the container \"to have only block-level boxes inside it\" — so a child "
           "is either a block-level box, or generates no box, or is inside the anonymous box wrapping its run "
           "— and the caller has already established that an inline box hangs below this child. So "
           "core/layout/block_flow.c classified it as block-level or as generating no box at all while this "
           "walk reached a box on a line through it, and filling the container's whole child list instead "
           "would partition a run this box is not in",
           lb_el_name(container, cbuf, sizeof cbuf), lb_el_display(container),
           lb_node_name(child, nbuf, sizeof nbuf));
    DCHECKF(found <= 1,
           "<%s> (display `%s`), child <%s>: "
           "one child of a block container is inside TWO of CSS 2.2 §9.2.1.1 \"Anonymous block boxes\"' runs. "
           "The section wraps each MAXIMAL run of inline-level content in one box, so the runs PARTITION the "
           "child list and cannot overlap — two hits are core/layout/block_flow.c's stack having emitted a run "
           "it did not then advance past, and the run and origin taken here would be whichever of the two "
           "boxes the walk reported last",
           lb_el_name(container, cbuf, sizeof cbuf), lb_el_display(container),
           lb_node_name(child, nbuf, sizeof nbuf));
}

/* THE INLINE FORMATTING CONTEXT `el` IS ON — the nearest ancestor that generates a block container box, reached
   by walking PAST the inline boxes between them, and then WHICH of §9.4.2's two shapes of that context holds
   this box. §9.4.2's own condition is asked over the container's WHOLE child list because that is the one shape
   with an element to name it (core/layout/block_flow.h); a MIXED container's runs are §9.2.1.1's anonymous
   block boxes, and the one holding this box supplies the run and the origin `lb_anon_run` looks up. */
static LbContext lb_establishing_context(lxb_dom_element_t *el)
{
    lxb_dom_node_t *a, *child = lxb_dom_interface_node(el);
    char ebuf[64], abuf[64];
    LbContext ctx;

    ctx.style = NULL;
    ctx.first = NULL;
    ctx.end = NULL;
    ctx.origin_x = css_px(0.0);
    ctx.origin_y = css_px(0.0);
    for (a = child->parent; a != NULL && a->type == LXB_DOM_NODE_TYPE_ELEMENT; child = a, a = a->parent) {
        lxb_dom_element_t *anc = lxb_dom_interface_element(a);
        char *d = lb_computed(anc, "display");
        bool container = block_flow_display_is_block_container(d);
        bool step_over = strcmp(d, "inline") == 0 || strcmp(d, "contents") == 0;

        /* THE CRASH BELOW READS `d`, SO THE FREE MOVES UNDER IT AND STAYS A SINGLE ONE. The condition gains
           `!container` and says the same thing it did: the `container` arm returns out of this function, so
           the original test was only ever reached with `container` false. Nothing else about the order moves —
           on every path that continues, the free still happens before the branch it used to precede. */
        if (!container && !step_over)
            DFAILF("<%s> (display `%s`), ancestor <%s> (display `%s`): "
                  "CSS 2.2 §9.4.2's inline formatting context was asked for an inline box whose nearest "
                  "box-generating ancestor is neither a BLOCK CONTAINER nor an inline box it can be reached "
                  "through. Its computed `display` makes it a table box, a table row or row group, a flex or "
                  "grid container, or a box that generates none at all — and each of those puts this box in a "
                  "formatting context a DIFFERENT module owns: CSS 2.1 §17.5 \"Visual layout of table "
                  "contents\" for the first three, css-flexbox §4 and css-grid §9 for the next two, which "
                  "BLOCKIFY their children so an `inline` child of one is not an inline box at all. Fix the "
                  "blockification where the child's `display` is computed (css-display §2.7's "
                  "blockification), or BUILD the module that owns the container",
                  lb_el_name(el, ebuf, sizeof ebuf), lb_el_display(el),
                  lb_el_name(anc, abuf, sizeof abuf), d);
        free(d);
        if (container) {
            ctx.style = anc;
            /* §9.4.2's condition over the WHOLE child list picks the shape, and it is asked ONCE through the
               component that owns it — so this branch and the enumeration `lb_anon_run` reads are the same
               classification of the same children rather than two that could disagree about which of them
               generates a box. */
            if (block_flow_establishes_inline_context(anc)) {
                ctx.first = a->first_child;
                return ctx;
            }
            lb_anon_run(anc, child, &ctx);
            return ctx;
        }
    }
    /* THE ONE ABORT ON THIS WALK WHOSE REMEDY'S OBJECT IS THE CALLER AND NOT THE BOX, which is why it names the
       element's own connectedness rather than a `display`: the fix is that some entry asked for a position
       without asking the has-a-box predicate first, and WHICH ancestor the walk stopped under is what says
       whether the tree is detached or merely rootless. */
    DFAILF("<%s> (display `%s`), highest ancestor reached <%s>: "
          "the inline formatting context walk ran out of ancestors without finding a block container box. "
          "CSS Display §2.8 makes the ROOT ELEMENT a block container whatever it declares (\"a display of "
          "contents computes to block on the root element\"), so every element inside a document has one above "
          "it — an element that reached this line is in a tree with no root element, which is a caller that "
          "asked where a DETACHED box is. core/dom/element_view.h's has-a-box predicate answers that before "
          "any position is asked for, so the two have come apart",
          lb_el_name(el, ebuf, sizeof ebuf), lb_el_display(el),
          lb_node_name(child, abuf, sizeof abuf));
    return ctx;
}

/* §9.4.2's TWO EDGE ITEMS OF ONE INLINE BOX, which delimit its content in the collected run. core/layout/
   line_box.c's own walk emits the opening edge, descends, and emits the closing edge (css-text-3 §5.5 puts them
   "at the box's boundaries" and not at every break inside it), so a box's items are the CONTIGUOUS half-open
   range `[open, close + 1)` — its two edges and everything its descendants contributed between them. That is
   what makes "which lines is this box on" a range intersection rather than a search: an inline box is on a line
   exactly when one of ITS items is, and a descendant's item is one of its items. */
static void lb_edge_items(const TextRunMeasure *m, lxb_dom_element_t *el, size_t count,
                          size_t *open, size_t *close)
{
    size_t i, found = 0;

    for (i = 0; i < count; i++) {
        if (text_run_measure_item_style(m, i) != el) continue;
        /* THE THREE KINDS THAT ARE NOT AN EDGE, EXCLUDED BY NAME. A CHARACTER and a FORCED BREAK carry an
           element that is not this box's boundary, and an ATOMIC INLINE is a box that HAS no boundaries — CSS
           2.2 §9.2.2 makes it "a single opaque box" and `lb_child` emits one item for the whole of it. None of
           the three can belong to `el` here (the caller has established that `el` is a non-replaced inline box
           and this walk does not descend into an atomic), so the test is what makes that a statement rather
           than an arrangement: a kind reaching the count below would delimit a fragment by something that is
           not a boundary of this box. */
        if (text_run_measure_item_is_text(m, i) || text_run_measure_item_is_forced_break(m, i) ||
            text_run_measure_item_is_atomic(m, i))
            continue;
        if (found == 0) *open = i; else *close = i;
        found++;
    }
    DCHECK(found == 2,
           "an inline box does not have exactly TWO box-edge items in the run its formatting context "
           "collected. core/layout/line_box.c emits one before descending into the box and one after "
           "(`text_run_measure_add_box_edge`, which css-text-3 §5.5 \"Line Breaking Details\" places at the "
           "box's two boundaries), and it emits them even when both are ZERO because an edge occupies a "
           "POSITION — so a count other than two is that walk and this reader disagreeing about which element "
           "owns an item, and every fragment below would be delimited by another box's boundary");
    DCHECK(found != 2 || *open < *close,
           "an inline box's CLOSING edge item is not after its opening one, so the range that is supposed to "
           "hold its content holds none of it");
}

/* THE ONE RUN ITEM OF AN ATOMIC INLINE-LEVEL BOX, which is the same delimitation question `lb_edge_items`
   answers for an inline box and has a different answer for the same reason the two boxes are different kinds.
   CSS 2.2 §9.2.2 "Inline-level elements and inline boxes": an atomic inline-level box "participate[s] in [its]
   inline formatting context as a SINGLE OPAQUE BOX", so `lb_child` emits ONE item for the whole of it and does
   not descend into it — there are no boundaries to bracket a range with, and the range is the item's own index.
   THE THREE KINDS THAT ARE NOT IT ARE EXCLUDED BY AN ASSERT AND NOT BY A `continue`, which is the one place
   this differs in shape from `lb_edge_items`: that walk skips kinds a box legitimately owns (its descendants'
   characters lie between its two edges), and this box owns NOTHING but its own item, so a character, a forced
   break or a box edge carrying this element is that walk and this reader disagreeing about what an atomic
   inline puts on a line. */
static size_t lb_atomic_item(const TextRunMeasure *m, lxb_dom_element_t *el, size_t count)
{
    size_t i, found = 0, at = 0;

    for (i = 0; i < count; i++) {
        if (text_run_measure_item_style(m, i) != el) continue;
        DCHECK(text_run_measure_item_is_atomic(m, i),
               "an ATOMIC INLINE-LEVEL box owns a run item that is not the atomic one — a character, a forced "
               "break, or one of the two box EDGES core/layout/line_box.c brackets an inline box's content "
               "with. CSS 2.2 §9.2.2 makes this box \"a single opaque box\" and CSS 2.1 §3.1 puts a replaced "
               "element's content \"outside the scope of the CSS formatting model\", so `lb_child` emits its "
               "one item and does not descend — an item of any other kind carrying this element is that walk "
               "having collected an inside this box does not have");
        at = i;
        found++;
    }
    DCHECK(found == 1,
           "an ATOMIC INLINE-LEVEL box does not have exactly ONE run item in the formatting context that "
           "collected it. `lb_child` emits one (`text_run_measure_add_atomic`, css-text-3 §5.5 \"Line Breaking "
           "Details\"' \"each replaced element or other atomic inline\") and returns without descending, so a "
           "count of zero is this box never having been collected — the walk skipped it as out of flow or as "
           "generating no box, which the caller's has-a-box predicate has already denied — and a count above "
           "one is two walks through one accumulator");
    return at;
}

size_t line_box_inline_fragments(lxb_dom_element_t *el, lxb_dom_element_t **establishing,
                                 LineBoxFragment **out)
{
    TextRunMeasure m;
    TextRunLine *lines = NULL;
    LineBoxFragment *frags;
    LbContext ctx;
    lxb_dom_element_t *style;
    CssPx top, lead, trail;
    CssPx before = css_px(0.0), after = css_px(0.0), drop = css_px(0.0), extent = css_px(0.0);
    size_t n, i, open = 0, close = 0, nf = 0;
    bool atomic;

    DCHECK(el != NULL && establishing != NULL && out != NULL,
           "CSSOM VIEW §6's box fragments were asked for with no element or nowhere to report them");
    DCHECK(lb_computed_is(el, "display", "inline"),
           "CSSOM VIEW §6's box fragments were asked for a box whose computed `display` is not `inline`, so it "
           "is not on a line box of the formatting context this walk fills. CSS 2.2 §9.2.2 \"Inline-level "
           "elements and inline boxes\" makes an `inline-block`, `inline-table`, `inline-flex` or "
           "`inline-grid` an ATOMIC inline-level box, which \"participate[s] in [its] inline formatting context "
           "as a SINGLE OPAQUE BOX\" — that IS the single-item shape below and it is not what is missing for "
           "one. WHAT KEEPS SUCH A BOX OUT OF THIS ENTRY IS THE CALLER AND NO LONGER `lb_child`: an "
           "`inline-block` whose `overflow` computes to other than `visible` is now COLLECTED onto a line "
           "(CSS 2.2 §10.8.1's exception states its baseline as the bottom margin edge, which `lb_atomic_extent` "
           "already answers), so a fill that holds its item does exist — and the only caller, "
           "core/layout/flow_position.c, crashes for every atomic inline-level box before asking, naming the "
           "SPLIT of that box's margin box at its baseline as the placement it still owes. So this entry's "
           "single-item delimitation would run, and what has not been decided is whether its `display` test "
           "should admit the routed box; deciding it is that file's crash and not a widening to make here. "
           "Everything else is block-level or generates no box at all, and the caller's own step "
           "(core/dom/element_view.h's fragment kind, core/layout/flow_position.c's placement) decides that "
           "before asking — so reaching here is those classifications having come apart");
    /* WHICH DELIMITATION THIS BOX'S FRAGMENTS TAKE, AND IT IS NOT THE `display` THE TEST ABOVE READS. HTML
       §15.4 "Replaced elements" makes an `img`, an `iframe`, a `video` or an `input` a replaced element while
       its computed `display` stays `inline`, and CSS 2.2 §9.2.2's opaque-box sentence covers it for the same
       reason it covers an `inline-block` — css-text-3 §5.5 "Line Breaking Details" names them together as
       "each replaced element or other atomic inline". So `lb_child` collects it as ONE run item rather than as
       a pair of boundaries, and its fragment is delimited by that item's own index: the edge walk would find
       none and delimit a fragment out of another box's.
       IT IS EXACTLY ONE FRAGMENT AND THAT IS §9.2.2's SENTENCE RATHER THAN A COUNT THIS FILE CHOOSES: a single
       opaque box is never the box §9.4.2 "SPLIT[s] into several boxes … distributed across several line boxes",
       so the one item is on one line and the loop's intersection selects it. The closing assert below is over
       both shapes and this one additionally asserts the count. */
    atomic = replaced_element_of(el).replaced;
    /* HTML §15.3.4 "Phrasing content"'s `br { display-outside: newline; }` and `wbr { display-outside:
       break-opportunity; }` reach this component as plain `display: inline` boxes carrying a fact the cascade
       cannot answer for (core/layout/phrasing_break.h), and `lb_child` puts them on the line as a FORCED BREAK
       or as a break opportunity rather than as an inline box with two boundaries — which is css-text-3 §5.5's
       own statement that a box boundary introduces no break and this element is nothing BUT one. So they have
       no edge items and the range below would find none. */
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
              "§10.6.1's content area out of its own first available font, which `lb_line_extent` already "
              "reads for it (`<br style=\"line-height:100px\">` makes a line 100px tall). A `wbr` does not "
              "reach even that: `lb_phrasing_break` crashes for the soft wrap opportunity itself, one walk "
              "earlier, so it is that crash and not this one that gates it");
    lb_require_horizontal_tb(el);
    ctx = lb_establishing_context(el);
    style = ctx.style;
    lb_require_horizontal_tb(style);
    *establishing = style;
    /* THE RUN IS FILLED AND NOT THE CHILD LIST, which is the whole of CSS 2.2 §9.2.1.1 "Anonymous block boxes"'
       effect on this walk. Inside a MIXED container the line boxes this box is on belong to ONE of the
       anonymous block boxes, and a fill over the container's whole child list would flow this box's items
       together with every OTHER run's — a different partition, on line boxes that do not exist, whose §10.8
       step 3 heights would be maxima over boxes that are not on one line. For the unmixed shape the run IS the
       whole child list, so this is the same fill it always was.
       THE STACK IS SEEDED AT THAT BOX'S OWN TOP CONTENT EDGE rather than at zero, which is what keeps every
       coordinate below in the frame core/layout/line_box.h promises the caller — an offset from the
       CONTAINER's content box origin — while the lines themselves are measured inside the box that holds them.
       The seed is zero for the unmixed shape, by the same derivation, so that caller's numbers are unchanged. */
    top = ctx.origin_y;
    n = lb_fill(&m, style, ctx.first, ctx.end, &lines);
    DCHECK(n >= 1,
           "CSS 2.2 §9.4.2's fill produced NO line box for a formatting context that contains an inline-level "
           "box. That box's items are content the fill partitions — \"line boxes are created as needed to hold "
           "inline-level content\", which is an inline box's two EDGES or an atomic inline's one item — so a "
           "run holding them has at least one line, and an empty answer means this element's items were never "
           "collected: the walk skipped it as out of flow or as generating no box, which the caller's "
           "has-a-box predicate has already denied");
    /* THE ITEM COUNT COMES OFF THE PARTITION AND NOT OFF THE ACCUMULATOR, because the partition is what this
       file is entitled to read: `text_run_measure_fill` asserts that its last line closes at the end of the
       collection ("[UAX14] LB3 … did not close §9.4.2's last line box over the whole ITEM collection"), so
       `lines[n - 1].to` IS that count and is the bound every index below is compared against anyway.
       THE TWO DELIMITATIONS PRODUCE ONE HALF-OPEN ITEM RANGE `[open, close + 1)` AND THE LOOP BELOW READS
       NOTHING ELSE, which is what makes the atomic a shape of this walk rather than a second walk beside it:
       an inline box's range is its two boundaries and everything its descendants put between them, and an
       atomic's is the one item CSS 2.2 §9.2.2's "single opaque box" contributes. */
    if (atomic) open = close = lb_atomic_item(&m, el, lines[n - 1].to);
    else lb_edge_items(&m, el, lines[n - 1].to, &open, &close);
    frags = malloc(n * sizeof *frags);
    CHECK(frags != NULL, "out of memory reporting CSSOM VIEW §6's box fragments. There is one entry per line "
                         "box of one formatting context and the fragments are a subset of them, so a failure "
                         "here is the physical floor");
    /* §6's step 3 asks for the BORDER area and the run carries the MARGIN one — an EDGE item is css-sizing-3
       §2.2's OUTER size at one boundary ("based on the outer size of the box"), so it holds that side's margin
       as well as its border and padding. The two margins therefore come off the span, and WHICH FRAGMENT each
       comes off is CSS 2.2 §9.4.2's own sentence rather than a distribution: "when an inline box is split,
       margins, borders, and padding have NO VISUAL EFFECT where the split occurs (or at any split, when there
       are several)." So the leading margin is on the fragment holding the OPENING edge item and the trailing
       one on the fragment holding the CLOSING edge item — which for an unsplit box is the same fragment, and
       for a split one leaves every middle fragment running edge to edge with nothing taken off it.
       AN ATOMIC INLINE TAKES THE SAME TWO OFF FOR THE SAME REASON READ ONE LINE EARLIER: `lb_child` sizes its
       ONE item with `used_value_margin_edge_px` — the MARGIN box, which CSS 2.2 §9.4.2 is what puts on the
       line ("horizontal margins, borders, and padding are respected between these boxes") — so the item span
       exceeds §6's border area by exactly this box's own two horizontal margins, and it is unsplit, so both
       come off its one fragment. */
    lead = used_value_px(el, "margin-left");
    trail = used_value_px(el, "margin-right");
    if (atomic) {
        /* CSS 2.2 §10.8's `vertical-align` definition, WHICH IS WHERE AN ATOMIC INLINE'S BLOCK AXIS IS DECIDED
           AND NOT §10.6.1's. Two of its sentences compose and `lb_atomic_extent` above is the same pair read
           for the line's own height: "for all other elements, the box used for alignment is the MARGIN BOX",
           and `baseline`'s own definition — "align the baseline of the box with the baseline of the parent
           box. IF THE BOX DOES NOT HAVE A BASELINE, ALIGN THE BOTTOM MARGIN EDGE with the parent's baseline."
           A REPLACED ELEMENT HAS NO BASELINE: §10.8 gives one to an `inline-table` and to an `inline-block`
           and to nothing else, and `lb_require_baseline_alignment` has already refused every other
           css-inline-3 §4.2 alignment before this box reached the run. So its BOTTOM MARGIN EDGE sits ON the
           line's baseline, which is the one coordinate the loop below has, and §8.1's nesting is the rest: the
           border area's far edge is that baseline less this box's own `margin-bottom`, and its near edge is
           one used BORDER EDGE EXTENT (§10.6.2 "Inline replaced elements, block-level replaced elements in
           normal flow, 'inline-block' replaced elements in normal flow and floating replaced elements",
           through core/layout/used_value.h) further out.
           IT IS THE SAME TWO NUMBERS `lb_atomic_extent` PUT ABOVE THE BASELINE, read back through §8.1 rather
           than restated: that function's `above` is `used_value_margin_edge_px(el, true)` and its `below` is
           zero, so the margin box's bottom IS the baseline and this composition and that one cannot come to
           describe different boxes. */
        drop = used_value_px(el, "margin-bottom");
        extent = used_value_border_edge_px(el, true);
    } else {
        /* CSS 2 §8.1 "Box dimensions"' two nestings between this box's CONTENT area and its BORDER area on the
           block axis. §10.6.1 is what says they are stated over the content area at all — "the vertical
           padding, border and margin of an inline, non-replaced box start at the top and bottom of the content
           area" — and they are read ONCE because they are a property of the BOX: §9.4.2's "when an inline box
           is split, margins, borders, and padding have no visual effect WHERE THE SPLIT OCCURS" cuts the
           INLINE axis, and a split leaves both block-axis edges on every fragment. */
        before = css_px_add(lb_border_px(el, "top"), used_value_px(el, "padding-top"));
        after = css_px_add(lb_border_px(el, "bottom"), used_value_px(el, "padding-bottom"));
    }
    for (i = 0; i < n; i++) {
        bool exists = false;
        LbExtent e = lb_line_extent(style, &m, lines[i], &exists);
        CssPx height = exists ? css_px_add(e.above, e.below) : css_px(0.0);

        if (lines[i].from <= close && lines[i].to > open) {
            size_t lo = lines[i].from > open ? lines[i].from : open;
            size_t hi = lines[i].to < close + 1 ? lines[i].to : close + 1;
            CssPx align = lb_align_offset(style, &m, lines[i], i, n);
            /* The offset along the line is inside the box holding it, and `ctx.origin_x` is that box's own
               inline-start content edge inside the container — zero, by §9.2.1.1's initial values with
               §10.3.3's constraint equation, and added rather than assumed for the reason the type's banner
               gives. */
            CssPx start = css_px_add(ctx.origin_x, css_px_add(align, text_run_measure_line_offset(&m, lines[i], lo)));
            CssPx end = css_px_add(ctx.origin_x, css_px_add(align, text_run_measure_line_offset(&m, lines[i], hi)));
            /* §10.8's step 3 measures the line box from its uppermost box top, and `e.above` is the maximum
               `A'` across the line — so the line's baseline sits exactly that far below its top edge, and
               every box on it hangs its own content area from that one line. */
            CssPx baseline = css_px_add(top, e.above);

            if (lo == open) start = css_px_add(start, lead);
            if (hi == close + 1) end = css_px_sub(end, trail);
            frags[nf].inline_start = start;
            frags[nf].inline_end = end;
            DCHECK(frags[nf].inline_end.px >= frags[nf].inline_start.px,
                   "a box fragment's END is before its START along the line. Both are "
                   "`text_run_measure_line_offset` over the SAME line at two bounds and that walk is a sum of "
                   "non-negative advances and non-negative box edges, so a decreasing prefix is an advance "
                   "measure or an edge that lost its sign");
            if (atomic) {
                /* §10.8's `vertical-align` `baseline` over a box with NO baseline, derived where the two terms
                   were read: the bottom MARGIN edge is the line's baseline, so the border area ends one
                   `margin-bottom` above it and begins one used border-edge extent further out. */
                frags[nf].block_end = css_px_sub(baseline, drop);
                frags[nf].block_start = css_px_sub(frags[nf].block_end, extent);
                /* THE ONE PLACE THIS RECTANGLE AND core/layout/used_value.h CAN DISAGREE, ASSERTED. The block
                   axis above IS that component's extent by construction, but the INLINE axis is not: it is
                   `text_run_measure_line_offset` at two adjacent item bounds less this box's two horizontal
                   margins, and it must come out at the SAME used border-edge extent CSSOM VIEW §6's step 3
                   reports for this element through core/dom/element_view.c's one-fragment arm — CSS 2.1
                   §10.3.2 "Inline, replaced elements"' used width with §8.1's padding and border around it.
                   The defect it catches is the item having been sized off a different element, an index that
                   named the wrong item, or css-text-3 §4.1.2's trimming having reached between two bounds that
                   are adjacent; the neighbourhood is IEEE-754's around one real number reached by two orders
                   of addition, exactly as `lb_strut_extent`'s identity is. */
                DCHECK(lb_close(css_px_sub(frags[nf].inline_end, frags[nf].inline_start).px,
                                used_value_border_edge_px(el, false).px),
                       "an ATOMIC INLINE-LEVEL box's fragment is a different width along the line than CSS 2.1 "
                       "§10.3.2 \"Inline, replaced elements\"' used BORDER EDGE. The run item carries "
                       "`used_value_margin_edge_px` — CSS 2.2 §9.4.2's \"horizontal margins, borders, and "
                       "padding are respected between these boxes\" — so its span at two ADJACENT item bounds "
                       "less the box's own two horizontal margins is that same nesting read back through CSS 2 "
                       "§8.1, and a disagreement is the item having been sized for a different element than "
                       "the margins were read for");
            } else {
                /* §10.6.1's CONTENT AREA out of the first available font's `A` and `D`, with CSS 2 §8.1's
                   padding and border nested outside it — "the vertical padding, border and margin of an
                   inline, non-replaced box START AT THE TOP AND BOTTOM OF THE CONTENT AREA, and has nothing to
                   do with the 'line-height'." The line's own height is therefore NOT this rectangle and is not
                   read for it. */
                frags[nf].block_start = css_px_sub(css_px_sub(baseline, css_font_ascent_px(el)), before);
                frags[nf].block_end = css_px_add(css_px_add(baseline, css_font_descent_px(el)), after);
            }
            nf++;
        }
        /* §9.4.2: "line boxes are stacked with NO VERTICAL SEPARATION (except as specified elsewhere) and they
           never overlap", and a line the section says "must be treated as ZERO-HEIGHT ... for the purposes of
           determining the positions of any elements inside of them" advances the stack by nothing — which is
           why the fragment above took `height` from `exists` rather than skipping the line: a box on such a
           line has a position, and it is the one the next line starts from. */
        top = css_px_add(top, height);
    }
    free(lines);
    text_run_measure_release(&m);
    DCHECK(nf >= 1,
           "an inline-level box landed on NO line box of the formatting context that collected it. Its items "
           "are inside `[0, count)` and CSS 2.2 §9.4.2's fill PARTITIONS those items across its lines, so every "
           "item is on exactly one line — an empty intersection is that partition having lost an item, which "
           "`text_run_measure_fill` asserts it does not");
    /* §9.2.2's "single opaque box" IS the count, asserted rather than assumed: the item range of an atomic
       inline is ONE index, the fill's lines partition the items, and a half-open range of one index therefore
       meets exactly one line. A second fragment would be that partition having put one item on two lines. */
    DCHECK(!atomic || nf == 1,
           "an ATOMIC INLINE-LEVEL box was reported as MORE THAN ONE box fragment. CSS 2.2 §9.2.2 "
           "\"Inline-level elements and inline boxes\" makes it \"a single opaque box\", so it is never the "
           "box §9.4.2 \"SPLIT[s] into several boxes … distributed across several line boxes\" — its one run "
           "item is on one line, and a second fragment is CSS 2.2 §9.4.2's fill having put one item on two");
    *out = frags;
    return nf;
}

void line_box_inline_margin_span(lxb_dom_element_t *el, lxb_dom_element_t **establishing,
                                 bool vertical, CssPx *lo, CssPx *hi)
{
    LineBoxFragment *frags = NULL;
    size_t n, i;

    DCHECK(el != NULL && establishing != NULL && lo != NULL && hi != NULL,
           "CSS 2.2 §9.4.2's margin span of an inline box was asked for with no element, nowhere to report the "
           "formatting context it is in, or nowhere to report one of the two edges");
    /* BOTH HALVES OF "NON-REPLACED INLINE BOX", ASKED HERE BECAUSE THIS ENTRY'S OWN BLOCK AXIS IS THE HALF
       THAT DIVIDES THEM AND `line_box_inline_fragments` NO LONGER ASKS IT. That entry answers a REPLACED
       element now — its fragment is CSS 2.2 §9.2.2's single opaque box, delimited by one run item — and this
       entry's vertical arm below reports the border area AS the margin edge on the strength of CSS 2.2 §8.3
       "Margin properties"' own exception, which is written over NON-REPLACED inline elements alone ("vertical
       margins will not have any effect on non-replaced inline elements"). A replaced element's vertical
       margins DO have an effect, so answering one here would drop them silently — the caller composes an
       ORIGIN PLUS AN EXTENT for it instead (core/layout/scrolling_area.c's own predicate is this same pair,
       and §10.3.2 and §10.6.2 give it both numbers), which is why this is an assert and not an arm. */
    DCHECK(!replaced_element_of(el).replaced,
           "CSS 2.2 §9.4.2's margin span was asked for a REPLACED inline element. §8.3 \"Margin properties\"' "
           "exception — \"these properties apply to all elements, but VERTICAL MARGINS WILL NOT HAVE ANY "
           "EFFECT ON NON-REPLACED INLINE ELEMENTS\" — is what lets the block arm below report a border area "
           "as a margin edge, and it does not cover this box: HTML §15.4 \"Replaced elements\" makes it "
           "replaced, so `margin-top` and `margin-bottom` are used values its margin edge is outside of. It "
           "also does not need this entry at all — CSS 2.1 §10.3.2 and §10.6.2 give it both extents and "
           "core/layout/flow_position.h gives it one origin, which is the composition "
           "core/layout/scrolling_area.c's own non-replaced-inline predicate routes it to");
    n = line_box_inline_fragments(el, establishing, &frags);
    DCHECK(n >= 1 && frags != NULL,
           "CSS 2.2 §9.4.2's fragments were reported as NONE for an inline box that generates one. That entry's "
           "own closing assert makes a zero count impossible — the box's two edge items are content the fill "
           "partitions and every item is on exactly one line — so this is that contract having been broken "
           "between two functions of one file");
    if (vertical) {
        /* CSS 2.2 §8.3: "vertical margins will not have any effect on non-replaced inline elements", so the
           margin edge on this axis IS §10.6.1's border area and EVERY fragment carries its own. */
        *lo = frags[0].block_start;
        *hi = frags[0].block_end;
        for (i = 1; i < n; i++) {
            *lo = css_px_min(*lo, frags[i].block_start);
            *hi = css_px_max(*hi, frags[i].block_end);
        }
    } else {
        /* §9.4.2's split sentence decides WHICH coordinate each fragment contributes, and the two ends are the
           only ones a margin reaches: the FIRST fragment's beginning and the LAST fragment's end are the box's
           own boundaries, every other fragment edge is a split where "margins, borders, and padding have no
           visual effect". A NEGATIVE margin is why the two ends are the SEED and not two more operands of a
           loop over all of them: with `margin-left: -20px` the first fragment's MARGIN edge is 20px INSIDE its
           border edge, and that border edge is then a coordinate no margin edge of this box occupies. */
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
}
