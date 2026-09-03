/* CSS 2 §9.4.1 "Block formatting contexts" — a box's position. See flow_position.h for the coordinate space,
   for why the root element is the one box that is answered, and for what each other box is waiting on. */
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include <lexbor/dom/dom.h>

#include "check.h"
#include "core/css/css_computed_value.h"
#include "core/css/css_length.h"
#include "core/dom/document.h"
#include "core/frame/viewport.h"
#include "core/layout/block_flow.h"
#include "core/layout/box_subject.h"
#include "core/layout/flow_position.h"
#include "core/layout/line_box.h"
#include "core/layout/replaced_element.h"
#include "core/layout/used_value.h"

/* THE SUBJECT OF EVERY CRASH IN THIS FILE, WHICH IS A BOX AND NOT A PLACE.
   §AN-ASSERT-THAT-NAMES-A-REMEDY's test is to count the call sites that can reach an abort and to make the
   ADDRESS part of the assert once that number is larger than one would read by hand. Measured by reverse
   reachability over every `.c` file under engine/host: `flow_border_box_origin` is reached from 73 call sites
   over 46 functions in 9 files, 7 of them outside core/layout — every CSSOM VIEW §6 member that asks for a
   position or an extent, §7's scroll algorithms, a Range's client rects, IntersectionObserver's update and the
   rendering step. So it is.
   AND THAT COUNT UNDERSTATES IT, because ONE call site reaches this file once per BOX IN THE DOCUMENT rather
   than once. core/layout/scrolling_area.c's descendant walk asks `flow_border_box_origin` for every
   descendant's margin edge, and core/dom/element_view.c's `ev_scroll_extent` runs the VIEWPORT's walk — over
   the document element's whole subtree — before its step 6 has-a-box guard, which is the step everyone reads
   as coming first. So one `element.scrollWidth` places every box in the document, and the caller a frame list
   names says nothing whatever about WHICH of them had no rule.
   WHICH ADDRESS IS THE QUESTION, and core/layout/line_box.c answered it for the same shape. A `__FILE__`/
   `__LINE__` threaded from those callers arrives through `sa_descendant_edge`, `fp_line_box_origin` and
   `flow_padding_box_origin` — a handful of forwarding functions for the whole tree, which is the capture point
   that rule names as the WRONG one — and it would still not say which box has no rule. Every remedy the aborts
   below state names a computed `display`, `position`, `float` or `writing-mode` and a module to build, so the
   address their reader is standing in front of is the ELEMENT and that value.
   THE COMPOSITION ITSELF IS core/layout/box_subject.h's, which is where the argument that it must be TOTAL now
   lives, together with the ownership contract and the `%.*s` rule. Two things this file owes that component:
   `box_subject_computed` is reached instead of `fp_computed_is` below, whose own DCHECK is correct where this
   file reads a property to compute geometry with and would REPLACE the defect being reported where it reads
   one to name a box; and `replaced_element_of` is reached from no message here, for the sharper form of the
   same reason — it ABORTS by name for an `embed`, a `video`, a `canvas`, an `object`, an `audio` and an
   `input`, so a message asking it whether this box is replaced would report core/layout/replaced_element.c's
   gap for precisely the boxes whose own gap is being reported.
   THE ONE ASSERT BELOW WHOSE SUBJECT IS A NODE rather than a box is the containing-block one, whose whole
   question is what this element's PARENT is — §10.1's first case is the root element's alone and
   core/layout/used_value.c decides the root by that parent being the Document — so `box_subject_node` is what
   it asks and "(no element)" would be a wrong answer there rather than a missing one. */
static bool fp_computed_is(lxb_dom_element_t *el, const char *name, const char *kw)
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

/* §9.4.1's TWO RULES ARE STATED IN PHYSICAL AXES — "laid out one after the other, VERTICALLY" and "each box's
   LEFT outer edge touches the LEFT edge of the containing block" — and those are the physical axes of a
   `horizontal-tb` writing mode and of no other. css-writing-modes-4 §3.2 "Block Flow Direction: the
   writing-mode property" is what makes the block flow direction a property at all, and §7.4 "Flow-Relative
   Mappings" is what re-states §9.4.1's rules over the flow-relative axes so a vertical mode can be laid out.
   THE VALUE IS READABLE NOW, WHICH IS WHY THIS IS AN ASSERT AND NOT A SILENCE: this component stacked
   downwards for every box while nothing could read the property, and a `vertical-rl` box got a coordinate
   computed by the wrong rule with nothing to say so. */
