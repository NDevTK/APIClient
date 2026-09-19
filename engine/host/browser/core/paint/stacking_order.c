/* CSS 2.1 §9.9 "Layered presentation" — the stacking-context tree and the order boxes paint in. See
   stacking_order.h for the contract, for why this is §9.9.1 rather than Appendix E, and for the residual that
   names the properties outside §9.9.1's own sentence. */
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include <lexbor/dom/dom.h>

#include "check.h"
#include "core/css/css_computed_value.h"
#include "core/dom/node.h"
#include "core/dom/shadow_root.h"
#include "core/layout/box_subject.h"
#include "core/layout/table_box.h"
#include "core/paint/stacking_order.h"

/* THE COMPUTED VALUE OF A KEYWORD PROPERTY, with the one invariant this component may assert about it: the
   cascade always answers. Every property read here is in lexbor's registry with an initial value, so the last
   layer of CSS Cascade §7's defaulting cannot come back empty — an absence would be this engine's pipeline and
   not the document's. */
static char *so_computed(lxb_dom_element_t *el, const char *name)
{
    char *v = css_computed_value(el, name);

    DCHECK(v != NULL, "the cascade produced no computed value for a property this engine models — every one of "
                      "them is in lexbor's registry with an initial value, so the last layer always answers");
    return v;
}

static bool so_computed_is(lxb_dom_element_t *el, const char *name, const char *want)
{
    char *v = so_computed(el, name);
    bool hit = strcmp(v, want) == 0;

    free(v);
    return hit;
}

/* "THE ROOT ELEMENT" — the element whose parent is the Document itself, which is the same question
   core/css/css_computed_value.c asks to give css-display-3 §2.8 "The Root Element's Principal Box" its own
   computed-value rules. It is spelled here rather than called because that file's copy is static; the two are
   one sentence and a third spelling would be the one that drifts, so when either is exported this one routes
   to it. */
static bool so_is_root_element(const lxb_dom_node_t *n)
{
    return n->parent != NULL && n->parent->type == LXB_DOM_NODE_TYPE_DOCUMENT;
}

/* DOES THIS ELEMENT GENERATE A BOX AT ALL? css-display-3 §2.5 "Box Generation: the none and contents keywords"
   gives both its values: `none` generates none, and `contents` generates none for the ELEMENT while its
   children still generate theirs. Neither is a box §9.9.1 can place, and the two are one question here because
   §9.9.1's subject is a box and both answers are that there isn't one. */
static bool so_generates_box(lxb_dom_element_t *el)
{
    char *d = so_computed(el, "display");
    bool boxless = strcmp(d, "none") == 0 || strcmp(d, "contents") == 0;

    free(d);
    return !boxless;
}

/* CSS 2.1 §9.3 "Positioning schemes"'s POSITIONED BOX, widened by css-position-3 §2 to the value CSS 2.1 does
   not have. §9.3.1: a box is positioned when its `position` is other than `static`; css-position-3 §2.2 states
   the same set from the other end — "The z-index property applies to all positioned boxes" — over the four
   values §2 declares. `sticky` is in neither of CSS 2.1's lists because CSS 2.1 has no such value at all. */
static bool so_is_positioned(lxb_dom_element_t *el)
{
    char *p = so_computed(el, "position");
    bool positioned = strcmp(p, "static") != 0;

    free(p);
    return positioned;
}

