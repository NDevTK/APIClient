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
#include <lexbor/core/base.h>   /* lexbor_realloc: the allocator the engine gives its refusal edge */

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
    CssDisplay d = css_computed_display(el);

    if (css_display_is_box(d, CSS_KEYWORD_NONE)) return PO_BOX_NONE;
    if (css_display_is_box(d, CSS_KEYWORD_CONTENTS)) return PO_BOX_CONTENTS;
    return PO_BOX_YES;
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
    return table_box_kind_is_internal(table_box_kind(css_computed_display(el)));
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
    if (!po_is_inline_level(el)) return PAINT_INLINE_NONE;
    if (replaced_element_of(el).replaced) return PAINT_INLINE_ATOMIC;
    return css_computed_display_is(el, CSS_KEYWORD_INLINE) ? PAINT_INLINE_NON_ATOMIC : PAINT_INLINE_ATOMIC;
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
        lxb_dom_element_t **v = lexbor_realloc(l->v, cap * sizeof *v);

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
               "a descendant that forms a stacking context is not in one of CSS 2.1 §9.9.1's three positioned "
               "layers, which is the two classifications disagreeing; the fix is at whichever of them is wrong "
               "rather than a member placed here. THE INVARIANT IS NO LONGER \"EVERY STACKING CONTEXT IS A "
               "POSITIONED BOX\", which is what this assert used to say and which CSS 2.1 §9.9.1 alone does "
               "support — \"Other stacking contexts are generated by any positioned element (including "
               "relatively positioned elements) having a computed value of 'z-index' other than 'auto'\". "
               "css-transforms-1 §2 \"The Transform Rendering Model\" retires it: a STATIC box with a "
               "transform forms one, and §9.9.1's own sentence says such rules were coming (\"In future "
               "levels of CSS, other properties may introduce stacking contexts\"). What still holds, and is "
               "what this condition now rests on, is that every such rule ALSO states the layer — "
               "css-transforms-1 §2's is \"at the same stacking order that would be used if it were a "
               "positioned element with z-index: 0\" — so a property that creates a context without saying "
               "where its box is painted is the shape that fires here, and the answer is that standard's "
               "sentence rather than a layer chosen in this walk");

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
    TableBoxKind kind = table_box_kind(css_computed_display(ce));

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

/* CSS 2.1 §E.2's step 7, for ONE of the boxes step 7 names: "If the element is a block-level replaced element, then:
   the replaced content, atomically." against "Otherwise, for each line box of that element:". The two are one
   test with two answers and they are offered as two steps because they are two different sub-lists — the
   first is a single atomic mark and the second is an enumeration this file does not yet sequence. */
static PaintStep po_content_step(lxb_dom_element_t *el)
{
    bool replaced = replaced_element_of(el).replaced && !po_is_inline_level(el);

    return replaced ? PAINT_STEP_REPLACED_CONTENT : PAINT_STEP_LINE_BOXES;
}

/* Where one stacking context (or pseudo-context) of the walk stands: the CSS 2.1 §E.2 step it is at. */
typedef enum {
    PO_CTX_ROOT = 0,   /* step 1 */
    PO_CTX_STEP_2,
    PO_CTX_NEGATIVE,   /* step 3 */
    PO_CTX_BLOCKS,     /* step 4 */
    PO_CTX_FLOATS,     /* step 5 */
    PO_CTX_CONTENT,    /* steps 6 and 7 */
    PO_CTX_ZERO,       /* step 8 */
    PO_CTX_POSITIVE,   /* step 9 */
    PO_CTX_DONE
} PoContextPhase;

/* Where one table arm stands: CSS 2.1 §E.2's seven table items, which restate CSS 2.1 §17.5.1 "Table layers and
   transparency"'s levels. */
typedef enum {
    PO_TABLE_BACKGROUND = 0,   /* item 1 */
    PO_TABLE_COLUMN_GROUPS,    /* item 2 */
    PO_TABLE_COLUMNS,          /* item 3 */
    PO_TABLE_ROW_GROUPS,       /* item 4 */
    PO_TABLE_ROWS,             /* item 5 */
    PO_TABLE_CELLS,            /* item 6 */
    PO_TABLE_BORDERS,          /* item 7 */
    PO_TABLE_DONE
} PoTablePhase;