static void fp_require_horizontal_tb(lxb_dom_element_t *el)
{
    char nbuf[160], wbuf[64];

    if (!fp_computed_is(el, "writing-mode", "horizontal-tb"))
        DFAILF("%s, computed `writing-mode` `%s`: "
              "this box's computed `writing-mode` is not `horizontal-tb`, so its BLOCK FLOW DIRECTION is not "
              "downwards and its inline axis is not horizontal — css-writing-modes-4 §3.2 \"Block Flow "
              "Direction: the writing-mode property\" gives `vertical-rl` and `sideways-rl` a right-to-left "
              "block flow and `vertical-lr` and `sideways-lr` a left-to-right one. CSS 2 §9.4.1's two rules are "
              "written in the PHYSICAL axes of a horizontal-tb mode ('one after the other, vertically' and "
              "'each box's left outer edge touches the left edge of the containing block'), and this component "
              "implements them physically, so placing a vertical-mode box by them would answer a coordinate "
              "from the wrong axis rather than fail. BUILD css-writing-modes-4 §7.4 \"Flow-Relative Mappings\", "
              "which restates §9.4.1 over the block and inline axes, and then this file's stacking and "
              "touching become the flow-relative pair with §6.4 \"Abstract-to-Physical Mappings\" applied once "
              "at the end. core/layout/block_flow.c's own vertical walk is the same subproblem seen from the "
              "height side and lands with it",
              box_subject(el, nbuf, sizeof nbuf),
              box_subject_computed(el, "writing-mode", wbuf, sizeof wbuf));
}

/* "The containing block in which the root element lives is a rectangle called the initial containing block"
   (§10.1) — the element whose parent is the Document itself, which is the same test core/layout/used_value.c
   makes for the base case of its own recursion. */
static bool fp_is_root(const lxb_dom_node_t *n)
{
    return n->parent != NULL && n->parent->type == LXB_DOM_NODE_TYPE_DOCUMENT;
}

/* THE INITIAL CONTAINING BLOCK'S WIDTH, asked of the ELEMENT'S own document and never of a running realm — an
   iframe's ICB is 300 CSS pixels wide and its parent's is 1280. The caller has established that the element has
   a box, which is defined over the document being presented, so the viewport exists here rather than being
   checked for a second time. */
static CssPx fp_icb_width(lxb_dom_element_t *el)
{
    lxb_dom_node_t *n = lxb_dom_interface_node(el);
    char nbuf[160];
    JSContext *dctx;

    DCHECKF(n->owner_document != NULL,
           "%s: "
           "the initial containing block was asked for an element whose node has no owner document — every node "
           "this engine mints belongs to the document that created it",
           box_subject(el, nbuf, sizeof nbuf));
    dctx = document_active_realm_of(lxb_dom_interface_node(n->owner_document));
    DCHECKF(dctx != NULL && viewport_exists(dctx),
           "%s (realm %s): "
           "CSS 2 §9.4.1's placement was asked for an element whose document is not being presented, so there "
           "is no viewport and §10.1's INITIAL CONTAINING BLOCK does not exist. The caller's own first step is "
           "the has-a-box predicate (core/dom/element_view.h), which is defined over exactly that question — so "
           "the two answers have come apart",
           box_subject(el, nbuf, sizeof nbuf), dctx == NULL ? "absent" : "present but presenting no viewport");
    return viewport_icb_width(dctx);
}

/* §9.4.1's HORIZONTAL RULE, which is one rule with two answers: "each box's left outer edge touches the left
   edge of the containing block (for right-to-left formatting, right edges touch)". WHICH ONE APPLIES IS THE
   CONTAINING BLOCK'S COMPUTED `direction`, which core/layout/used_value.h answers with §10.1's own first-case
   exception folded in ("the 'direction' property of the initial containing block is the same as for the root
   element").
   THE TWO ANSWERS ARE NOT A SLACK APART — they are the SAME DISTANCE measured from opposite edges, and stating
   them that way is what makes the rule one rule. In `ltr` the box's left MARGIN edge is at the containing
   block's left content edge, so its border box begins one `margin-left` in. In `rtl` its right margin edge is
   at the right content edge, so its border box begins at the block's width less its own `margin-right` and
   less its border box's width. Where §10.3.3's constraint equation held exactly — every `width: auto` box in
   normal flow, and every over-constrained box now that used_value.c recomputes the ignored margin — the two
   answers COINCIDE, which is the agreement this function used to assert and no longer needs to: an
   over-constrained `rtl` box now differs from an `ltr` one exactly where the spec says it does. */
static CssPx fp_left_offset(lxb_dom_element_t *el, CssPx cb_width)
{
    if (!used_value_containing_block_is_rtl(el)) return used_value_px(el, "margin-left");
    return css_px_sub(css_px_sub(cb_width, used_value_px(el, "margin-right")),
                      used_value_border_edge_px(el, false));
}

