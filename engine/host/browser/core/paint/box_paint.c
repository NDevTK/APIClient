/* THE PAINTER — CSS 2.1 §E.2 "Painting order"'s offers turned into core/paint/display_list.h's marks. See
   box_paint.h for the seam, for why the offer count is an answer rather than an instrument, and for the two
   residuals that name the steps this file counts and does not paint. */
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include <lexbor/dom/dom.h>
#include <lexbor/html/html.h>   /* lxb_html_tree_node_is — the ELEMENT TYPE test CSS 2.1 §14.2 is stated over */

#include "check.h"
#include "core/css/css_color.h"
#include "core/css/css_computed_value.h"
#include "core/css/css_length.h"
#include "core/dom/document.h"       /* document_active_realm_of — CSS 2.1 §10.1's initial containing block is
                                        the ELEMENT's document's, never the running realm's */
#include "core/dom/element_view.h"
#include "core/frame/viewport.h"
#include "core/paint/box_paint.h"
#include "core/paint/display_list.h"
#include "core/paint/paint_order.h"
#include "quickjs.h"

typedef struct {
    JSContext   *ctx;
    DisplayList *out;
    unsigned     offers;
} BpState;

/* "THE ROOT ELEMENT", ASKED OF THE DOCUMENT RATHER THAN OF THE PARENT. core/paint/paint_order.c and
   core/paint/stacking_order.c each hold one spelling of it — "the element whose parent is the Document" — and
   each says in its own words that a third would be the one that drifts and that it would route to the other
   when either is exported. Neither is, so this file asks lexbor's OWN entry instead of writing a fourth copy
   of that sentence: `lxb_dom_document_element` is the DOM's answer to the same question and cannot drift from
   a predicate it does not contain. */
static bool bp_is_root_element(lxb_dom_element_t *el)
{
    lxb_dom_node_t *n = lxb_dom_interface_node(el);

    DCHECK(n->owner_document != NULL, "a box was painted out of a node with no owner document — every node a "
                                      "tree walk reaches was minted by a Document and carries it");
    return lxb_dom_document_element(n->owner_document) == el;
}

/* CSS 2.1 §14.2 "The background"'s FIRST `body` ELEMENT CHILD of a root, which is the element CSS 2.1 §14.2
   names: "that element's first" HTML or XHTML `body` element child. A second `body` under one root is
   therefore not the element the rule reaches. The walk is over ELEMENT children because CSS 2.1 §14.2's own
   words are element child; a text node between the root and its body is not one. NULL when the root has no
   such child. */
static lxb_dom_element_t *bp_first_body_child(lxb_dom_element_t *root)
{
    lxb_dom_node_t *child;

    for (child = lxb_dom_interface_node(root)->first_child; child != NULL; child = child->next)
        if (child->type == LXB_DOM_NODE_TYPE_ELEMENT && lxb_html_tree_node_is(child, LXB_TAG_BODY))
            return lxb_dom_interface_element(child);
    return NULL;
}

