/* CSSOM VIEW §2 "Terminology"'s SCROLLING AREA, for an element. See scrolling_area.h for why the term is a
   component, for the shape §2's four-row table shares, and for why an extent cannot stand in for it. */
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include <lexbor/dom/dom.h>

#include "check.h"
#include "core/css/css_computed_value.h"
#include "core/css/css_length.h"
#include "core/dom/document.h"
#include "core/dom/element_view.h"
#include "core/layout/block_flow.h"
#include "core/layout/box_subject.h"
#include "core/layout/flow_position.h"
#include "core/layout/line_box.h"
#include "core/layout/replaced_element.h"
#include "core/layout/scrolling_area.h"
#include "core/layout/used_value.h"

/* THE SUBJECT OF EVERY CRASH IN THIS FILE, AND IT IS NOT ALWAYS `el`.
   §AN-ASSERT-THAT-NAMES-A-REMEDY's test is to count the call sites that can reach an abort. Measured by reverse
   reachability over every `.c` file under engine/host: `sa_descendants_extreme` is reached from 19 call sites
   over 13 functions in 4 files and `sa_computed_is` from 33 over 18, the callers outside core/layout being
   CSSOM VIEW §6's element members, §7's scroll algorithms and core/frame/viewport.c's own row. So the ADDRESS
   is part of these asserts rather than decoration on them.
   AND THE COUNT UNDERSTATES IT FOR THE SAME REASON core/layout/flow_position.c's does: the walk below runs ONCE
   PER BOX IN A SUBTREE, and for §2's VIEWPORT row that subtree is the document element's — every box in the
   document. core/dom/element_view.c's `ev_scroll_extent` takes the viewport row BEFORE its step 6 has-a-box
   guard, so one `element.scrollWidth` measures every box there is and a frame list names only the member that
   asked.
   WHICH ADDRESS IS THE QUESTION, AND THE ANSWER IS NOT `__FILE__`/`__LINE__` HERE EITHER. A threaded site
   arrives through `sa_descendant_edge`, `sa_inline_box_edge` and `sa_fold_span` — a handful of forwarding
   functions for the whole tree, the capture point that rule names as the wrong one. It would also be
   REDUNDANT in a way it is not in most files: the two callers a reader of `sa_fold_span`'s abort has to tell
   apart are `sa_inline_context_extreme` and `sa_anon_block_boxes_extreme`, two DISTINCT FUNCTIONS a symbolized
   frame list already names, whereas the box being folded appears in no frame list at all. The asker is the
   half an instrument answers; the subject is the half that gets guessed.
   SO THE SUBJECT TRAVELS, AND WHICH SUBJECT IS PER ASSERT RATHER THAN PER FILE. `sa_excluded`'s is the
   DESCENDANT and not the element whose area is being measured; `sa_fold_span`'s is the box that ESTABLISHES
   the context, beside the two coordinates it reported; the text-run abort's is the text node's PARENT. Naming
   `el` at any of those would read as authoritative and be wrong, which is worse than naming nothing.
   THE COMPOSITION ITSELF IS core/layout/box_subject.h's, which is where the argument that it must be TOTAL now
   lives, together with the ownership contract and the `%.*s` rule. Two things this file owes that component:
   `box_subject_computed` is reached instead of `sa_computed_is` below, whose own DCHECK would be exactly that
   replacement; and `replaced_element_of` is reached from no message here, which matters most at the text-run
   abort — that function ABORTS by name for an `embed`, a `video`, a `canvas`, an `object`, an `audio` and an
   `input`, and a REPLACED INLINE parent (`<object>x</object>`) is one of the cases that abort exists to tell
   apart, so asking it would report core/layout/replaced_element.c's gap for precisely the case whose own gap is
   being reported.
   THE SITES THAT NAME A NODE rather than a box are the ones that DELIMIT A RUN: §2's extreme is over "all of
   the element's descendants' boxes" and the walk below descends through TEXT nodes as well as elements — a
   text run's anonymous inline box is one of those descendants — so `box_subject_node` is what they ask and
   "(no element)" would be a wrong answer there rather than a missing one. */
static bool sa_computed_is(lxb_dom_element_t *el, const char *name, const char *kw)
{
    char *v = css_computed_value(el, name);
    char nbuf[160];
    bool same;

    DCHECKF(v != NULL,
            "%s, property `%s`: "
            "the cascade produced no computed value for a property this engine models — every one of "
            "them is in lexbor's registry with an initial value, so the last layer always answers",
            box_subject(el, nbuf, sizeof nbuf), name);
    same = strcmp(v, kw) == 0;
    free(v);
    return same;
}

/* CSS 2.1 §9.2.1 "Block-level elements and block boxes"' BLOCK CONTAINER BOX, for this file's one caller of it
   — core/layout/block_flow.h owns the list and is asked, so the two cannot disagree about whether an
   `inline-block` is one. It is a separate question from `block_flow_establishes_inline_context`, which is that
   list AND §9.4.2's own condition over the child list: a MIXED container answers FALSE there and TRUE here,
   and telling those apart is exactly what the text-run check below needs. */
static bool sa_is_block_container(lxb_dom_element_t *el)
{
    char *d = css_computed_value(el, "display");
    char nbuf[160];
    bool container;

    DCHECKF(d != NULL, "%s: the cascade produced no computed `display` for an element — the UA layer answers "
                       "`inline` for every element it does not name, so this cannot be unset",
                       box_subject(el, nbuf, sizeof nbuf));
    container = block_flow_display_is_block_container(d);
    free(d);
    return container;
}

/* §2's OVERFLOW DIRECTIONS, reduced to the one bit each axis needs: does the ENDING edge of the scrolling area
   sit at the larger coordinate on this axis. "A scrolling box of a viewport or element has two overflow
   directions, which are the block-end and inline-end directions for that viewport or element", and
   css-writing-modes-4 §6.2 "Flow-relative Directions" derives those: block-end is "the side opposite
   block-start", which in `horizontal-tb` mode is the physical bottom, and inline-end is "the side opposite
   start" — the line-right side for a used `direction` of `ltr` and the line-left side for `rtl`.
   ONE PROPERTY DECIDES EACH AXIS HERE, and that is a fact about `horizontal-tb` rather than a general rule:
   the block axis is the vertical one and the inline axis the horizontal one only in a horizontal writing mode,
   which is why the assertion below is what lets this function take the axis rather than the abstract
   dimension. §6.2 states the split in so many words — "while determining the block-start and block-end sides
   of a box depends only on the writing-mode property, determining the inline-start and inline-end sides of a
   box depends not only on the writing-mode property but also the direction property". */