/* §9.9.1's `z-index`, PARSED. Two arms and no third: `auto`, for which `*level` is §9.9.1's own "The stack
   level of the generated box in the current stacking context is 0", and an `<integer>`, which "is the stack
   level of the generated box in the current stacking context". Returns whether the value was an integer, which
   is the SECOND fact the same read answers and the one §9.9.1's stacking-context test is stated over.
   THE MAGNITUDE IS SATURATED AND THAT IS THE SPEC'S ARM, NOT A CLAMP PAST A BROKEN INVARIANT. css-values-4
   §5.1 "Range Restrictions and Range Definition Notation": "CSS theoretically supports infinite precision and
   infinite ranges for all value types; however in reality implementations have finite capacity." `strtol`
   saturates at this UA's capacity, which preserves the sign and every ordering comparison §9.9.1 makes of a
   stack level — the only things the order is built out of. A DCHECK on the magnitude would be a page-supplied
   value handed an abort switch over the engine, which is the one thing a cascade value may never be.
   THE ASSERT IS ON THE GRAMMAR AND THAT IS THIS ENGINE'S OWN LOGIC RATHER THAN THE PAGE'S. lexbor rejects a
   declaration outside §9.9.1's `auto | <integer> | inherit` at parse time, so the cascade never carries one
   and CSS Cascade §7's defaulting supplies the initial `auto` instead; a third spelling arriving here is this
   parse-and-serialize pipeline disagreeing with itself. */
static bool so_z_index(lxb_dom_element_t *el, int *level)
{
    char nbuf[160];
    char *v = so_computed(el, "z-index");
    char *end = NULL;
    long n;

    if (strcmp(v, "auto") == 0) {
        free(v);
        *level = 0;
        return false;
    }
    n = strtol(v, &end, 10);
    DCHECKF(end != NULL && end != v && *end == '\0',
            "%s: CSS 2.1 §9.9.1 \"Specifying the stack level: the 'z-index' property\" declares `auto | "
            "<integer> | inherit` and the cascade handed back neither an integer nor `auto`. That is not a "
            "value a document can produce: lexbor rejects a declaration outside the grammar at parse time, so "
            "the property is then undeclared and CSS Cascade §7's defaulting supplies the initial `auto`. So "
            "this is the parse, the cascade or the serializer disagreeing with the grammar all three are "
            "stated over, and the fix is at whichever of them produced the spelling — never a default here, "
            "which would put a real box at a stack level no declaration asked for",
            box_subject(el, nbuf, sizeof nbuf));
    free(v);
    *level = n > (long)INT_MAX ? INT_MAX : (n < (long)INT_MIN ? INT_MIN : (int)n);
    return true;
}

/* IS THIS COMPUTED `display` INLINE-LEVEL — §9.9.1's layer 5 against its layer 3, and the only thing those two
   layers differ by. §9.9.1 states layer 5 as "the in-flow, inline-level, non-positioned descendants, including
   inline tables and inline blocks", and the `inline-table` half is asked through the component that owns
   §17.2's box-type vocabulary rather than through a second prefix test on this file's own list.
   A TABLE-INTERNAL BOX IS NOT INLINE-LEVEL AND IS NOT A SEPARATE LAYER. §9.9.1's seven layers name no table
   box at all; what states a row's ink before a cell's is Appendix E §E.2's elaboration of layer 3, which is a
   sequence of MARKS inside one layer rather than a layer of its own. So a row, a cell, a row group, a column,
   a column group and a caption are all in-flow non-inline-level boxes here, which is what §9.9.1's own words
   make them, and the finer order is §E.2's to state when it exists.
   A `display` NEITHER LIST COVERS CRASHES, naming the same classification core/layout/block_flow.c names: the
   two-value `<display-outside> <display-inside>` grammar admits pairs this engine has never answered for, and
   the place that decides them is the computed value rather than each walk that reads one. */
static bool so_is_inline_level(lxb_dom_element_t *el, const char *display)
{
    char nbuf[160];
    TableBoxKind table;

    if (strcmp(display, "inline") == 0 || strcmp(display, "inline-block") == 0 ||
        strcmp(display, "inline-flex") == 0 || strcmp(display, "inline-grid") == 0)
        return true;
    table = table_box_kind(display);
    if (table == TABLE_BOX_INLINE_TABLE) return true;
    if (table != TABLE_BOX_NOT_A_TABLE_BOX) return false;
    if (strcmp(display, "block") == 0 || strcmp(display, "flow-root") == 0 ||
        strcmp(display, "list-item") == 0 || strcmp(display, "flex") == 0 || strcmp(display, "grid") == 0)
        return false;
    DFAILF("%s: this box's computed `display` is one CSS 2 §9.2 \"Controlling box generation\"'s box types do "
           "not cover, so §9.9.1's layers 3 and 5 cannot be told apart for it: it is neither block-level, nor "
           "inline-level, nor a table box. css-display-3 §2 \"Box Layout Modes: the display property\"'s "
           "`<display-outside> <display-inside>` grammar admits pairs this engine has never had to answer for "
           "(`ruby`, `math`, a two-value `inline flow-root`), and each has an OUTER role that decides this "
           "question for every reader at once. BUILD the classification in core/css/css_computed_value.c's "
           "computed_display, whose css-display-3 §2.7 \"Automatic Box Type Transformations\" blockification "
           "already normalises the values this walk does understand — a list here would be a second copy of an "
           "answer that belongs to the computed value",
           box_subject(el, nbuf, sizeof nbuf));
    return false;
}