/* CSS 2 §8.1's ONE edge between a box's BORDER box and its PADDING box on the leading side of one axis. */
static CssPx fp_border_before(lxb_dom_element_t *el, bool vertical)
{
    CssLength b = css_computed_length(el, vertical ? "border-top-width" : "border-left-width");
    char nbuf[160];

    DCHECKF(b.kind == CSS_LENGTH_ABSOLUTE,
           "%s, property `%s`, computed kind %d: "
           "a `border-*-width` computed to something that is not an absolute length. css-backgrounds-3 §3.3's "
           "`Computed value:` line is `absolute length, snapped as a border width`, so every arm of that "
           "derivation produces one and a percentage or a keyword here is a rule that did not run",
           box_subject(el, nbuf, sizeof nbuf),
           vertical ? "border-top-width" : "border-left-width", (int) b.kind);
    return b.px;
}

/* CSS 2 §8.1's two edges between a box's BORDER box and its CONTENT box on the leading side of one axis — the
   top border and padding, or the left pair. §10.1's second case makes a containing block the CONTENT edge of a
   box whose own origin is its BORDER edge, and this is the whole of the difference. */
static CssPx fp_edge_before(lxb_dom_element_t *el, bool vertical)
{
    return css_px_add(fp_border_before(el, vertical),
                      used_value_px(el, vertical ? "padding-top" : "padding-left"));
}

/* WHICH BOX TYPES §9.4.1 PLACES, asked HERE and not left to whichever component this one calls next. The two
 * rules §9.4.1 states — "boxes are laid out one after the other, vertically, beginning at the top of a
 * containing block" and "each box's left outer edge touches the left edge of the containing block" — are
 * written about a BLOCK-LEVEL box in a block formatting context, and they say nothing whatever about a box of
 * any other type. This used to test the one value `inline`, on the reading that everything else this engine
 * could compute a `display` for was block-level; core/css/css_style_declaration.c's UA sheet now answers
 * `inline-block` for `input`, `button`, `select`, `textarea` and `marquee` and the eight table-internal types
 * for a table's own children, so that reading is gone and the rest would have fallen through to the two rules
 * below and come out with an x and a y for a box neither rule describes.
 *
 * A CRASH TWO COMPONENTS AWAY IS NOT THE SAME ASSERT. Every one of these does still abort today — the walk in
 * core/layout/block_flow.c classifies the same child and refuses it — but that is a crash at the box's PARENT,
 * naming the parent's height walk, for a question that was asked about the CHILD's position. The invariant
 * belongs where it is born (CLAUDE.md §Offensive programming), and it is the only thing standing between a
 * release build and a coordinate computed from the wrong rule.
 *
 * §9.4.1 IS NOT ASKED ABOUT A BOX THAT DOES NOT EXIST, which is the caller's own first step (flow_position.h),
 * so `none` and `contents` are a DCHECK rather than an arm: reaching here with either means the predicate that
 * decides box existence and this one disagree. */
static bool fp_is_on_a_line_box(lxb_dom_element_t *el)
{
    /* THE QUESTION IS THE BOX'S LEVEL AND NOT WHETHER IT IS REPLACED, which is what this predicate used to
       answer and is the reason a replaced element crashed here. CSS 2.1 §9.2.2 "Inline-level elements and
       inline boxes" puts BOTH kinds on the line: a non-replaced `display: inline` element "generates an inline
       box", and an inline-level box that is not one — "such as replaced inline-level elements, inline-block
       elements, and inline-table elements" — is an ATOMIC inline-level box that "participate[s] in [its]
       inline formatting context as a single opaque box". §9.4's two normal-flow formatting contexts are
       alternatives decided by exactly that level, so both leave through §9.4.2 and neither is placed by
       §9.4.1's two rules.
       WHAT SEPARATES THEM IS THE FRAGMENT'S SHAPE AND core/layout/line_box.h OWNS IT: an inline box is
       delimited by its two EDGE items and split across as many line boxes as it spans, a replaced element by
       the ONE run item css-text-3 §5.5 "Line Breaking Details" collects for "each replaced element or other
       atomic inline". One answer, one component, and this file composes the coordinate out of it either way.
       THE `inline-block` COMES THROUGH HERE TOO, AND IT IS THE SAME SENTENCE THAT PUTS IT HERE. §9.2.2 names
       it in the list that defines the class — "such as replaced inline-level elements, inline-block elements,
       and inline-table elements" — so it is on a line box for exactly the reason a replaced `display: inline`
       element is, and core/layout/line_box.h delimits its fragment by the same single item index. WHAT USED TO
       KEEP IT OUT WAS NOT ITS LEVEL BUT A MISSING NUMBER, and the number now exists: §10.8.1 "Leading and
       half-leading" puts a non-replaced `inline-block`'s baseline INSIDE the box ("The baseline of an
       'inline-block' is the baseline of its last line box in the normal flow, unless it has either no in-flow
       line boxes or if its 'overflow' property has a computed value other than 'visible', in which case the
       baseline is the bottom margin edge"), so its margin box hangs BELOW the line's baseline rather than
       resting on it, and `line_box_inline_fragments` reads that split (`lb_atomic_extent`) instead of assuming
       the bottom margin edge. BOTH KINDS OF `inline-block` take this arm: CSS 2.2 §10.3.10 "'Inline-block',
       replaced elements in normal flow" is one sentence long — "Exactly as inline replaced elements." — and
       core/layout/used_value.c asks `replaced_element_of` on its `auto` arm BEFORE it asks `uv_box_kind`, so
       the replaced half is sized by §10.3.2 and §10.6.2 with nothing here to arrange.
       THE THREE REMAINING ATOMIC INLINE-LEVEL BOXES DO NOT COME THROUGH HERE, and it is no longer their
       `display` that stops them but a MODULE this engine does not have: an `inline-table`, an `inline-flex`
       and an `inline-grid` leave through `fp_require_placeable` below, which names what each still needs. A
       REPLACED element whose `display` is `block` is not inline-level at all: §9.2.1 makes it a block box and
       §9.4.1's two rules DO place it, so it takes the induction at the end of this function, with §10.3.4's
       and §10.6.2's used extents under it. */
    return fp_computed_is(el, "display", "inline") || fp_computed_is(el, "display", "inline-block");
}