bool scrolling_area_ending_edge_at_higher_coordinate(lxb_dom_element_t *el, bool vertical)
{
    bool horizontal_tb = sa_computed_is(el, "writing-mode", "horizontal-tb");
    char nbuf[160], vbuf[64];

    DCHECKF(horizontal_tb,
           "%s, computed `writing-mode` `%s`: "
           "CSSOM VIEW §2's overflow directions were mapped onto the physical axes for a box whose computed "
           "`writing-mode` is not `horizontal-tb`, where the block axis is not the vertical one. The element's "
           "own box was placed by core/layout/flow_position.c, which crashes for exactly that value before any "
           "coordinate exists to measure — so this element has a position and a writing mode that disagree, and "
           "the two tests have come apart",
           box_subject(el, nbuf, sizeof nbuf), box_subject_computed(el, "writing-mode", vbuf, sizeof vbuf));
    if (vertical) return true;   /* block-end is downward in every horizontal writing mode */
    /* inline-end is the line-right side for a used `direction` of `ltr` and the line-left side for `rtl`, and
       the property admits no third value — asserted here rather than read as "not rtl", so a spelling the
       cascade should have refused crashes instead of quietly meaning `ltr`. */
    /* THE VALUE THE CASCADE ACTUALLY ANSWERED IS THE WHOLE OF WHAT THIS ONE HAD TO SAY AND DID NOT. Its
       sentence is that the value is neither of the two the property admits, and a reader can only act on it by
       learning WHICH third value arrived — so the message stated the negation of the fact it was standing on
       top of. */
    DCHECKF(sa_computed_is(el, "direction", "ltr") || sa_computed_is(el, "direction", "rtl"),
           "%s, computed `direction` `%s`: "
           "a computed `direction` is neither `ltr` nor `rtl`. css-writing-modes-4 §2.1 \"Specifying "
           "Directionality: the direction property\" gives the property the `Value:` line `ltr | rtl` and "
           "nothing else",
           box_subject(el, nbuf, sizeof nbuf), box_subject_computed(el, "direction", vbuf, sizeof vbuf));
    return !sa_computed_is(el, "direction", "rtl");
}

/* §2's "excluding boxes that have an ancestor of the element as their containing block". A box laid out
   against a rectangle that belongs to an ANCESTOR of this element does not move when this element scrolls, so
   it is not in this element's scrolling area however deep in the tree it sits — an absolutely positioned
   descendant whose nearest positioned ancestor is above `el` is the case the sentence is written for. CSS 2.1
   §10.1's four cases are core/layout/used_value.h's and are asked, never re-derived here.
   THIS IS THE ELEMENT COLUMN'S CLAUSE AND THE VIEWPORT COLUMN HAS NONE, which is why the walk below takes an
   exclusion ANCHOR and not a flag: the clause is stated over "an ancestor of THE ELEMENT", a viewport has no
   ancestor, and §2 writes no such sentence in the viewport half of any of its four rows. */
static bool sa_excluded(lxb_dom_element_t *el, lxb_dom_element_t *descendant)
{
    lxb_dom_element_t *cb = used_value_containing_block(descendant);
    const lxb_dom_node_t *cbn, *a;
    char dbuf[160], ebuf[160];

    /* THE SUBJECT HERE IS THE DESCENDANT AND NOT `el`, which is the one thing about this abort a reader cannot
       reconstruct: `el` is the element whose scrolling area is being measured and is named by the caller a
       frame list already gives, while the descendant is one of however many boxes the walk reached. The
       element is named BESIDE it because the claim being contradicted is that the second is strictly below the
       first, and both halves of that are needed to check it. */
    DCHECKF(cb != NULL,
           "descendant %s, of %s: "
           "§10.1's FIRST case — the initial containing block — was answered for a strict DESCENDANT of an "
           "element, and that case is the ROOT ELEMENT's alone. A descendant of any element has a root element "
           "above it, so this is the root test and the tree's own shape having come apart",
           box_subject(descendant, dbuf, sizeof dbuf), box_subject(el, ebuf, sizeof ebuf));
    cbn = lxb_dom_interface_node(cb);
    /* An ANCESTOR of the element, which is strictly above it: a box whose containing block is the ELEMENT
       ITSELF is in the scrolling area, so the walk starts at the element's parent. */
    for (a = lxb_dom_interface_node(el)->parent; a != NULL; a = a->parent)
        if (a == cbn) return true;
    return false;
}

/* CSS 2.2 §10.3.1 "Inline, non-replaced elements" and §10.6.1 "Inline, non-replaced elements"' OWN BOX — the
   one shape of descendant whose margin edge is not an origin plus an extent, so it is asked about BEFORE the
   composition below is attempted rather than inside it.
   BOTH HALVES OF THE NAME ARE THE TEST AND THE SECOND IS NOT DECORATION. §10.3.2 "Inline, replaced elements"
   gives an inline `img` a real used `width`, and §8.3's `Applies to:` exception is written over "non-replaced
   inline elements" alone — so a replaced inline HAS the extent `sa_descendant_edge` composes and takes that
   path, while a non-replaced one has none and would abort inside core/layout/used_value.c's applicability
   assert, naming CSSOM §9 and core/css/css_property_applies.c for a question this file asked. */
static bool sa_is_non_replaced_inline(lxb_dom_element_t *el)
{
    return sa_computed_is(el, "display", "inline") && !replaced_element_of(el).replaced;
}

/* ONE DESCENDANT'S CONTRIBUTION — the margin edge §2's table names on the ENDING side of this axis. A margin
   edge is CSS 2 §8.1's outermost edge, so it is the placed BORDER box's origin moved outward by the margin on
   that side. Where the ENDING edge is at the higher coordinate that is the far border edge plus the
   trailing margin, and where it is at the lower one it is the origin less the leading margin.
   IT IS ONE ORIGIN AND ONE EXTENT, SO IT IS ASKED ONLY OF A BOX THAT HAS BOTH — see `sa_is_non_replaced_inline`
   above and `sa_inline_box_edge` below for the box that has neither. */