/* One frame of the walk's explicit stack. A context frame is CSS 2.1 §E.2's sequence for `el`; it pushes a child
   frame where the sequence enters a child stacking context (steps 3, 8's `z-index: 0` arm, 9), a pseudo-context
   (step 5, step 8's `z-index: auto` arm) or a block-level table's arm (steps 2 and 4). Every push descends. */
typedef struct {
    bool               table;
    lxb_dom_element_t *el;        /* the context element, or the table */
    /* a context frame */
    bool               pseudo;
    PoContextPhase     phase;
    PoList             blocks;    /* step 4's set, read again at step 7; built on entering step 4 */
    PoList             members;   /* the current step's positioned members (3, 8, 9) or floats (5) */
    bool               built;     /* `members` holds the current step's list */
    bool               self;      /* step 7 has offered the element's own content */
    size_t             i;
    /* a table frame */
    PaintStep          background;
    PoTablePhase       tphase;
    TableColumnBoxMap  map;
    TableBoxRow       *rows;
    TableBoxRowGroup  *groups;
    size_t             nrows, ngroups, c, j;
    lxb_dom_element_t *prev;
} PoFrame;

struct PaintOrderCursor {
    JSContext *ctx;
    PoFrame   *stack;
    size_t     n, cap;
};

static void po_push_frame(PaintOrderCursor *c, const PoFrame *f)
{
    if (c->n == c->cap) {
        size_t cap = c->cap ? c->cap * 2 : 8;
        PoFrame *v = lexbor_realloc(c->stack, cap * sizeof *v);

        CHECK(v != NULL, "paint: OOM growing CSS 2.1 §E.2's walk stack");
        c->stack = v;
        c->cap = cap;
    }
    c->stack[c->n++] = *f;
}

static void po_push_context(PaintOrderCursor *c, lxb_dom_element_t *ce, bool pseudo)
{
    PoFrame f;
    /* Asked as work in both builds: the root's first ask builds its cascade, which later priced asks read. */
    bool forms = pseudo || stacking_context_forms(ce);

    DCHECK(forms,
           "CSS 2.1 §E.2's painting order was walked for an element that neither forms a stacking context nor "
           "is one CSS 2.1 §E.2 says to treat as if it did. CSS 2.1 §E.2 is stated as \"The painting order for the "
           "descendants of an element generating a stacking context\", so there is no order to state for any "
           "other box: its marks belong to the sequence of whichever context it is IN");
    memset(&f, 0, sizeof f);
    f.el = ce;
    f.pseudo = pseudo;
    f.phase = PO_CTX_ROOT;
    po_push_frame(c, &f);
}

/* One box at CSS 2.1 §E.2's step 2 or step 4, routed to the arm its box type takes: an offer at `*step`, or a
   pushed table arm (false). The two arms are not two sub-lists of one offer — a consumer that took
   `PAINT_STEP_CONTEXT_BOX` for a table would lay the table's border at item 3's position where CSS 2.1 §E.2 puts
   it at item 7 — so the routing is here and never at the visitor. */
static bool po_box(PaintOrderCursor *c, lxb_dom_element_t *el, bool context, PaintStep *step)
{
    char nbuf[160];
    TableBoxKind kind = table_box_kind(css_computed_display(el));
    PoFrame f;

    if (!table_box_kind_generates_table_box(kind)) {
        *step = context ? PAINT_STEP_CONTEXT_BOX : PAINT_STEP_DESCENDANT_BOX;
        return true;
    }
    DCHECKF(kind == TABLE_BOX_TABLE,
            "%s: an `inline-table` box reached CSS 2.1 §E.2's table arm, which is stated over a BLOCK-LEVEL table — "
            "step 2's arm reads \"Otherwise, if the element is a block level table:\" and step 4's runs inside a "
            "walk of block-level descendants, which CSS 2.1 §9.2.1 \"Block-level elements and block boxes\" closes at "
            "\"'block', 'list-item', and 'table'\". An inline-table's own background belongs to its PARENT's step "
            "7.2.1 as a box in a line box, so `po_takes_step_2` or step 4's member list has disagreed with that "
            "classification",
            box_subject(el, nbuf, sizeof nbuf));
    memset(&f, 0, sizeof f);
    f.table = true;
    f.el = el;
    f.background = context ? PAINT_STEP_CONTEXT_TABLE_BACKGROUND : PAINT_STEP_DESCENDANT_TABLE_BACKGROUND;
    f.tphase = PO_TABLE_BACKGROUND;
    po_push_frame(c, &f);
    return false;
}