bool stacking_context_forms(lxb_dom_element_t *el)
{
    char nbuf[160];
    char *pos;
    int level;
    bool forms;

    DCHECK(el != NULL, "CSS 2.1 §9.9.1's stacking-context test was asked about no element");
    DCHECKF(so_generates_box(el),
            "%s: CSS 2.1 §9.9.1's stacking-context test was asked about an element that generates no box. "
            "CSS 2.1 §9.9 is stated over BOXES throughout — \"each box has a position in three dimensions\" — so there "
            "is nothing here to form a context or to fail to, and answering `false` would put an absent box "
            "and a box that forms no context behind one word. The caller's own walk is what knows the "
            "difference: css-display-3 §2.5's `none` generates nothing and its `contents` generates nothing "
            "for the element while its children still generate theirs, and the second of those is a SPLICE "
            "into the caller's child list rather than a box to classify",
            box_subject(el, nbuf, sizeof nbuf));

    /* §9.9.1's first sentence: "The root element forms the root stacking context." It is asked FIRST because
       it is the one arm that holds whatever `position` and `z-index` say, and because it is what terminates
       every upward walk in this file. */
    if (so_is_root_element(lxb_dom_interface_node(el))) return true;

    /* §9.9.1's second: "Other stacking contexts are generated by any positioned element (including relatively
       positioned elements) having a computed value of 'z-index' other than 'auto'." */
    if (!so_is_positioned(el)) return false;
    forms = so_z_index(el, &level);
    (void)level;
    if (forms) return true;

    /* css-position-3 §2.2 "Painting Order and Stacking Contexts"'s `auto` arm, which is an AMENDMENT to the
       sentence above and not a restatement of it: "Fixed and sticky positioned boxes nonetheless form a
       stacking context." CSS 2.1 alone answers `false` here, and the box it answers it for is the ordinary
       `position: fixed` header with no `z-index` — so every positioned descendant of one would be placed in
       the wrong context by the older sentence. `sticky` has no CSS 2.1 sentence to amend at all. */
    pos = so_computed(el, "position");
    forms = strcmp(pos, "fixed") == 0 || strcmp(pos, "sticky") == 0;
    free(pos);
    return forms;
}

