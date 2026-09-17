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
 * whatever lays ink. CSS 2.1 §E.2's step 2 TABLE arm and its step 7.2 are ENUMERATIONS, and an enumeration is order
 * and is therefore this component's. The TABLE arm is sequenced here; step 7.2's line boxes are a named
 * residual at the foot of this header, with what builds them.
 *
 * THE TABLE ARM IS SEVEN ITEMS OF WHICH SIX ARE BACKGROUNDS, and both numbers are written down because one
 * sentence carrying a count and the list it counts is where the two drift. CSS 2.1 §E.2 lists "table backgrounds
 * (color then image)", then column group, column, row group, row and cell backgrounds — SIX levels, of which
 * the TABLE's own is the first — and then a seventh item, "all table borders (in tree order for separated
 * borders)". CSS 2.1 §17.5.1 "Table layers and transparency" states the same six as "six superimposed layers",
 * bottom-up, in that order: the table box, the column groups, the columns, the row groups, the rows, the cells.
 * MEASURED by diffing CSS 2.1 §E.2's two table arms item by item rather than by reading either of them: they
 * differ in EXACTLY ONE item — step 2's item 1 carries "unless it is the root element" and step 4's does not —
 * and items 2 through 7 are byte-identical, which is why one `PaintStep` serves both arms for every item but
 * the first and two serve the first.
 *
 * WHY STEP 10 IS A RESIDUAL AND A TABLE IS NOT, WHICH IS THE ONE PLACE THIS FILE MAKES A JUDGEMENT. CLAUDE.md's
 * test is whether the code is WRONG or merely NARROWER. Omitting CSS 2.1 §E.2's step 10 leaves a prefix that IS CSS 2.1 §E.2's
 * first nine steps — every offer this component made was made at the position CSS 2.1 §E.2 puts it, and the outlines
 * are simply not drawn — so it is narrower and it is named. Omitting a TABLE in the middle of step 4 would
 * leave a sequence that CLAIMS to be steps 1 through 9 and is not one: the consumer would paint every other
 * box and never learn that a box was passed over. That is wrong rather than narrow, so a table arm is offered
 * as CSS 2.1 §E.2's own seven items and never as a silent skip; what is left inside item 7 is named below.
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
    PAINT_STEP_CONTEXT_BOX,       /* CSS 2.1 §E.2 step 2's BLOCK arm — the context element's background and border */
    PAINT_STEP_DESCENDANT_BOX,    /* CSS 2.1 §E.2 step 4's BLOCK arm — an in-flow non-positioned block-level one's */
    /* CSS 2.1 §E.2's TABLE ARM, item by item. The arm's SEVEN items are offered as seven calls rather than as one,
       for the reason this header's opening paragraph gives about boxes: the table box contributes item 1 AND
       item 7 with five other boxes' marks BETWEEN them, so a consumer that painted one offer completely before
       taking the next would lay a cell background over a row border CSS 2.1 §17.5.1 "Table layers and transparency"
       puts beneath it. The arm applies to a `display: table` box only — CSS 2.1 §E.2's step 2 arm reads "block level
       table" and its step 4 arm is inside a walk of block-level descendants, and CSS 2.1 §9.2.1 "Block-level elements
       and block boxes" closes that list at "'block', 'list-item', and 'table'". An `inline-table` therefore
       takes neither, exactly as it takes neither arm of step 2 today. */
    PAINT_STEP_CONTEXT_TABLE_BACKGROUND,    /* step 2's TABLE arm item 1 — "table backgrounds (color then image)
                                               unless it is the root element" */
    PAINT_STEP_DESCENDANT_TABLE_BACKGROUND, /* step 4's TABLE arm item 1 — the same without the root clause,
                                               which is the ONLY item the two arms spell differently */
    PAINT_STEP_COLUMN_GROUP_BACKGROUND,     /* item 2 — one 'table-column-group' box */
    PAINT_STEP_COLUMN_BACKGROUND,           /* item 3 — one 'table-column' box */
    PAINT_STEP_ROW_GROUP_BACKGROUND,        /* item 4 — one row group box */
    PAINT_STEP_ROW_BACKGROUND,              /* item 5 — one 'table-row' box */
    PAINT_STEP_CELL_BACKGROUND,             /* item 6 — one 'table-cell' box */
    /* item 7 — "all table borders (in tree order for separated borders)", offered ONCE carrying the TABLE
       element. It is the item WHOLE and not the table's own border: the borders of the table and of every
       internal box inside it are one item of CSS 2.1 §E.2's list, laid after all six background levels. Which boxes
       and in what order is the residual at the foot of this header. */
    PAINT_STEP_TABLE_BORDERS,
    PAINT_STEP_INLINE_LINE_BOXES, /* CSS 2.1 §E.2 step 6   — the line boxes an inline stacking context is in */
    PAINT_STEP_REPLACED_CONTENT,  /* CSS 2.1 §E.2 step 7.1 — a block-level replaced element's content, atomically */
    PAINT_STEP_LINE_BOXES,        /* CSS 2.1 §E.2 step 7.2 — the line boxes of a block-level box */
    PAINT_STEP_OUTLINES           /* CSS 2.1 §E.2 step 10  — see the residual: NOT offered at this revision */
} PaintStep;

