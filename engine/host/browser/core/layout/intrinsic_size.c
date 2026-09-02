/* css-sizing-3 §5.1's intrinsic inline sizes over a box's own content. See intrinsic_size.h for why §5.1 is a
   definition and CSS 2.2 §10.3.5 is the operation, for the split with core/layout/text_run.h, and for which box
   this walk is the algorithm of. */
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include <lexbor/dom/dom.h>

#include "check.h"
#include "core/css/css_computed_value.h"
#include "core/css/css_length.h"
#include "core/layout/block_flow.h"
#include "core/layout/intrinsic_size.h"
#include "core/layout/phrasing_break.h"
#include "core/layout/replaced_element.h"
#include "core/layout/text_run.h"

static char *is_computed(lxb_dom_element_t *el, const char *name)
{
    char *v = css_computed_value(el, name);

    DCHECK(v != NULL, "the cascade produced no computed value for a property this engine models — every one of "
                      "them is in lexbor's registry with an initial value, so the last layer always answers");
    return v;
}

static bool is_computed_is(lxb_dom_element_t *el, const char *name, const char *kw)
{
    char *v = is_computed(el, name);
    bool same = strcmp(v, kw) == 0;

    free(v);
    return same;
}

/* css-sizing-3 §2.2's outer size at one side of an inline box — see intrinsic_size.h for the contract, for why
   it is exported, and for why a percentage is a CYCLE rather than a gap.
   `auto` IS ZERO BY THE SPEC'S OWN SENTENCE and not by this file's convenience — §2.2's "for this purpose auto
   margins are treated as zero", which CSS 2.2 §10.3.1 "Inline, non-replaced elements" states again for the used
   value ("a computed value of 'auto' for 'margin-left' or 'margin-right' becomes a used value of '0'"). */
CssPx intrinsic_inline_box_edge_px(lxb_dom_element_t *el, bool trailing)
{
    const char *const NAME[2][3] = {
        { "margin-left",  "padding-left",  "border-left-width"  },
        { "margin-right", "padding-right", "border-right-width" },
    };
    CssPx sum = css_px(0.0);
    unsigned i;

    for (i = 0; i < 3; i++) {
        CssLength len = css_computed_length(el, NAME[trailing ? 1 : 0][i]);

        /* §2.2's auto-margin sentence. `auto` is the only keyword any of the six can compute to: a padding and
           a border width have no keyword in their `Value:` lines at all, so a keyword that is not `auto` is a
           cascade that produced a value the property does not have and leaves through the crash below. */
        if (len.kind == CSS_LENGTH_KEYWORD && strcmp(len.keyword, "auto") == 0) continue;
        if (len.kind == CSS_LENGTH_ABSOLUTE) { sum = css_px_add(sum, len.px); continue; }
        DFAIL("css-sizing-3 §2.2 \"Intrinsic Contributions\" needs this INLINE BOX's own horizontal margin, "
              "border or padding — the contributions \"are based on the OUTER SIZE of the box\" — and the "
              "cascade answered with a PERCENTAGE or a calculation rather than an absolute length. A "
              "percentage on any of the six resolves against the CONTAINING BLOCK WIDTH, and for the "
              "shrink-to-fit box these sizes are being measured for, that width is CSS 2.2 §10.3.5's own "
              "output — so resolving it here would ask for the answer this walk exists to produce. That cycle "
              "is css-sizing-3's to break and this engine has not built its resolution; a zero substituted "
              "here would under-report every percentage-padded inline box's contribution with nothing "
              "downstream able to contradict it. BUILD css-sizing-3's percentage resolution against an "
              "indefinite containing block, then this arm is a read of it");
        return sum;
    }
    return sum;
}

/* ONE CHILD NODE of the box whose intrinsic sizes are being measured. The shape mirrors core/layout/
   line_box.c's own child walk deliberately: both are iterating the SAME inline formatting context, and the two
   answering differently about which children are in it would be one classification with two copies. */
static void is_child(TextRunMeasure *m, lxb_dom_element_t *parent, lxb_dom_node_t *n);

static void is_walk(TextRunMeasure *m, lxb_dom_element_t *el)
{
    lxb_dom_node_t *n = lxb_dom_interface_node(el), *c;

    for (c = n->first_child; c != NULL; c = c->next) is_child(m, el, c);
}

