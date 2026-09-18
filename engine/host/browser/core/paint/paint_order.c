/* CSS 2.1 §E.2 "Painting order" — the sequence a stacking context's boxes are offered in. See paint_order.h
   for the contract, for why CSS 2.1 §E.2's top-level order can be complete while almost none of the ink exists, and
   for the residuals that name what this file still does not sequence. The count is not written down here
   because it moves as each one lands and the list is one screen away in the header. */
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include <lexbor/dom/dom.h>

#include "check.h"
#include "core/css/css_computed_value.h"
#include "core/dom/shadow_root.h"
#include "core/layout/box_subject.h"
#include "core/layout/replaced_element.h"
#include "core/layout/table_box.h"
#include "core/layout/table_column_box.h"
#include "core/paint/paint_order.h"
#include "core/paint/stacking_order.h"

/* THE COMPUTED `display`, with the one invariant this file may assert about it — the same one
   core/paint/stacking_order.c asserts for every keyword property it reads, and for the same reason: `display`
   is in lexbor's property registry with an initial value, so CSS Cascade §7's last defaulting layer always
   answers and an absence would be this engine's pipeline rather than the document's. */
static char *po_display(lxb_dom_element_t *el)
{
    char *v = css_computed_value(el, "display");

    DCHECK(v != NULL, "the cascade produced no computed `display` — the property is in lexbor's registry with "
                      "an initial value, so CSS Cascade §7's last layer cannot come back empty");
    return v;
}

/* css-display-3 §2.5 "Box Generation: the none and contents keywords"'s THREE answers, which CSS 2.1 §E.2 needs kept
   apart where core/paint/stacking_order.c could fold two of them together. CSS 2.1 §9.9.1's layers only ever asked
   whether a box EXISTS; a walk asks what to do with the element's CHILDREN, and `none` and `contents` answer
   that oppositely — `none` generates no box for the element or its descendants, while with `contents` "The
   element itself does not generate any boxes, but its children and pseudo-elements still generate boxes and
   text sequences as normal". So a `none` subtree is skipped whole and a `contents` element is stepped over
   with its subtree still walked. */
typedef enum { PO_BOX_NONE, PO_BOX_CONTENTS, PO_BOX_YES } PoBoxGen;

static PoBoxGen po_box_generation(lxb_dom_element_t *el)
{
    char *d = po_display(el);
    PoBoxGen gen = PO_BOX_YES;

    if (strcmp(d, "none") == 0) gen = PO_BOX_NONE;
    else if (strcmp(d, "contents") == 0) gen = PO_BOX_CONTENTS;
    free(d);
    return gen;
}

/* IS THIS ONE OF CSS 2.1 §17.2 "The CSS table model"'s INTERNAL TABLE BOXES — "A 'table-cell' box,
   'table-row' box, row group box, 'table-column' box, or 'table-column-group' box"?
   THIS IS WHERE CSS 2.1 §E.2 IS FINER THAN CSS 2.1 §9.9.1 AND WHERE core/paint/stacking_order.h HANDS THE
   QUESTION OVER. That file puts a cell, a row, a row group, a column and a column group in CSS 2.1 §9.9.1's
   layer 3 and says why in its own words — the seven layers name no table box at all, so a table-internal box
   is an in-flow non-inline-level one and "the finer order is §E.2's to state when it exists". This
   is that statement. CSS 2.1 §E.2's step 4 is stated over "block-level" descendants and CSS 2.1 §9.2.1
   "Block-level elements and block boxes" closes that list: "The following values of the 'display' property
   make an element block-level: 'block', 'list-item', and 'table'." A cell is none of them, and what paints its
   background is the TABLE's own offer — CSS 2.1 §E.2's step 4 table arm, whose "cell backgrounds (color then
   image)" is one of six levels inside that one offer. Reporting a cell at step 4 as well would offer its
   background TWICE.
   A CAPTION IS NOT INTERNAL AND IS REPORTED, which is the same section's own vocabulary rather than a
   judgement here: CSS 2.1 §17.2's internal-table-box list does not contain it, and CSS 2.1 §17.4 "Tables in
   the visual formatting model" renders a caption "as normal block boxes inside the table wrapper box", so it
   is a box CSS 2.1 §E.2's step 4 block arm answers for like any other.
   THE SUBTREE IS STILL WALKED. A cell's own in-flow block-level descendants are descendants of the same
   stacking context and CSS 2.1 §E.2's step 4 says "all" of them; only the internal box itself is passed
   over. */
static bool po_is_internal_table_box(lxb_dom_element_t *el)
{
    char *d = po_display(el);
    bool internal = table_box_kind_is_internal(table_box_kind(d));

    free(d);
    return internal;
}

/* IS THIS BOX IN ONE OF CSS 2.1 §9.9.1 "Specifying the stack level: the 'z-index' property"'s THREE POSITIONED
   LAYERS — asked of a layer already read, because the two sites that ask it are a walk that has just read one
   and a table level that has not, and a second spelling of the test is the copy that drifts. It takes the
   LAYER rather than the element so the walk does not pay a second cascade read per box. */
