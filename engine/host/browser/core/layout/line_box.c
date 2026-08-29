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
              "than one baseline set is a box with more than one line inside it, which is an ATOMIC INLINE, "
              "and this component crashes for one of those before it reaches this check. So the two answers "
              "have come apart: either an atomic inline reached the line without being classified as one, or "
              "a declaration set `baseline-source` on a non-replaced inline box, where §4.2.1's own "
              "`Applies to:` line still admits it and the first and last baseline sets are the same set. "
              "DECIDE which, here, before reading a source that selects between one thing");
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

static void lb_take(LbExtent *line, LbExtent box)
{
    line->above = css_px_max(line->above, box.above);
    line->below = css_px_max(line->below, box.below);
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
   as exactly that — two EDGE items carrying a width and no code point, invisible to [UAX14]. These two
   elements are inline boxes for which that is FALSE. HTML §15.3.4 "Phrasing content" names the two
   elements that are inline boxes and are NOT merely boundaries:
     `br  { display-outside: newline; }`             a FORCED line break, so there are two line boxes and the
                                                     first "ends with a preserved newline", which is the
                                                     conjunct of §9.4.2's zero-height rule that makes the line
                                                     EXIST even though nothing is on it;
     `wbr { display-outside: break-opportunity; }`   a SOFT WRAP OPPORTUNITY, so how many line boxes there are
                                                     becomes a function of the widths on either side of it.
   THE TEST IS BY LOCAL NAME, AND THE REASON IS A GAP IN THE SPECS THEMSELVES rather than a shortcut taken
   here. Those two values are declared by HTML and defined by NO CSS module: css-display-4 §2 "Box Layout
   Modes: the display property" has the term `display-outside` and neither the keyword `newline` nor
   `break-opportunity` anywhere in it, so there is no `Computed value:` line to derive and no computed value
   to ask for. What a user agent actually implements is the css-text-3 §5 "Line Breaking and Word Boundaries"
   behaviour those two names stand for — a FORCED LINE BREAK for one and a SOFT WRAP OPPORTUNITY for the
   other — and where that fact enters this engine is a decision for whoever builds it. The alternative to
   asking the name is answering a height of ZERO for `<div><br></div>`, which every user agent renders one
   line tall. So this function computes nothing and only ever crashes, and it is deleted by whatever mechanism
   ends up carrying the break: a layout component reading an HTML tag name is reading around the cascade. */
static void lb_require_no_break_element(lxb_dom_element_t *el)
{
    size_t n = 0;
    const lxb_char_t *tag = lxb_dom_element_local_name(el, &n);

    DCHECK(tag != NULL, "an element in an inline formatting context has no local name — every element this "
                        "engine's parsers mint is created with one");
    if (tag == NULL) return;
    if (n == 2 && memcmp(tag, "br", 2) == 0)
        DFAIL("HTML §15.3.4 \"Phrasing content\" gives `br` the UA declaration `display-outside: newline`, "
              "which is a FORCED LINE BREAK — css-text-3 §5 \"Line Breaking and Word Boundaries\" calls a break "
              "\"due to explicit line-breaking controls (such as a preserved newline character)\" forced. "
              "COLLECTED AS IT STANDS IT WOULD BE A PAIR OF ZERO-WIDTH EDGE ITEMS, which §5.5 says introduces "
              "no break at all — so CSS 2.2 §9.4.2's fill would run every line of this context together and "
              "answer a line COUNT that is simply wrong rather than absent, and css-sizing-3 §2.1's "
              "max-content size would be the whole paragraph on one line. It also makes the line EXIST: "
              "§9.4.2 exempts a line box only when it \"do[es] not end with a preserved newline\", and this "
              "one does. THE STACKING IS NO LONGER WHAT IS MISSING — core/layout/text_run.h's fill already "
              "distributes a run across as many line boxes as its forced breaks and its available width "
              "produce, and this file already measures §10.8 over each of them. WHAT IS MISSING IS ONE FACT "
              "REACHING THE RUN: the value is DEFINED BY NO CSS MODULE — css-display-4 §2 \"Box Layout Modes: "
              "the display property\" does not contain the keyword `newline` at all — so there is no computed "
              "value to derive and this element arrives as a plain `display: inline` box with nothing to "
              "distinguish it. BUILD the forced break as what css-text-3 §5 makes it, carried wherever the "
              "cascade can answer for it, and give text_run.h a way to place one in the collected run beside "
              "its CHAR and EDGE items — [UAX14] cannot decide it, because there is no code point here for it "
              "to decide about");
    if (n == 3 && memcmp(tag, "wbr", 3) == 0)
        DFAIL("HTML §15.3.4 \"Phrasing content\" gives `wbr` the UA declaration `display-outside: "
              "break-opportunity`, which puts a SOFT WRAP OPPORTUNITY on this line — css-text-3 §5 says "
              "\"wrapping is only performed at an allowed break point, called a soft wrap opportunity\", and "
              "the UA \"must minimize the amount of content overflowing a line by wrapping the line at a soft "
              "wrap opportunity, if one exists\". THE WIDTH-DRIVEN SEARCH IS BUILT and this is no longer part "
              "of what this names: core/layout/text_run.h's fill decides every soft wrap opportunity in a run "
              "against the available width of each line, which is exactly the comparison this element adds one "
              "more position to. The keyword is defined by no CSS module either (css-display-4 §2 does not "
              "contain `break-opportunity`), so it arrives as a plain `display: inline` box exactly as `br` "
              "does, and its opportunity is invisible to [UAX14] for the same reason `br`'s break is — there "
              "is no code point here to decide about. BUILD it with `br`'s forced break, as a third item kind "
              "in the collected run; the atomic inline needs the same item carrying a WIDTH as well as an "
              "opportunity, which is why css-text-3 §5.5's \"soft wrap opportunity before and after each "
              "replaced element or other atomic inline\" is the next case and not a separate one");
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
    bool atomic, inline_box;

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
        DFAIL("css-display §3.1 gives this child `display: contents`, which \"does not generate any boxes "
              "itself, but its children and pseudo-elements still generate boxes and text runs as normal\" — so "
              "the boxes on this line are the child's children spliced in at its position. That splice is a "
              "BOX-TREE construction step belonging to every walk over children rather than to this one, and "
              "core/layout/block_flow.c's own child walk names the same absence. BUILD css-display §3's "
              "box-tree flattening as the thing both walks iterate");
        return;
    }
    atomic = strcmp(d, "inline-block") == 0 || strcmp(d, "inline-flex") == 0 ||
             strcmp(d, "inline-grid") == 0 || strcmp(d, "inline-table") == 0;
    inline_box = strcmp(d, "inline") == 0;
    free(d);
    if (atomic)
        DFAIL("CSS 2.2 §9.2.2 makes this an ATOMIC INLINE-LEVEL box, which \"participate[s] in [its] inline "
              "formatting context as a single opaque box\" — and css-text-3 §5.5 \"Line Breaking Details\" "
              "states the consequence this component cannot absorb: \"for Web-compatibility there is a SOFT "
              "WRAP OPPORTUNITY before and after each replaced element or other atomic inline\". One atomic "
              "inline therefore puts a break opportunity on the line, so how many line boxes there are becomes "
              "a function of this box's own used WIDTH — §10.3.9's for an inline-block, its own module's for an "
              "inline-flex or inline-grid — and §10.8's step 1 wants its MARGIN BOX height rather than its "
              "`line-height` (\"for replaced elements, inline-block elements, and inline-table elements, this "
              "is the height of their margin box\"). THE BREAK SEARCH IS BUILT and is no longer part of what "
              "this names: core/layout/text_run.h's fill already distributes a run against each line's "
              "available width. TWO THINGS ARE LEFT AND THEY ARE BOTH ABOUT THIS BOX. (1) Its used inline "
              "size, from core/layout/used_value.c, carried into the run as an item that has a WIDTH and its "
              "own two opportunities — which is neither of the two item kinds text_run.h has, since a CHAR is "
              "sized by an advance measure and an EDGE introduces no break. (2) §10.8's step 1 over its margin "
              "box, which `lb_strut_extent` does not compute: that function is §10.8.1's strut for a box "
              "containing no glyphs, and an atomic inline is a box whose own height is a layout");
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
    if (!inline_box)
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
    if (replaced_element_of(el).replaced)
        DFAIL("HTML §15.4 makes this a REPLACED ELEMENT, so §10.8's step 1 takes \"the height of their margin "
              "box\" rather than its `line-height`, and css-text-3 §5.5 puts a soft wrap opportunity before "
              "and after it — the same two consequences an atomic inline has, reached through the element's "
              "own nature rather than through its `display`. Its natural dimensions are already answered "
              "(core/layout/replaced_element.h) and so is the break search the width feeds — "
              "core/layout/text_run.h's fill — so what is missing is the used width §10.3.2 derives from "
              "those dimensions, and the same run item the atomic inline case above names: one carrying a "
              "WIDTH and its own two soft wrap opportunities. BUILD it with that case, since one item kind "
              "serves both");
    /* THE ELEMENTS THAT CHANGE WHERE THE RUN BREAKS ARE REFUSED BEFORE IT IS COLLECTED, because the fill's
       whole answer is a function of the break positions and a run collected without one of them is a partition
       of different text. */
    lb_require_no_break_element(el);
    /* §10.8's step 2 for this box. It is asked at the WALK and not at the per-line pass below even though the
       height is measured there, because it is a question about the BOX — whether §10.8.1's `A'` and `D'` are
       measured from the same baseline as its neighbours' — and every box in this context is reached exactly
       once here, while the per-line pass reaches a box once per ITEM it owns. */
    lb_require_baseline_alignment(el);
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
   line holding a character item holds a character the document contains. The other three conjuncts crash on
   the way here — preserved white space in `tr_wraps`, in-flow content at the atomic-inline and replaced arms,
   the preserved newline at `lb_require_no_break_element` — leaving the box-edges conjunct as the only one this
   function still decides. */
static LbExtent lb_line_extent(lxb_dom_element_t *style, const TextRunMeasure *m, TextRunLine line, bool *exists)
{
    LbExtent out = lb_strut_extent(style);
    size_t i;

    DCHECK(line.from < line.to,
           "CSS 2.2 §9.4.2's fill reported a line box holding NO items. \"Line boxes are created as needed to "
           "hold inline-level content\", so a line with nothing on it is not one the section creates — and its "
           "height would be the strut's, added to §10.6.3's total for content that is not there");
    *exists = false;
    for (i = line.from; i < line.to; i++) {
        lxb_dom_element_t *box = text_run_measure_item_style(m, i);

        lb_take(&out, lb_strut_extent(box));
        if (text_run_measure_item_is_text(m, i) || lb_has_nonzero_box_edges(box)) *exists = true;
    }
    return out;
}

CssPx line_box_content_height(lxb_dom_element_t *style, lxb_dom_node_t *first, lxb_dom_node_t *end,
                              bool *any_line_box)
{
    TextRunMeasure m;
    TextRunLine *lines = NULL;
    CssPx height = css_px(0.0), available = css_px(0.0);
    lxb_dom_node_t *c;
    size_t n, i;

    DCHECK(style != NULL && any_line_box != NULL,
           "CSS 2.2 §9.4.2's line boxes were asked for with no element or nowhere to report whether any of "
           "them exists — the second is not an optional out-parameter, it is §8.3.1's own question");
    DCHECK(first == NULL || first->parent == lxb_dom_interface_node(style),
           "the run handed to CSS 2.2 §9.4.2's inline formatting context does not start at a CHILD of the "
           "element whose properties the box has. Those two arguments are the two halves of §9.2.1.1's "
           "anonymous block box — the content is one run of the container's children and the style is the "
           "enclosing non-anonymous box's — so a run from somewhere else would measure one document's line "
           "boxes against another element's font and `line-height`");
    /* §9.4.2's CONTENT, COLLECTED BEFORE ANY OF IT IS MEASURED, which [UAX14] forces rather than anyone
       choosing: its rules read forward past the boundary they decide, so no per-character state can settle a
       break as the character arrives. core/layout/text_run.h states it in full. */
    text_run_measure_init(&m);
    for (c = first; c != NULL && c != end; c = c->next) lb_child(&m, style, c);
    DCHECK(c == end,
           "the half-open run handed to CSS 2.2 §9.4.2's inline formatting context ran off the end of the "
           "child list without ever meeting its exclusive end, so the two came from different lists or the "
           "tree changed under the walk — and the boxes just collected are some prefix of a formatting context "
           "no box has");
    text_run_measure_finish(&m);
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
    if (text_run_measure_splits(&m)) available = used_value_content_px(style, false);
    n = text_run_measure_fill(&m, available, &lines);
    DCHECK(n == 0 || lines != NULL, "CSS 2.2 §9.4.2's fill reported line boxes and handed back none of them");
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