/* CSS 2.1 §14.2's CANVAS PROPAGATION AS ONE FACT — WHICH ELEMENT'S BACKGROUND PROPERTIES PAINT THE CANVAS.
 * Two of CSS 2.1 §E.2 "Painting order"'s steps turn on this and they ask OPPOSITE questions: step 1 asks
 * WHOSE colour goes on the canvas, and steps 2 and 4 ask whether THIS box's background has been moved off it.
 * They are ONE FACT and two questions asked of it, so the fact is derived here once and each question is a
 * predicate over the answer. A second derivation would be a second answer free to disagree with the first,
 * and two answers disagreeing is a page whose background is painted twice or painted nowhere.
 *
 * CSS 2.1 §14.2's SENTENCE AND ITS TWO CONJUNCTS. The rule is that the root element's background becomes the
 * canvas's and "covers the entire canvas", and that "The root element does not paint this background again".
 * The condition that moves it names a root that is an HTML or XHTML `html` element "that has computed values
 * of 'transparent' for 'background-color' and 'none' for 'background-image'", and its consequence is that
 * user agents "must instead use the computed value of the background properties from that element's first"
 * HTML or XHTML `body` element child when painting backgrounds for the canvas, "and must not paint a
 * background for that child element".
 *
 * THE SECOND CONJUNCT IS READ THROUGH CSSOM §9 "Resolved Values" RATHER THAN THROUGH THE COMPUTED-VALUE
 * ENTRY. `css_computed_value` does not model `background-image` and crashes when asked for it;
 * `css_resolved_value` answers, and what it answers IS the computed value, because CSSOM §9's own last row is
 * "Any other property: the resolved value is the computed value" and `background-image` is in none of
 * CSSOM §9's special lists. So the conjunct is asked of the entry that has it.
 *
 * WHAT THE ANSWER RESTS ON IS ONE LAYER DOWN AND IS NOT THIS COMPONENT'S TO ASSERT. `background-image` is in
 * no property registry lexbor carries; what makes the cascade answer at all is that css-backgrounds-3 §2.3's
 * `Initial:` line is reachable through core/css/css_background_shorthand.c, so CSS Cascade §7.1's last
 * defaulting layer has something to fall to. Whether a DECLARED `url()` survives that route to be seen here
 * is a question about the CASCADE, and a painter that held its own crash for it would be asserting on a
 * neighbouring component's answer rather than on anything it computed — which is the assert CLAUDE.md forbids
 * standing on a page's own bytes, one layer removed. This file asks the question and takes the answer.
 *
 * ANSWERS THE ROOT wherever CSS 2.1 §14.2's condition does not hold, and also where it holds and the root has
 * no `body` element child at all — CSS 2.1 §14.2 names "that element's first" such child, so with none there
 * is nothing for the rule to use. NEVER NULL. */
static lxb_dom_element_t *bp_canvas_background_element(JSContext *ctx, lxb_dom_element_t *root)
{
    lxb_dom_element_t *body;
    CssColor root_bg;
    JSValue image;
    const char *image_text;
    bool is_none;

    DCHECK(root != NULL, "CSS 2.1 §14.2's canvas background was derived for a document with no root "
                         "element. CSS 2.1 §E.2's step 1 is offered only FOR a root element, and CSS 2.1 "
                         "§E.2's steps 2 and 4 reach this through a box whose own document answered one");
    if (!lxb_html_tree_node_is(lxb_dom_interface_node(root), LXB_TAG_HTML)) return root;
    /* THE FIRST CONJUNCT. A false answer from the derivation is not a third state here: its own crash has
       already named the absence in a dev build, and a root whose used colour this engine cannot derive is one
       whose background it cannot paint either — so the rule that moves the background onto the canvas may as
       well have applied. This is the arm this file already took before CSS 2.1 §E.2's step 1 laid any ink, kept
       deliberately rather than re-decided: it is reachable only in a release build, no gate here can exercise
       it, and what it now does with the `body`'s colour is strictly more than the nothing it used to do. */
    if (!css_used_color(root, "background-color", &root_bg)) {
        body = bp_first_body_child(root);
        return body != NULL ? body : root;
    }
    /* CSS Color 4 §6.3 "The transparent keyword" — "transparent specifies a transparent black" — so §14.2's
       first conjunct is an alpha of exactly zero, and the comparison is exact rather than approximate because
       the zero is the PARSE's and not arithmetic's: nothing between the declaration and here multiplies it. */
    if (root_bg.a != 0.0) return root;
    /* THE SECOND CONJUNCT, and the string is released on every arm including the one that cannot read it. */
    image = css_resolved_value(ctx, root, "background-image");
    DCHECK(JS_IsString(image),
           "CSSOM §9's resolved value of `background-image` is not a string. §9's last row makes it the "
           "COMPUTED value, and every arm of this engine's computed-value road answers text — an exception "
           "here is this engine's own allocation failing rather than anything the document declared");
    image_text = JS_ToCString(ctx, image);
    is_none = image_text != NULL && strcmp(image_text, "none") == 0;
    if (image_text != NULL) JS_FreeCString(ctx, image_text);
    JS_FreeValue(ctx, image);
    if (!is_none) return root;
    body = bp_first_body_child(root);
    return body != NULL ? body : root;
}

