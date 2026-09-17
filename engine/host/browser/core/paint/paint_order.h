/* CSS 2.1 §E.2 "Painting order" — WHICH BOX'S MARKS GO DOWN WHEN, inside one of CSS 2.1 §9.9.1's layers, and
 * the second half of the answer core/paint/stacking_order.h began.
 *
 * WHY THIS IS A SECOND COMPONENT AND NOT A SECOND ENTRY ON THE FIRST. stacking_order.h states the reason in
 * its own words and this file is the other side of it: CSS 2.1 §9.9.1 is a total order over BOXES and CSS 2.1 §E.2 is a total
 * order over MARKS. That distinction is not a refinement, it is a change of KIND, and the proof is inside
 * CSS 2.1 §E.2's own text. Step 4 is "For all its in-flow, non-positioned, block-level descendants in tree order:"
 * followed by that box's background and border; step 7 is "Otherwise: first for the element, then for all its
 * in-flow, non-positioned, block-level descendants in tree order:" followed by that box's CONTENT. The two
 * steps walk THE SAME SET. So for two such boxes A and B with A first in tree order, CSS 2.1 §E.2 lays A's border
 * down, then B's border, and only then A's text — and a comparator over BOXES cannot say that, because a total
 * order must put A wholly before B or B wholly before A. `stacking_order_compare` is therefore correct, is the
 * answer for hit-testing and occlusion, and CANNOT be the frame of a display list; a painter built by sorting
 * boxes with it is right exactly while every box contributes ONE mark and is wrong at the first box that
 * contributes two.
 *
 * WHAT THIS COMPONENT STATES AND WHAT IT DOES NOT, DRAWN AS A LINE AND NOT AS A HEDGE. It states CSS 2.1 §E.2's
 * TOP-LEVEL order: which box's marks are offered, under which of CSS 2.1 §E.2's steps, in which sequence. It does not
 * state the order of marks WITHIN one of those offers — CSS 2.1 §E.2's own sub-lists — and the two are separable
 * because every operand of the first is a computed `display`, `position`, `float` and `z-index` plus tree
 * order and NOTHING ELSE, which is the identical property that let CSS 2.1 §9.9.1's layers be complete rather than
 * partial. Colour, geometry, font, table grid and image decoding appear only inside the sub-lists. So this
 * component can be COMPLETE for what it states, at a revision where almost none of the ink exists.
 *
 * THE SUB-LISTS SPLIT INTO TWO KINDS AND ONLY ONE OF THEM IS ORDER. CSS 2.1 §E.2's step 2 block arm is three marks in
 * a fixed sequence — background colour, background image, border — which is a MARK VOCABULARY and belongs to
 * whatever lays ink. CSS 2.1 §E.2's step 2 TABLE arm and its step 7.2 are ENUMERATIONS (six background levels over a
 * table's column groups, columns, row groups, rows and cells; the line boxes of a block and the boxes inside
 * each line), and an enumeration is order and is therefore this component's. Both are named residuals at the
 * foot of this header, with what builds them.
 *
 * WHY STEP 10 IS A RESIDUAL AND A TABLE IS NOT, WHICH IS THE ONE PLACE THIS FILE MAKES A JUDGEMENT. CLAUDE.md's
 * test is whether the code is WRONG or merely NARROWER. Omitting CSS 2.1 §E.2's step 10 leaves a prefix that IS CSS 2.1 §E.2's
 * first nine steps — every offer this component made was made at the position CSS 2.1 §E.2 puts it, and the outlines
 * are simply not drawn — so it is narrower and it is named. Omitting a TABLE in the middle of step 4 would
 * leave a sequence that CLAIMS to be steps 1 through 9 and is not one: the consumer would paint every other
 * box and never learn that a box was passed over. That is wrong rather than narrow, so a table arm is offered
 * like any other box and the enumeration inside it is the residual, never a silent skip.
 *
 * THE WALK IS OVER ONE DOCUMENT'S TREE AND CANNOT LEAVE IT, WHICH IS A SECURITY INVARIANT AND NOT A SCOPE. A
 * display list carries text runs and URLs read out of a document's DOM, so a CROSS-ORIGIN child's display list
 * may never cross into its parent's instance — HTML §7.2.1 "Security infrastructure for Window, WindowProxy,
 * and Location objects"' fixed cross-origin member list is exactly the enumeration that forbids it, and
 * engine/host/solver/world.h already keys one WASM instance on `(browsing-context group, origin)`. This walk
 * cannot cross BY CONSTRUCTION rather than by a check somebody remembered: it advances with
 * core/dom/shadow_root.h's shadow-including tree walker, whose only edges are a node's children and an
 * element's shadow root, and a child navigable's Document is reached by NEITHER — it hangs off the `iframe`
 * element's WRAPPER (core/html/html_iframe.h), which is a per-flow fact no tree walk sees. The assert below
 * that both ends of a step stay in one Document is the statement of that, not its enforcement.
 * AND CSS 2.1 §E.2 ALREADY HAS THE RIGHT SLOT FOR A FOREIGN SUBTREE, so admitting one needs no new step and no new
 * shape. An `iframe` is a REPLACED element in this engine (core/layout/replaced_element.h, off HTML §15.4.1
 * "Embedded content"), and CSS 2.1 §E.2 places a replaced element's content with the words "the replaced content,
 * atomically" — once at step 7.1 for a block-level one and once inside step 7.2's line-box sub-list for an
 * inline-level one. Atomically IS a surface: one opaque handle composited as a unit, with the content rect and
 * the clip and transform chain above it owned by the PARENT and nothing whatever read out of the child. What
 * such an item must additionally carry is the WORLD it was built under, because a surface belongs to a world
 * and not to a moment — two arms of one fork produce two parent surfaces, and composing one against a child
 * surface from a contradicting world fabricates a timeline neither document was in, which is the defect
 * CLAUDE.md forbids by name for a cross-document message. That field is on the ITEM, so it is the ink
 * vocabulary's and not this component's; what this component owes it is the slot, and step 7.1 is the slot.
 *
 * NOTHING HERE IS STORED, for core/layout/used_value.h's reason and stacking_order.h's: CSS 2.1 §E.2's order is a
 * function of the running flow's own tree and its own cascade, so two flows that styled one document
 * differently have two orders and a cached one would be shared state the COW delta does not swap. The walk
 * derives everything per call and hands each offer to a visitor; it allocates only the per-step member arrays
 * it frees before returning. HOW A CACHE WOULD SHOW: an order that does not move when the running flow's tree
 * or cascade does.
 *
 * A CASCADE VALUE IS THE PAGE'S AND IS NEVER ASSERTED ON, which this file inherits whole from
 * stacking_order.h — every read here goes through that component's entries, which already state the one
 * invariant this engine may assert about a `z-index` (its GRAMMAR, because lexbor rejects a nonconforming
 * declaration at parse time) and refuse to assert its MAGNITUDE. */