static CssPx sa_descendant_edge(lxb_dom_element_t *d, bool vertical, bool ending_at_hi)
{
    FlowPoint o = flow_border_box_origin(d);
    CssPx origin = vertical ? o.y : o.x;

    if (!ending_at_hi)
        return css_px_sub(origin, used_value_px(d, vertical ? "margin-top" : "margin-left"));
    return css_px_add(css_px_add(origin, used_value_border_edge_px(d, vertical)),
                      used_value_px(d, vertical ? "margin-bottom" : "margin-right"));
}

/* CSS 2 §8.1's CONTENT BOX ORIGIN on one axis — the placed padding box moved inward by the leading padding.
   It is the rectangle CSS 2.2 §9.4.2 gives a block container's LINE BOXES ("the width of a line box is
   determined by a containing block", and §10.1's second case makes that the CONTENT edge), so it is the origin
   core/layout/line_box.h's span is measured from and the one place the two components have to agree. */
static CssPx sa_content_origin(lxb_dom_element_t *b, bool vertical)
{
    FlowPoint o = flow_padding_box_origin(b);

    return css_px_add(vertical ? o.y : o.x, used_value_px(b, vertical ? "padding-top" : "padding-left"));
}

/* ONE NON-REPLACED INLINE DESCENDANT'S CONTRIBUTION, which is `sa_descendant_edge`'s question over a box that
   answers neither of its two operands. CSS 2.2 §9.4.2 "Inline formatting contexts" is why: "when an inline box
   exceeds the width of a line box, it is SPLIT into several boxes and these boxes are distributed across
   several line boxes", so this descendant is a SET of border areas rather than one placed rectangle, and
   §10.3.1 and §10.6.1 remove the extent that would have sized a single one ("the 'width' property does not
   apply", "the 'height' property does not apply"). core/layout/line_box.h owns the extreme over that set,
   because which fragment carries which of the box's two margins is §9.4.2's own split sentence and that is the
   file whose fill establishes it.
   THE FRAME IS THE ESTABLISHING BOX'S CONTENT BOX, which is the frame `sa_fold_span` already composes for the
   two shapes of §9.4.2's context above — the same `sa_content_origin`, over whichever block container
   core/layout/line_box.h reports this box's lines belong to, which for a `<span>` nested in inline ancestors is
   not its parent. A MIXED container puts those lines on §9.2.1.1's ANONYMOUS BLOCK BOX, which no element names,
   and that costs this file nothing: line_box.h reports the CONTAINER either way and adds the anonymous box's
   own origin to the coordinates it measures inside it, so the one frame composed here is right for both shapes
   and there is no second origin to hand back. */
static CssPx sa_inline_box_edge(lxb_dom_element_t *d, bool vertical, bool ending_at_hi)
{
    lxb_dom_element_t *establishing = NULL;
    char nbuf[160];
    CssPx lo, hi;

    line_box_inline_margin_span(d, &establishing, vertical, &lo, &hi);
    DCHECKF(establishing != NULL,
           "inline box %s, %s axis, span [%g, %g]: "
           "CSS 2.2 §9.4.2's formatting context was reported as NO BOX for an inline box whose fragments were "
           "measured in it. core/layout/line_box.c walks past every inline ancestor to a block container and "
           "crashes there when it runs out, so a NULL here is that walk having returned without either",
           box_subject(d, nbuf, sizeof nbuf), vertical ? "block" : "inline", lo.px, hi.px);
    return css_px_add(sa_content_origin(establishing, vertical), ending_at_hi ? hi : lo);
}

/* ONE BLOCK CONTAINER'S INLINE FORMATTING CONTEXT folded into the running extreme — the boxes CSS 2.2 §9.2.2.1
   "Anonymous inline boxes" generates for its text runs, which are §2's "descendants' boxes" with no element of
   their own to be placed through.
   IT IS ASKED OF THE ELEMENT THAT ESTABLISHES THE CONTEXT AND NOT OF EACH TEXT NODE, because §9.4.2's
   distribution is a fact about the WHOLE context — "when several inline-level boxes cannot fit horizontally
   within a single line box, they are distributed among two or more vertically-stacked line boxes" — so which
   line a run's fragments land on is a function of every character before them, and a per-node question could
   not be answered at all. core/layout/block_flow.h owns which elements those are.
   BOTH EDGES ARE ASKED FOR AND THE RIGHT ONE IS TAKEN, because the context's own `direction` decides which end
   of its lines the content grows from and `el`'s decides which end of the SCROLLING AREA is the ending one:
   a `rtl` block inside a `ltr` scroller overflows toward the LOWER coordinate, which is `el`'s BEGINNING edge.
   §2's two rows are then one expression rather than a case per combination. */
static CssPx sa_fold_span(lxb_dom_element_t *style, lxb_dom_node_t *first, lxb_dom_node_t *end, CssPx origin,
                          bool vertical, bool ending_at_hi, CssPx best)
{
    char sbuf[160], fbuf[160], ebuf[160];
    CssPx lo, hi;

    line_box_content_span(style, first, end, vertical, &lo, &hi);
    /* THE ESTABLISHING BOX AND THE RUN'S TWO DELIMITERS ARE THE SUBJECT, AND THE TWO COORDINATES ARE THE FACT.
       Which of this function's two callers reached it is already in a frame list — they are distinct functions
       — so the address that is missing is the RUN: `first` and `end` are §9.2.1.1's own delimiters and are as
       often text nodes as elements, and an inverted pair says nothing about whether it is a sign error or two
       different lines until the two numbers are beside it. */
    DCHECKF(css_px_sub(hi, lo).px >= 0.0,
           "establishing box %s, run [%s, %s), %s axis, span [%g, %g]: "
           "CSS 2.2 §9.4.2's line boxes reported a span whose ENDING edge is before its BEGINNING edge. The "
           "two are a maximum and a minimum over the same set of boxes seeded at the same corner, so an "
           "inverted pair is the two edges having been derived from different lines",
           box_subject(style, sbuf, sizeof sbuf), box_subject_node(first, fbuf, sizeof fbuf),
           end == NULL ? "end of the child list" : box_subject_node(end, ebuf, sizeof ebuf),
           vertical ? "block" : "inline", lo.px, hi.px);
    if (ending_at_hi) return css_px_max(best, css_px_add(origin, hi));
    return css_px_min(best, css_px_add(origin, lo));
}