static bool po_layer_is_positioned(StackingLayer layer)
{
    return layer == STACKING_LAYER_NEGATIVE || layer == STACKING_LAYER_LEVEL_ZERO ||
           layer == STACKING_LAYER_POSITIVE;
}

/* "THE ROOT ELEMENT" — the element whose parent is the Document, which core/paint/stacking_order.c spells the
   same way and for the reason it states there: the two are one sentence, a third spelling would be the one
   that drifts, and this one routes to that file's when either is exported. */
static bool po_is_root_element(const lxb_dom_node_t *n)
{
    return n->parent != NULL && n->parent->type == LXB_DOM_NODE_TYPE_DOCUMENT;
}

/* IS THIS BOX INLINE-LEVEL — asked through core/paint/stacking_order.h's own partition rather than through a
   second list of `display` values here. CSS 2.1 §9.9.1's layer 5 IS the inline-level half ("the in-flow, inline-level,
   non-positioned descendants, including inline tables and inline blocks") and its layer 3 is the block-level
   half, so a box that classifies into layer 5 is inline-level and one that classifies into layer 3 is not.
   A SECOND LIST WOULD BE THE COPY THAT DRIFTS: `so_is_inline_level` already crashes by name for a computed
   `display` css-display-3 §2's two-value grammar admits and this engine has never classified, and a list
   written here would answer for such a value instead of reaching that crash. The cost is that only the
   NON-POSITIONED, non-floating box can be asked this way, which is exactly the population CSS 2.1 §E.2's steps 4, 5,
   6 and 7 are stated over; the positioned half never needs the question, because CSS 2.1 §E.2's steps 3, 8 and 9 sort
   it by stack level and never by box type. */
static bool po_is_inline_level(lxb_dom_element_t *el)
{
    /* THE ROOT ELEMENT IS ANSWERED HERE AND NOT BY THE PARTITION, because CSS 2.1 §9.9.1's layers are a
       partition of a context's DESCENDANTS and core/paint/stacking_order.c crashes by name when asked for the root's — "the
       root forms the root stacking context, so there is no context above it holding a list for it to be in".
       The root is nevertheless a box CSS 2.1 §E.2's steps 2 and 7 are stated over, because the root element IS
       a stacking context and CSS 2.1 §E.2 walks it like any other. It cannot be inline-level: CSS 2.1 §9.7
       "Relationships between 'display', 'position', and 'float'" has a bullet for exactly this element —
       "Otherwise, if the element is the root element, 'display' is set according to the table below, except
       that it is undefined in CSS 2.1 whether a specified value of 'list-item' becomes a computed value of
       'block' or 'list-item'" — whose table maps `inline`, `inline-block` and every table-internal value to
       `block` and `inline-table` to `table`, and css-display-3 §2.7 "Automatic Box Type Transformations"
       blockifies the values CSS 2.1's table predates. core/css/css_computed_value.c performs that
       transformation, which is why this is a derivation and not a second list of `display` values. */
    if (po_is_root_element(lxb_dom_interface_node(el))) return false;
    return stacking_layer_of(el) == STACKING_LAYER_INLINE;
}

/* CSS 2.1 §E.2 STEP 7.2.1's ITEM 4 CLASSIFICATION — see core/paint/paint_order.h for which three lists that
   item holds, why they are one question rather than three predicates, and why the replaced test is asked of
   the ELEMENT rather than of its `display`. What is here is only the derivation.
   A NON-REPLACED `display: inline` BOX IS THE ONE §E.2 WALKS THROUGH: its item 4 reaches "all the element's
   in-flow, non-positioned, inline-level children that are in this line box, and all runs of text inside the
   element that is on this line box, in tree order", which is a walk into the box and not a mark for it. */
PaintInlineKind paint_order_inline_kind(lxb_dom_element_t *el)
{
    char *d;
    bool atomic;

    if (!po_is_inline_level(el)) return PAINT_INLINE_NONE;
    if (replaced_element_of(el).replaced) return PAINT_INLINE_ATOMIC;
    d = po_display(el);
    atomic = strcmp(d, "inline") != 0;
    free(d);
    return atomic ? PAINT_INLINE_ATOMIC : PAINT_INLINE_NON_ATOMIC;
}

/* ---- the member lists CSS 2.1 §E.2's steps are stated over ---------------------------------------------------- */

typedef struct {
    lxb_dom_element_t **v;
    size_t n, cap;
} PoList;

static void po_push(PoList *l, lxb_dom_element_t *el)
{
    if (l->n == l->cap) {
        size_t cap = l->cap ? l->cap * 2 : 8;
        lxb_dom_element_t **v = realloc(l->v, cap * sizeof *v);

        CHECK(v != NULL, "paint: OOM growing a CSS 2.1 §E.2 step's member list — a dropped member is a box "
                         "whose marks are never offered, which is a silently incomplete painting order");
        l->v = v;
        l->cap = cap;
    }
    l->v[l->n++] = el;
}

static void po_list_free(PoList *l)
{
    free(l->v);
    l->v = NULL;
    l->n = l->cap = 0;
}