static void is_child(TextRunMeasure *m, lxb_dom_element_t *parent, lxb_dom_node_t *n)
{
    lxb_dom_element_t *el;
    char *d;
    bool inline_box;

    switch (n->type) {
    case LXB_DOM_NODE_TYPE_TEXT:
        /* CSS 2.2 §9.2.2.1 "Anonymous inline boxes" first: a run of white space this element's `white-space`
           collapses away "does not generate any anonymous inline boxes", so it is not content and contributes
           nothing. core/layout/block_flow.h answers that for every walk over a block container's children, and
           asking it here rather than re-deriving it is what keeps this walk and §9.4.1's agreeing about what a
           document's white space is. */
        if (block_flow_text_child_generates_box(parent, n)) text_run_measure_add_text(m, parent, n);
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
              "measured — the tree this walk iterates holds elements, text, comments, processing instructions "
              "and a doctype, and a CDATA section, a document or a fragment is not a child any parser this "
              "engine runs produces there. Find the writer that inserted it");
        return;
    }
    el = lxb_dom_interface_element(n);
    /* css-display-3 §2.5 "Box Generation: the none and contents keywords" on `display: none`: "The element and
       its descendants generate no boxes or text sequences." So there is no box to contribute. This is the one
       element case that is nothing rather than something unbuilt, which is why it is not a crash.
       WHAT STOOD HERE cited §3 "Box Generation" and quoted `none` as turning "off the display of an element so
       that it has no effect on layout at all" — a sentence that appears NOWHERE in css-display-3, beside a
       number that is "Display Order: the order property". Box generation is §2.5 and always has been. */
    if (is_computed_is(el, "display", "none")) return;
    /* OUT OF FLOW CONTRIBUTES NOTHING, and css-text-3 §5.5 confirms it introduces no break either ("out-of-flow
       boxes and inline box boundaries do not introduce a forced line break or soft wrap opportunity in the
       flow"). CSS 2.2 §9.3.1 takes an absolutely positioned box out of normal flow, so it is not on the line
       whose size is being measured and its own text is not in this run. core/layout/line_box.c returns at the
       same test for the same reason, which is why the two walks still agree about what is in the context. */
    if (is_computed_is(el, "position", "absolute") || is_computed_is(el, "position", "fixed")) return;
    if (!is_computed_is(el, "float", "none"))
        DFAIL("CSS 2.2 §9.5 \"Floats\" takes this child out of the line, and css-sizing-3 §5.2 \"Intrinsic "
              "Contributions\" still counts it: CSS 2.2 §10.3.5's preferred width is the box \"formatted "
              "without breaking lines other than where explicit line breaks occur\", and a float shortens the "
              "line boxes beside it rather than leaving them alone — §9.4.2's own \"line boxes may vary in "
              "width if available horizontal space is reduced due to floats\". So the contribution is neither "
              "this walk's sum along the line nor a maximum over it, and there is no arm here that is right by "
              "default. BUILD §9.5.1's float placement, which core/layout/line_box.c and "
              "core/layout/flow_position.c both name as the same absent capability");
    d = is_computed(el, "display");
    inline_box = strcmp(d, "inline") == 0;
    /* FREED BEFORE EITHER CRASH AND READ AGAIN BY NEITHER, for the reason `tr_wraps` states: a DFAIL is
       compiled out in release and control falls through it, so a `free` on each side of one is a double free
       that only the release build reaches. */
    free(d);
    if (inline_box) {
        /* HTML §15.3.4 "Phrasing content" FIRST, because what it declares changes the PARTITION and not a term
           in the sum. css-sizing-3 §2.1's max-content inline size is CSS 2.2 §10.3.5's content "formatted
           without breaking lines OTHER THAN WHERE EXPLICIT LINE BREAKS OCCUR", so a `br` this walk did not see
           would not be a small error in the answer — it would report a whole paragraph's width as the width of
           its longest line. core/layout/line_box.c's walk over the same children asks the same component the
           same question, which is what keeps the two from disagreeing about how many lines a run has. */
        switch (phrasing_break_of(el)) {
        case PHRASING_BREAK_FORCED:
            text_run_measure_add_forced_break(m, el);
            return;
        case PHRASING_BREAK_OPPORTUNITY:
            DFAIL("HTML §15.3.4 \"Phrasing content\" gives `wbr` the UA declaration `display-outside: "
                  "break-opportunity`, which puts a SOFT WRAP OPPORTUNITY inside this run — and css-sizing-3 "
                  "§2.1 defines the MIN-CONTENT inline size as the size \"if ALL soft wrap opportunities "
                  "within the box were taken\", so an opportunity this walk cannot see reports a `<wbr>`-split "
                  "word as one unbreakable segment and gives CSS 2.2 §10.3.5's shrink-to-fit a preferred "
                  "minimum width no line of this run has. It leaves the max-content answer alone, since §2.1 "
                  "takes NONE of them — which is why this is one absent capability with two different wrong "
                  "answers rather than one. THE ITEM KIND IS NO LONGER WHAT IS MISSING: text_run.h carries "
                  "`br`'s FORCED break, collected as the U+000A css-text-3 §4 names so [UAX14] LB5 and LB6 "
                  "decide its boundaries. An opportunity needs a DIFFERENT code point and a different pair of "
                  "rules — U+200B ZERO WIDTH SPACE, class ZW, under LB7 `× ZW` and LB8 `ZW SP* ÷` — and it "
                  "needs §5.5's enabling question answered from the `wbr`'s OWN computed `white-space`, which "
                  "is neither of the two cases `tr_opportunity_enabled` implements. BUILD both in "
                  "core/layout/text_run.c, where core/layout/line_box.c's own `wbr` crash names the same two");
            return;
        case PHRASING_BREAK_NONE:
            break;
        }
        /* CSS 2.2 §9.2.2 makes a REPLACED element an ATOMIC inline-level box wherever its `display` puts it,
           so the `inline` above does not settle which of the two this is. It is asked before the descent
           because an atomic inline has no text of this run inside it to descend to. */
        if (replaced_element_of(el).replaced)
            DFAIL("CSS 2.2 §9.2.2 makes this REPLACED element an ATOMIC INLINE-LEVEL box, which "
                  "\"participate[s] in [its] inline formatting context as a single opaque box\" — so what it "
                  "adds to this run is css-sizing-3 §5.2's outer size over its OWN used inline size (CSS 2.2 "
                  "§10.3.2 \"Inline, replaced elements\" over the natural dimensions "
                  "core/layout/replaced_element.h already answers), not the text of its children. AND IT CUTS "
                  "THE MIN-CONTENT SEGMENT: css-text-3 §5.5 \"Line Breaking Details\" puts \"a soft wrap "
                  "opportunity before and after each replaced element or other atomic inline\". THE ITEM KIND "
                  "IS NO LONGER WHAT IS MISSING: core/layout/text_run.h now carries §5.5's atomic inline as a "
                  "fourth kind — a margin-box inline size at a position plus the U+FFFC whose [UAX14] class CB "
                  "is those two opportunities — and core/layout/line_box.c's walk over the same children "
                  "already emits one for this element. WHAT IS LEFT IS THE OUTER SIZE, AND IT IS A CYCLE "
                  "RATHER THAN AN ABSENCE, which is why this arm cannot simply call what that walk calls. "
                  "`used_value_margin_edge_px` resolves a PERCENTAGE margin or padding against the containing "
                  "block's width, and for a box whose width is CSS 2.2 §10.3.5's shrink-to-fit that width is "
                  "THE ANSWER THIS WALK IS BEING RUN TO PRODUCE — so the call would not crash, it would "
                  "recurse. `intrinsic_inline_box_edge_px` is the same six lengths with that percentage "
                  "REFUSED, and it is refused there for exactly this reason. BUILD the outer size out of it: "
                  "the leading edge, plus §10.3.2's used CONTENT width (`used_value_content_px`, whose arms "
                  "over natural dimensions consult no containing-block width — its ratio arm reads the used "
                  "HEIGHT, a different axis, and its undefined arm crashes), plus the trailing edge. That is "
                  "ONE derivation of the same number line_box.c reads whole, differing only in refusing the "
                  "input that would make it self-referential, and the refusal is the point");
        /* §5.5: "inline box boundaries do not introduce a forced line break or soft wrap opportunity in the
           flow", and css-text-3 §4.1.1's collapsing crosses the boundary too ("even one outside the boundary
           of the inline containing that space, provided both spaces are within the same inline formatting
           context"). So an inline box's text is not a second run beside this one — it IS this run, and the
           descent adds it in document order exactly as a text child of `parent` would be. Each character
           carries its OWN element as its style, which is what makes css-text-3 §5.5's per-element questions
           (the advance measure, and the `white-space` at a boundary) answerable per character afterwards. */
        /* THE TWO EDGES BRACKET THE DESCENT, which is the whole of §2.2's placement: emitted in call order, so
           the opening edge precedes the box's content in the run and the closing edge follows it, and the
           run's own break-position mapping is what then puts each on the line its fragment is on. They are
           emitted even when both are ZERO, because an edge occupies a POSITION and that position is what says
           which line an otherwise-empty inline box sits on. */
        text_run_measure_add_box_edge(m, el, intrinsic_inline_box_edge_px(el, false));
        is_walk(m, el);
        text_run_measure_add_box_edge(m, el, intrinsic_inline_box_edge_px(el, true));
        return;
    }
    DFAIL("css-sizing-3 §5.2 \"Intrinsic Contributions\" is what an ELEMENT CHILD adds to this box's intrinsic "
          "inline sizes, and it is a different quantity from the text beside it: §2.2 defines the max-content "
          "contribution as \"the size that a box contributes to its containing block's max-content size\" and "
          "says both contributions \"are based on the OUTER SIZE of the box; for this purpose auto margins are "
          "treated as zero\" — so a child's own margins, borders and padding are in the number, and its own "
          "intrinsic sizes are inside that. WHICH ALGORITHM COMBINES THEM DEPENDS ON THE FORMATTING CONTEXT "
          "THIS BOX ESTABLISHES, and the two do not share a step: CSS 2.2 §9.4.2's INLINE formatting context "
          "flows the child ALONG the line, so its contribution is ADDED to the run's (and css-text-3 §5.5 "
          "\"Line Breaking Details\" puts \"a soft wrap opportunity before and after each replaced element or "
          "other ATOMIC INLINE\", which cuts the min-content segment there), while §9.4.1's BLOCK formatting "
          "context stacks the child DOWN a column, so this box's intrinsic sizes are the MAXIMUM over its "
          "in-flow children's contributions and not a sum at all. §9.2.1.1 \"Anonymous block boxes\" is what "
          "decides which — a container holding both gets an anonymous block box per run of inline-level "
          "children — and core/layout/block_flow.c already delimits exactly those runs for the height walk. "
          "BUILD the contribution in that order: take block_flow.c's own child classification and its run "
          "delimitation as the thing THIS walk iterates too (so one list answers for both), then §5.2's outer "
          "size over a child whose used inline size is itself an intrinsic one — which is this same entry, one "
          "level down. THE THIRD ARM, a non-replaced `display: inline` box, IS BUILT and is no longer part of "
          "what this names: its text joins the SAME run this walk accumulates, which is what §4.1.1's "
          "boundary-crossing collapsing and §5.5's \"inline box boundaries do not introduce a forced line break "
          "or soft wrap opportunity in the flow\" together make correct. What that arm still crashes for is its "
          "own horizontal edges, and that crash names the item-based run both it and CSS 2.2 §9.4.2's line "
          "boxes need");
}