/* CSS 2 §9.4.2 "Inline formatting contexts"' PLACEMENT of a box on a line — "boxes are laid out horizontally,
   one after the other, beginning at the top of a containing block", broken into line boxes whose width the
   containing block decides. It answers a NON-REPLACED inline box's fragments and an ATOMIC inline-level box's
   one fragment through the same entry, because §9.4.2 places both and core/layout/line_box.h owns the one
   difference between them (a range of two edge items, or one atomic item's own index). The atomic half is
   every shape this engine can measure: a REPLACED element, and both halves of the `inline-block` CSS 2.2
   §10.3.9 and §10.3.10 divide between.
   ITS ORIGIN IS ITS FIRST FRAGMENT'S, and that is §9.4.2's own consequence rather than a choice among several:
   "when an inline box exceeds the width of a line box, it is SPLIT into several boxes and these boxes are
   distributed across several line boxes", so the box has as many border areas as it has fragments and exactly
   one of them begins first in content order. CSSOM VIEW §7's `offsetTop`/`offsetLeft` and §6's
   `getBoundingClientRect` both want that one; §6's `getClientRects` wants them ALL and does not come through
   here — core/dom/element_view.c asks core/layout/line_box.h for the list directly, so this entry answering
   the first is not this component deciding the others do not matter.
   THE FRAME IS THE ESTABLISHING BOX'S CONTENT BOX, which core/layout/line_box.h reports in and which is the
   same second case of §10.1 the block-level arm below composes: that box's own origin (this function, one
   level up) plus CSS 2 §8.1's leading border and padding. */
size_t flow_inline_fragment_rects(lxb_dom_element_t *el, FlowRect **out)
{
    lxb_dom_element_t *style = NULL;
    LineBoxFragment *frags = NULL;
    FlowRect *rects;
    FlowPoint o;
    CssPx left, top;
    char ebuf[160], sbuf[160];
    size_t n, i;

    DCHECK(el != NULL && out != NULL,
           "CSS 2 §9.4.2's fragment rectangles were asked for with no element or nowhere to report them");
    n = line_box_inline_fragments(el, &style, &frags);
    DCHECKF(n >= 1 && frags != NULL && style != NULL,
           "%s, establishing box %s, %zu fragment(s) reported%s: "
           "CSS 2 §9.4.2's fragments were reported as none for an inline-level box that generates one. That "
           "entry's own asserts make a zero count impossible — an inline box's two edge items and an atomic "
           "inline-level box's one item are alike content the fill partitions — so this is that contract "
           "having been broken between the two files",
           box_subject(el, ebuf, sizeof ebuf), box_subject(style, sbuf, sizeof sbuf), n,
           frags == NULL ? " and no fragment array" : "");
    /* §10.1's SECOND CASE, composed exactly as the block-level arm below composes it: the establishing box's
       own origin (this function, one level up — which is where a float, an out-of-flow ancestor or a vertical
       writing mode crashes by its own section) plus CSS 2 §8.1's leading border and padding, which is the
       difference between that box's BORDER edge and the CONTENT edge core/layout/line_box.h measures from. */
    o = flow_border_box_origin(style);
    left = css_px_add(o.x, fp_edge_before(style, false));
    top = css_px_add(o.y, fp_edge_before(style, true));
    rects = malloc(n * sizeof *rects);
    CHECK(rects != NULL, "out of memory placing CSS 2 §9.4.2's box fragments — one entry per fragment of one "
                         "inline box, so a failure here is the physical floor");
    for (i = 0; i < n; i++) {
        rects[i].x = css_px_add(left, frags[i].inline_start);
        rects[i].y = css_px_add(top, frags[i].block_start);
        rects[i].width = css_px_sub(frags[i].inline_end, frags[i].inline_start);
        rects[i].height = css_px_sub(frags[i].block_end, frags[i].block_start);
    }
    free(frags);
    *out = rects;
    return n;
}