static CssPx sa_inline_context_extreme(lxb_dom_element_t *b, bool vertical, bool ending_at_hi, CssPx best)
{
    if (!block_flow_establishes_inline_context(b)) return best;
    return sa_fold_span(b, lxb_dom_interface_node(b)->first_child, NULL, sa_content_origin(b, vertical),
                        vertical, ending_at_hi, best);
}

/* §9.2.1.1's ANONYMOUS BLOCK BOXES of one block container, folded in. This is the OTHER shape of §9.4.2's
   context — the one with no element to be asked about — and it is reached through core/layout/block_flow.h's
   own enumeration rather than by this file delimiting runs for itself, because the run's boundaries and the
   box's POSITION are two halves of one derivation: the position is a distance down §9.4.1's stack, which is
   the walk that generated the run in the first place, and a second copy here could disagree with it about
   where a margin collapsed.
   THE BOX'S OWN MARGIN EDGE IS FOLDED BESIDE ITS CONTENT'S, AND THAT IS NOT DOUBLE COUNTING. §2's extreme is
   over "the … margin edge of all of the element's descendants' boxes", and an anonymous block box IS one of
   them — a real box in the box tree, unlike CSS 2.2 §10.8's STRUT, which core/layout/line_box.h excludes from
   its span for exactly that reason. So its top edge and its bottom edge are both operands, and where its own
   line boxes are taller than anything on them (a large `line-height`) the box reaches further than its content
   does. §9.2.1.1's initial values are what make those two edges derivable from one number: no margin, no
   border and no padding, so the content box, the border box and the margin box are one rectangle.
   ITS INLINE-AXIS EDGES ARE NOT OPERANDS and block_flow.h states why: `width: auto` with zero margins puts
   them exactly on the container's own CONTENT edges, which CSS 2 §8.1 nests inside the padding edge this
   file's caller has already seeded the extreme with. */
static CssPx sa_anon_block_boxes_extreme(lxb_dom_element_t *b, bool vertical, bool ending_at_hi, CssPx best)
{
    BlockFlowAnonBox *v;
    size_t n = block_flow_anonymous_boxes(b, &v), i;
    CssPx content;

    /* THE CONTAINER'S OWN CONTENT ORIGIN IS ASKED FOR ONLY WHERE THERE IS A BOX TO MEASURE FROM IT, and that
       is a crash surface and not a saving: core/layout/flow_position.h ABORTS for every positioning scheme it
       does not implement, so reading an origin for a container that generates none of §9.2.1.1's boxes would
       raise a float's or an out-of-flow box's crash at an element this fold has nothing to say about. */
    if (n == 0) { free(v); return best; }
    content = sa_content_origin(b, vertical);
    for (i = 0; i < n; i++) {
        CssPx origin = css_px_add(content, vertical ? v[i].content_y : v[i].content_x);

        best = sa_fold_span(b, v[i].first, v[i].end, origin, vertical, ending_at_hi, best);
        if (!vertical) continue;
        if (ending_at_hi) best = css_px_max(best, css_px_add(origin, v[i].height));
        else              best = css_px_min(best, origin);
    }
    free(v);
    return best;
}

/* THE EXTREME OVER EVERY DESCENDANT'S BOX, in tree order. WHICH NODES HAVE A BOX IS TWO QUESTIONS AND NOT ONE,
   which is css-display-3 §2.5 "Box Generation: the none and contents keywords"' own split and is what decides
   where this walk stops. `none` is stated over a SUBTREE — "the element and its descendants generate no boxes
   or text sequences" — so the walk does not descend under one. `contents` is stated over ONE BOX — "the
   element itself does not generate any boxes, but its children and pseudo-elements still generate boxes and
   text sequences as normal" — so the walk folds nothing for the element and descends anyway, because those
   children are descendants of `el` exactly as any other box is. core/dom/element_view.h answers both, and both
   are ASKED: reading its per-element predicate as the subtree answer is what put this walk inside every
   `<script>` and `<style>` in every document (HTML §15.3.1 Hidden elements gives all fourteen `display: none`)
   and made it ask the layout where a run that generates no box at all had been placed.
   A TEXT RUN IS A DESCENDANT BOX TOO, and it is the one this walk could silently miss. §2 says "all of the
   element's descendants' boxes" and an anonymous inline box holding a text run is one of them, so a
   `<div style="width:50px">verylongword</div>` has a scrolling area WIDER than its padding box in every user
   agent — and a walk over ELEMENTS alone would report the padding box and be wrong in the one direction that
   matters, because `scrollWidth > clientWidth` is precisely what a page asks this to decide.
   THOSE BOXES ARE FOLDED PER FORMATTING CONTEXT AND NOT PER TEXT NODE, which CSS 2.2 §9.4.2 forces: its
   distribution is stated over the whole context, so which line box a run's fragments land on is a function of
   every character before them and a per-node question has no answer at all. The walk therefore asks
   core/layout/line_box.h once per CONTEXT, and CSS 2.2 §9.2.1 gives a block container exactly TWO ways to hold
   one — "either contains only block-level boxes or establishes an inline formatting context" — so the walk
   asks both at every element and there is no third: the element's own context where it establishes one (`el`
   included, since the anonymous inline boxes in `el`'s own context hold `el`'s descendant TEXT NODES and are
   their boxes), and §9.2.1.1's ANONYMOUS BLOCK BOXES where the container is mixed, each of which is a real box
   in the box tree with a run of its own and a position of its own down §9.4.1's stack. §9.2.2.1's rule is then
   asked about each text node only to CHECK that one of the two has already covered it, and the crash below
   names which box is missing rather than restating that one is.
   THIS IS NOT THE HEIGHT WALK ASKED TWICE — that walk never sees the contents of a box with a declared height
   (core/layout/block_flow.c's `bf_height_needs_content`), and a text run overflowing a declared-height box is
   exactly the case this member exists for.
   THE WALK ROOT AND THE EXCLUSION ANCHOR ARE TWO PARAMETERS BECAUSE §2's TWO COLUMNS SPLIT THEM. For an element
   they are the same element: the subtree walked is its own and the excluded boxes are those whose containing
   block is above it. For a VIEWPORT the subtree walked is the DOCUMENT ELEMENT's — a viewport's descendants are
   every box in the document — and `exclude_under` is NULL, because §2's viewport column carries no exclusion
   clause at all. A NULL anchor is the spec's own absence and not a mode switch: there is no ancestor of a
   viewport for a containing block to be, so there is nothing for the test to be asked about. */