/* ONE STEP OF Appendix E §E.1's TREE ORDER — "Preorder depth-first traversal of the rendering tree, in logical
   (not visual) order for bidirectional content, after taking into account properties that move boxes around" —
   with the option to pass over the subtree of the node just visited.
   IT IS THE CANONICAL WALKER AND NOT AN INDEX COMPARISON, for the argument core/paint/stacking_order.c makes
   at its own tie-break: `shadow_root_next_in_shadow_including` is DOM §4.8 "Interface ShadowRoot"'s
   shadow-including tree order one step at a time, the rendering tree is the flattened one, and a second
   traversal written here would be free to disagree with it at a shadow boundary — the one place the two trees
   differ. THE SKIP IS SPELLED AS REPEATED STEPS for the same reason: advancing until the walk has left `n`'s
   shadow-including subtree asks `shadow_root_is_shadow_including_inclusive_ancestor`, which is DOM §4.8's own
   containment relation, where a next-sibling shortcut would be a third idea of the same tree. The cost is a
   walk of the skipped subtree, paid once per skipped box.
   A CHILD NAVIGABLE IS NOT REACHABLE FROM HERE AND THAT IS THE POINT: both edges this walker has are a node's
   children and an element's shadow root, and an `iframe`'s content Document hangs off the element's WRAPPER
   (core/html/html_iframe.h) rather than off the node, so CSS 2.1 §E.2's "one document's tree" is a property of the
   walker rather than a rule this file enforces. */
static lxb_dom_node_t *po_next(JSContext *ctx, lxb_dom_node_t *n, lxb_dom_node_t *root, bool skip_subtree)
{
    lxb_dom_node_t *m = shadow_root_next_in_shadow_including(ctx, n, root);

    if (!skip_subtree) return m;
    while (m != NULL && shadow_root_is_shadow_including_inclusive_ancestor(n, m))
        m = shadow_root_next_in_shadow_including(ctx, m, root);
    return m;
}

/* WHICH OF CSS 2.1 §E.2's THREE DESCENDANT SETS IS BEING COLLECTED. They are three walks and not one filtered walk,
   because they DESCEND DIFFERENTLY and the difference is the whole of what CSS 2.1 §E.2 says twice. The positioned set
   walks THROUGH a float and through a `z-index: auto` positioned box, because CSS 2.1 §E.2's steps 5 and 8 both say
   "any positioned descendants and descendants which actually create a new stacking context should be
   considered part of the parent stacking context, not this new one"; the other two sets STOP at the same
   boxes, because those boxes' interiors are painted in the pseudo-context CSS 2.1 §E.2 tells you to treat them as. A
   single walk with a predicate on top would have to answer both, and it would answer whichever one its author
   had in mind. */
typedef enum {
    PO_SET_IN_FLOW_BLOCK,  /* CSS 2.1 §E.2 steps 4 and 7: "all its in-flow, non-positioned, block-level descendants" */
    PO_SET_FLOAT,          /* CSS 2.1 §E.2 step 5: "All non-positioned floating descendants, in tree order" */
    PO_SET_POSITIONED      /* CSS 2.1 §E.2 steps 3, 8 and 9: every positioned descendant, for the caller to split */
} PoSet;

static void po_collect(JSContext *ctx, lxb_dom_element_t *context_el, PoSet set, PoList *out)
{
    lxb_dom_node_t *root = lxb_dom_interface_node(context_el);
    lxb_dom_node_t *n;

    for (n = shadow_root_next_in_shadow_including(ctx, root, root); n != NULL; ) {
        lxb_dom_element_t *el;
        StackingLayer layer;
        PoBoxGen gen;
        bool forms, positioned, report = false, skip = false;

        if (n->type != LXB_DOM_NODE_TYPE_ELEMENT) {
            n = po_next(ctx, n, root, false);
            continue;
        }
        el = lxb_dom_interface_element(n);
        gen = po_box_generation(el);
        if (gen != PO_BOX_YES) {
            /* `none` takes its whole subtree with it; `contents` keeps its children's boxes and has none of
               its own, so there is nothing here for any of CSS 2.1 §E.2's steps to place. */
            n = po_next(ctx, n, root, gen == PO_BOX_NONE);
            continue;
        }

        forms = stacking_context_forms(el);
        layer = stacking_layer_of(el);
        positioned = po_layer_is_positioned(layer);
        DCHECK(!forms || positioned,
               "CSS 2.1 §9.9.1 makes every stacking context below the root a POSITIONED box — \"Other stacking "
               "contexts are generated by any positioned element (including relatively positioned elements) "
               "having a computed value of 'z-index' other than 'auto'\", and css-position-3 §2.2 \"Painting "
               "Order and Stacking Contexts\" amends only that sentence's `auto` arm — so a descendant that "
               "forms one and is not in one of CSS 2.1 §9.9.1's three positioned layers is the two classifications "
               "disagreeing, and the fix is at whichever of them is wrong rather than a member placed here");

        switch (set) {
        case PO_SET_POSITIONED:
            /* A real child stacking context is ATOMIC (CSS 2.1 §9.9.1: "boxes in other stacking contexts may not come
               between any of its boxes"), so its interior is its own walk and this one stops. A positioned box
               that forms none is a step-8 pseudo-context and this walk CONTINUES THROUGH IT, which is CSS 2.1 §E.2's
               step 8 in its own words. */
            report = positioned;
            skip = forms;
            break;
        case PO_SET_IN_FLOW_BLOCK:
            if (forms || positioned || layer == STACKING_LAYER_FLOAT) skip = true;
            else if (layer == STACKING_LAYER_INLINE) skip = paint_order_inline_kind(el) == PAINT_INLINE_ATOMIC;
            /* CSS 2.1 §9.9.1's layer 3 is "in-flow, non-positioned" and non-inline-level, which is the
               COARSER bucket: CSS 2.1 §E.2's step 4 says "block-level", and CSS 2.1 §17.2's internal table
               boxes are neither that nor a layer of their own. See po_is_internal_table_box. */
            else report = !po_is_internal_table_box(el);
            break;
        case PO_SET_FLOAT:
            if (forms || positioned) skip = true;
            else if (layer == STACKING_LAYER_FLOAT) { report = true; skip = true; }
            else if (layer == STACKING_LAYER_INLINE) skip = paint_order_inline_kind(el) == PAINT_INLINE_ATOMIC;
            break;
        }
        if (report) po_push(out, el);
        n = po_next(ctx, n, root, skip);
    }
}