int stacking_level(lxb_dom_element_t *el)
{
    char nbuf[160];
    int level;

    DCHECK(el != NULL, "CSS 2.1 §9.9.1's stack level was asked about no element");
    /* THE BOX QUESTION RUNS BEFORE THE POSITIONED ONE, which is CSS 2.1 §9.7 "Relationships between
       'display', 'position', and 'float'"'s own order and the order `stacking_layer_of` below runs its
       own three tests in. A `display: none; position: absolute` element satisfies the test beneath this
       one, so asking the two the other way round hands a stack level to a box CSS 2.1 §9.2
       "Controlling box generation" generates none of. */
    DCHECKF(so_generates_box(el),
            "%s: CSS 2.1 §9.9.1's stack level was asked of an element that generates no box. The concept is "
            "stated over one — \"Each positioned box in a given stacking context has an integer stack "
            "level\" — and css-display-3 §2.5 \"Box Generation: the none and contents keywords\" says there "
            "is no box here to have one: \"Elements with either of these values do not have inner or outer "
            "display types\". A caller that reached here with such an element has classified it into a layer "
            "a stack level orders, which is the defect to fix rather than a 0 to return",
            box_subject(el, nbuf, sizeof nbuf));
    DCHECKF(so_is_positioned(el) || so_is_root_element(lxb_dom_interface_node(el)),
            "%s: CSS 2.1 §9.9.1's stack level was asked of a box that has none. `z-index`'s own \"Applies to:\" "
            "line is \"positioned elements\", and §9.9.1 says the same thing where it defines the concept — "
            "\"Each positioned box in a given stacking context has an integer stack level\" — so a static box "
            "is not one the property answers for. §9.9.1's layers 2, 6 and 7 are the only ones a stack level "
            "orders and every member of the three is positioned; a caller that reached here with a static box "
            "has classified it into the wrong layer, which is the defect to fix rather than a 0 to return",
            box_subject(el, nbuf, sizeof nbuf));
    (void)so_z_index(el, &level);
    return level;
}

lxb_dom_element_t *stacking_context_of(lxb_dom_element_t *el)
{
    char nbuf[160];
    lxb_dom_element_t *p;

    DCHECK(el != NULL, "the stacking context of no element was asked for");
    DCHECKF(so_generates_box(el),
            "%s: the stacking context of an element that generates no box was asked for. CSS 2.1 §9.9.1 "
            "states that membership over a BOX and never over an element — \"Each box belongs to one "
            "stacking context\" — so there is no member here for any context to hold. THE WALK BELOW STILL "
            "STEPS OVER A BOXLESS ANCESTOR and that stays, because it is the other question: such an "
            "ancestor is a real flat-tree parent of boxes that DO exist, while an `el` with no box of its "
            "own contributes nothing for a context to contain",
            box_subject(el, nbuf, sizeof nbuf));
    /* THE FLAT TREE AND NOT THE CONTAINING-BLOCK CHAIN, which §9.9.1 states outright: "Stacking contexts are
       not necessarily related to containing blocks." `css_parent_element` is CSS Cascade §7.2's flattened
       element tree — the parent node when it is an element and the HOST when it is a shadow root — which is
       the tree Appendix E §E.1's "rendering tree" is, since a shadow host's stacking context contains the
       boxes its shadow tree generates.
       A BOXLESS ANCESTOR IS STEPPED OVER AND IS NOT A QUESTION FOR §9.9.1 TO ANSWER, which is why this walk
       asks about box generation itself rather than handing every ancestor to the test below. css-display-3
       §2.5 "Box Generation: the none and contents keywords" puts a real element on this chain with no box of
       its own — "The element itself does not generate any boxes, but its children and pseudo-elements still
       generate boxes and text sequences as normal" — so a `display: contents` ancestor is an ordinary flat-tree
       parent of boxes that DO exist, and §9.9.1, whose every sentence is about a box, has nothing to say about
       it. Stepping over it is the ELEMENT tree and the BOX tree being different shapes, which is the same
       splice core/layout/block_flow.c names at its own child walk; a `display: none` ancestor cannot be
       reached at all, because the element this was asked about would have no box either.
       IT TERMINATES AT THE ROOT because the root element always forms a context, and a NULL return therefore
       means `el` IS that root rather than that the walk ran out. */
    for (p = css_parent_element(el); p != NULL; p = css_parent_element(p))
        if (so_generates_box(p) && stacking_context_forms(p)) return p;
    return NULL;
}