static CssPx sa_descendants_extreme(lxb_dom_element_t *el, lxb_dom_element_t *exclude_under,
                                    bool vertical, bool ending_at_hi, CssPx seed)
{
    lxb_dom_node_t *root = lxb_dom_interface_node(el), *n = root->first_child;
    char wbuf[160], pbuf[160];
    /* `el`'s OWN TWO SHAPES OF §9.4.2's CONTEXT first, in the same pair and the same order the walk asks them
       in below. `el` is not one of its own descendants and this is not a special case for it: the boxes folded
       here are the anonymous inline boxes around `el`'s descendant TEXT NODES, which ARE descendants, and the
       box establishing the context they sit on — `el` itself, or one of §9.2.1.1's anonymous block boxes
       inside it — is simply where §9.4.2 can be asked about them. `el` has a box (the caller's own
       precondition) so there is no second existence test, and its own subtree is rendered with it. */
    CssPx best = sa_anon_block_boxes_extreme(el, vertical, ending_at_hi,
                                             sa_inline_context_extreme(el, vertical, ending_at_hi, seed));

    while (n != NULL) {
        /* css-display-3 §2.5's `none` is the one answer that reaches past this node — see the banner. */
        bool descend = n->type != LXB_DOM_NODE_TYPE_ELEMENT || element_view_subtree_has_boxes(n);

        if (n->type == LXB_DOM_NODE_TYPE_ELEMENT && descend) {
            lxb_dom_element_t *d = lxb_dom_interface_element(n);

            if (element_view_has_box(n)) {
                /* THE EXCLUSION IS PER BOX AND IS NOT INHERITED BY THE SUBTREE, which is §2's own sentence:
                   it excludes "boxes THAT HAVE an ancestor of the element as their containing block", and the
                   containing block of a box inside an excluded one is that excluded box — not an ancestor of
                   `el`. So a descendant of an out-of-flow box is IN the scrolling area while the box itself is
                   out of it, and the inline formatting context an excluded box establishes is folded for the
                   same reason: the anonymous inline boxes on its lines have IT as their containing block. */
                if (exclude_under == NULL || !sa_excluded(exclude_under, d)) {
                    /* WHICH DERIVATION ANSWERS THIS BOX'S MARGIN EDGE is CSS 2.2 §9.2.2's own split between an
                       inline box and everything else, asked here because §9.4.2 gives the first a SET of
                       border areas and no `width` or `height` to size one with. */
                    CssPx e = sa_is_non_replaced_inline(d) ? sa_inline_box_edge(d, vertical, ending_at_hi)
                                                           : sa_descendant_edge(d, vertical, ending_at_hi);

                    best = ending_at_hi ? css_px_max(best, e) : css_px_min(best, e);
                }
                best = sa_inline_context_extreme(d, vertical, ending_at_hi, best);
                best = sa_anon_block_boxes_extreme(d, vertical, ending_at_hi, best);
            }
        } else if (n->type == LXB_DOM_NODE_TYPE_TEXT) {
            lxb_dom_element_t *parent;

            DCHECKF(n->parent != NULL && n->parent->type == LXB_DOM_NODE_TYPE_ELEMENT,
                   "walk root %s, the text node's parent is %s: "
                   "a TEXT node inside an element's subtree has no element parent — the walk started at an "
                   "element and descends only through its children, so a text node here belongs to one",
                   box_subject(el, wbuf, sizeof wbuf), box_subject_node(n->parent, pbuf, sizeof pbuf));
            parent = lxb_dom_interface_element(n->parent);
            /* §9.2.2.1's rule answers whether the run is a box at all; collapsible white space is not and
               contributes nothing. A run that IS a box is on a line box of ONE of the two contexts folded
               above, and CSS 2.2 §9.2.1 is what makes that exhaustive for a BLOCK CONTAINER: it "either
               contains only block-level boxes or establishes an inline formatting context", and §9.2.1.1's
               forcing is what makes the sentence true of a container holding both, by wrapping each maximal
               run of inline-level children in a box of its own. So the parent being a block container IS the
               guarantee — the run is either on the element's own context or inside one of its anonymous block
               boxes, and both were folded when the walk reached it. */
            if (block_flow_text_child_generates_box(parent, n) && !sa_is_block_container(parent) &&
                !sa_is_non_replaced_inline(parent))
                DFAILF("text run's parent %s, walk root %s: "
                      "CSSOM VIEW §2's scrolling area takes the extreme over \"all of the element's "
                      "descendants' boxes\", and one of them is the ANONYMOUS INLINE BOX (CSS 2.2 §9.2.2.1 "
                      "\"Anonymous inline boxes\") holding this text run — but the element this run is a child "
                      "of is NEITHER A BLOCK CONTAINER BOX NOR A NON-REPLACED INLINE BOX, so neither of the "
                      "two shapes CSS 2.2 §9.2.1 \"Block-level elements and block boxes\" gives an inline "
                      "formatting context is here to be asked, and the run has not been folded in. "
                      "A NON-REPLACED INLINE PARENT IS NOT ONE OF THESE AND USED TO BE, and what it was "
                      "waiting on is built: §9.2.2.1's own sentence is that \"any text that is DIRECTLY "
                      "contained inside a block container element (not inside an inline element) must be "
                      "treated as an anonymous inline element\", so a `<div>a<span>b</span></div>`'s run is on "
                      "the lines of the nearest block container ANCESTOR and was always folded there — what "
                      "was missing was the `span`'s OWN margin edge, which `sa_inline_box_edge` above now "
                      "takes over §9.4.2's fragments. FOUR THINGS ARE KNOWN TO REACH THIS LINE AND EACH NAMES A "
                      "DIFFERENT MISSING BOX. (1) A `display: contents` parent, whose children css-display-3 "
                      "§2.5 \"Box "
                      "Generation: the none and contents keywords\" splices into the grandparent's box list — "
                      "\"the element must be treated as if it had been replaced in the element tree by its "
                      "contents\" — so this run's context is the grandparent's and the splice is what is "
                      "missing; core/layout/block_flow.c's own child walk names the same absence. (2) A FLEX "
                      "or GRID container parent, whose text is not §9.2.1.1's box at all: css-flexbox-1 §4 "
                      "\"Flex Items\" says \"each child text sequence is wrapped in an ANONYMOUS BLOCK "
                      "CONTAINER FLEX ITEM\", css-grid-2 §6 \"Grid Items\" says the same with `grid item` for "
                      "`flex item`, and both add that a sequence of only white space \"is instead not "
                      "rendered\" — a different box, generated by a different rule, laid out by a different "
                      "algorithm. (3) A REPLACED INLINE parent (`<object>x</object>`): CSS 2.2 §3.1 "
                      "\"Definitions\" says of a replaced element that \"the content of replaced elements is "
                      "not considered in the CSS rendering model\", so this run generates NO BOX and the "
                      "missing thing is not a box at all but the SUPPRESSION — the walk should not have "
                      "descended into it. core/layout/replaced_element.h answers which elements are replaced "
                      "and in what state; BUILD the child-box suppression over it, in the ONE predicate that "
                      "decides box existence (core/dom/element_view.h's), so this walk and "
                      "core/layout/line_box.c's fill stop descending together rather than one at a time. "
                      "(4) A TABLE BOX or a table-internal parent, whose text is not CSS 2.2 §9.2.1.1 "
                      "Anonymous block boxes' business but CSS 2.1 §17.2.1 Anonymous table objects': \"If a "
                      "child C of a 'table' or "
                      "'inline-table' box is not a proper table child, then generate an anonymous 'table-row' "
                      "box around C and all consecutive siblings of C that are not proper table children\", and "
                      "then the NEXT RULE OF THAT SAME SECOND STAGE — not its THIRD stage, which is about a "
                      "MISPARENTED box outside a table and generates the table itself — wraps that row's "
                      "non-cell children: \"If a child C of a 'table-row' box is not a 'table-cell', then "
                      "generate an anonymous 'table-cell' box around C and all consecutive siblings of C that "
                      "are not 'table-cell' boxes.\" THAT CELL IS GENERATED NOW and this line used to say it "
                      "was not: core/layout/table_box.h answers §17.2.1's first two stages and reports the "
                      "anonymous cell as a range of siblings. What the run inside it still lacks is the cell's "
                      "USED WIDTH — §17.5.2 Table width algorithms: the 'table-layout' property's, over §17.5 "
                      "Visual layout of table contents' grid — without which its line boxes have no available "
                      "width to be filled into. "
                      "WHICH OF THE FOUR THIS IS, THE PARENT'S COMPUTED `display` NAMED ABOVE ALREADY SAYS, and "
                      "it says it ALONE: `contents` is (1); `flex`, `grid`, `inline-flex` and `inline-grid` are "
                      "(2); `inline` is (3), because reaching this line with `inline` means "
                      "`sa_is_non_replaced_inline` answered FALSE for a parent whose `display` IS `inline`, "
                      "which it can only do for a REPLACED one; and `table`, `inline-table`, `table-row`, a row "
                      "group or `table-column`/`table-column-group` is (4). A SPELLING IN NONE OF THOSE FOUR "
                      "GROUPS IS ITSELF THE FINDING — this list is what has been reasoned about, not a proof "
                      "that nothing else reaches here, and a fifth arrival is a fifth missing box to name. "
                      "THE `display` IS WHY THIS ABORT ASKS NOTHING ELSE: the sentence here used to send its "
                      "reader on to `replaced_element_of`, and asking that from a crash message would ABORT for "
                      "an `embed`, a `video`, a `canvas`, an `object`, an `audio` or an `input` — reporting "
                      "core/layout/replaced_element.c's own gap for exactly case (3), in place of this one",
                      box_subject(parent, pbuf, sizeof pbuf), box_subject(el, wbuf, sizeof wbuf));
        }
        if (descend && n->first_child != NULL) { n = n->first_child; continue; }
        while (n != root && n->next == NULL) n = n->parent;
        if (n == root) break;
        n = n->next;
    }
    return best;
}