/* One level of the table arm for `box`: false when the box is positioned and so not offered here. A positioned
   table-internal box is placed by CSS 2.1 §9.9.1 in a positioned layer, and `po_takes_step_2` aborts on it in a
   dev build; the release arm offers it at step 3, 8 or 9, so skipping it here offers it exactly once. */
static bool po_table_level(const PoFrame *t, lxb_dom_element_t *box)
{
    char nbuf[160];

    DCHECK(box != NULL, "CSS 2.1 §E.2's table arm was handed a level with no element to offer — the two boxes "
                        "core/layout/table_box.h reports without one (an anonymous row, an anonymous cell) are "
                        "answered at the call site by CSS 2.1 §9.2.1.1 \"Anonymous block boxes\" and never reach here");
    DCHECKF(lxb_dom_interface_node(box)->owner_document == lxb_dom_interface_node(t->el)->owner_document,
            "%s: a box offered inside CSS 2.1 §E.2's table arm belongs to a different Document from the table whose "
            "arm is being run; core/layout/table_box.h's and table_column_box.h's walks must not leave it",
            box_subject(box, nbuf, sizeof nbuf));
    return !po_layer_is_positioned(stacking_layer_of(box));
}

/* The table frame on top's next offer, or false when its arm is complete. Each item's boxes go in CSS 2.1 §17.2
   "The CSS table model"'s display order, which is the order core/layout/table_box.h and table_column_box.h report
   them in; a column or column group occupying several grid columns is offered at the first entry naming it. */
static bool po_table_next(PoFrame *t, PaintStep *step, lxb_dom_element_t **el)
{
    lxb_dom_element_t *box;

    for (;;) {
        switch (t->tphase) {
        case PO_TABLE_BACKGROUND:
            /* The grid is not built: table_column_box.h sizes its map to the greater of the grid's columns and
               `noccupied`, and every column box stands in the first `noccupied` entries. */
            table_column_boxes_build(t->el, 0, &t->map);
            DCHECK(t->map.ncols == t->map.noccupied,
                   "core/layout/table_column_box.h sized its map to something other than what the column boxes "
                   "occupy after being asked for a zero-column grid — its own contract is that `ncols` is the "
                   "greater of the two");
            t->nrows = table_box_rows(t->el, &t->rows);
            t->ngroups = table_box_row_groups(t->el, &t->groups);
            t->tphase = PO_TABLE_COLUMN_GROUPS;
            *step = t->background;
            *el = t->el;
            return true;
        case PO_TABLE_COLUMN_GROUPS:
        case PO_TABLE_COLUMNS:
            while (t->c < t->map.ncols) {
                box = t->tphase == PO_TABLE_COLUMN_GROUPS ? t->map.cols[t->c].column_group : t->map.cols[t->c].column;
                t->c++;
                if (box == NULL || box == t->prev) { t->prev = box; continue; }
                t->prev = box;
                if (!po_table_level(t, box)) continue;
                *step = t->tphase == PO_TABLE_COLUMN_GROUPS ? PAINT_STEP_COLUMN_GROUP_BACKGROUND
                                                            : PAINT_STEP_COLUMN_BACKGROUND;
                *el = box;
                return true;
            }
            t->c = 0;
            t->prev = NULL;
            t->tphase = t->tphase == PO_TABLE_COLUMN_GROUPS ? PO_TABLE_COLUMNS : PO_TABLE_ROW_GROUPS;
            break;
        case PO_TABLE_ROW_GROUPS:
            while (t->c < t->ngroups) {
                box = t->groups[t->c++].element;
                DCHECK(box != NULL,
                       "core/layout/table_box.h reported a row group box with no element. CSS 2.1 §17.2.1 "
                       "\"Anonymous table objects\" generates no anonymous row group, so a NULL here is that walk "
                       "having generated a box the section does not");
                if (!po_table_level(t, box)) continue;
                *step = PAINT_STEP_ROW_GROUP_BACKGROUND;
                *el = box;
                return true;
            }
            t->c = 0;
            t->tphase = PO_TABLE_ROWS;
            break;
        case PO_TABLE_ROWS:
            /* Items 5 and 6 are two passes over one array: every row's background goes down before any cell's. */
            while (t->c < t->nrows) {
                box = t->rows[t->c++].element;
                if (box == NULL || !po_table_level(t, box)) continue;
                *step = PAINT_STEP_ROW_BACKGROUND;
                *el = box;
                return true;
            }
            t->c = 0;
            t->j = 0;
            t->tphase = PO_TABLE_CELLS;
            break;
        case PO_TABLE_CELLS:
            while (t->c < t->nrows) {
                if (t->j >= t->rows[t->c].ncells) { t->c++; t->j = 0; continue; }
                box = t->rows[t->c].cells[t->j++].element;
                if (box == NULL || !po_table_level(t, box)) continue;
                *step = PAINT_STEP_CELL_BACKGROUND;
                *el = box;
                return true;
            }
            t->tphase = PO_TABLE_BORDERS;
            break;
        case PO_TABLE_BORDERS:
            /* Item 7, offered whole — see paint_order.h's residual. */
            t->tphase = PO_TABLE_DONE;
            *step = PAINT_STEP_TABLE_BORDERS;
            *el = t->el;
            return true;
        case PO_TABLE_DONE:
            return false;
        }
    }
}