StackingLayer stacking_layer_of(lxb_dom_element_t *el)
{
    char nbuf[160];
    char *display;
    bool inline_level;
    int level;

    DCHECK(el != NULL, "CSS 2.1 §9.9.1's layer was asked about no element");
    DCHECKF(!so_is_root_element(lxb_dom_interface_node(el)),
            "%s: CSS 2.1 §9.9.1's layers were asked of the ROOT element, which is not a member of any of them. "
            "Every one of the seven is stated over an element's DESCENDANTS — \"Within each stacking context, "
            "the following layers are painted in back-to-front order\" — and the root forms the root stacking "
            "context, so there is no context above it holding a list for it to be in. Its own background and "
            "border are layer 1 OF ITSELF, which is `stacking_order_compare`'s ancestor arm and not a value "
            "this entry can return",
            box_subject(el, nbuf, sizeof nbuf));

    /* CSS 2.1 §9.7 "Relationships between 'display', 'position', and 'float'"'s ORDER, which is the rule and
       not a convenience: `display` decides whether there is a box at all, then `position` takes it out of
       flow, and only then does `float` apply. Asking `float` first reports a float for `display: none;
       float: left`, which is a box §9.7 says does not exist. */
    DCHECKF(so_generates_box(el),
            "%s: CSS 2.1 §9.9.1's layer was asked of an element that generates no box. The seven layers are a "
            "partition of a stacking context's BOXES, so there is no member here to place, and a layer "
            "returned for one would put ink in a sequence for an element css-display-3 §2.5 says draws none",
            box_subject(el, nbuf, sizeof nbuf));

    /* Layers 2, 6 and 7 — the positioned half. §9.9.1's layer 6 is "the child stacking contexts with stack
       level 0 and the positioned descendants with stack level 0", which is why ONE arm answers for both: a
       positioned box at level 0 and a stacking context at level 0 are in the same layer and the distinction
       between them is not one this partition makes. Layers 2 and 7 name only child stacking contexts, and
       nothing else can reach them: a nonzero stack level requires a declared `<integer>`, and §9.9.1's own
       second sentence makes a positioned box with an integer `z-index` a stacking context. */
    if (so_is_positioned(el)) {
        level = stacking_level(el);
        if (level < 0) return STACKING_LAYER_NEGATIVE;
        if (level > 0) return STACKING_LAYER_POSITIVE;
        return STACKING_LAYER_LEVEL_ZERO;
    }

    /* Layer 4 — "the non-positioned floats". The `position` test above is what makes the adjective true here
       rather than a second read: §9.7's own table sets the computed `float` to `none` for an absolutely
       positioned box, so a box that is still floating at this point is one no positioning scheme took out of
       flow. */
    if (!so_computed_is(el, "float", "none")) return STACKING_LAYER_FLOAT;

    /* Layers 3 and 5 — the in-flow half, which the two tests above have already established. `display` is read
       here and not at the top because the two layers are the only readers of it and §9.7's order puts it
       first only for the box-generation question, which `so_generates_box` above is. */
    display = so_computed(el, "display");
    inline_level = so_is_inline_level(el, display);
    free(display);
    return inline_level ? STACKING_LAYER_INLINE : STACKING_LAYER_BLOCK;
}

/* TREE ORDER between two boxes that sit in ONE stacking context — §9.9.1's tie-break, "Boxes with the same
   stack level in a stacking context are stacked back-to-front according to document tree order", over the tree
   Appendix E §E.1 names: "Preorder depth-first traversal of the rendering tree".
   IT WALKS THE CANONICAL WALKER RATHER THAN COMPARING DEPTHS AND INDICES. core/dom/shadow_root.h's
   `shadow_root_next_in_shadow_including` is DOM §4.8 "Interface ShadowRoot"'s shadow-including tree order one
   step at a time — "In shadow-including tree order is shadow-including preorder, depth-first traversal of a node
   tree" — and it is the rendering tree's order because the rendering tree is the flattened one; an
   index-and-depth comparison written here would be a second traversal free to disagree with it at a shadow
   boundary, which is the one place the two trees differ and the only place either is hard. The cost is one
   walk of the context's subtree per tie, and it is paid only on a tie. */