/* CSS 2.1 §E.2's "in z-index order (most negative first) then tree order" and "in z-index order (smallest first) then
   tree order" — one direction stated twice, over a list `po_collect` already built in tree order. The sort is
   INSERTION and therefore STABLE, which is what makes the second half of each phrase free: equal stack levels
   keep the order they were collected in, and that order is Appendix E §E.1's tree order. A comparison sort
   that reordered equal members would need tree order re-derived as a tie-break, which is the walk of the
   context's subtree core/paint/stacking_order.c pays per tie. */
static void po_sort_by_level(PoList *l)
{
    size_t i, j;

    for (i = 1; i < l->n; i++) {
        lxb_dom_element_t *key = l->v[i];
        int kv = stacking_level(key);

        for (j = i; j > 0 && stacking_level(l->v[j - 1]) > kv; j--)
            l->v[j] = l->v[j - 1];
        l->v[j] = key;
    }
}

/* ---- the walk ------------------------------------------------------------------------------------------ */

static bool po_walk(JSContext *ctx, lxb_dom_element_t *ce, bool pseudo, PaintOrderVisit visit, void *user);

/* CSS 2.1 §E.2's step 7, for ONE of the boxes step 7 names: "If the element is a block-level replaced element, then:
   the replaced content, atomically." against "Otherwise, for each line box of that element:". The two are one
   test with two answers and they are offered as two steps because they are two different sub-lists — the
   first is a single atomic mark and the second is an enumeration this file does not yet sequence. */
static bool po_offer_content(lxb_dom_element_t *el, PaintOrderVisit visit, void *user)
{
    bool replaced = replaced_element_of(el).replaced && !po_is_inline_level(el);

    return visit(replaced ? PAINT_STEP_REPLACED_CONTENT : PAINT_STEP_LINE_BOXES, el, user);
}

/* ---- CSS 2.1 §E.2's TABLE ARM — the seven items of steps 2 and 4 that a `display: table` box takes --------- */

/* THE ONE TABLE THE ARM IS BEING RUN FOR — the table element, the visitor and its user pointer, which is three
   things and is written as a struct because every level below needs all three and a level that took two of
   them would be the one that drifts. */
typedef struct {
    lxb_dom_element_t *table;
    PaintOrderVisit visit;
    void *user;
} PoTableArm;

/* ONE BACKGROUND LEVEL of CSS 2.1 §E.2's table arm, for one internal box that an element names.
   THE DOCUMENT IS ASSERTED HERE AND NOWHERE ELSE IN THIS FILE, which is the split paint_order.h states at the
   visitor: every other offer's box comes from core/dom/shadow_root.h's walker, which cannot leave the
   Document by construction, and these come from core/layout/table_box.h and core/layout/table_column_box.h —
   other components' walks, whose containment is a fact about them rather than about this file.
   THE POSITIONED SKIP IS NOT A READING OF CSS 2.1 AND MUST NOT BE READ AS ONE. A positioned table-internal box
   is a box CSS 2.1 §17.5.1 "Table layers and transparency" places in this sequence and CSS 2.1 §9.9.1 "Specifying the
   stack level: the 'z-index' property" places in a positioned layer OUTSIDE it, and CSS 2.1 states no order for
   the pair — which is why `po_takes_step_2` ABORTS on exactly that box in a dev build, naming it. So in a dev
   build this skip is unreachable and decides nothing. What it decides is what the RELEASE arm of that abort
   leaves behind, and it is chosen so the two arms offer such a box EXACTLY ONCE between them: the release arm
   of `po_takes_step_2` goes on offering it at CSS 2.1 §E.2's step 3, 8 or 9 as it does today, so offering it here
   too would paint its background once UNDER the table's content and again OVER it, which is a worse answer
   than either arm alone and is one this diff would have introduced. The condition is `po_layer_is_positioned`
   and not a second test, because the box reaches those steps exactly when `po_collect` reports it positioned. */
