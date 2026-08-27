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

/* css-sizing-3 §2.2 "Intrinsic Contributions"' OUTER SIZE, for the one box type this walk descends into. The
   section states both contributions as "based on the OUTER SIZE of the box; for this purpose auto margins are
   treated as zero" — so an inline box's own horizontal margins, borders and padding are part of what it
   contributes, and css-text-3 §5.5 "Line Breaking Details" is what says WHERE: "inline box boundaries do not
   introduce a forced line break or soft wrap opportunity in the flow", so those edges sit at the box's two
   boundaries and NOT at every break inside it. A box whose text spans three lines puts its left edge on the
   first and its right edge on the third, and adding either to the whole sum would report a min-content size no
   line of the run ever has.
   THAT PLACEMENT IS THE CAPABILITY THIS FILE DOES NOT HAVE, so a non-zero edge CRASHES here rather than being
   summed anywhere. The accumulator holds CHARACTERS — core/layout/text_run.h's `TextRunChar` is a code point,
   its inline box, and two booleans — and [UAX14] runs over exactly those code points, so there is no way to
   put a WIDTH into the run at a position without either inventing a character (which changes the breaking, and
   §5.5 says a box boundary must not) or letting the character indices and the break-action indices come apart.
   `auto` IS ZERO BY THE SPEC'S OWN SENTENCE and not by this file's convenience — §2.2's "for this purpose auto
   margins are treated as zero", which CSS 2.2 §10.3.1 "Inline, non-replaced elements" states again for the used
   value ("a computed value of 'auto' for 'margin-left' or 'margin-right' becomes a used value of '0'").
   A PERCENTAGE IS NOT RESOLVED HERE, DELIBERATELY, and that is a cycle rather than a gap in the model: an
   intrinsic size is what CSS 2.2 §10.3.5's shrink-to-fit consumes to DECIDE the containing block width, and a
   percentage margin resolves against that same width — so resolving one here would ask for the answer this
   walk is being run to produce. It leaves through the same crash. */
static void is_require_zero_inline_edges(lxb_dom_element_t *el)
{
    static const char *const EDGE[] = {
        "margin-left", "margin-right", "padding-left", "padding-right",
        "border-left-width", "border-right-width",
    };
    unsigned i;

    for (i = 0; i < sizeof(EDGE) / sizeof(EDGE[0]); i++) {
        CssLength len = css_computed_length(el, EDGE[i]);

        if (len.kind == CSS_LENGTH_KEYWORD && strcmp(len.keyword, "auto") == 0) continue;
        if (len.kind == CSS_LENGTH_ABSOLUTE && len.px.px == 0.0) continue;
        DFAIL("css-sizing-3 §2.2 \"Intrinsic Contributions\" puts this INLINE BOX's own horizontal margin, "
              "border or padding into what it contributes to its container's intrinsic inline sizes — the "
              "contributions \"are based on the OUTER SIZE of the box\" — and css-text-3 §5.5 \"Line Breaking "
              "Details\" places those edges at the box's TWO BOUNDARIES rather than at every break inside it, "
              "because \"inline box boundaries do not introduce a forced line break or soft wrap opportunity in "
              "the flow\". So the edge is a WIDTH AT A POSITION IN THE RUN, and core/layout/text_run.h's "
              "accumulator has no such thing: it holds code points, and [UAX14] runs over exactly the code "
              "points it holds, so an edge added as a character would change the breaking §5.5 forbids it from "
              "changing, and an edge added beside the collection would put the character indices and the "
              "break-action indices into two different coordinate systems. BUILD the run out of ITEMS rather "
              "than characters — a text item and a box-edge item, the second carrying an inline size and "
              "invisible to `line_break_actions` while visible to the per-line sum — which is the same "
              "structure CSS 2.2 §9.4.2's line boxes need in order to say which line an EMPTY inline box is "
              "on. A PERCENTAGE leaves through this crash too and is not a second gap: it resolves against the "
              "containing block width, which for a shrink-to-fit box IS what these sizes are being measured to "
              "decide, so it is a cycle that §5.2's own definition breaks and not an arithmetic this file is "
              "missing");
        return;
    }
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
    /* css-display §3 "Box Generation": `display: none` "turns off the display of an element so that it has no "
       effect on layout at all", so there is no box to contribute. This is the one element case that is
       genuinely nothing rather than something unbuilt, which is why it is not a crash. */
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
                  "opportunity before and after each replaced element or other atomic inline\", so it is a "
                  "break this accumulator's [UAX14] pass cannot see, since that pass runs over code points and "
                  "this box is not one. BUILD the item-based run this file's edge crash names — a box item "
                  "carrying an inline size and its own two break opportunities — then §10.3.2's used width "
                  "into it");
        /* §5.5: "inline box boundaries do not introduce a forced line break or soft wrap opportunity in the
           flow", and css-text-3 §4.1.1's collapsing crosses the boundary too ("even one outside the boundary
           of the inline containing that space, provided both spaces are within the same inline formatting
           context"). So an inline box's text is not a second run beside this one — it IS this run, and the
           descent adds it in document order exactly as a text child of `parent` would be. Each character
           carries its OWN element as its style, which is what makes css-text-3 §5.5's per-element questions
           (the advance measure, and the `white-space` at a boundary) answerable per character afterwards. */
        is_require_zero_inline_edges(el);
        is_walk(m, el);
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
                  "cell minima and maxima over the box structure §17.2's anonymous table-object generation "
                  "builds. BUILD the one this `display` names; each needs its own box tree first and none of "
                  "them is this walk with a different accumulator");
    }
    text_run_measure_init(&m);
    n = lxb_dom_interface_node(el);
    for (c = n->first_child; c != NULL; c = c->next) is_child(&m, el, c);
    /* THE MEASUREMENT DOES NOT EXIST UNTIL THIS RUNS, and that is [UAX14]'s doing rather than a lifecycle
       anybody chose: its rules read forward past the boundary they decide (LB25's `PO × OP IS NU` by three
       characters) and LB9 puts an unbounded run of combining marks between the two, so no per-character state
       can settle a break as the character arrives. core/layout/text_run.h states it in full. This call also
       releases the characters the walk collected, so the walk above and this line are one operation. */
    text_run_measure_finish(&m);
    out.min_content = text_run_measure_min_content(&m);
    out.max_content = text_run_measure_max_content(&m);
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