#ifndef ENGINE_HOST_BROWSER_CORE_PAINT_PAINT_ORDER_H
#define ENGINE_HOST_BROWSER_CORE_PAINT_PAINT_ORDER_H

#include <stdbool.h>

#include <lexbor/dom/dom.h>

#include "quickjs.h"

/* THE CSS 2.1 §E.2 STEP AN OFFER IS BEING MADE UNDER — which of CSS 2.1 §E.2's ten steps put this box's marks here, so a
   visitor can perform that step's own sub-list and cite it.
   THE VALUES ARE NOT CSS 2.1 §E.2's STEP NUMBERS AND THE ENUM'S ORDER IS NOT THE PAINT ORDER, which is the one thing a
   reader arriving from stacking_order.h will assume, because that file's enum IS CSS 2.1 §9.9.1's list in CSS 2.1 §9.9.1's
   order. It cannot be so here: CSS 2.1 §E.2's steps 3, 5, 8 and 9 are RECURSIONS into another stacking context or into
   a step-5/step-8 pseudo-context, so the offers they produce are the offers of the steps INSIDE them, and the
   sequence a visitor sees is therefore not monotone in this enum. Two members also share one step number
   (CSS 2.1 §E.2's 7.1 and 7.2), which a numbered enum could not spell. The CSS 2.1 §E.2 coordinate is on each row instead. */
typedef enum {
    PAINT_STEP_ROOT_BACKGROUND,   /* CSS 2.1 §E.2 step 1   — the root element's background, over the entire canvas */
    PAINT_STEP_CONTEXT_BOX,       /* CSS 2.1 §E.2 step 2   — the context element's own background and border */
    PAINT_STEP_DESCENDANT_BOX,    /* CSS 2.1 §E.2 step 4   — an in-flow non-positioned block-level descendant's */
    PAINT_STEP_INLINE_LINE_BOXES, /* CSS 2.1 §E.2 step 6   — the line boxes an inline stacking context is in */
    PAINT_STEP_REPLACED_CONTENT,  /* CSS 2.1 §E.2 step 7.1 — a block-level replaced element's content, atomically */
    PAINT_STEP_LINE_BOXES,        /* CSS 2.1 §E.2 step 7.2 — the line boxes of a block-level box */
    PAINT_STEP_OUTLINES           /* CSS 2.1 §E.2 step 10  — see the residual: NOT offered at this revision */
} PaintStep;

/* ONE OFFER. Returns whether the walk should go on; `false` stops it where it stands, and everything already
   offered stays offered — which is what makes a consumer that meets an operand it cannot compute able to keep
   its prefix instead of losing the run.
   `el` IS ALWAYS AN ELEMENT OF THE DOCUMENT THE WALK WAS STARTED IN and always generates a box; both are
   asserted at the walk rather than left to each visitor to re-establish. */
typedef bool (*PaintOrderVisit)(PaintStep step, lxb_dom_element_t *el, void *user);