CssPx scrolling_area_extent_px(lxb_dom_element_t *el, bool vertical)
{
    CssPx padding, lo, hi;
    bool ending_at_hi;
    char nbuf[160];
    FlowPoint origin;

    DCHECK(el != NULL, "a scrolling area was asked for with no element");
    /* §2's ELEMENT ROW IS WRITTEN OVER ONE PADDING EDGE, AND A NON-REPLACED INLINE BOX HAS ONE PER FRAGMENT —
       so the row has no answer for it and this crashes HERE rather than three files down. §2's beginning edges
       are "the element's top padding edge" and "the element's left padding edge", each a single coordinate,
       while CSS 2.2 §9.4.2 "Inline formatting contexts" splits this box across lines ("when an inline box
       exceeds the width of a line box, it is SPLIT into several boxes and these boxes are distributed across
       several line boxes") and §10.3.1 and §10.6.1 leave it no `width` and no `height` to make one rectangle
       out of. It is REACHABLE and not a corner: CSSOM VIEW §6's `scrollWidth` terminates early only for an
       element with no associated box — its `clientWidth` neighbour is the member whose step 1 also terminates
       when "the box is inline" — so `document.querySelector("span").scrollWidth` arrives here.
       WHAT IT USED TO DO IS THE REASON THIS IS A CRASH AND NOT A COMMENT: the padding-edge read below asks
       core/layout/used_value.h for an extent, which asserts §10.3.1's and §10.6.1's applicability and aborts
       naming CSSOM §9 and core/css/css_property_applies.c — a correct assert, in a shared helper, reporting a
       contract that had not come apart, for a question asked in this file. */
    if (sa_is_non_replaced_inline(el))
        DFAILF("%s: "
              "CSSOM VIEW §2 \"Terminology\"'s SCROLLING AREA was asked for a NON-REPLACED INLINE box. §2's "
              "element row seeds both of its beginning edges with \"the element's top padding edge\" and "
              "\"the element's left padding edge\" and folds the same padding edge into each ending extreme, "
              "and CSS 2.2 §9.4.2 gives this box one padding edge PER FRAGMENT rather than one — §10.3.1 "
              "\"Inline, non-replaced elements\" (\"the 'width' property does not apply\") and §10.6.1 "
              "\"Inline, non-replaced elements\" (\"the 'height' property does not apply\") are what leave it "
              "no single rectangle. So §2 does not define this element's scrolling area and no user agent "
              "reads it off one box. WHAT TO BUILD IS THE SEED, not the walk: `line_box_inline_margin_span` "
              "(core/layout/line_box.h) already answers the extreme over this box's fragments on one axis, and "
              "the same entry over its PADDING edges is what §2's row wants in place of the one padding box "
              "below — everything after the seed is unchanged, because the descendant walk is over `el`'s "
              "subtree and does not read `el`'s own extent again. Take that to the CSSOM View editors as well: "
              "§2's row states a single padding edge for a box the CSS 2.2 sections above give several",
              box_subject(el, nbuf, sizeof nbuf));
    /* THE ELEMENT'S OWN PADDING BOX FIRST, because its two edges are §2's table's own BEGINNING edges and
       because the placement inside it is where a box type this engine cannot lay out crashes by its own name.
       core/layout/flow_position.h owns the border-to-padding step, so the read of `border-top-width` that used
       to sit in this file is gone rather than duplicated — css-overflow-3 §2.3 "Scrolling Overflow"'s scrollport
       is the same rectangle and would have been the second copy. */
    origin = flow_padding_box_origin(el);
    ending_at_hi = scrolling_area_ending_edge_at_higher_coordinate(el, vertical);
    padding = used_value_padding_edge_px(el, vertical);
    /* §2's two BEGINNING edges for an element are "the element's top padding edge" and "the element's left
       padding edge", and its two ENDING edges are the extreme over that same padding edge and the descendants'
       margin edges — so the element's own padding box is in the extreme rather than beside it, which is what
       makes a scrolling area at least as large as the padding box for every element in every document. */
    lo = vertical ? origin.y : origin.x;
    hi = css_px_add(lo, padding);
    if (ending_at_hi) hi = sa_descendants_extreme(el, el, vertical, true, hi);
    else              lo = sa_descendants_extreme(el, el, vertical, false, lo);
    DCHECKF(css_px_sub(hi, lo).px >= padding.px,
           "%s, %s axis, beginning edge %g, ending edge %g, own padding edge %g: "
           "an element's scrolling area came out SMALLER than its own padding box. §2's table takes the ending "
           "edge as an extreme OVER that padding edge, so the padding box is one of the operands of the maximum "
           "and the result cannot be below it — a smaller answer is the beginning edge and the ending edge "
           "having been derived from different boxes",
           box_subject(el, nbuf, sizeof nbuf), vertical ? "block" : "inline", lo.px, hi.px, padding.px);
    return css_px_sub(hi, lo);
}