static int so_tree_order(JSContext *ctx, lxb_dom_element_t *context, lxb_dom_element_t *a, lxb_dom_element_t *b)
{
    lxb_dom_node_t *root = lxb_dom_interface_node(context);
    lxb_dom_node_t *an = lxb_dom_interface_node(a), *bn = lxb_dom_interface_node(b);
    lxb_dom_node_t *n;

    for (n = root; n != NULL; n = shadow_root_next_in_shadow_including(ctx, n, root)) {
        if (n == an) return -1;
        if (n == bn) return 1;
    }
    DFAIL("CSS 2.1 §9.9.1's tree-order tie-break walked the whole of a stacking context's subtree without "
          "meeting one of the two boxes it was asked to order. Both reached this walk through "
          "`stacking_context_of`, which climbs CSS Cascade §7.2's flattened element tree from each of them and "
          "returned this element for both — so each IS a shadow-including descendant of it, and a walk that "
          "does not find one has a different idea of that tree from the one the climb used. The two are DOM "
          "§4.8 \"Interface ShadowRoot\"'s shadow-including tree order and CSS Cascade §7.2's flat-tree "
          "parent chain, and they are inverses only while a shadow root's host and an element's shadow root "
          "name each other; the fix is at whichever of them has stopped doing so, never an order invented "
          "here");
    return 0;
}

/* THE ORDER BETWEEN TWO BOXES THAT SIT IN ONE STACKING CONTEXT: §9.9.1's layers, then its stack level, then
   its tree order. The three are asked in that sequence because that is the sequence §9.9.1 states them in, and
   because each is a tie-break on the one before it.
   THE STACK-LEVEL COMPARISON IS ASCENDING IN BOTH DIRECTIONS, which CSS 2.1 §E.2's two steps say in two
   different phrasings of one rule: step 3 is "Stacking contexts formed by positioned descendants with negative
   z-indices (excluding 0) in z-index order (most negative first) then tree order" and step 9 is "Stacking
   contexts formed by positioned descendants with z-indices greater than or equal to 1 in z-index order
   (smallest first) then tree order". Most negative first and smallest first are the same direction. */
static int so_compare_in(JSContext *ctx, lxb_dom_element_t *context, lxb_dom_element_t *a, lxb_dom_element_t *b)
{
    StackingLayer la = stacking_layer_of(a), lb = stacking_layer_of(b);
    int va, vb;

    if (la != lb) return (int)la - (int)lb;
    if (la == STACKING_LAYER_NEGATIVE || la == STACKING_LAYER_POSITIVE) {
        va = stacking_level(a);
        vb = stacking_level(b);
        if (va != vb) return va < vb ? -1 : 1;
    }
    return so_tree_order(ctx, context, a, b);
}

