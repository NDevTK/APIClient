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
#include "core/layout/flow_position.h"
#include "core/layout/used_value.h"

static bool fp_computed_is(lxb_dom_element_t *el, const char *name, const char *kw)
{
    char *v = css_computed_value(el, name);
    bool same;

    DCHECK(v != NULL, "the cascade produced no computed value for a property this engine models — every one of "
                      "them is in lexbor's registry with an initial value, so the last layer always answers");
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
    if (!fp_computed_is(el, "writing-mode", "horizontal-tb"))
        DFAIL("this box's computed `writing-mode` is not `horizontal-tb`, so its BLOCK FLOW DIRECTION is not "
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
              "height side and lands with it");
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
    JSContext *dctx;

    DCHECK(n->owner_document != NULL,
           "the initial containing block was asked for an element whose node has no owner document — every node "
           "this engine mints belongs to the document that created it");
    dctx = document_active_realm_of(lxb_dom_interface_node(n->owner_document));
    DCHECK(dctx != NULL && viewport_exists(dctx),
           "CSS 2 §9.4.1's placement was asked for an element whose document is not being presented, so there "
           "is no viewport and §10.1's INITIAL CONTAINING BLOCK does not exist. The caller's own first step is "
           "the has-a-box predicate (core/dom/element_view.h), which is defined over exactly that question — so "
           "the two answers have come apart");
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

/* CSS 2 §8.1's two edges between a box's BORDER box and its CONTENT box on the leading side of one axis — the
   top border and padding, or the left pair. §10.1's second case makes a containing block the CONTENT edge of a
   box whose own origin is its BORDER edge, and this is the whole of the difference. */
static CssPx fp_edge_before(lxb_dom_element_t *el, bool vertical)
{
    CssLength b = css_computed_length(el, vertical ? "border-top-width" : "border-left-width");

    DCHECK(b.kind == CSS_LENGTH_ABSOLUTE,
           "a `border-*-width` computed to something that is not an absolute length. css-backgrounds-3 §3.3's "
           "`Computed value:` line is `absolute length, snapped as a border width`, so every arm of that "
           "derivation produces one and a percentage or a keyword here is a rule that did not run");
    return css_px_add(b.px, used_value_px(el, vertical ? "padding-top" : "padding-left"));
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
static void fp_require_placeable(lxb_dom_element_t *el)
{
    static const char *const TABLE_INTERNAL[] = {
        "table-row-group", "table-header-group", "table-footer-group", "table-row",
        "table-cell", "table-column-group", "table-column", "table-caption",
    };
    char *d = css_computed_value(el, "display");
    bool table_internal = false, inline_level;
    unsigned i;

    DCHECK(d != NULL, "the cascade produced no computed `display` for an element whose box is being placed");
    if (d == NULL) return;
    DCHECK(strcmp(d, "none") != 0 && strcmp(d, "contents") != 0,
           "CSS 2 §9.4.1's placement was asked for an element that GENERATES NO BOX — css-display §3.1 gives "
           "`contents` no box of its own and `none` no box at all, and core/dom/element_view.h's one box "
           "predicate reads exactly those two values. The caller's own step establishes the box exists before "
           "asking where it is, so this is that predicate and this test disagreeing");
    for (i = 0; i < sizeof(TABLE_INTERNAL) / sizeof(TABLE_INTERNAL[0]); i++)
        if (strcmp(d, TABLE_INTERNAL[i]) == 0) table_internal = true;
    inline_level = strcmp(d, "inline") == 0 || strcmp(d, "inline-block") == 0 ||
                   strcmp(d, "inline-table") == 0 || strcmp(d, "inline-flex") == 0 ||
                   strcmp(d, "inline-grid") == 0;
    /* THE STRING IS RELEASED BEFORE EITHER CRASH AND NEITHER CRASH READS IT AGAIN. `DFAIL` is compiled out in
       release, so a `free` beside one is a `free` the release build FALLS THROUGH — freeing inside the loop and
       carrying on comparing would be a use-after-free there, and freeing in each arm would be a double free at
       the end. The classification is decided first, the buffer is released once, and the two arms then hold
       nothing. */
    free(d);
    if (table_internal)
        DFAIL("this box is TABLE-INTERNAL, and CSS 2.1 §17.5 'Visual layout of table contents' positions it "
              "rather than §9.4.1: a row group, a row, a cell, a column, a column group and a caption are laid "
              "out inside the TABLE's own grid of rows and columns, so a cell's position is the accumulated "
              "widths of the columns before it and the accumulated heights of the rows above it, and neither "
              "is a distance §9.4.1's two rules can produce. It needs the box structure §17.2.1 'Anonymous "
              "table objects' generates — the row groups, rows and cells a UA inserts around whatever the "
              "author wrote — and then §17.5.2's and §17.5.3's algorithms over it, which core/layout/"
              "used_value.c already crashes for when a table's EXTENT is asked. BUILD §17.2.1, then §17.5");
    if (inline_level)
        DFAIL("this box is INLINE-LEVEL, so CSS 2 §9.4.2 'Inline formatting contexts' places it and §9.4.1 "
              "does not: 'boxes are laid out horizontally, one after the other, beginning at the top of a "
              "containing block', and the line box that results is as tall as the boxes in it. CSS 2.1 §9.2.2 "
              "'Inline-level elements and inline boxes' is what puts `inline-block` and `inline-table` here "
              "beside `inline` — they are ATOMIC inline-level boxes, which 'participate in their inline "
              "formatting context as a single opaque box', so their CONTENTS are laid out differently and "
              "their POSITION is a position on a line box exactly as an inline box's is. css-display §2's "
              "`inline flex` and `inline grid` are the same case one level up. So this needs LINE BREAKING, "
              "which is the half of the measurement core/css/font_metrics.h does not answer — it holds CSS 2 "
              "§10.8.1 'Leading and half-leading''s `A` and `D`, which say how TALL a line box is, and no "
              "advance for an arbitrary glyph, which is what says where the run breaks. The same fact is why "
              "an inline element can be SEVERAL fragments and therefore several rectangles. BUILD the per-"
              "glyph advance beside `A` and `D`, then §9.4.2's line boxes over it");
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

    DCHECK(el != NULL, "a border box's position was asked for with no element");
    n = lxb_dom_interface_node(el);

    /* §9.4.1 PLACES BOXES IN NORMAL FLOW, so a box that is not in one leaves through its own section first —
       §9.3's positioning scheme is what decides which, and it decides it before any placement rule applies. */
    if (fp_computed_is(el, "position", "absolute") || fp_computed_is(el, "position", "fixed"))
        DFAIL("CSS 2 §9.3.1 takes an ABSOLUTELY POSITIONED box out of normal flow, so §9.4.1's placement does "
              "not apply to it at all: its position is §9.3.2's `top`/`right`/`bottom`/`left` resolved against "
              "the containing block §10.1's third and fourth cases give it, and §10.6.4 solves the vertical "
              "pair the same way §10.3.7 solves the horizontal one. Both sections fall back on the STATIC "
              "POSITION — 'where the box would have been in normal flow' — for their `auto` cases, so this is "
              "not an alternative to §9.4.1's flow layout but a consumer of it. BUILD §9.4.1's vertical "
              "stacking first, then §10.3.7 and §10.6.4 over it");
    if (!fp_computed_is(el, "float", "none"))
        DFAIL("CSS 2 §9.5 'Floats' positions a FLOATING box, and §9.4.1's rule does not: a float is shifted to "
              "the left or right edge of its containing block and then down past any earlier float it would "
              "overlap, under §9.5.1's nine constraints. Its own used width is a SHRINK-TO-FIT (§10.3.5), which "
              "core/layout/used_value.c crashes on for want of an intrinsic size, so the extent and the "
              "position are blocked on two different components. BUILD §9.5.1's float placement over the line "
              "boxes it interacts with");
    fp_require_horizontal_tb(el);
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
    DCHECK(cb != NULL,
           "CSS 2.1 §10.1 answered NULL — its first case, the initial containing block — for an element that is "
           "not the root. The two tests are the same test (a node whose parent is the Document), so they have "
           "come apart");
    o = flow_border_box_origin(cb);
    p.x = css_px_add(css_px_add(o.x, fp_edge_before(cb, false)),
                     fp_left_offset(el, used_value_containing_block_width(el)));
    p.y = css_px_add(css_px_add(o.y, fp_edge_before(cb, true)), block_flow_child_top(el));
    return p;
}