static bool po_table_level(PoTableArm *a, lxb_dom_element_t *box, PaintStep step)
{
    char nbuf[160];

    DCHECK(box != NULL, "CSS 2.1 §E.2's table arm was handed a level with no element to offer — the two boxes "
                        "core/layout/table_box.h reports without one (an anonymous row, an anonymous cell) are "
                        "answered at the call site by CSS 2.1 §9.2.1.1 \"Anonymous block boxes\" and never reach here");
    DCHECKF(lxb_dom_interface_node(box)->owner_document ==
                lxb_dom_interface_node(a->table)->owner_document,
            "%s: a box offered inside CSS 2.1 §E.2's table arm belongs to a DIFFERENT Document from the table whose "
            "arm is being run. A display list carries text runs and URLs read out of a document's DOM, so the "
            "walk paint_order.h argues cannot leave one Document must not be able to leave it through a table "
            "either — and this half is asserted rather than argued because the boxes come from "
            "core/layout/table_box.h's and core/layout/table_column_box.h's walks and not from this file's",
            box_subject(box, nbuf, sizeof nbuf));
    if (po_layer_is_positioned(stacking_layer_of(box))) return true;
    return a->visit(step, box, a->user);
}

/* CSS 2.1 §E.2's TABLE ARM, item by item, in CSS 2.1 §E.2's own order. `background_step` is item 1's, which is the only
   item whose two arms differ — see the enum in paint_order.h.
   AN ANONYMOUS ROW OR CELL IS PASSED OVER AND THAT IS NOT A NARROWING, WHICH IS WHY IT IS A DERIVATION HERE
   AND NOT A RESIDUAL. core/layout/table_box.h reports a box CSS 2.1 §17.2.1 "Anonymous table objects"' second stage
   generated with `element == NULL`, which is its own positive statement that no element names it; and
   CSS 2.1 §9.2.1.1 "Anonymous block boxes" fixes what such a box is styled with — "The properties of anonymous
   boxes are inherited from the enclosing non-anonymous box … Non-inherited properties have their initial
   value" — so its `background-color` is the initial `transparent` and its `background-image` the initial
   `none`. There is no ink at that level to offer, and no selector can put any there, because an anonymous box
   has no element for a selector to match. A visitor that was handed one could therefore only be handed a NULL,
   which is the one thing paint_order.h's visitor contract says `el` is never. */
static bool po_offer_table_arm(PoTableArm *a, PaintStep background_step)
{
    TableColumnBoxMap map;
    TableBoxRow *rows = NULL;
    TableBoxRowGroup *groups = NULL;
    lxb_dom_element_t *prev;
    size_t nrows, ngroups, i, j, c;
    bool ok;

    /* CSS 2.1 §E.2's item 1 — "table backgrounds (color then image)". */
    ok = a->visit(background_step, a->table, a->user);

    /* THE GRID IS NOT BUILT AND `ngrid` IS 0 DELIBERATELY. core/layout/table_column_box.h sizes its map to the
       GREATER of the grid's column count and `noccupied`, and `noccupied` is "grid columns the column and
       column-group boxes together occupy" — so every column box this arm has to enumerate stands inside the
       first `noccupied` entries whatever the grid is, and the entries a grid would add beyond them hold no
       box by construction. Building core/layout/table_grid.h's grid to reach them would be a second walk over
       every cell of the table for a number this arm never reads. */
    table_column_boxes_build(a->table, 0, &map);
    DCHECK(map.ncols == map.noccupied,
           "core/layout/table_column_box.h sized its map to something other than what the column boxes occupy "
           "after being asked for a zero-column grid — its own contract is that `ncols` is the greater of the "
           "two, so the two disagreeing here means the arm is reading a map whose tail it cannot account for");
    nrows = table_box_rows(a->table, &rows);
    ngroups = table_box_row_groups(a->table, &groups);

    /* CSS 2.1 §E.2's item 2 — "column group backgrounds (color then image)", and item 3 — "column backgrounds
       (color then image)". The map is indexed by GRID COLUMN and a box occupying several appears in each, so
       each box is offered at the FIRST entry that names it. A run is contiguous and needs no set to prove it:
       core/layout/table_column_box.c places the boxes with an index that only ever moves forward, one span at
       a time, and asserts at its own write that no grid column is reached twice. The order is CSS 2.1 §17.5
       "Visual layout of table contents"' rule 3 — "Column boxes are placed next to each other in the order they
       occur" — which is the only order CSS 2.1 states over these boxes; CSS 2.1 §E.2 states none within a level. */
    for (prev = NULL, c = 0; ok && c < map.ncols; c++) {
        lxb_dom_element_t *g = map.cols[c].column_group;

        if (g != NULL && g != prev) ok = po_table_level(a, g, PAINT_STEP_COLUMN_GROUP_BACKGROUND);
        prev = g;
    }
    for (prev = NULL, c = 0; ok && c < map.ncols; c++) {
        lxb_dom_element_t *col = map.cols[c].column;

        if (col != NULL && col != prev) ok = po_table_level(a, col, PAINT_STEP_COLUMN_BACKGROUND);
        prev = col;
    }

    /* CSS 2.1 §E.2's item 4 — "row group backgrounds (color then image)", in CSS 2.1 §17.2 "The CSS table model"'s
       display order, which is the order core/layout/table_box.h reports them in and the same order its rows
       are in, because ONE walk produces both. */
    for (i = 0; ok && i < ngroups; i++) {
        DCHECK(groups[i].element != NULL,
               "core/layout/table_box.h reported a row group box with no element. CSS 2.1 §17.2.1 \"Anonymous "
               "table objects\" generates no anonymous row group — that header says so of its own answer — so a "
               "NULL here is that walk having generated a box the section does not");
        ok = po_table_level(a, groups[i].element, PAINT_STEP_ROW_GROUP_BACKGROUND);
    }

    /* CSS 2.1 §E.2's item 5 — "row backgrounds (color then image)", and item 6 — "cell backgrounds (color then
       image)". Two passes over ONE array rather than one pass offering both, because they are two LEVELS of
       CSS 2.1 §17.5.1 "Table layers and transparency" and every row's background goes down before any cell's. */
    for (i = 0; ok && i < nrows; i++)
        if (rows[i].element != NULL) ok = po_table_level(a, rows[i].element, PAINT_STEP_ROW_BACKGROUND);
    for (i = 0; ok && i < nrows; i++)
        for (j = 0; ok && j < rows[i].ncells; j++)
            if (rows[i].cells[j].element != NULL)
                ok = po_table_level(a, rows[i].cells[j].element, PAINT_STEP_CELL_BACKGROUND);

    /* CSS 2.1 §E.2's item 7 — "all table borders (in tree order for separated borders)", offered WHOLE. See
       paint_order.h's residual: the item covers the borders of the table and of every internal box in it, and
       CSS 2.1 states an order over them for the separated model only. */
    if (ok) ok = a->visit(PAINT_STEP_TABLE_BORDERS, a->table, a->user);

    table_column_boxes_release(&map);
    table_box_rows_free(rows, nrows);
    free(groups);
    return ok;
}