int stacking_order_compare(JSContext *ctx, lxb_dom_element_t *a, lxb_dom_element_t *b)
{
    char nbuf[160];
    lxb_dom_element_t *pa, *pb, *prev_a, *prev_b;

    DCHECK(ctx != NULL, "CSS 2.1 §9.9.1's paint order was asked for with no realm — Appendix E §E.1's tree "
                        "order is DOM §4.8 \"Interface ShadowRoot\"'s shadow-including one, whose walker reads "
                        "a per-flow association kept on an element's wrapper");
    DCHECK(a != NULL && b != NULL, "CSS 2.1 §9.9.1's paint order was asked about a NULL element");
    /* THE BOX PRECONDITION, ASKED AT THIS ENTRY AND PER OPERAND. stacking_order.h states it for this call
       in its own words — both elements "must each generate a box" — and asserting it only where the
       partition is finally read reports `stacking_layer_of`'s line for a caller that never named it, which
       is the one shape a crash naming a remedy cannot be acted on in: the remedy is the CALLER's walk and
       the address printed is a helper two frames down. It is ONE check per operand so the abort names
       WHICH of the two, and it runs before the `a == b` arm because a caller comparing a boxless element
       with itself has made the identical mistake and a 0 would answer it. */
    DCHECKF(so_generates_box(a),
            "%s: CSS 2.1 §9.9.1's paint order was asked about a FIRST element that generates no box. Every "
            "sentence the order is built out of is stated over one — \"Each box belongs to one stacking "
            "context\", \"Boxes with greater stack levels are always formatted in front of boxes with lower "
            "stack levels\" — so there is nothing here to paint before or behind anything, and a number "
            "returned for it would order a box css-display-3 §2.5 \"Box Generation: the none and contents "
            "keywords\" says draws nothing",
            box_subject(a, nbuf, sizeof nbuf));
    DCHECKF(so_generates_box(b),
            "%s: CSS 2.1 §9.9.1's paint order was asked about a SECOND element that generates no box — see "
            "the first operand's abort above this line for why the order has no answer over one",
            box_subject(b, nbuf, sizeof nbuf));
    if (a == b) return 0;
    DCHECKF(lxb_dom_interface_node(a)->owner_document == lxb_dom_interface_node(b)->owner_document,
            "%s: CSS 2.1 §9.9.1's paint order was asked of two elements in DIFFERENT documents. CSS 2.1 §E.2 "
            "\"Painting order\" says why there is no answer: the canvas \"is infinite in extent and contains the "
            "root element\", one root element, and two documents are two canvases with no stacking context above "
            "either. What a caller holding boxes from two documents actually wants is the order of the two "
            "NAVIGABLES' boxes in the embedding document, which is a question about the embedder's own tree "
            "and is asked there",
            box_subject(a, nbuf, sizeof nbuf));

    /* THE ANCESTOR ARMS, FIRST, because they are the ONE case where the answer is a layer rather than a
       comparison. §9.9.1's layer 1 is "the background and borders of the element forming the stacking
       context", which is painted before every one of that context's descendants — so a box compared against
       the stacking context it lives inside is ordered by that sentence alone, whatever layer it is in. */
    for (pa = stacking_context_of(a); pa != NULL; pa = stacking_context_of(pa))
        if (pa == b) return 1;
    for (pb = stacking_context_of(b); pb != NULL; pb = stacking_context_of(pb))
        if (pb == a) return -1;

    /* THE REDUCTION ATOMICITY BUYS. §9.9.1: "A stacking context is atomic from the point of view of its parent
       stacking context; boxes in other stacking contexts may not come between any of its boxes." So two boxes
       in different contexts are ordered by whatever orders their contexts, recursively — climb both chains to
       the NEAREST common context and compare the two members that sit directly in it.
       THE OUTER LOOP IS OVER `a`'s CHAIN AND THAT IS WHAT MAKES IT THE NEAREST. Each chain is ordered
       deepest-first, so the first member of `a`'s chain that appears anywhere in `b`'s is the deepest common
       one; iterating `b`'s outermost would find whichever pairing came first rather than the deepest, and a
       common context that is not the nearest orders the wrong pair of members.
       BOTH CHAINS END AT THE ROOT, so the loops terminate with a match: `stacking_context_of` returns NULL
       only for the root element itself, and the root forms a context. */
    for (pa = a, prev_a = NULL; pa != NULL; prev_a = pa, pa = stacking_context_of(pa))
        for (pb = b, prev_b = NULL; pb != NULL; prev_b = pb, pb = stacking_context_of(pb))
            if (pa == pb) {
                DCHECK(prev_a != NULL && prev_b != NULL,
                       "CSS 2.1 §9.9.1's paint order found its nearest common stacking context AT one of the "
                       "two elements, which the ancestor arms above have already answered and returned for. "
                       "Reaching here means those two walks and this one disagree about the same chain — they "
                       "are the identical `stacking_context_of` climb, so the disagreement is that the chain "
                       "changed under the comparison, which a question about a document may not do");
                return so_compare_in(ctx, pa, prev_a, prev_b);
            }
    DFAIL("CSS 2.1 §9.9.1's paint order climbed both elements' stacking-context chains to the end without "
          "meeting a common context, and every chain ends at the root element because §9.9.1's first sentence "
          "makes the root form one. Both elements are in one document — asserted above — so they share a root, "
          "and a climb that does not reach it has left the element tree: `css_parent_element` is CSS Cascade "
          "§7.2's flattened parent and returns NULL for a node whose parent is a DocumentFragment that is not "
          "a shadow root, which is a node that is not IN the document at all. The caller's own walk is what "
          "established these boxes exist, so the fix is there rather than a root invented here");
    return 0;
}