static void po_frame_release(PoFrame *f)
{
    if (f->table) {
        if (f->tphase != PO_TABLE_BACKGROUND) {
            table_column_boxes_release(&f->map);
            table_box_rows_free(f->rows, f->nrows);
            free(f->groups);
        }
        return;
    }
    po_list_free(&f->blocks);
    po_list_free(&f->members);
}

PaintOrderCursor *paint_order_begin(JSContext *ctx, lxb_dom_element_t *context_el)
{
    PaintOrderCursor *c;

    DCHECK(ctx != NULL, "CSS 2.1 §E.2's painting order was asked for with no realm — Appendix E §E.1's tree "
                        "order is DOM §4.8 \"Interface ShadowRoot\"'s shadow-including one, whose walker reads "
                        "a per-flow association kept on an element's wrapper");
    DCHECK(context_el != NULL, "CSS 2.1 §E.2's painting order was asked about no element");
    c = lexbor_calloc(1, sizeof *c);
    CHECK(c != NULL, "paint: OOM allocating CSS 2.1 §E.2's walk");
    c->ctx = ctx;
    po_push_context(c, context_el, false);
    return c;
}

bool paint_order_next(PaintOrderCursor *c, PaintStep *step, lxb_dom_element_t **el)
{
    DCHECK(c != NULL && step != NULL && el != NULL, "CSS 2.1 §E.2's walk was advanced with nowhere to put its offer");
    while (c->n > 0) {
        PoFrame *f = &c->stack[c->n - 1];
        lxb_dom_element_t *ce = f->el, *d;

        if (f->table) {
            if (po_table_next(f, step, el)) return true;
            po_frame_release(f);
            c->n--;
            continue;
        }
        switch (f->phase) {
        case PO_CTX_ROOT:
            /* Step 1: "If the element is a root element:" — its background "over the entire canvas". A
               pseudo-context is never the root (CSS 2.1 §9.3 makes the root out of flow and nothing's descendant). */
            f->phase = PO_CTX_STEP_2;
            if (!f->pseudo && po_is_root_element(lxb_dom_interface_node(ce))) {
                *step = PAINT_STEP_ROOT_BACKGROUND;
                *el = ce;
                return true;
            }
            break;
        case PO_CTX_STEP_2:
            /* Step 2: the context element's own background and border, through the arm its box type takes. */
            f->phase = PO_CTX_NEGATIVE;
            if (po_takes_step_2(ce) && po_box(c, ce, true, step)) {
                *el = ce;
                return true;
            }
            break;
        case PO_CTX_NEGATIVE:
        case PO_CTX_ZERO:
        case PO_CTX_POSITIVE:
            /* Steps 3, 8 and 9 over the positioned descendants, which a pseudo-context leaves to its parent.
               Steps 3 and 9 are "in z-index order ... then tree order" and step 8 "in tree order": the stable sort
               by level leaves step 8's members (all level 0) in the tree order they were collected in. */
            if (f->pseudo) { f->phase = f->phase == PO_CTX_NEGATIVE ? PO_CTX_BLOCKS : PO_CTX_DONE; break; }
            if (!f->built) {
                po_collect(c->ctx, ce, PO_SET_POSITIONED, &f->members);
                if (f->phase != PO_CTX_ZERO) po_sort_by_level(&f->members);
                f->built = true;
                f->i = 0;
            }
            while (f->i < f->members.n) {
                int level;

                d = f->members.v[f->i++];
                level = stacking_level(d);
                if (f->phase == PO_CTX_NEGATIVE ? level >= 0 : (f->phase == PO_CTX_POSITIVE ? level <= 0 : level != 0))
                    continue;
                /* Step 8: "For those with 'z-index: auto', treat the element as if it created a new stacking
                   context, but any positioned descendants and descendants which actually create a new stacking
                   context should be considered part of the parent stacking context, not this new one. For those
                   with 'z-index: 0', treat the stacking context generated atomically." `stacking_context_forms`
                   separates them (css-position-3 §2.2 widens the atomic arm beyond CSS 2.1's sentence). */
                po_push_context(c, d, !stacking_context_forms(d));
                goto pushed;
            }
            po_list_free(&f->members);
            f->built = false;
            f->phase = f->phase == PO_CTX_NEGATIVE ? PO_CTX_BLOCKS
                     : f->phase == PO_CTX_ZERO ? PO_CTX_POSITIVE : PO_CTX_DONE;
            break;
        case PO_CTX_BLOCKS:
            /* Step 4: "For all its in-flow, non-positioned, block-level descendants in tree order:" their
               background and border. The set is kept for step 7, which walks the same set. */
            if (!f->built) {
                po_collect(c->ctx, ce, PO_SET_IN_FLOW_BLOCK, &f->blocks);
                f->built = true;
                f->i = 0;
            }
            if (f->i < f->blocks.n) {
                d = f->blocks.v[f->i++];
                if (po_box(c, d, false, step)) {
                    *el = d;
                    return true;
                }
                goto pushed;
            }
            f->built = false;
            f->phase = PO_CTX_FLOATS;
            break;
        case PO_CTX_FLOATS:
            /* Step 5: "All non-positioned floating descendants, in tree order. For each one of these, treat the
               element as if it created a new stacking context, but any positioned descendants and descendants
               which actually create a new stacking context should be considered part of the parent stacking
               context, not this new one." */
            if (!f->built) {
                po_collect(c->ctx, ce, PO_SET_FLOAT, &f->members);
                f->built = true;
                f->i = 0;
            }
            if (f->i < f->members.n) {
                po_push_context(c, f->members.v[f->i++], true);
                goto pushed;
            }
            po_list_free(&f->members);
            f->built = false;
            f->self = false;
            f->i = 0;
            f->phase = PO_CTX_CONTENT;
            break;
        case PO_CTX_CONTENT:
            /* Steps 6 and 7, one test with two answers: "If the element is an inline element that generates a
               stacking context, then:" its line boxes, "Otherwise: first for the element, then for all its
               in-flow, non-positioned, block-level descendants in tree order:" their content. */
            if (po_is_inline_level(ce)) {
                po_list_free(&f->blocks);
                f->phase = PO_CTX_ZERO;
                *step = PAINT_STEP_INLINE_LINE_BOXES;
                *el = ce;
                return true;
            }
            if (!f->self) {
                f->self = true;
                *step = po_content_step(ce);
                *el = ce;
                return true;
            }
            if (f->i < f->blocks.n) {
                d = f->blocks.v[f->i++];
                *step = po_content_step(d);
                *el = d;
                return true;
            }
            po_list_free(&f->blocks);
            f->phase = PO_CTX_ZERO;
            break;
        case PO_CTX_DONE:
            /* Step 10 is not offered: see paint_order.h's second residual. */
            po_frame_release(f);
            c->n--;
            break;
        }
        continue;
    pushed:
        continue;
    }
    return false;
}

void paint_order_free(PaintOrderCursor *c)
{
    DCHECK(c != NULL, "CSS 2.1 §E.2's walk was released through nothing");
    while (c->n > 0) po_frame_release(&c->stack[--c->n]);
    free(c->stack);
    free(c);
}