/* ONE OFFER. Returns whether the walk should go on; `false` stops it where it stands, and everything already
   offered stays offered — which is what makes a consumer that meets an operand it cannot compute able to keep
   its prefix instead of losing the run.
   `el` IS ALWAYS AN ELEMENT OF THE DOCUMENT THE WALK WAS STARTED IN, and the two halves of the walk guarantee
   that DIFFERENTLY, which is why this sentence names both rather than claiming one mechanism. The boxes
   CSS 2.1 §E.2's own steps collect come from core/dom/shadow_root.h's shadow-including walker, whose only edges
   are a node's children and an element's shadow root, so they cannot leave the Document BY CONSTRUCTION — the
   paragraph above on a child navigable is that argument. The boxes of CSS 2.1 §E.2's TABLE arm come from
   core/layout/table_box.h and core/layout/table_column_box.h, which are OTHER components' walks, so that half
   is ASSERTED at each offer instead.
   `el` DOES NOT ALWAYS GENERATE A BOX THAT IS PAINTED AS A BOX, AND THE TABLE ARM IS WHY. CSS 2.1 §17.2 "The CSS
   table model" says "Elements with 'display' set to 'table-column' or 'table-column-group' are not rendered
   (exactly as if they had 'display: none'), but they are useful, because they may have attributes which induce
   a certain style for the columns they represent", and CSS 2.1 §17.5.1 "Table layers and transparency"
   nevertheless gives both a background LAYER. The two sentences do not conflict: such a box EXISTS, occupies
   grid cells (CSS 2.1 §17.5 "Visual layout of table contents"' rules 3 and 4), is not painted as an ordinary box,
   and its background is a level of its TABLE's sequence. That is the whole reason this component must
   enumerate the level — a column box is reachable from no other step of CSS 2.1 §E.2 at all, so a consumer handed
   only the boxes CSS 2.1 §E.2's steps collect could never learn that one exists. */
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