/* CSS 2.1 §E.2's PAINTING ORDER FOR ONE STACKING CONTEXT, offered one box at a time, in CSS 2.1 §E.2's own sequence. Answers
   true when the walk ran to the end and false when the visitor stopped it.
   `context_el` MUST FORM A STACKING CONTEXT — CSS 2.1 §E.2's whole text is "The painting order for the descendants of
   an element generating a stacking context (see the 'z-index' property) is:", so an element that forms none
   has no painting order of its own and asking for one is asking a question CSS 2.1 §E.2 does not answer. It crashes
   rather than walking the subtree anyway, because the boxes under a non-context element belong to whichever
   context it is IN and offering them here would put them in a sequence twice.
   THE WALK RECURSES and every recursion is one of CSS 2.1 §E.2's own: step 3, step 8's `z-index: 0` arm and step 9
   enter a CHILD STACKING CONTEXT, which CSS 2.1 §9.9.1 makes atomic, and step 5 and step 8's `z-index: auto` arm enter
   a PSEUDO-CONTEXT — CSS 2.1 §E.2's "treat the element as if it created a new stacking context, but any positioned
   descendants and descendants which actually create a new stacking context should be considered part of the
   parent stacking context, not this new one", which is the sentence CSS 2.1 §E.2 states twice and which is the whole
   difference between the two kinds. It terminates because every recursion descends. */
bool paint_order_walk(JSContext *ctx, lxb_dom_element_t *context_el, PaintOrderVisit visit, void *user);

/* NAMED RESIDUAL — CSS 2.1 §E.2's SUB-LIST ENUMERATIONS, WHICH ARE ORDER AND ARE THEREFORE THIS COMPONENT'S.
   WHAT IS NOT COVERED: three enumerations inside CSS 2.1 §E.2's steps, each offered to the visitor as a single box and
   none of them sequenced here. (a) The TABLE arm of steps 2 and 4 — "table backgrounds (color then image)",
   then column group, column, row group, row and cell backgrounds, then "all table borders (in tree order for
   separated borders)" — which is a walk over a table's internal boxes. (b) Step 7.2's line boxes and the
   boxes inside each of them, which is where every run of text in a document is placed and which carries its
   own recursion ("Otherwise, jump to 7.2.1 for that element"). (c) Step 6, which is (b) reached from an inline
   element that forms a stacking context. A visitor handed one of these offers must sequence it itself, and
   nothing here states that sequence for it.
   WHAT THE NEXT DIFF BUILDS: (a) first, because its operands exist — core/layout/table_grid.h and
   core/layout/table_box.h already enumerate a table's rows, row groups, columns and cells, and
   CSS 2.1 §17.5.1 "Table layers and transparency" states the same six levels CSS 2.1 §E.2 does, so it is a
   routing of an existing walk rather than a new one. (b) next, over core/layout/line_box.h's fragments.
   HOW ITS ABSENCE WOULD SHOW: two boxes whose marks interleave in CSS 2.1 §E.2 are offered as two whole boxes, so a
   consumer that paints each offer completely before the next lays a table's cell background over a row border
   that CSS 2.1 §17.5.1 puts on top of it, and lays a line's text under a background belonging to a box later in tree
   order. It is observable as ink from one box covering ink from another with no `z-index` between them.
   RETIREMENT: this record loses a clause as each of the three enumerations lands here, and goes when
   `paint_order_walk` offers no box whose CSS 2.1 §E.2 sub-list is an enumeration it has not sequenced.

   NAMED RESIDUAL — CSS 2.1 §E.2 STEP 10's OUTLINES, WHICH ARE BLOCKED ON A PROPERTY AND NOT ON AN ORDER.
   WHAT IS NOT COVERED: `PAINT_STEP_OUTLINES` is declared and never offered, so nothing this component
   sequences draws an outline. CSS 2.1 §E.2 step 10 is "Finally, implementations that do not draw outlines in steps
   above must draw outlines from this stacking context at this stage. (It is recommended to draw outlines in
   this step and not in the steps above.)", and offering it would be offering a step whose ink cannot be
   computed: `outline-color`, `outline-style` and `outline-width` are in NO registry this engine reads —
   lexbor's carries none of them and core/css/css_style_declaration.c's CSSD_INITIAL_UNREGISTERED has no row
   for them — so the cascade answers nothing for every element on every page, and an engine that offered the
   step would be unable to tell a page that declared no outline from one whose outline it cannot read.
   WHAT THE NEXT DIFF BUILDS: the three longhands, by the route core/css/css_computed_value.c's own DCHECK
   names for exactly this pair of properties — the `outline` shorthand's row in css_shorthand.c's table, the
   longhands in `css_shorthand_complete_for`, and an initial value each in css_style_declaration.c's
   CSSD_INITIAL_UNREGISTERED, which is the three steps the four `border-*-width` longhands already went
   through. css-ui-4 §3.1 "Outlines Shorthand: the outline property" is the shorthand and its `invert` keyword
   is the one value with no colour behind it.
   HOW ITS ABSENCE WOULD SHOW: a document renders with no outline anywhere — no focus ring on a focused
   control, and no ink at all from a rule that declares one — while every other mark of that box is present,
   so the box is drawn and only its outermost ring is missing.
   RETIREMENT: this record goes when `paint_order_walk` offers `PAINT_STEP_OUTLINES`. */

#endif