/* css-writing-modes-4 §8 "The Principal Writing Mode"' ELEMENT — the one whose used `writing-mode` and
 * `direction` the VIEWPORT's overflow directions are those of. §8: "The principal writing mode of the document
 * is determined by the used writing-mode, direction, and text-orientation values of the root element. This
 * writing mode is used, for example, to determine the direction of scrolling"; §8.1 "Propagation to the Initial
 * Containing Block": "The principal writing mode is propagated to the initial containing block and to the
 * viewport, thereby affecting … the scrolling direction of the viewport."
 * AND ITS HTML SPECIAL CASE IS THE WHOLE REASON THIS IS A FUNCTION RATHER THAN A READ OF THE ROOT. §8: "As a
 * special case for handling HTML documents, if the root element has a body child element whose display value is
 * not none, the used value of the of writing-mode and direction properties on root element are taken from the
 * computed writing-mode and direction of the first such child element instead of from the root element's own
 * values." So `<html><body dir=rtl>` gives the VIEWPORT a leftward inline-end overflow direction while the root
 * element's own computed `direction` is still `ltr` — the propagation is on USED values and §8 says in so many
 * words that it "does not affect the computed values … of the root element itself". Reading the root's computed
 * value would answer the wrong direction for the single most common way an author writes a right-to-left page.
 * IT LIVES IN THIS FILE BECAUSE §2's VIEWPORT ROW IS ONE OF ITS TWO READERS. It used to be a static in
 * core/dom/element_scrolling.c, whose comment gave as the reason that §2's overflow directions were all anyone
 * asked of it "so it is answered here rather than in core/frame/viewport.h, whose §2 column is the viewport's
 * SCROLLING AREA — a rectangle this file does not touch and which that component derives as the initial
 * containing block on both axes, WITH NO DIRECTION IN IT". That last clause was the argument, and building §2's
 * viewport row below retires it: the scrolling area now has a direction in it, so the fact has two readers and
 * belongs beside the table that is the reason both of them ask. */
static lxb_dom_element_t *sa_principal_writing_mode_element(lxb_dom_node_t *doc)
{
    lxb_dom_node_t *root = document_document_element_of(doc);
    lxb_dom_node_t *body = document_body_of(doc);
    char rbuf[160];

    /* THE COMPOUND CONDITION MERGES TWO STATES AND THE SUBJECT IS WHAT TELLS THEM APART — a document with NO
       document element at all, and one whose document element is not an element node. `box_subject_node` answers
       both from the one operand, which is why this is the subject rather than a second assert. */
    DCHECKF(root != NULL && root->type == LXB_DOM_NODE_TYPE_ELEMENT,
           "the document's document element is %s: "
           "css-writing-modes-4 §8's principal writing mode was asked of a document with no ROOT ELEMENT. §8 "
           "determines it from the root element's used values, and both readers reach here only for a document "
           "that HAS a viewport — CSSOM VIEW §2's viewport scrolling-area row below and CSSOM VIEW §6.1's "
           "ancestor walk — while a viewport exists only for a document a navigable is presenting, and such a "
           "document has a root element",
           box_subject_node(root, rbuf, sizeof rbuf));
    /* "…a body child element whose display value is not none". `document_body_of` is HTML §3.1.7's body
       element, which is already the FIRST `body` or `frameset` child of the root; the display half is
       core/dom/element_view.h's one predicate, which reads the computed `display` of the element and of its
       ancestors and is the same question §8's clause asks. */
    if (body != NULL && element_view_has_box(body)) return lxb_dom_interface_element(body);
    return lxb_dom_interface_element(root);
}