/* CSS 2.1 §14.2's RULE READ FROM THE BOX'S SIDE — HAS THIS ELEMENT'S BACKGROUND BEEN MOVED OFF IT? It has
   exactly when CSS 2.1 §14.2's consequence names this element: the canvas takes its background properties,
   and it is not the root, whose own clause is a different sentence and is performed by the step 2 arm below.
   THE `body` TEST IN FRONT IS A PRECONDITION AND NOT A SECOND ANSWER. `bp_canvas_background_element` answers
   either the root or a `body` element and the root is excluded on the line under it, so an element that is
   not a `body` is one that derivation cannot name — the test decides nothing, and what it buys is keeping a
   resolved-value query on the root off the path of every ordinary box the walk offers. THE CALLER HAS ALREADY
   ESTABLISHED THAT THIS ELEMENT HAS INK TO LAY — a background colour whose alpha is not zero — because for a
   transparent box CSS 2.1 §14.2's whole question is moot: it paints nothing under either answer. */
static bool bp_background_propagates_to_canvas(JSContext *ctx, lxb_dom_element_t *el)
{
    lxb_dom_node_t *n = lxb_dom_interface_node(el);
    lxb_dom_element_t *root;

    if (!lxb_html_tree_node_is(n, LXB_TAG_BODY)) return false;
    /* THE ROOT IS THE DOCUMENT'S OWN ANSWER, for `bp_is_root_element`'s reason above. */
    root = n->owner_document != NULL ? lxb_dom_document_element(n->owner_document) : NULL;
    if (root == NULL || root == el) return false;
    return bp_canvas_background_element(ctx, root) == el;
}

/* CSS 2.1 §2.3.1 "The canvas"'s RENDERED REGION FOR THIS DOCUMENT, in the CLIENT coordinates every rectangle
 * in a display list is stated in. CSS 2.1 §2.3.1 makes the canvas infinite and leaves the region "established
 * by the user agent according to the target medium"; CSS 2.1 §10.1 "Definition of "containing block""
 * establishes it for continuous media by making the initial containing block have "the dimensions of the
 * viewport" and be "anchored at the canvas origin", and CSS 2.1 §E.2 anchors the viewport there too —
 * "Initially, the viewport is anchored with its top left corner at the canvas origin".
 * core/paint/display_list.h holds the argument for why a canvas mark carries this rectangle at all rather
 * than leaving it to whoever rasterizes.
 *
 * THE ORIGIN IS (0, 0) WHATEVER THE SCROLL POSITION, and that is a fact about the coordinates rather than an
 * approximation: client coordinates are the viewport's own, the fill covers an INFINITE area, and so
 * whichever part of the canvas the viewport is showing is inside it. Nothing here reads a scroll offset
 * because there is no question for one to answer.
 *
 * THE REALM IS THE DOCUMENT'S AND NOT THE WALK'S. core/frame/viewport.h states why they differ — a child
 * navigable's viewport is 300 CSS pixels wide where the top-level traversable's is 1280 — so the ICB is asked
 * per the document the element is in, which is the road core/layout/used_value.c already takes for the same
 * fact.
 *
 * ANSWERS FALSE where that document is presented by no navigable. CSS 2.1 §10.1's ICB "has the dimensions of
 * the VIEWPORT", and a DOMParser document, an XHR `responseXML` or the document of a destroyed navigable has
 * none — so no region was ever established, and there is no rectangle to fill rather than a rectangle this
 * file could pick. That is core/paint/box_paint.h's own contract for an operand this painter cannot compute,
 * and it is not a second copy of core/layout/used_value.c's crash for the same absence: reaching that crash
 * requires a BOX, and CSS 2.1 §E.2's step 1 is offered before any box is. */
static bool bp_canvas_region(lxb_dom_element_t *root, CssPx out[4])
{
    lxb_dom_node_t *n = lxb_dom_interface_node(root);
    JSContext *dctx;

    DCHECK(n->owner_document != NULL, "CSS 2.1 §2.3.1's rendered region was asked for a root element whose "
                                      "node has no owner document — every node this engine mints belongs to "
                                      "the document that created it");
    dctx = document_active_realm_of(lxb_dom_interface_node(n->owner_document));
    if (dctx == NULL || !viewport_exists(dctx)) return false;
    out[0] = css_px(0.0);
    out[1] = css_px(0.0);
    out[2] = viewport_icb_width(dctx);
    out[3] = viewport_icb_height(dctx);
    return true;
}