/* THE ORIGIN OF A BOX §9.4.2 PLACES, WHICH IS ITS FIRST FRAGMENT'S CORNER FOR BOTH SHAPES OF SUCH A BOX. An
   inline box has one border area per fragment and exactly one begins first in content order; an ATOMIC
   inline-level box is CSS 2.1 §9.2.2's "single opaque box" and has exactly one, so the same index answers both
   and no caller has to know which it is holding. */
static FlowPoint fp_line_box_origin(lxb_dom_element_t *el)
{
    FlowRect *rects = NULL;
    FlowPoint p;

    (void)flow_inline_fragment_rects(el, &rects);
    p.x = rects[0].x;
    p.y = rects[0].y;
    free(rects);
    return p;
}

static void fp_require_placeable(lxb_dom_element_t *el)
{
    static const char *const TABLE_INTERNAL[] = {
        "table-row-group", "table-header-group", "table-footer-group", "table-row",
        "table-cell", "table-column-group", "table-column", "table-caption",
    };
    char *d = css_computed_value(el, "display");
    char nbuf[160];
    bool table_internal = false, inline_level;
    unsigned i;

    /* THERE IS NO GUARD UNDER THIS ASSERT, AND THE REASON IS A PROPERTY OF THE ENTRY RATHER THAN A PREFERENCE.
       `css_computed_value(el, "display")` cannot answer NULL: it routes `display` through `computed_display`,
       which dereferences the specified value at its own first `strcmp` — so a NULL would fault THERE and never
       reach this line — and every one of that function's return paths is either the specified string itself or
       a `css_cv_strdup`, which CHECKs its allocation. A `if (d == NULL) return;` here was therefore dead in
       both builds while reading as the thing standing between this file and a null, which is worse than
       either: in dev the assert already aborts, and in release it silently declined to place a box for a state
       that cannot arise. A pointer whose non-nullness the callee establishes is asserted here and guarded
       nowhere — the guard's only effect was to make the assert look recoverable. */
    DCHECKF(d != NULL, "%s: the cascade produced no computed `display` for an element whose box is being placed",
            box_subject(el, nbuf, sizeof nbuf));
    DCHECKF(strcmp(d, "none") != 0 && strcmp(d, "contents") != 0,
           "%s: "
           "CSS 2 §9.4.1's placement was asked for an element that GENERATES NO BOX — css-display-3 §2.5 "
           "\"Box Generation: the none and contents keywords\" gives `contents` no box of its own and `none` "
           "no box at all (\"The element and its descendants generate no boxes or text sequences.\"), and "
           "core/dom/element_view.h's one box "
           "predicate reads exactly those two values. The caller's own step establishes the box exists before "
           "asking where it is, so this is that predicate and this test disagreeing",
           box_subject(el, nbuf, sizeof nbuf));
    for (i = 0; i < sizeof(TABLE_INTERNAL) / sizeof(TABLE_INTERNAL[0]); i++)
        if (strcmp(d, TABLE_INTERNAL[i]) == 0) table_internal = true;
    /* NEITHER `inline` NOR `inline-block` IS IN THIS LIST, and that is what makes this function's name true: a
       box whose computed `display` is either one is PLACED now, by §9.4.2 through core/layout/line_box.h, and
       it leaves through `fp_line_box_origin` before this classification is asked — a non-replaced inline box
       out of its two edge items, and every ATOMIC inline-level box this engine can measure out of the one
       atomic item that carries it. What is left here is the atomic inline-level boxes CSS 2.1 §9.2.2
       "Inline-level elements and inline boxes" separates from an inline box AND for which a MODULE outside CSS
       2.1 §10 owns a number §9.4.2's line cannot be filled without. */
    inline_level = strcmp(d, "inline-table") == 0 || strcmp(d, "inline-flex") == 0 ||
                   strcmp(d, "inline-grid") == 0;
    /* THE STRING IS RELEASED BEFORE EITHER CRASH AND NEITHER CRASH READS IT AGAIN. `DFAIL` is compiled out in
       release, so a `free` beside one is a `free` the release build FALLS THROUGH — freeing inside the loop and
       carrying on comparing would be a use-after-free there, and freeing in each arm would be a double free at
       the end. The classification is decided first, the buffer is released once, and the two arms then hold
       nothing.
       AND THE TWO CRASHES NAME THE `display` WITHOUT READING `d`, which is why this rule survived their gaining
       an address: `box_subject` asks the cascade for its own copy and releases it inside itself, so the arms
       still hold nothing after the free above. Folding `d` into either message to save that second read is the
       use-after-free this paragraph is about, arriving through the part of the line that looks like prose. */
    free(d);
    if (table_internal)
        DFAILF("%s: "
              "this box is TABLE-INTERNAL, and CSS 2.1 §17.5 'Visual layout of table contents' positions it "
              "rather than §9.4.1: a row group, a row, a cell, a column, a column group and a caption are laid "
              "out inside the TABLE's own grid of rows and columns, so a cell's position is the accumulated "
              "widths of the columns before it and the accumulated heights of the rows above it, and neither "
              "is a distance §9.4.1's two rules can produce. THE BOX STRUCTURE IS NO LONGER WHAT IT NEEDS: "
              "core/layout/table_box.h answers §17.2.1 Anonymous table objects' first two stages — this "
              "table's rows in §17.2's display order, each with its cells — so what is left is §17.5 Visual "
              "layout of table contents' grid over those rows — which core/layout/table_grid.h answers, spans "
              "and all. NEITHER IS THE COLUMN WIDTH ANY LONGER: CSS 2.1 §17.5.2 Table width algorithms: the "
              "'table-layout' property is core/layout/table_width.h, which answers a used width for EVERY "
              "column of the grid at once, so a cell's horizontal offset is a prefix sum over "
              "`TableUsedWidths.columns` plus CSS 2.1 §17.6.1 The separated borders model's spacing before it. "
              "AND NEITHER IS THE OTHER AXIS OR THE PER-CELL ROUTE, both of which this line used to ask for: "
              "CSS 2.1 §17.5.3 Table height algorithms is core/layout/table_height.h, which answers a used "
              "height for EVERY grid row at once, and core/layout/used_value.c hands ONE cell the used width "
              "of the columns its rectangle covers and the used height of the rows it covers rather than "
              "crashing for either. WHAT IS LEFT IS THIS FUNCTION'S OWN SUBJECT AND NOTHING BELOW IT: a "
              "coordinate. §17.5's grid gives a cell a RECTANGLE OF GRID CELLS and no origin, so the two "
              "prefix sums — the column widths and §17.6.1's spacing before this cell, and the row heights and "
              "the vertical spacing above it — are what turns that rectangle into the distance this entry "
              "reports, and the ROW and ROW GROUP and COLUMN boxes need the same sums taken over their own "
              "spans. BUILD those two prefix sums here, over `TableUsedWidths.columns` and "
              "`TableUsedHeights.rows`. "
              "THEY CANNOT ALL BE KEYED BY `table_grid_cell_of`, WHICH IS WHAT THIS LINE USED TO SAY, AND THE "
              "BOX THAT ABORTS HERE IS THE COUNTEREXAMPLE: that entry maps a CELL element to its rectangle, so "
              "a ROW is reachable only through the cells anchored in it and `<tr></tr>` has none — a real row "
              "with a real grid row and a real height (§17.5.3's maximum over no cell) that no mapping in "
              "core/layout/table_grid.h names. A ROW GROUP and a COLUMN are unreachable the same way. So the "
              "FIRST diff is that mapping, where the rows are already generated in grid order with their "
              "elements — core/layout/table_box.h's `table_box_rows` is what `table_grid_build` walks — "
              "reported as a row INDEX beside the grid, so this becomes a lookup rather than a search. "
              "core/layout/used_value.c's ROW-height read states the same missing mapping in its own crash and "
              "is its second consumer, which is how you will know it fired: both stop naming it. "
              "A CAPTION IS NOT IN THE GRID AT ALL and no longer waits on a box nothing can name — §17.4 "
              "Tables in the visual formatting model puts it in the table WRAPPER, which §10.1's walk now "
              "reports (core/layout/used_value.h) and whose used width and height "
              "core/layout/used_value.c answers; what a caption still waits on is §9.4.1's stack over the "
              "WRAPPER'S OWN CHILD BOX LIST, which core/layout/block_flow.c names in full",
              box_subject(el, nbuf, sizeof nbuf));
    if (inline_level)
        DFAILF("%s: "
              "CSS 2.1 §9.2.2 'Inline-level elements and inline boxes' makes this an ATOMIC INLINE-LEVEL box — "
              "an `inline-table`, or css-display-3 §2's `inline flex` and `inline grid` — boxes CSS 2.1 §9.2.2 "
              "calls atomic \"because they participate in their inline formatting context as a single opaque "
              "box\". So §9.4.2 places it, not §9.4.1, and its position is a position ON A LINE BOX exactly as "
              "a non-replaced inline box's is. "
              "THE PLACEMENT IS BUILT AND SO IS THE `inline-block`, WHICH IS WHY NEITHER IS STILL NAMED HERE: "
              "`line_box_inline_fragments` (core/layout/line_box.h) delimits an atomic inline's fragment by its "
              "own item index, hangs its margin box from the line's baseline SPLIT at the box's own baseline "
              "(`lb_atomic_extent`), and `fp_line_box_origin` above turns that rectangle into this coordinate — "
              "for a `display: inline` box, for a REPLACED element, and for both halves of the `inline-block` "
              "CSS 2.2 §10.3.9 and §10.3.10 divide between. Following an earlier form of this line would have "
              "built that a second time. "
              "WHAT IS LEFT IS NOT A PLACEMENT AT ALL: each of the three boxes above needs a NUMBER from a "
              "module outside CSS 2.1 §10, without which there is nothing for §9.4.2's fill to put on a line. "
              "AN `inline-table` HAS CSS 2.1 §17.2 The CSS table model's box structure — the row groups, rows "
              "and cells §17.2.1 Anonymous table objects generates, which core/layout/table_box.h answers — so "
              "what it still needs is NOT that inline size — CSS 2.1 §17.5.2 Table width algorithms: the "
              "'table-layout' property is core/layout/table_width.h and core/layout/used_value.c routes a "
              "table box's `width` to it, so `used_value_margin_edge_px` answers an `inline-table`'s margin-box "
              "inline size like any other box's — but the baseline CSS 2.2 §10.8.1 \"Leading and half-leading\" states "
              "for it (\"The baseline of an 'inline-table' is the baseline of the first row of the table\"), "
              "which is a position inside that structure and not a rule that can be built beside it. "
              "AN `inline-flex` AND AN `inline-grid` ARE ALREADY CLASSIFIED — `uv_box_kind`'s "
              "`UV_BOX_INLINE_FLEX_GRID` — so an `auto` width on one CRASHES by its own module's name rather "
              "than answering with §10.3.3's constraint equation over a box no part of §10.3 describes. WHAT "
              "THAT CLASSIFICATION LEFT IS THE MODULE'S OWN INTRINSIC MAIN SIZES, which §10.3.9's "
              "shrink-to-fit reads as its two terms: css-flexbox-1 §9.9.1 \"Flex Container Intrinsic Main "
              "Sizes\" and css-grid-1 §5.2 \"Sizing Grid Containers\", whose max-content size is \"the sum "
              "of the grid container's track sizes (including gutters) in the appropriate axis\". Each module "
              "owns that box's BASELINE too and §10.8.1 names neither of them — css-flexbox-1 §8.5 \"Flex "
              "Container Baselines\" derives one from the items on the container's startmost flex line and "
              "css-grid-1 §10.6 \"Grid Container Baselines\" does the same — so both numbers fall out of the "
              "module's layout and neither is this file's to invent. BUILD the module this box's `display` "
              "names as a second producer of `IntrinsicInlineSizes`. An `inline-table` is NOT waiting on that "
              "and this line used to say it was: its inline size is answered, and what it needs is §10.8.1's "
              "own sentence above, which is a position inside CSS 2.1 §17.5.3 Table height algorithms' rows. "
              "Then this box reaches the line and this arm deletes",
              box_subject(el, nbuf, sizeof nbuf));
    /* What is left is a box CSS 2.1 §9.2.1 'Block-level elements and block boxes' makes block-level, which is
       what the two rules below are written about. A `table` is one of them and stays on this path: §17.4
       'Tables in the visual formatting model' says the table wrapper box is block-level for `display: table`
       and inline-level for `display: inline-table`, so the wrapper's own origin is §9.4.1's and what §17.5
       owns is everything INSIDE it — which is the arm above, plus the `table-caption` §17.4 renders as a
       normal block box inside the table WRAPPER box and therefore not over this containing-block chain. */
}