/* ONE BOX AT CSS 2.1 §E.2's STEP 2 OR STEP 4, routed to the arm its box type takes. The two arms are not two
   sub-lists of one offer — they are different lengths and their third item is not the same mark — so a
   consumer that took `PAINT_STEP_CONTEXT_BOX` for a table would lay the table's BORDER at item 3's position
   where CSS 2.1 §E.2 puts it at item 7, after five other boxes' backgrounds. That is exactly the defect the table
   arm exists to prevent, so the routing is here and never at the visitor. */
static bool po_offer_box(lxb_dom_element_t *el, bool context, PaintOrderVisit visit, void *user)
{
    char nbuf[160];
    char *d = po_display(el);
    TableBoxKind kind = table_box_kind(d);
    PoTableArm arm;

    free(d);
    if (!table_box_kind_generates_table_box(kind))
        return visit(context ? PAINT_STEP_CONTEXT_BOX : PAINT_STEP_DESCENDANT_BOX, el, user);

    DCHECKF(kind == TABLE_BOX_TABLE,
            "%s: an `inline-table` box reached CSS 2.1 §E.2's table arm, which is stated over a BLOCK-LEVEL table — "
            "step 2's arm reads \"Otherwise, if the element is a block level table:\" and step 4's runs inside a "
            "walk of block-level descendants, which CSS 2.1 §9.2.1 \"Block-level elements and block boxes\" closes at "
            "\"'block', 'list-item', and 'table'\". An inline-table's own background belongs to its PARENT's step "
            "7.2.1 as a box in a line box, so the two sites that can reach this one — `po_takes_step_2`, which "
            "refuses an inline-level context element, and step 4's member list, which holds no inline-level box "
            "— have disagreed with that classification and the fix is at whichever of them is wrong",
            box_subject(el, nbuf, sizeof nbuf));
    arm.table = el;
    arm.visit = visit;
    arm.user = user;
    return po_offer_table_arm(&arm, context ? PAINT_STEP_CONTEXT_TABLE_BACKGROUND
                                            : PAINT_STEP_DESCENDANT_TABLE_BACKGROUND);
}

/* CSS 2.1 §E.2's step 2 — does this stacking context's own element have a background and border to lay at step 2 at
   all? The step has two arms and an implicit neither: "If the element is a block, list-item, or other block
   equivalent:" and "Otherwise, if the element is a block level table:", and an INLINE-LEVEL context element
   matches neither. That is not a gap — CSS 2.1 §E.2 places such a box's own background and border in its PARENT's
   step 7.2.1, as one of the boxes in a line box, and step 6 is what CSS 2.1 §E.2 gives it for its own descendants. So
   the answer here is exactly "is it block-level", and the two arms differ only in the SUB-LIST that follows,
   which is the visitor's.
   A TABLE-INTERNAL BOX MATCHES NEITHER ARM EITHER AND IS NOT THE SAME CASE, so it crashes. CSS 2.1 places a
   cell's, a row's and a row group's background in CSS 2.1 §17.5.1 "Table layers and transparency"'s six levels —
   which CSS 2.1 §E.2's own table arm restates — and those levels are a sub-list of the TABLE's step 2 or step 4 offer,
   not a step of their own. A positioned table-internal box forms a stacking context all the same, and CSS 2.1
   states no painting order for the case at any section: it is a box whose background CSS 2.1 §17.5.1 puts inside its
   table's sequence while CSS 2.1 §9.9.1 puts the box itself outside it. Guessing either way would double-paint the
   background or drop it. */