IntrinsicInlineSizes intrinsic_inline_sizes(lxb_dom_element_t *el)
{
    TextRunMeasure m;
    IntrinsicInlineSizes out;
    lxb_dom_node_t *n, *c;

    DCHECK(el != NULL, "css-sizing-3 §5.1's intrinsic inline sizes were asked for with no element");
    /* THE REPLACED QUESTION IS ASKED FIRST, exactly as CSS 2.2 §10.3 asks it before the box type: a replaced
       element's intrinsic sizes come from its NATURAL DIMENSIONS and not from its children, which for most of
       them it has none of in the DOM at all. Answering it from this walk would report an `img` as being as wide
       as the text of its `alt` attribute is not. */
    if (replaced_element_of(el).replaced)
        DFAIL("css-sizing-3 §5.1 \"Intrinsic Sizes\" gives a REPLACED ELEMENT its own rules, and this walk over "
              "children is not them: the section defines the intrinsic sizes \"of replaced elements WITHOUT "
              "NATURAL SIZES\" from the preferred aspect ratio and the available space, and one WITH a natural "
              "size takes it — CSS 2.2 §10.3.2 \"Inline, replaced elements\"' first arm, \"if 'height' and "
              "'width' both have computed values of 'auto' and the element also has an intrinsic width, then "
              "that intrinsic width is the used value\". The natural dimensions are already answered "
              "(core/layout/replaced_element.h) and core/layout/used_value.c already runs §10.3.2 over them, so "
              "what is missing is only the wiring: make the min-content and max-content sizes of a replaced box "
              "§10.3.2's used width, with §5.1's zero-min-content arm for one that has a ratio and no natural "
              "size");
    /* §9.4.2's OWN CONDITION, asked before anything is measured: "an inline formatting context is established "
       by a block container box that CONTAINS NO BLOCK-LEVEL BOXES". A box that is not a block container at all
       does not establish either of CSS 2.2's two formatting contexts, so neither algorithm applies to it. */
    {
        char *d = is_computed(el, "display");
        bool container = block_flow_display_is_block_container(d);

        free(d);
        if (!container)
            DFAIL("css-sizing-3 §5.1's intrinsic inline sizes were asked of a box that is NOT A BLOCK CONTAINER, "
                  "so neither of CSS 2.2 §9.4's two formatting contexts is what lays its content out and "
                  "neither §9.4.2's line boxes nor §9.4.1's stack is the algorithm. Which module owns it is its "
                  "own `display`: css-flexbox-1 §9.9 \"Intrinsic Sizes\" derives a flex container's from its "
                  "flex lines, css-grid-2 §11.5 sizes the TRACKS and the container's intrinsic size follows "
                  "from them, and CSS 2.2 §17.5.2's automatic table layout derives a table's from its COLUMNS' "
                  "cell minima and maxima over the box structure §17.2.1 Anonymous table objects generates — "
                  "which core/layout/table_box.h answers, so a table's remaining input is §17.5 Visual layout "
                  "of table contents' grid: which column each cell occupies and how many it spans. BUILD the "
                  "one this `display` names; the flex and grid arms each need their own box tree first and "
                  "none of the three is this walk with a different accumulator");
    }
    text_run_measure_init(&m);
    n = lxb_dom_interface_node(el);
    for (c = n->first_child; c != NULL; c = c->next) is_child(&m, el, c);
    /* THE MEASUREMENT DOES NOT EXIST UNTIL THIS RUNS, and that is [UAX14]'s doing rather than a lifecycle
       anybody chose: its rules read forward past the boundary they decide (LB25's `PO × OP IS NU` by three
       characters) and LB9 puts an unbounded run of combining marks between the two, so no per-character state
       can settle a break as the character arrives. core/layout/text_run.h states it in full. It RETAINS what
       the walk collected — CSS 2.2 §9.4.2's fill is a third partition over the same [UAX14] pass — so the
       release below is this function's to make and the two numbers are read between them. */
    text_run_measure_finish(&m);
    out.min_content = text_run_measure_min_content(&m);
    out.max_content = text_run_measure_max_content(&m);
    /* §5.1's answer is a PAIR OF NUMBERS and not a view onto the run, so the measurement ends here: this
       component asks §9.4.2 nothing, and a caller of it holds no items to be handed. */
    text_run_measure_release(&m);
    /* THE TWO ARE NON-NEGATIVE because every advance summed into them is, and a negative intrinsic size would
       make CSS 2.2 §10.3.5's formula produce a negative used width for a box with content in it — which
       css-sizing-3 §3.3's "as the content width and height cannot be negative, this computation is floored at
       zero" would then hide rather than the derivation being fixed. Asserted here, at the boundary where the
       two numbers leave this component, so a caller never has to floor them. */
    DCHECK(out.min_content.px >= 0.0 && out.max_content.px >= 0.0,
           "css-sizing-3 §5.1's intrinsic inline sizes came out NEGATIVE. Each is a sum of advance measures, "
           "every one of which is a non-negative OpenType 'hmtx' advanceWidth times a non-negative computed "
           "`font-size`, so a negative here is arithmetic that lost a sign rather than a document");
    return out;
}