FlowPoint flow_border_box_origin(lxb_dom_element_t *el)
{
    FlowPoint p = { { 0.0, CSS_ENV_NONE, NULL }, { 0.0, CSS_ENV_NONE, NULL } };
    lxb_dom_node_t *n;
    lxb_dom_element_t *cb;
    FlowPoint o;
    /* `vbuf` holds a computed value at two sites and a NODE NAME at the third, so it is sized for the wider of
       the two rather than for the value it happens to carry first. */
    char nbuf[160], vbuf[160];

    DCHECK(el != NULL, "a border box's position was asked for with no element");
    n = lxb_dom_interface_node(el);

    /* §9.4.1 PLACES BOXES IN NORMAL FLOW, so a box that is not in one leaves through its own section first —
       §9.3's positioning scheme is what decides which, and it decides it before any placement rule applies. */
    if (fp_computed_is(el, "position", "absolute") || fp_computed_is(el, "position", "fixed"))
        DFAILF("%s, computed `position` `%s`: "
              "CSS 2 §9.3.1 takes an ABSOLUTELY POSITIONED box out of normal flow, so §9.4.1's placement does "
              "not apply to it at all: its position is §9.3.2's `top`/`right`/`bottom`/`left` resolved against "
              "the containing block §10.1's third and fourth cases give it, and §10.6.4 solves the vertical "
              "pair the same way §10.3.7 solves the horizontal one. Both sections fall back on the STATIC "
              "POSITION — 'where the box would have been in normal flow' — for their `auto` cases, so this is "
              "not an alternative to §9.4.1's flow layout but a consumer of it. BUILD §9.4.1's vertical "
              "stacking first, then §10.3.7 and §10.6.4 over it",
              box_subject(el, nbuf, sizeof nbuf), box_subject_computed(el, "position", vbuf, sizeof vbuf));
    if (!fp_computed_is(el, "float", "none"))
        DFAILF("%s, computed `float` `%s`: "
              "CSS 2 §9.5 'Floats' positions a FLOATING box, and §9.4.1's rule does not: a float is shifted to "
              "the left or right edge of its containing block and then down past any earlier float it would "
              "overlap, under §9.5.1's nine constraints. Its own used width is a SHRINK-TO-FIT (§10.3.5) and "
              "core/layout/used_value.c COMPUTES ONE NOW, over core/layout/intrinsic_size.h's measurement of "
              "the box's content — so the extent is no longer the blocker and the POSITION is the whole of what "
              "is left. BUILD §9.5.1's nine constraints over the line boxes the float interacts with",
              box_subject(el, nbuf, sizeof nbuf), box_subject_computed(el, "float", vbuf, sizeof vbuf));
    fp_require_horizontal_tb(el);
    /* §9.4's TWO NORMAL-FLOW FORMATTING CONTEXTS ARE ALTERNATIVES, and which one places a box is its own
       inline-or-block LEVEL — §9.4.1 is written about a block-level box and §9.4.2 about the boxes on a line.
       A box on a line therefore leaves through its own section HERE, before the classification below, exactly
       as a float and an out-of-flow box leave through theirs above: the two rules at the end of this function
       are §9.4.1's and say nothing about a box on a line box. */
    if (fp_is_on_a_line_box(el)) return fp_line_box_origin(el);
    fp_require_placeable(el);

    /* §10.1's FIRST CASE, which is §9.4.1's base case as well: the root element's containing block is the ICB,
       "anchored at the canvas origin", so both of §9.4.1's rules reduce to this box's own two margins.
       VERTICALLY, "boxes are laid out one after the other, vertically, beginning at the top of a containing
       block" — there is no preceding sibling, so the box begins at the ICB's top, and §8.3.1 'Collapsing
       margins' states that "margins of the root element's box do not collapse", so the distance from that top
       to the border edge is the root's OWN used `margin-top` and not a collapsed one. */
    if (fp_is_root(n)) {
        p.x = fp_left_offset(el, fp_icb_width(el));
        /* §9.4.1's vertical rule at the top of the containing block, with §8.3.1's root-element exception. */
        p.y = used_value_px(el, "margin-top");
        return p;
    }

    /* §10.1's SECOND CASE, and §9.4.1's inductive step: the containing block is "the CONTENT EDGE of the
       nearest block container ancestor box", so this box's origin is that box's origin plus its own top and
       left border and padding — the two edges CSS 2 §8.1 nests between a border box and a content box — plus
       what §9.4.1 puts between the content edge and this box. HORIZONTALLY that is the same left-outer-edge
       touching the root arm above states, over the containing block's width rather than the ICB's. VERTICALLY
       it is core/layout/block_flow.h's walk: the used height of every preceding in-flow sibling with §8.3.1's
       collapsing between them, which is the one subproblem §10.6.3's content-based height was waiting on too.
       THE RECURSION TERMINATES AT THE ROOT, whose containing block is §10.1's first case — the ICB, which is
       no element's box — so `used_value_containing_block` answering NULL is exactly the arm above. */
    cb = used_value_containing_block(el);
    DCHECKF(cb != NULL,
           "%s, parent %s: "
           "CSS 2.1 §10.1 answered NULL — its first case, the initial containing block — for an element that is "
           "not the root. The two tests are the same test (a node whose parent is the Document), so they have "
           "come apart",
           box_subject(el, nbuf, sizeof nbuf), box_subject_node(n->parent, vbuf, sizeof vbuf));
    o = flow_border_box_origin(cb);
    p.x = css_px_add(css_px_add(o.x, fp_edge_before(cb, false)),
                     fp_left_offset(el, used_value_containing_block_width(el)));
    p.y = css_px_add(css_px_add(o.y, fp_edge_before(cb, true)), block_flow_child_top(el));
    return p;
}

/* CSS 2 §8.1 "Box dimensions"' PADDING BOX ORIGIN — the border box origin above moved inward by the ONE leading
   border on each axis. It is a second ENTRY and not a second answer: both coordinates come from
   `flow_border_box_origin`, so a box this component cannot place crashes there, by its own section, before a
   padding edge is ever asked for. */
FlowPoint flow_padding_box_origin(lxb_dom_element_t *el)
{
    FlowPoint p = flow_border_box_origin(el);

    p.x = css_px_add(p.x, fp_border_before(el, false));
    p.y = css_px_add(p.y, fp_border_before(el, true));
    return p;
}