bool scrolling_area_viewport_ending_edge_at_higher_coordinate(lxb_dom_node_t *doc, bool vertical)
{
    char nbuf[160];

    DCHECKF(doc != NULL && doc->type == LXB_DOM_NODE_TYPE_DOCUMENT,
           "the node asked was %s: "
           "CSSOM VIEW §2's overflow directions for a VIEWPORT were asked of a node that is not a Document. The "
           "directions are css-writing-modes-4 §8's principal writing mode, which §8 states over THE DOCUMENT's "
           "root element, so there is no other node this question has an answer for",
           box_subject_node(doc, nbuf, sizeof nbuf));
    /* §2 gives "a scrolling box of a viewport or element" ONE definition of its two overflow directions, so
       once §8 has named the element whose used values the viewport's are, this is the element form and not a
       second derivation of the same bit. */
    return scrolling_area_ending_edge_at_higher_coordinate(sa_principal_writing_mode_element(doc), vertical);
}

CssPx scrolling_area_viewport_extent_px(lxb_dom_node_t *doc, CssPx icb_extent, bool vertical)
{
    lxb_dom_node_t *rootn;
    bool ending_at_hi;
    char nbuf[160];
    CssPx lo, hi;

    DCHECKF(doc != NULL && doc->type == LXB_DOM_NODE_TYPE_DOCUMENT,
           "the node asked was %s: "
           "CSSOM VIEW §2's scrolling area of a VIEWPORT was asked of a node that is not a Document — the row "
           "is stated over the initial containing block and \"all of the viewport's descendants' boxes\", and "
           "both of those are facts about the document a viewport is presenting",
           box_subject_node(doc, nbuf, sizeof nbuf));
    DCHECKF(icb_extent.px >= 0.0,
           "%s axis, ICB extent %g: "
           "CSS 2.2 §10.1 \"Definition of 'containing block'\" gives the initial containing block \"the "
           "dimensions of the viewport\", and a viewport has no negative dimension — so this is the ICB extent "
           "and its viewport having come apart, not a layout result",
           vertical ? "block" : "inline", icb_extent.px);
    rootn = document_document_element_of(doc);
    DCHECK(rootn != NULL,
           "CSSOM VIEW §2's scrolling area of a VIEWPORT was asked of a document with no DOCUMENT ELEMENT. The "
           "extreme over \"all of the viewport's descendants' boxes\" is then over the EMPTY SET and the area "
           "is the initial containing block alone — a derivation the caller makes for itself, because it is "
           "the caller that knows a realm is presenting no document at all");
    ending_at_hi = scrolling_area_viewport_ending_edge_at_higher_coordinate(doc, vertical);
    /* THE INITIAL CONTAINING BLOCK'S TWO EDGES ON THIS AXIS, which are the coordinates 0 and `icb_extent` and
       are DERIVED rather than assumed: CSS 2.2 §10.1 anchors the ICB "at the canvas origin", and
       core/layout/flow_position.h places every box in that same space ("THE COORDINATE SPACE IS THE INITIAL
       CONTAINING BLOCK'S"), which is what lets a descendant's margin edge and the ICB's own edge be operands of
       one extreme at all. §2's viewport row takes ONE of them as the beginning edge outright — "the top edge of
       the initial containing block", "the left edge of the initial containing block" — and folds the OTHER into
       the extreme on the ending side, so both appear here and neither is beside the walk. */
    lo = css_px(0.0);
    hi = icb_extent;
    /* "ALL OF THE VIEWPORT'S DESCENDANTS' BOXES" — every box in the document, so the walk is the document
       element's subtree with the DOCUMENT ELEMENT'S OWN margin edge folded in beside it. The root is one of the
       viewport's descendants and it is the one box a walk over its own subtree cannot contribute for itself;
       for an element the same box is the caller, which is why the element row has no such term.
       A ROOT WITH NO BOX contributes nothing and neither does anything under it — core/dom/element_view.h's one
       predicate is the same one the walk applies per descendant — so the extreme is over the empty set and the
       area is the ICB. That is §2's answer for a `display: none` root, not a shrug at one. */
    if (element_view_has_box(rootn)) {
        lxb_dom_element_t *root = lxb_dom_interface_element(rootn);
        CssPx own = sa_descendant_edge(root, vertical, ending_at_hi);

        if (ending_at_hi) hi = sa_descendants_extreme(root, NULL, vertical, true,  css_px_max(hi, own));
        else              lo = sa_descendants_extreme(root, NULL, vertical, false, css_px_min(lo, own));
    }
    DCHECKF(css_px_sub(hi, lo).px >= icb_extent.px,
           "document element %s, %s axis, beginning edge %g, ending edge %g, ICB extent %g: "
           "a VIEWPORT's scrolling area came out SMALLER than its initial containing block. §2's viewport row "
           "takes the ending edge as an extreme OVER the ICB's own edge on that side — \"the bottom-most edge "
           "of the bottom edge of the initial containing block and the bottom margin edge of all of the "
           "viewport's descendants' boxes\" — so the ICB extent is one of the operands of the extreme and the "
           "result cannot be below it. This is the invariant CSSOM VIEW §6's `scrollWidth`/`scrollHeight` "
           "max(area, viewport) and §4's `scroll()` clamp both rest on, and a smaller answer is the beginning "
           "edge and the ending edge having been derived from different boxes",
           box_subject_node(rootn, nbuf, sizeof nbuf), vertical ? "block" : "inline",
           lo.px, hi.px, icb_extent.px);
    return css_px_sub(hi, lo);
}