/* CSS 2.1 §E.2's STEP 1, FIRST ITEM — "background color of element over the entire canvas", where WHICH
   element is CSS 2.1 §14.2's answer above and WHAT AREA is CSS 2.1 §2.3.1's. `root` is the element
   CSS 2.1 §E.2's step 1 was offered for, which core/paint/paint_order.c offers only when it is the
   document's root.
   AN ALPHA OF ZERO IS NO INK AND NOT A SMALL AMOUNT OF IT, exactly as it is for a box: a canvas whose colour
   is `transparent` is CSS 2.1 §E.2's own "The canvas is transparent if contained within another", and a fill
   that changes no pixel is a mark nothing downstream could distinguish from its absence. */
static bool bp_canvas_background(BpState *st, lxb_dom_element_t *root)
{
    DisplayMark mark;
    CssColor color;

    if (!css_used_color(bp_canvas_background_element(st->ctx, root), "background-color", &color)) return false;
    if (color.a == 0.0) return true;
    if (!bp_canvas_region(root, mark.rect)) return false;
    mark.kind = DISPLAY_MARK_FILL_CANVAS;
    mark.color = color;
    display_list_append(st->out, &mark);
    return true;
}

/* CSS 2.1 §E.2's FIRST MARK for a block-level box — its step 2 block arm opens "background color of element
   unless it is the root element" and its step 4 block arm opens "background color of element", which are the
   same mark under two steps and are therefore one function.
   THE RECTANGLE IS THE BORDER BOX, which is CSS 2.1 §14.2's own sentence: "in terms of the box model,
   background refers to the background of the content, padding and border areas", followed by "margins are
   always transparent". core/dom/element_view.h's `element_view_bounding_box_px` answers exactly that area in
   CSSOM VIEW §6 "Extensions to the Element Interface"'s client coordinates, which is why the mark takes it
   whole rather than assembling four edges here: a second derivation of one rectangle is a second answer free
   to disagree with what `getBoundingClientRect` reports about the same box. */
static bool bp_background_color(BpState *st, lxb_dom_element_t *el)
{
    DisplayMark mark;
    CssColor color;

    if (!css_used_color(el, "background-color", &color)) return false;
    /* AN ALPHA OF ZERO IS NO INK AND NOT A SMALL AMOUNT OF IT, so skipping the mark is exact rather than an
       approximation — compositing a fully transparent fill changes no pixel. It is also what makes §14.2's
       question below moot for the common case: a box with nothing to lay has nothing for that rule to move. */
    if (color.a == 0.0) return true;
    if (bp_background_propagates_to_canvas(st->ctx, el)) return true;
    mark.kind = DISPLAY_MARK_FILL_RECT;
    element_view_bounding_box_px(el, mark.rect);
    mark.color = color;
    display_list_append(st->out, &mark);
    return true;
}