static bool po_takes_step_2(lxb_dom_element_t *ce)
{
    char nbuf[160];
    char *d = po_display(ce);
    TableBoxKind kind = table_box_kind(d);

    free(d);
    DCHECKF(!table_box_kind_is_internal(kind),
            "%s: a TABLE-INTERNAL box forms a stacking context, and CSS 2.1 states no painting order for one. "
            "CSS 2.1 §E.2's step 2 has two arms — a block equivalent, or a block level table — and a row, a row group, "
            "a column, a column group or a cell is neither; what places such a box's background is CSS 2.1 §17.5.1 "
            "\"Table layers and transparency\", whose six levels are a SUB-LIST of the table's own step 2 or "
            "step 4 offer rather than a step, so the box's background sits inside a sequence its box has just "
            "been taken out of. A guess here either paints the background twice or never offers it at all. "
            "WHERE THE RESOLUTION IS, AND WHAT THIS CRASH USED TO SAY IT WAS: it used to send its reader to "
            "css-tables-3 as the place the interaction between a positioned table-internal box and "
            "CSS 2.1 §17.5.1's layers is stated, and that is not in the document. MEASURED on the fetched "
            "Editor's Draft: the word stacking occurs ZERO times in it and the phrase painting order ZERO "
            "times, and css-tables-3 §5.1 \"Paint order of cells\" is one sentence about DOM order that says "
            "nothing about positioning. What css-tables-3 does state DISSOLVES the interaction instead of "
            "ordering it. css-tables-3 §5.3.2 \"Drawing cell backgrounds\" moves four of the six levels OFF "
            "those boxes and onto the CELL — \"In addition to its own background, table-cell boxes also "
            "render the backgrounds of the table-track and table-track-group boxes in which they belong\" — "
            "so no row, row group, column or column group has an offer of its own for CSS 2.1 §9.9.1 to take "
            "away; and css-tables-3 §3.6.1 \"Overrides applying in all modes\" independently ignores `z-index` "
            "on a table-track and table-track-group box outright. A positioned table-CELL is the one member "
            "that survives, and it survives WHOLE, because under that reading the cell carries all five "
            "non-table levels itself. SO THE RESOLUTION IS REAL AND TAKING IT IS A DECISION AND NOT A LOOKUP: "
            "css-tables-3 is a Working Draft whose own status section says it is inappropriate to cite as "
            "other than a work in progress, and the CSS Snapshot — the document CSS 2.1's own boilerplate "
            "points at for a list of the specifications that replace its sections — names no Tables module at "
            "all. So css-tables-3 §5.3.2 would be a work in progress replacing the Official Definition of CSS "
            "for every table on every page. BUILD it deliberately, with that trade stated, or leave this "
            "crash standing",
            box_subject(ce, nbuf, sizeof nbuf));
    return !po_is_inline_level(ce);
}

static bool po_walk_positioned(JSContext *ctx, lxb_dom_element_t *ce, PaintOrderVisit visit, void *user,
                               bool negative, bool level_zero, bool positive)
{
    PoList members = {NULL, 0, 0};
    bool ok = true;
    size_t i;

    po_collect(ctx, ce, PO_SET_POSITIONED, &members);
    /* Steps 3 and 9 are "in z-index order ... then tree order" and step 8 is "in tree order" alone, so the
       sort is applied to the list and the step-8 pass reads it back in tree order — which a stable sort by a
       key every step-8 member shares (a stack level of 0) leaves untouched. */
    if (negative || positive) po_sort_by_level(&members);
    for (i = 0; ok && i < members.n; i++) {
        lxb_dom_element_t *d = members.v[i];
        int level = stacking_level(d);
        bool forms = stacking_context_forms(d);

        if (level < 0 ? !negative : (level > 0 ? !positive : !level_zero)) continue;
        /* CSS 2.1 §E.2 step 8: "For those with 'z-index: auto', treat the element as if it created a new stacking
           context, but any positioned descendants and descendants which actually create a new stacking context
           should be considered part of the parent stacking context, not this new one. For those with
           'z-index: 0', treat the stacking context generated atomically." `stacking_context_forms` is the
           question that separates them, and it is not the same as "is the level 0": css-position-3 §2.2
           "Painting Order and Stacking Contexts" makes a `fixed` or `sticky` box at `z-index: auto` form one,
           so the atomic arm is wider than CSS 2.1's own sentence and the `auto` read alone would put such a
           box's descendants in the wrong context. */
        ok = po_walk(ctx, d, !forms, visit, user);
    }
    po_list_free(&members);
    return ok;
}