/* NAMED RESIDUAL — CSS 2.1 §E.2's LINE-BOX ENUMERATIONS, WHICH ARE ORDER AND ARE THEREFORE THIS COMPONENT'S.
   WHAT IS NOT COVERED: two enumerations inside CSS 2.1 §E.2's steps, each offered to the visitor as a single box and
   neither sequenced here. (b) Step 7.2's line boxes and the boxes inside each of them, which is where every
   run of text in a document is placed and which carries its own recursion ("Otherwise, jump to 7.2.1 for that
   element"). (c) Step 6, which is (b) reached from an inline element that forms a stacking context. A visitor
   handed one of these offers must sequence it itself, and nothing here states that sequence for it. The
   lettering is (b) and (c) because (a), the TABLE arm, has landed and its clause is retired below.
   WHAT THE NEXT DIFF BUILDS: (b), over core/layout/line_box.h's fragments.
   HOW ITS ABSENCE WOULD SHOW: two boxes whose marks interleave in CSS 2.1 §E.2 are offered as two whole boxes, so a
   consumer that paints each offer completely before the next lays a line's text under a background belonging
   to a box later in tree order. It is observable as ink from one box covering ink from another with no
   `z-index` between them.
   RETIREMENT: this record loses a clause as each remaining enumeration lands here, and goes when
   `paint_order_walk` offers no box whose CSS 2.1 §E.2 sub-list is an enumeration it has not sequenced.

   RETIRED CLAUSE (a), KEPT BECAUSE ITS NEXT-DIFF HALF WAS WRONG AND A READER WHO RE-DERIVES IT WILL BE WRONG
   THE SAME WAY. It called the table arm a routing of an existing walk rather than a new one, over
   core/layout/table_grid.h and core/layout/table_box.h, which it said already enumerate a table's rows, row
   groups, columns and cells. Its SPEC half was exact: CSS 2.1 §17.5.1 "Table layers and transparency" does state
   the same six levels in the same order, and both sections were fetched before this was written. Its TREE
   half named the wrong files for two of the six. core/layout/table_grid.h says in its own header that it does
   NOT place the column and column-group boxes, gives CSS 2.1 §17.2 "The CSS table model"'s not-rendered sentence
   as the reason, calls what is wanted a mapping onto the column indices it already numbers, and hands that
   mapping to core/layout/table_column_box.h — which the clause never names. So the arm is a routing of THREE
   components' walks and not two, and a reader who had reached for `table_grid.h` would have found rows and
   cells there and no column box at any entry. This is the shape CLAUDE.md rates as the commonest: the spec
   half is a claim about a document anyone can fetch, and the remedy half is a claim about THIS TREE written by
   someone who knew what was missing and was guessing at what fills it.
   RETIREMENT: this record goes when no residual in this file names a component without having grepped its
   entries, which is not a state a diff can reach — so it goes instead when the line-box clauses above land and
   this whole block is rewritten around what they turn out to have got wrong.

   NAMED RESIDUAL — CSS 2.1 §E.2's TABLE ARM ITEM 7, WHOSE ORDER CSS 2.1 STATES FOR ONE BORDER MODEL AND NOT THE OTHER.
   WHAT IS NOT COVERED: `PAINT_STEP_TABLE_BORDERS` is offered ONCE, carrying the table element, and the boxes
   whose borders that item covers are not enumerated. CSS 2.1 §E.2's item is "all table borders (in tree order for
   separated borders)", so the item is every border of the table and of every internal box inside it, and the
   parenthesis states an order for the SEPARATED model only. CSS 2.1 §17.6.2 "The collapsing border model" resolves
   which border WINS at each edge and states no painting order over the boxes, so for a collapsing table
   CSS 2.1 states no order here at all and this component may not invent one.
   WHAT THE NEXT DIFF BUILDS: the separated-model half, which is the half CSS 2.1 answers — a tree-order walk of
   the table's own subtree, which is `po_next` and needs no new walker, gated on the computed `border-collapse`
   being `separate`. The collapsing half is a SECOND diff and its first question is whether CSS 2.1 §17.6.2's
   conflict resolution, which core/layout/table_border_collapse.h already runs, leaves an order over EDGES
   rather than over boxes — in which case item 7 for a collapsing table is not an enumeration of boxes and this
   residual is asking the wrong question of it.
   HOW ITS ABSENCE WOULD BE OBSERVED: a consumer that lays every border of one offer before taking the next
   draws a table with `border-collapse: separate` in whatever order it reaches the boxes, so two cells whose
   borders touch draw in an order no rule fixed — visible where the two borders differ in colour or width, and
   never as a crash.
   RETIREMENT: this record loses its first clause when the separated-model walk lands and goes when
   `PAINT_STEP_TABLE_BORDERS` names a box rather than the table.

   NAMED RESIDUAL — A TABLE BOX CSS 2.1 §17.2.1 "Anonymous table objects" GENERATES HAS NO ELEMENT TO BE OFFERED AS.
   WHAT IS NOT COVERED: the table arm is reached from an ELEMENT whose computed `display` generates a table
   box, so a table box CSS 2.1 §17.2.1's third stage generates — around a `table-row` or a `table-cell` sitting in a
   `<div>` — is never offered and its six levels are never enumerated. The internal boxes inside such a table
   are then offered by nobody at all: CSS 2.1 §E.2's step 4 passes over an internal table box precisely because
   its table's own offer covers it (see `po_is_internal_table_box`), and here there is no such offer to cover
   it. The ANONYMOUS table box itself contributes no ink either way — CSS 2.1 §9.2.1.1 "Anonymous block boxes" says
   "The properties of anonymous boxes are inherited from the enclosing non-anonymous box … Non-inherited
   properties have their initial value", and `background-color`, `background-image` and `border-style` are all
   non-inherited — so what is lost is the background of the ELEMENTS inside it and never the box's own.
   WHAT THE NEXT DIFF BUILDS: CSS 2.1 §17.2.1's third stage, "Generate missing parents", which
   core/layout/table_box.h declines by name and assigns to the BLOCK walk's own child list
   (core/layout/block_flow.c) because its subject is a child list that is not a table's. That stage is what
   would give such a box an identity for this walk to offer, and until it exists there is nothing here to
   route to.
   HOW ITS ABSENCE WOULD BE OBSERVED: a `<div style="display: table-cell; background: red">` with no table
   ancestor renders with no background at all, while the byte-identical markup inside a `<table>` renders it —
   so the same declaration paints or does not paint depending on an ancestor that declares nothing.
   RETIREMENT: this record goes when the table arm is reachable for a table box no element generates.

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