static bool bp_visit(PaintStep step, lxb_dom_element_t *el, void *user)
{
    BpState *st = user;

    DCHECK(st != NULL, "CSS 2.1 §E.2's walk offered a box to this painter with no state behind it");
    DCHECK(el != NULL, "CSS 2.1 §E.2's walk offered a box with no element — paint_order.h asserts that every "
                       "offer names an element of the document the walk was started in");
    st->offers++;
    switch (step) {
    /* CSS 2.1 §E.2's STEP 1 — THE CANVAS. Its FIRST item only; the second is the canvas's background IMAGE
       and is box_paint.h's first residual, which is the same missing mark kind every other step's image item
       waits on. CSS 2.1 §E.2 offers this step for the root element, and CSS 2.1 §14.2 decides whether the
       root's own background properties or its first `body` child's are the ones the canvas takes. */
    case PAINT_STEP_ROOT_BACKGROUND:
        return bp_canvas_background(st, el);
    /* CSS 2.1 §E.2's STEP 2, BOTH ARMS — and the ROOT CLAUSE, which is the sub-list's and therefore this
       file's. Step 2's block arm reads "background color of element UNLESS IT IS THE ROOT ELEMENT" and its
       table arm's first item carries the same clause; core/paint/paint_order.h offers step 2 for the root like
       any other context element, because the clause is a statement about the MARK and that component states
       order. CSS 2.1 §14.2 "The background" is the same sentence from the other side — "The root element does
       not paint this background again" — so painting here would be ink at the root's border box that no
       browser lays, which is WRONG rather than narrow and is the one thing a painter may not be. */
    case PAINT_STEP_CONTEXT_BOX:
    case PAINT_STEP_CONTEXT_TABLE_BACKGROUND:
        if (bp_is_root_element(el)) return true;
        return bp_background_color(st, el);
    /* CSS 2.1 §E.2's STEP 4, BOTH ARMS. Neither carries the root clause and neither needs one: step 4 walks a
       context's DESCENDANTS and the root element is nothing's descendant. */
    case PAINT_STEP_DESCENDANT_BOX:
    case PAINT_STEP_DESCENDANT_TABLE_BACKGROUND:
    /* CSS 2.1 §17.5.1 "Table layers and transparency"' SIXTH LAYER — the cells. A cell's background is its own
       box's, which is what makes it the one internal level this mark can place; the four between it and the
       table box are box_paint.h's third residual. */
    case PAINT_STEP_CELL_BACKGROUND:
        return bp_background_color(st, el);
    /* NO MARK KIND YET — counted as offers and painted nowhere. The four intermediate table background levels
       are box_paint.h's third residual and are a GEOMETRY this engine does not derive; the borders and the
       three content steps are its second, and are a mark VOCABULARY core/paint/display_list.h does not have. */
    case PAINT_STEP_COLUMN_GROUP_BACKGROUND:
    case PAINT_STEP_COLUMN_BACKGROUND:
    case PAINT_STEP_ROW_GROUP_BACKGROUND:
    case PAINT_STEP_ROW_BACKGROUND:
    case PAINT_STEP_TABLE_BORDERS:
    case PAINT_STEP_INLINE_LINE_BOXES:
    case PAINT_STEP_REPLACED_CONTENT:
    case PAINT_STEP_LINE_BOXES:
        return true;
    case PAINT_STEP_OUTLINES:
        /* A GUARD AND NOT A GAP. `PaintStep` is a closed enum THIS engine writes and paint_order.h states in
           its own outline residual that `PAINT_STEP_OUTLINES` is declared and never offered — so this arm is
           unreachable by construction, and what would make it reachable is that component gaining step 10, at
           which point the remedy is a mark here and not a repair to this crash. */
        DFAIL("CSS 2.1 §E.2 \"Painting order\"'s step 10 was offered to a painter that has no outline mark. "
              "core/paint/paint_order.h's own residual says the step is declared and never offered at this "
              "revision, and it retires when `paint_order_walk` offers it — so reaching this arm means that "
              "diff landed and this one did not. BUILD the outline mark in core/paint/display_list.h beside "
              "the fill, over the three longhands css-ui-4 §3.1 \"Outlines Shorthand: the outline property\" "
              "defines");
        return false;
    }
    /* THE COMPILER IS THE REAL GUARD AND THIS IS ITS BACKSTOP. The switch above carries no `default:` label, so
       `-Wswitch` names THIS FILE at the moment core/paint/paint_order.h grows a step — which is how the eight
       table steps arrived here rather than being discovered by a document that painted nothing where a table
       was. A value that reaches this line is therefore not a step somebody added; it is a `PaintStep` outside
       the closed enum this engine writes, which is this engine's own memory. */
    DFAIL("CSS 2.1 §E.2 \"Painting order\" offered a step whose value is outside core/paint/paint_order.h's "
          "`PaintStep`. A step this painter has no arm for is caught at COMPILE time by -Wswitch, so what "
          "reaches here is not a new step but an out-of-range value");
    return false;
}

bool box_paint_stacking_context(JSContext *ctx, lxb_dom_element_t *context_el, DisplayList *out,
                                unsigned *offers)
{
    BpState st;
    bool ok;

    DCHECK(out != NULL, "CSS 2.1 §E.2's ink was asked for with no display list to lay it in");
    DCHECK(offers != NULL,
           "CSS 2.1 §E.2's ink was asked for with nowhere to report the offer count. A list of zero marks "
           "means two things a caller must tell apart — the walk ran and every box it offered was transparent, "
           "or the walk reached no box at all — and the count is the only thing that separates them, which is "
           "why it is required rather than optional");
    st.ctx = ctx;
    st.out = out;
    st.offers = 0;
    /* `ctx` and `context_el` are asserted by `paint_order_walk`, which owns both preconditions: the realm
       because Appendix E §E.1's tree order is the shadow-including one, and the stacking context because CSS
       2.1 §E.2 has no painting order for an element that forms none. Restating either here would be a second
       copy of one rule, reported at the caller instead of at the component that depends on it. */
    ok = paint_order_walk(ctx, context_el, bp_visit, &st);
    *offers = st.offers;
    return ok;
}