static bool po_walk(JSContext *ctx, lxb_dom_element_t *ce, bool pseudo, PaintOrderVisit visit, void *user)
{
    PoList blocks = {NULL, 0, 0}, floats = {NULL, 0, 0};
    bool ok = true;
    size_t i;

    DCHECK(pseudo || stacking_context_forms(ce),
           "CSS 2.1 §E.2's painting order was walked for an element that neither forms a stacking context nor "
           "is one CSS 2.1 §E.2 says to treat as if it did. CSS 2.1 §E.2 is stated as \"The painting order for the descendants "
           "of an element generating a stacking context\", so there is no order to state for any other box: "
           "its marks belong to the sequence of whichever context it is IN, and a second sequence here would "
           "offer them twice");

    /* CSS 2.1 §E.2 step 1: "If the element is a root element:" — its background colour and image "over the entire
       canvas". A pseudo-context is never the root: CSS 2.1 §E.2's two pseudo-context sentences are about a float and a
       positioned descendant, and CSS 2.1 §9.3 makes the root element out of flow and nothing's descendant. */
    if (!pseudo && po_is_root_element(lxb_dom_interface_node(ce)) &&
        !visit(PAINT_STEP_ROOT_BACKGROUND, ce, user))
        return false;

    /* CSS 2.1 §E.2 step 2 — the context element's own background and border, through the arm its box type takes. */
    if (po_takes_step_2(ce) && !po_offer_box(ce, true, visit, user)) return false;

    /* CSS 2.1 §E.2 step 3: "Stacking contexts formed by positioned descendants with negative z-indices (excluding 0)
       in z-index order (most negative first) then tree order." A pseudo-context performs none of the
       positioned steps — its positioned descendants are the parent's, which is the sentence CSS 2.1 §E.2 states at
       step 5 and again at step 8. */
    if (!pseudo && !po_walk_positioned(ctx, ce, visit, user, true, false, false)) return false;

    /* CSS 2.1 §E.2 step 4: "For all its in-flow, non-positioned, block-level descendants in tree order:" followed by
       that box's background and border. */
    po_collect(ctx, ce, PO_SET_IN_FLOW_BLOCK, &blocks);
    for (i = 0; ok && i < blocks.n; i++) ok = po_offer_box(blocks.v[i], false, visit, user);

    /* CSS 2.1 §E.2 step 5: "All non-positioned floating descendants, in tree order. For each one of these, treat the
       element as if it created a new stacking context, but any positioned descendants and descendants which
       actually create a new stacking context should be considered part of the parent stacking context, not
       this new one." */
    if (ok) {
        po_collect(ctx, ce, PO_SET_FLOAT, &floats);
        for (i = 0; ok && i < floats.n; i++) ok = po_walk(ctx, floats.v[i], true, visit, user);
        po_list_free(&floats);
    }

    /* CSS 2.1 §E.2 steps 6 and 7, which are one test with two answers: "If the element is an inline element that
       generates a stacking context, then:" its line boxes, "Otherwise: first for the element, then for all its
       in-flow, non-positioned, block-level descendants in tree order:" their content. The set step 7 walks is
       step 4's set, which is why it is collected once above and read twice here — two collections would be two
       answers to one membership question, free to disagree after a cascade read. */
    if (ok) {
        if (po_is_inline_level(ce)) {
            ok = visit(PAINT_STEP_INLINE_LINE_BOXES, ce, user);
        } else {
            ok = po_offer_content(ce, visit, user);
            for (i = 0; ok && i < blocks.n; i++) ok = po_offer_content(blocks.v[i], visit, user);
        }
    }
    po_list_free(&blocks);
    if (!ok) return false;

    /* CSS 2.1 §E.2 step 8: "All positioned descendants with 'z-index: auto' or 'z-index: 0', in tree order." */
    if (!pseudo && !po_walk_positioned(ctx, ce, visit, user, false, true, false)) return false;

    /* CSS 2.1 §E.2 step 9: "Stacking contexts formed by positioned descendants with z-indices greater than or equal to
       1 in z-index order (smallest first) then tree order." */
    if (!pseudo && !po_walk_positioned(ctx, ce, visit, user, false, false, true)) return false;

    /* CSS 2.1 §E.2 step 10 is not offered. See paint_order.h's second residual: the step is an outline and this engine
       has no `outline-color`, `outline-style` or `outline-width` in any registry it reads, so it cannot tell a
       page that declared no outline from one whose outline it cannot read. */
    return true;
}

bool paint_order_walk(JSContext *ctx, lxb_dom_element_t *context_el, PaintOrderVisit visit, void *user)
{
    DCHECK(ctx != NULL, "CSS 2.1 §E.2's painting order was asked for with no realm — Appendix E §E.1's tree "
                        "order is DOM §4.8 \"Interface ShadowRoot\"'s shadow-including one, whose walker reads "
                        "a per-flow association kept on an element's wrapper");
    DCHECK(context_el != NULL, "CSS 2.1 §E.2's painting order was asked about no element");
    DCHECK(visit != NULL, "CSS 2.1 §E.2's painting order was walked with no visitor, so every offer it makes "
                          "would be discarded — the walk derives the order and stores none of it, so a walk "
                          "with nowhere to put its offers computes a sequence and then forgets it");
    return po_walk(ctx, context_el, false, visit, user);
}
