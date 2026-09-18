/* THE PAINTER — CSS 2.1 §E.2 "Painting order"'s offers turned into core/paint/display_list.h's marks. See
   box_paint.h for the seam, for why the offer count is an answer rather than an instrument, and for the two
   residuals that name the steps this file counts and does not paint. */
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
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
#include "core/html/html_image.h"    /* html_image_decoded_rgba — CSS 2.1 §E.2 step 7.2.1 item 4's
                                        third arm, "the replaced content", for the one replaced element that
                                        fetches its own */
#include "core/layout/block_flow.h"  /* CSS 2.2 §9.2.1.1's TWO shapes of one inline formatting context */
#include "core/layout/line_box.h"    /* line_box_glyphs — CSS 2.1 §E.2 step 7.2.1's "the text", placed */
#include "core/layout/replaced_element.h" /* the `replaced` bit that separates item 4's THIRD arm from
                                            its second — an inline-block is atomic and is not replaced */
#include "core/layout/used_value.h"   /* used_value_border_widths_px — CSS 2.1 §8.5's four USED widths, which
                                         the cascade does not hold for a table box under either border model */
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
 * in a display list is stated in. core/paint/display_list.h holds the argument for why a canvas mark carries
 * this rectangle at all rather than leaving it to whoever rasterizes.
 *
 * THE FOUR NUMBERS ARE core/frame/viewport.h's AND THIS FILE ASSEMBLES NONE OF THEM, which is what this
 * function is for: every operand of CSS 2.1 §2.3.1's region — CSS 2.1 §10.1's initial containing block and
 * the origin it is anchored at — belongs to the component that owns the viewport, and a second assembly of
 * them here would be a second answer free to disagree with what `innerWidth` reports about the same
 * viewport. What is left is the one part that is a PAINTER's question and not a frame's: WHICH REALM to ask.
 * That is also why a rasterizing caller holding a `JSContext` asks `viewport_canvas_region` directly rather
 * than reaching through an element for it.
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
    if (dctx == NULL) return false;
    return viewport_canvas_region(dctx, out);
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
static bool bp_background_color_at(BpState *st, lxb_dom_element_t *el, const CssPx rect[4])
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
    memcpy(mark.rect, rect, sizeof mark.rect);
    mark.color = color;
    display_list_append(st->out, &mark);
    return true;
}

/* THE SAME MARK AT THE BOX'S OWN BORDER BOX, which is every block-level caller's rectangle. It is a second
   ENTRY and emphatically not a second MARK: an inline box is laid at a FRAGMENT rectangle instead (see
   `bp_inline_box_marks`), and CSS 2.1 §E.2's step 7.2.1 item 1 and its step 2 and step 4 item 1 are one item
   under three steps, so one function builds the mark and the callers differ only in where it goes. */
static bool bp_background_color(BpState *st, lxb_dom_element_t *el)
{
    CssPx rect[4];

    element_view_bounding_box_px(el, rect);
    return bp_background_color_at(st, el, rect);
}

/* CSS 2.1 §8.5.3 "Border style: 'border-top-style', 'border-right-style', 'border-bottom-style',
 * 'border-left-style', and 'border-style'"' COMPUTED KEYWORD FOR ONE SIDE, as the ink vocabulary's enumerator.
 * THE KEYWORD TABLE IS §8.5.3's `<border-style>` IN §8.5.3's ORDER, so the index IS the enumerator and the two
 * cannot come apart by a rotation — the same device `CSS_BORDER_SIDES` uses one component over.
 * IT IS A SECOND LIST OF THOSE TEN NAMES AND THAT IS A DECISION. core/css/css_shorthand.c holds the first, as
 * a GRAMMAR: it decides what the parser admits, so that CSS Syntax drops a `border-style: nope` before it can
 * reach a computed value. That list is `static` and the component does not export it, and a painter is not
 * where an export to `core/css/` belongs. What keeps the two from drifting is the crash below rather than a
 * shared array: the grammar filters every declaration, so a computed style outside these ten is this engine's
 * GRAMMAR and this engine's VOCABULARY disagreeing and is not a keyword any document wrote — which is exactly
 * the line CLAUDE.md draws for what a DCHECK may stand on, and is the same standing core/layout/used_value.c's
 * `box-sizing` and `border-*-width` asserts have over the same road.
 * THE RELEASE ARM IS `none`, WHICH PAINTS NOTHING. A style this engine cannot name is one whose ink it cannot
 * draw, so the two available answers are no ink and a guess; §8.5.3's own initial value is `none` and no ink
 * is the one of the two that cannot put the WRONG thing on a page. */
static DisplayBorderStyle bp_border_style(lxb_dom_element_t *el, int side)
{
    static const char *const NAMES[4] = {
        "border-top-style", "border-right-style", "border-bottom-style", "border-left-style",
    };
    static const char *const KEYWORDS[] = {
        "none", "hidden", "dotted", "dashed", "solid", "double", "groove", "ridge", "inset", "outset",
    };
    char *v;
    unsigned i;

    DCHECK(side >= 0 && side < 4, "CSS 2.1 §8.5.3's line style was asked for a box side outside CSS 2.1 §8.1 "
                                  "\"Box dimensions\"' four");
    v = css_computed_value(el, NAMES[side < 0 ? 0 : side]);
    DCHECK(v != NULL, "the cascade produced no computed value for a `border-*-style`. CSS 2.1 §8.5.3's "
                      "`Initial:` line is `none` and core/css/css_style_declaration.c carries that initial for "
                      "all four sides, so CSS Cascade §7.1's last defaulting layer always answers");
    for (i = 0; v != NULL && i < sizeof KEYWORDS / sizeof KEYWORDS[0]; i++) {
        if (strcmp(v, KEYWORDS[i]) == 0) {
            free(v);
            return (DisplayBorderStyle)i;
        }
    }
    free(v);
    DFAIL("a `border-*-style` computed to a keyword outside CSS 2.1 §8.5.3 \"Border style: "
          "'border-top-style', 'border-right-style', 'border-bottom-style', 'border-left-style', and "
          "'border-style'\"' `<border-style>` value type. That type is a CLOSED list of ten and "
          "core/css/css_shorthand.c validates every `border-*-style` declaration against it, dropping the ones "
          "that do not match — so this is not a keyword a document wrote. Either that grammar admitted a value "
          "it should have dropped, or CSS 2.1 §8.5.3 gained a value and core/paint/display_list.h's "
          "`DisplayBorderStyle` was not grown with it. BUILD the missing enumerator in that vocabulary and add "
          "its keyword to the table above, which is the same ten in the same order");
    return DISPLAY_BORDER_STYLE_NONE;
}

/* CSS 2.1 §E.2's "border of element" — the THIRD item of its step 2 and step 4 block arms, and ONE mark for
 * the whole box. core/paint/display_list.h holds the argument for why it is one and not four.
 * THE RECTANGLE IS THE BORDER BOX, the same four numbers the background mark above takes, because
 * CSS 2.1 §8.1 "Box dimensions"' border edge is the outer edge of both areas — the background covers "the
 * content, padding and border areas" and the border is drawn inward from that same edge. Taking it from
 * `element_view_bounding_box_px` rather than assembling it here is the background mark's own reason: a second
 * derivation of one rectangle is a second answer free to disagree with what `getBoundingClientRect` reports.
 * THE WIDTHS COME FROM core/layout/used_value.h AND NOT FROM THE CASCADE, which is that entry's whole reason
 * for existing: CSS 2.1 §17.6.1 "The separated borders model" makes a row, row group, column or column group
 * box's border widths zero and CSS 2.1 §17.6.2 "The collapsing border model" replaces a table's and a cell's
 * with halves of resolved edges, and none of those is a value `border-*-width` holds.
 * A BOX WHOSE FOUR USED WIDTHS ARE ALL ZERO GETS NO MARK, which is the same rule `bp_background_color` applies
 * to an alpha of zero and is exact rather than an approximation: a border area of zero extent on every side
 * covers no pixel, so a mark for it is one nothing downstream could distinguish from its absence. A single
 * zero side is NOT that case and is stated positively beside its three siblings — that is what one mark per
 * box buys, and display_list.h says why.
 * A TRANSPARENT SIDE IS STILL A SIDE, deliberately, and the asymmetry with the background is CSS 2.1 §8.5.3's
 * own: it says of `groove`, `ridge`, `inset` and `outset` that their colour "depends on the element's border
 * color properties, but UAs may choose their own algorithm to calculate the actual colors used". So an alpha
 * of zero on a border side does not settle whether that side paints, and dropping ink on it here would be this
 * component answering a question CSS 2.1 leaves to whoever rasterizes. */
static bool bp_border_at(BpState *st, lxb_dom_element_t *el, const CssPx rect[4])
{
    static const char *const COLORS[4] = {
        "border-top-color", "border-right-color", "border-bottom-color", "border-left-color",
    };
    DisplayMark mark;
    CssPx width[4];
    int i;
    bool any = false;

    used_value_border_widths_px(el, width);
    for (i = 0; i < 4; i++)
        if (width[i].px != 0.0) any = true;
    if (!any) return true;
    for (i = 0; i < 4; i++) {
        mark.side[i].width = width[i];
        mark.side[i].style = bp_border_style(el, i);
        /* CSSOM §9 puts all four `border-*-color` longhands in its unconditional used-value list, so this is a
           question that entry answers rather than one it crashes on; a FALSE is this engine having no used
           colour for the property at all, and its own crash at the site says which absence that is. */
        if (!css_used_color(el, COLORS[i], &mark.side[i].color)) return false;
    }
    mark.kind = DISPLAY_MARK_BORDER;
    memcpy(mark.rect, rect, sizeof mark.rect);
    display_list_append(st->out, &mark);
    return true;
}

/* THE SAME MARK AT THE BOX'S OWN BORDER BOX — `bp_background_color`'s split, one item over and for the same
   reason: CSS 2.1 §E.2's step 7.2.1 item 3 and its step 2 and step 4 item 3 are one item under three steps. */
static bool bp_border(BpState *st, lxb_dom_element_t *el)
{
    CssPx rect[4];

    element_view_bounding_box_px(el, rect);
    return bp_border_at(st, el, rect);
}

/* CSS 2 §8.1 "Box dimensions"' CONTENT BOX ORIGIN of `el`, in CLIENT COORDINATES — the border box corner
   `element_view_bounding_box_px` answers, moved inward by the leading BORDER and the leading PADDING on each
   axis and by nothing else. §8.1 nests the four boxes in that order, so this is the section read forward
   rather than a coordinate derived a second way: core/layout/flow_position.h answers a padding box origin in
   the INITIAL CONTAINING BLOCK's space and every rectangle a mark carries is in CLIENT space, which
   core/dom/element_view.c reaches by subtracting the window scroll — so composing the two spaces here would
   be this file re-deriving that subtraction. */
static void bp_content_box_origin(lxb_dom_element_t *el, CssPx *x, CssPx *y)
{
    CssPx box[4], width[4];

    element_view_bounding_box_px(el, box);
    used_value_border_widths_px(el, width);
    *x = css_px_add(box[0], css_px_add(width[3], used_value_px(el, "padding-left")));
    *y = css_px_add(box[1], css_px_add(width[0], used_value_px(el, "padding-top")));
}

/* CSS 2.1 §E.2 "Painting order"'s STEP 7.2.1, ITEM 4, THIRD ARM — "For inline-level replaced elements: the
 * replaced content, atomically".
 *
 * WHICH OF THE TWO "replaced content, atomically" ITEMS THIS IS, because §E.2 has two and they are different
 * steps. The other is STEP 7.1, "If the element is a block-level replaced element, then: the replaced content,
 * atomically", which core/paint/paint_order.h offers as `PAINT_STEP_REPLACED_CONTENT` and which the switch in
 * `bp_step_marks` still answers with no ink. THIS one is inside step 7.2.1's own enumeration, reached for a
 * box on a LINE, and it is the one a default `img` takes: HTML §4.8.1 "Embedded content" makes `img` a
 * replaced element and the UA sheet leaves it `display: inline`, so it is INLINE-LEVEL and never block-level.
 * Building the block-level item on top of this one is a `used_value_content_px` and a rectangle away and is
 * deliberately not done here: it is offered through a different step, it is reached for a different set of
 * boxes, and a producer written at both at once would have one failure with two possible causes.
 *
 * "ATOMICALLY" IS DISCHARGED BY THE MARK BEING ONE MARK. §E.2 gives the replaced content as a single item of
 * its sub-list and core/paint/paint_order.h's own paragraph reads that as making the box a UNIT; a
 * `DISPLAY_MARK_IMAGE` is one mark carrying one rectangle and one bitmap, so there is no interior for another
 * step's ink to land inside and nothing here has to arrange that.
 *
 * THE RECTANGLE IS THE CONTENT BOX, and it is composed from the two derivations that already exist rather than
 * from a third: `bp_content_box_origin` is CSS 2 §8.1 "Box dimensions"' nesting read forward, which this file
 * already uses for the glyph origin, and `used_value_content_px` is core/layout/used_value.h's one answer for
 * a content extent — the same one CSS 2.1 §10.4 "Minimum and maximum widths" and §10.3.2's replaced-element
 * arm feed. An image is composited into that area and NOT into its natural size: css-images-3 §4.1
 * "Object-Sizing Terminology"' natural dimensions are the SOURCE's, and a `width` attribute or a `width`
 * declaration is exactly what makes the two differ.
 *
 * A REFUSAL LAYS NO MARK AND IS NOT AN ERROR. `html_image_decoded_rgba` answers NULL for an element that has
 * issued no request, one whose reply has not arrived, one the network refused, one whose state is `broken`
 * and one whose bytes are in a format this engine has no decoder for — and every one of those is a document's
 * or a server's doing rather than this engine's, so the answer is ink that is absent and never an abort. That
 * is the same shape `dlr_glyph` gives an empty outline one kind over: the mark count beside the surface is
 * what a reader tells the two apart by.
 *
 * IT RETURNS TRUE ON A REFUSAL, which is this file's own convention and is worth saying because the opposite
 * reading is available: a `false` return here means AN OPERAND THIS PAINTER COULD NOT COMPUTE, which stops
 * the walk and keeps the prefix, and an image nobody fetched is not that — it is a box that contributed no
 * ink, exactly as a `background-color: transparent` does. */
static bool bp_inline_replaced_content(BpState *st, lxb_dom_element_t *el)
{
    DisplayMark m;
    uint8_t *rgba;
    uint32_t w = 0, h = 0;

    st->offers++;
    rgba = html_image_decoded_rgba(st->ctx, el, &w, &h);
    if (rgba == NULL) return true;

    memset(&m, 0, sizeof m);
    m.kind = DISPLAY_MARK_IMAGE;
    bp_content_box_origin(el, &m.rect[0], &m.rect[1]);
    m.rect[2] = used_value_content_px(el, false);
    m.rect[3] = used_value_content_px(el, true);
    /* THE PIXELS ENTER THE LIST BEFORE THE MARK THAT NAMES THEM, which is the order core/paint/display_list.h
       forces and the only one that works: `display_list_add_bitmap` ANSWERS the index, so a mark built first
       would carry a promise about a bitmap that had not arrived, and `display_list_append` asserts against
       exactly that. The buffer is this caller's to free and the list took a COPY — see display_list.h's
       `DisplayImage` for why the list owns its pixels rather than borrowing them. */
    m.image.bitmap = display_list_add_bitmap(st->out, rgba, w, h, (size_t)w * (size_t)h * 4);
    free(rgba);
    display_list_append(st->out, &m);
    return true;
}

/* ---- CSS 2.1 §E.2's STEP 7.2.1 — THE BOXES IN A LINE BOX, AND THE MARKS EACH OF THEM LAYS ------------------ */

/* ONE CHARACTER, LAID AS A MARK. `origin_x` and `origin_y` are where the box holding this formatting context
   has its CONTENT BOX ORIGIN in CSSOM VIEW §6 "Extensions to the Element Interface"' CLIENT COORDINATES, and
   core/layout/line_box.h reports each character as an offset from exactly that corner — so the composition is
   one addition per axis and never a second derivation.
   THE COLOUR AND THE EM ARE READ FROM THE CHARACTER'S OWN ELEMENT, not from the box that establishes the
   context. CSS 2.1 §14.1 "Foreground color: the 'color' property" makes `color` "the foreground color of an
   element's text content" and it is an INHERITED property, so `<p>plain <b style="color:red">red</b></p>` is
   one formatting context whose characters have two colours; css-values-4 §6.1.1's em is the same shape one
   property over. `LineBoxGlyph.style` is the inline box each character is in for precisely this reason. */
static bool bp_glyph(BpState *st, const LineBoxGlyph *g, CssPx origin_x, CssPx origin_y)
{
    DisplayMark mark;

    mark.kind = DISPLAY_MARK_GLYPH;
    mark.glyph.cp = g->cp;
    mark.glyph.origin_x = css_px_add(origin_x, g->origin_x);
    mark.glyph.origin_y = css_px_add(origin_y, g->baseline_y);
    mark.glyph.em = css_font_size_px(g->style);
    /* CSSOM §9 puts `color` in its unconditional used-value list, so this is a question that entry answers
       rather than one it crashes on; a FALSE is this engine having no used colour for the property at all, and
       its own crash at the site says which absence that is. It is the same read `bp_border` makes for each of
       CSS 2.1 §8.5's four, one property over. */
    if (!css_used_color(g->style, "color", &mark.color)) return false;
    display_list_append(st->out, &mark);
    return true;
}

/* CSS 2.1 §E.2's STEP 7.2.1 ITEMS 1 AND 3 FOR ONE INLINE BOX — "background color of element" and "border of
 * element", laid ONCE PER FRAGMENT.
 *
 * ONE MARK PER FRAGMENT AND NOT ONE PER BOX, WHICH IS THE WHOLE DIFFERENCE FROM EVERY OTHER CALLER OF THESE
 * TWO MARKS. CSS 2.2 §9.4.2 "Inline formatting contexts" SPLITS an inline box across the line boxes it spans,
 * and CSS 2.1 §E.2 says so in its own note — "Some of the boxes may have been generated by line splitting or
 * the Unicode bidirectional algorithm". `element_view_bounding_box_px` answers CSSOM VIEW §6's
 * get-the-bounding-box, which is the UNION of those fragments, so painting a two-line `<span>`'s background
 * there would fill the rectangle spanning BOTH lines INCLUDING the gap between them and including whatever
 * sits to the left of the second line's start. That is ink in a place CSS 2.1 §14.2 "The background" does not
 * put it — WRONG rather than narrow, which is the one thing a painter may not be — so the rectangle comes from
 * §6's `getClientRects()` instead, whose step 3 is "one for each box fragment" and which
 * core/layout/line_box.h answers as `LineBoxFragment` in that section's own words.
 *
 * THE FRAME IS THE ESTABLISHING CONTAINER'S CONTENT BOX AND IS ASKED FOR HERE RATHER THAN PASSED IN, and that
 * is a correctness requirement and not a convenience. `LineBoxFragment` is measured from the content box origin
 * of the block container `line_box_inline_fragments` reports, and that entry states it "ADDS that origin to the
 * coordinates it measures inside the box" for CSS 2.2 §9.2.1.1's anonymous block boxes — so a fragment is
 * already in the CONTAINER's frame, while a glyph is in the ANONYMOUS BOX's and its caller adds that box's own
 * offset. The two frames differ by exactly that offset, so taking the glyph caller's origin here would move
 * every fragment of a MIXED container by one anonymous box's position.
 *
 * THE GEOMETRY IS ASKED FOR ONLY WHERE THERE IS A MARK TO PUT IN IT, which is the same rule
 * core/layout/scrolling_area.c states for its origin read: `line_box_inline_fragments` ABORTS for the writing
 * modes and box types it does not place, so asking it about a box that lays no ink would raise a crash at an
 * element this walk has nothing to draw for. */
static bool bp_inline_box_marks(BpState *st, lxb_dom_element_t *el)
{
    lxb_dom_element_t *establishing = NULL;
    LineBoxFragment *frag = NULL;
    CssColor color;
    CssPx width[4], ox, oy;
    size_t n, i;
    int side;
    bool ink = false, ok = true;

    if (!css_used_color(el, "background-color", &color)) return false;
    if (color.a != 0.0) ink = true;
    used_value_border_widths_px(el, width);
    for (side = 0; side < 4; side++)
        if (width[side].px != 0.0) ink = true;
    if (!ink) return true;

    n = line_box_inline_fragments(el, &establishing, &frag);
    DCHECK(establishing != NULL,
           "core/layout/line_box.h reported an inline box's fragments with no block container to measure them "
           "from — its own contract is that `*establishing` receives that container in both of CSS 2.2 "
           "§9.2.1.1's shapes, so a NULL here is that entry and this caller disagreeing about the frame");
    bp_content_box_origin(establishing, &ox, &oy);
    for (i = 0; i < n && ok; i++) {
        CssPx rect[4];

        rect[0] = css_px_add(ox, frag[i].inline_start);
        rect[1] = css_px_add(oy, frag[i].block_start);
        rect[2] = css_px_sub(frag[i].inline_end, frag[i].inline_start);
        rect[3] = css_px_sub(frag[i].block_end, frag[i].block_start);
        ok = bp_background_color_at(st, el, rect) && bp_border_at(st, el, rect);
    }
    free(frag);
    return ok;
}

/* CSS 2.1 §E.2's STEP 7.2.1, OVER THE BOXES THAT ARE CHILDREN OF `parent` IN THIS FORMATTING CONTEXT — "For
 * each box that is a child of that element, in that line box, in tree order:" and, for an inline box, its own
 * item 4 recursion, which §E.2 spells "Otherwise, jump to 7.2.1 for that element."
 *
 * `from` AND `to` ARE THE HALF-OPEN SIBLING RANGE THIS CONTEXT COVERS, which is `BlockFlowRun` read as
 * core/layout/block_flow.h defines it and is why the top-level call cannot simply walk every child. §9.2.1.1
 * puts a MIXED container's inline content in one anonymous block box PER MAXIMAL RUN, so a container's child
 * list holds the boxes of SEVERAL contexts; a walk that took the whole list would lay every inline box's
 * background once per run. The recursion passes `first_child` and NULL because an inline box's children are all
 * in the one run its own box is in.
 *
 * THE GLYPH CURSOR IS THE SECOND HALF OF THE ENUMERATION AND IS WHY THIS IS ONE WALK AND NOT TWO. §E.2's item 4
 * interleaves BOXES with RUNS OF TEXT — "all the element's in-flow, non-positioned, inline-level children that
 * are in this line box, AND ALL RUNS OF TEXT INSIDE THE ELEMENT that is on this line box, in tree order" — so a
 * nested box's background goes down BETWEEN two of its parent's runs of text, and a walk that laid the boxes
 * and the text in two passes could not place it. `line_box_glyphs` reports the context's characters in CONTENT
 * order, which IS Appendix E §E.1's tree order over the same nodes, so ONE monotone cursor over that array
 * interleaves correctly with this tree walk and no character is measured a second time —
 * core/layout/text_run.h's own rule that "two sums of one line hand a caller a fragment whose left edge and
 * width describe different text".
 *
 * A TEXT NODE IS §9.2.2.1's ANONYMOUS INLINE BOX AND LAYS ONLY ITEM 4. CSS 2.2 §9.2.2.1 "Anonymous inline
 * boxes" makes "any text that is directly contained inside a block container element ... an anonymous inline
 * element", so such a box IS one of the boxes item 7.2.1 enumerates — and CSS 2.2 §9.2.1.1 gives it "the
 * properties of anonymous boxes are inherited from the enclosing non-anonymous box … Non-inherited properties
 * have their initial value", of which `background-color` and `border-style` are two. Its items 1 and 3 are
 * therefore derivably no ink, which is why the walk lays its text and asks for no geometry: an anonymous box
 * has no element for `element_view_bounding_box_px` to be asked about.
 *
 * EVERY ELEMENT CHILD IS DESCENDED INTO AND ONLY A NON-ATOMIC INLINE BOX GETS MARKS, which is two rules and
 * not one loose one. The MARKS are item 7.2.1's and are stated over the boxes item 4 names — "in-flow,
 * non-positioned, inline-level" — so `paint_order_inline_kind` decides them. The DESCENT is about the glyph
 * cursor: a character whose box this walk stepped over would leave the cursor standing on it, and every later
 * character of the context would be laid at the wrong point in the sequence. Descending into a box that lays
 * no marks therefore keeps the CHARACTERS where they were while adding ink for the boxes §E.2 gives ink to,
 * and a box that contributes no character (an atomic inline reaches the line as one U+FFFC item, which
 * `line_box_glyphs` does not emit) consumes nothing and costs nothing. */
static bool bp_step_7_2_1(BpState *st, lxb_dom_element_t *parent, lxb_dom_node_t *from, lxb_dom_node_t *to,
                          const LineBoxGlyph *g, size_t n, size_t *cursor, CssPx origin_x, CssPx origin_y)
{
    lxb_dom_node_t *child;

    for (child = from; child != to && child != NULL; child = child->next) {
        if (child->type == LXB_DOM_NODE_TYPE_TEXT) {
            while (*cursor < n && g[*cursor].style == parent) {
                if (!bp_glyph(st, &g[*cursor], origin_x, origin_y)) return false;
                (*cursor)++;
            }
            continue;
        }
        if (child->type == LXB_DOM_NODE_TYPE_ELEMENT) {
            lxb_dom_element_t *box = lxb_dom_interface_element(child);

            if (paint_order_inline_kind(box) == PAINT_INLINE_NON_ATOMIC && !bp_inline_box_marks(st, box))
                return false;
            /* ITEM 4's THIRD ARM. `PAINT_INLINE_ATOMIC` is item 4's second AND third arms together —
               core/paint/paint_order.c answers it for an inline-block and an inline-table as well as for a
               replaced element — so the `replaced` test is what separates the two, and without it an
               `inline-block` would be asked for a replaced content it does not have. The second arm is a
               pseudo-stacking-context and is core/paint/paint_order.h's own residual, not this one's. */
            if (paint_order_inline_kind(box) == PAINT_INLINE_ATOMIC && replaced_element_of(box).replaced
                && !bp_inline_replaced_content(st, box))
                return false;
            if (!bp_step_7_2_1(st, box, child->first_child, NULL, g, n, cursor, origin_x, origin_y))
                return false;
        }
    }
    return true;
}

/* ONE FORMATTING CONTEXT, PAINTED AS CSS 2.1 §E.2's STEP 7.2.1. `origin_x` and `origin_y` are the CONTENT BOX
   ORIGIN of the box holding this context, in CLIENT COORDINATES.
   THE CURSOR IS ASSERTED TO REACH THE END, which is this engine's own two-sided invariant and not a statement
   about any document: `line_box_glyphs` and this tree walk are two readings of ONE formatting context that
   this codebase computed, so a character the fill placed and the walk never reached is those two components
   disagreeing about which nodes the context holds. It is checked only on a TRUE return, because a painter that
   stopped at an operand it could not compute is meant to keep its prefix. */
static bool bp_context_step_7_2_1(BpState *st, lxb_dom_element_t *style, BlockFlowRun run,
                                  CssPx origin_x, CssPx origin_y)
{
    LineBoxGlyph *g = NULL;
    size_t n = line_box_glyphs(style, run, &g), cursor = 0;
    lxb_dom_node_t *from = run.after != NULL ? run.after->next
                                             : lxb_dom_interface_node(style)->first_child;
    bool ok = bp_step_7_2_1(st, style, from, run.end, g, n, &cursor, origin_x, origin_y);

    DCHECKF(!ok || cursor == n,
            "CSS 2.1 §E.2 \"Painting order\"'s step 7.2.1 walked one inline formatting context's boxes in tree "
            "order and laid %zu of the %zu characters core/layout/line_box.h placed on its lines. Both numbers "
            "are this engine's own readings of ONE context — the fill's items and this walk's child lists — so "
            "a character the walk never reached is a node `line_box_glyphs` collected and "
            "`BlockFlowRun`'s sibling range does not cover",
            cursor, n);
    free(g);
    return ok;
}


/* CSS 2.1 §E.2's STEP 7.2 OVER ONE BLOCK-LEVEL BOX — every inline formatting context inside it, in tree
 * order, with each context's step 7.2.1 performed at the position CSS 2.2 §9.4.1's stack put its box.
 *
 * IT IS TWO SHAPES BECAUSE CSS 2.2 §9.2.1 GIVES ONE CONTEXT TWO, which core/layout/block_flow.h states and
 * core/layout/scrolling_area.c already walks exactly this way — a container CSS 2.2 §9.4.2 "Inline formatting
 * contexts" describes as one "that contains no block-level boxes" establishes ONE context that its own
 * ELEMENT names, and a MIXED container's inline-level children
 * sit inside §9.2.1.1's anonymous block boxes, one per maximal run, none of which the element tree contains.
 * A box that is neither — one holding only block-level boxes — has no context and lays no mark here, which is
 * a positive answer and not a skip: its descendants get their own offers, because §E.2's step 7 is stated
 * "first for the element, then for all its in-flow, non-positioned, block-level descendants in tree order".
 * THE ANONYMOUS BOX'S OWN OFFSET IS ADDED AND NOT ASSUMED AWAY. `BlockFlowAnonBox` reports it and its inline
 * component is derivably zero — §9.2.1.1's initial values with CSS 2.1 §10.3.3's constraint equation — but the
 * enumeration REPORTS it, and a consumer that dropped a reported field would be reading one of two numbers and
 * trusting the other, which is core/layout/line_box.c's own words about the same pair.
 *
 * RETIRED CLAUSE — THIS ARM'S OWN NEXT-DIFF HALF SENT ITS READER TO THE WRONG COMPONENT, AND IT IS KEPT
 * BECAUSE THE REASONING THAT PRODUCED IT IS THE REASONING A READER RE-DERIVES. It read, in one run:
 * `core/paint/paint_order.h's own residual (b) — that component sequences step 7.2.1, offering each box in a line box in tree order instead of offering the block once, at which point this arm becomes one offer per box and the three items ahead of the text are three arms beside this one.`
 * Its SPEC half is exact and is what landed: §E.2's step 7.2.1 IS an enumeration over the boxes in a line box
 * in tree order, and the items ahead of the text are the three this file now lays two of. Its MECHANISM half —
 * that `paint_order_walk` performs it — is not buildable, on THREE independent grounds, each checkable without
 * running anything.
 * FIRST, `PaintOrderVisit` NAMES AN ELEMENT AND THIS ENUMERATION'S MEMBERS ARE NOT ALL ELEMENTS. Appendix E
 * §E.1 "Definitions" says outright that in §E.2 "'element' refers to actual elements, pseudo-elements, and
 * ANONYMOUS BOXES", and CSS 2.2 §9.2.2.1 "Anonymous inline boxes" makes every run of text directly inside a
 * block container one of them. So the boxes step 7.2.1 enumerates for `<p>A <span>B</span></p>` are an
 * anonymous inline box and the `span`, and offering the first is a call that component's visitor has no
 * argument for — which is the shape paint_order.h's own third residual already records for an anonymous TABLE
 * box. Replacing the block's single offer with one per box, as the clause says, would therefore have made
 * every run of text directly inside a block unreachable.
 * SECOND, THE ENUMERATION INTERLEAVES BOXES WITH RUNS OF TEXT, AND A RUN OF TEXT IS NOT A BOX EITHER. §E.2's
 * item 4 is stated over "all the element's in-flow, non-positioned, inline-level children that are in this line
 * box, AND ALL RUNS OF TEXT INSIDE THE ELEMENT that is on this line box, in tree order", so a nested box's
 * background goes down BETWEEN two of its parent's runs — and one element has as many runs as it has text
 * children, which a `(step, element)` pair cannot tell apart. An order component that offered only the boxes
 * would hand this file a sequence with no place to put the text.
 * THIRD, "FOR EACH LINE BOX" IS LINE-BREAKING GEOMETRY, WHICH paint_order.h's OWN OPERAND CRITERION EXCLUDES:
 * that header's argument for why it can be complete while no ink exists is that "every operand of the first is
 * a computed `display`, `position`, `float` and `z-index` plus tree order and NOTHING ELSE", with "colour,
 * geometry, font, table grid and image decoding appear only inside the sub-lists". Which fragment landed on
 * which line is geometry by that sentence's own classification.
 * WHAT LANDED is the enumeration HERE, in `bp_step_7_2_1`, walking the child lists §9.2.1.1's run delimits and
 * carrying ONE monotone cursor over `line_box_glyphs`' characters so the boxes and the runs of text interleave
 * in one pass. `paint_order_walk` still offers the block once, and what it gained is the CLASSIFICATION
 * `paint_order_inline_kind` — which is a question about a computed `display` and a `position`, so it is that
 * component's by the same criterion that keeps the enumeration out of it.
 *
 * NAMED RESIDUAL — STEP 7.2.1's ITEM 2 IS THE BACKGROUND IMAGE, WHICH NO STEP OF §E.2 CAN LAY.
 * WHAT IS NOT COVERED: `bp_inline_box_marks` lays items 1 and 3 — "background color of element" and "border of
 * element" — and not item 2, "background image of element". It is the same ONE gap box_paint.h's first
 * residual names for the canvas: no mark kind in core/paint/display_list.h holds an image, and nothing in this
 * engine turns a `<url>` into pixels, so every image item of every step of §E.2 waits on one diff and this item
 * is not a step-7.2.1 absence at all.
 * WHAT THE NEXT DIFF BUILDS: not this component's — see box_paint.h's first residual, which names the `<image>`
 * road that ends at core/css/css_image.h's validity test.
 * HOW ITS ABSENCE WOULD BE OBSERVED: a `<span>` whose background is declared as an image alone paints no
 * background, while the same span declaring a colour beside the image paints the colour and none of the image.
 * RETIREMENT: this record goes when `bp_inline_box_marks` appends a mark for §E.2's step 7.2.1 item 2.
 *
 * NAMED RESIDUAL — A BOX §E.2 REACHES BY ANOTHER STEP STILL HAS ITS CHARACTERS LAID HERE.
 * WHAT IS NOT COVERED: `bp_step_7_2_1` descends into EVERY element child to keep the glyph cursor in step, and
 * lays marks only where `paint_order_inline_kind` answers `PAINT_INLINE_NON_ATOMIC`. A POSITIONED inline box is
 * therefore walked THROUGH — its characters are laid at this block's step 7.2 offer — while §E.2 puts them in
 * its step 8 or step 9, inside that box's own pseudo-context or stacking context. That is not a change this
 * diff made: `line_box_glyphs` reports one formatting context's characters whoever owns them, and laying them
 * here is what this file did before the enumeration existed. What the enumeration adds is that such a box gets
 * NO background and NO border here, which is the half §E.2 would otherwise have put at the wrong stack level.
 * WHAT THE NEXT DIFF BUILDS: the characters' removal from this context, which is core/layout/line_box.h's
 * question and not this file's — a positioned box is out of flow, so whether its items belong to the run at all
 * is `lb_fill`'s partition to state, and until it states one there is nothing here to route.
 * HOW ITS ABSENCE WOULD BE OBSERVED: `<p>a <span style="position:relative; z-index:1">b</span></p>` over a
 * later positioned box paints "b" UNDER it, where a browser puts "b" on top — the text is drawn, at the
 * enclosing block's stack level rather than at its own.
 * RETIREMENT: this record goes when the characters of a box §E.2 reaches by another step are not in the
 * establishing context's glyph list.
 *
 * NAMED RESIDUAL — AN INLINE BOX THAT PLACES NO CHARACTER BETWEEN TWO RUNS OF ONE PARENT IS LAID LATE.
 * WHAT IS NOT COVERED: a text child's characters are consumed while `LineBoxGlyph.style` is that text node's
 * parent, which is the only delimiter `line_box_glyphs` reports — it carries the element a character's box
 * has and not the text node the character came from. Two runs of one parent separated by an inline box that
 * places NO character are therefore consumed together, at the FIRST of the two, so that box's background and
 * border go down after BOTH runs instead of between them. A box that places a character delimits the two runs
 * by itself and is exact; this is the empty case alone — `<p>A <span style="background:yellow"><img></span>
 * B</p>`, where the span's only content reaches the line as css-text-3 §5.5's U+FFFC and `line_box_glyphs`
 * emits no character for it.
 * WHAT THE NEXT DIFF BUILDS: a delimiter on the character `line_box_glyphs` reports — the text NODE beside the
 * element whose properties the box has — which is one field on `LineBoxGlyph` and is core/layout/text_run.h's
 * to carry, since `tr_append` already asserts that a character's `style` is its text node's parent and
 * therefore holds both halves at the point it appends.
 * HOW ITS ABSENCE WOULD BE OBSERVED: the span's background is painted OVER the parent's later text where the
 * two overlap, which needs a negative margin to be visible at all — both runs are on the same line and the
 * span sits between them, so their rectangles are disjoint without one.
 * RETIREMENT: this record goes when a run of text is delimited by its own node rather than by its parent. */
static bool bp_line_boxes(BpState *st, lxb_dom_element_t *el)
{
    BlockFlowAnonBox *v;
    CssPx x, y;
    size_t n, i;
    bool ok = true;

    if (block_flow_establishes_inline_context(el)) {
        BlockFlowRun whole;

        /* §9.4.2's context with an ELEMENT to name it is the WHOLE of this container's content — the run with
           no break on either side of it, which is the shape core/layout/block_flow.h's enumeration yields for
           a container that holds no block-level box, and the two offsets between the frames are then zero
           because there is no box between them. */
        whole.after = NULL;
        whole.end = NULL;
        bp_content_box_origin(el, &x, &y);
        return bp_context_step_7_2_1(st, el, whole, x, y);
    }
    n = block_flow_anonymous_boxes(el, &v);
    /* THE CONTAINER'S OWN CONTENT ORIGIN IS ASKED FOR ONLY WHERE THERE IS A BOX TO MEASURE FROM IT, which is
       a crash surface and not a saving — core/layout/scrolling_area.c states the identical reason for the
       identical read: core/layout/flow_position.h ABORTS for every positioning scheme it does not implement,
       so reading an origin for a container that generates none of §9.2.1.1's boxes would raise a float's or
       an out-of-flow box's crash at an element this walk has nothing to lay. */
    if (n == 0) { free(v); return true; }
    bp_content_box_origin(el, &x, &y);
    for (i = 0; i < n && ok; i++)
        ok = bp_context_step_7_2_1(st, el, v[i].run, css_px_add(x, v[i].content_x), css_px_add(y, v[i].content_y));
    free(v);
    return ok;
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
        /* AND THE ROOT CLAUSE REACHES THE BACKGROUND AND NOT THE BORDER, which is why this member no longer
           shares an arm with the table one. CSS 2.1 §E.2's step 2 block arm is three items and the clause is
           written on TWO of them — "background color of element unless it is the root element", "background
           image of element unless it is the root element", "border of element" — so a root element paints no
           background here and DOES paint its border. CSS 2.1 §14.2 "The background" is the same sentence from
           the other side and is about the background alone: it moves "the background properties" onto the
           canvas and says "The root element does not paint this background again", naming nothing of §8.5's
           twelve. An arm that returned early for the root would therefore suppress ink CSS 2.1 §E.2 lays for
           every bordered root in every document. */
        if (!bp_is_root_element(el) && !bp_background_color(st, el)) return false;
        return bp_border(st, el);
    /* CSS 2.1 §E.2's step 2 TABLE arm item 1, which carries the clause on the item itself — "table
       backgrounds (color then image) unless it is the root element". That arm's borders are its item 7 and are
       `PAINT_STEP_TABLE_BORDERS` below, offered separately with five background levels between them. */
    case PAINT_STEP_CONTEXT_TABLE_BACKGROUND:
        if (bp_is_root_element(el)) return true;
        return bp_background_color(st, el);
    /* CSS 2.1 §E.2's STEP 4, BOTH ARMS. Neither carries the root clause and neither needs one: step 4 walks a
       context's DESCENDANTS and the root element is nothing's descendant. */
    case PAINT_STEP_DESCENDANT_BOX:
        return bp_background_color(st, el) && bp_border(st, el);
    case PAINT_STEP_DESCENDANT_TABLE_BACKGROUND:
    /* CSS 2.1 §17.5.1 "Table layers and transparency"' SIXTH LAYER — the cells. A cell's background is its own
       box's, which is what makes it the one internal level this mark can place; the four between it and the
       table box are box_paint.h's third residual. */
    case PAINT_STEP_CELL_BACKGROUND:
        return bp_background_color(st, el);
    /* NO MARK LAID — counted as offers and painted nowhere, for two different reasons. The four intermediate
       table background levels are box_paint.h's third residual and are a GEOMETRY this engine does not derive.
       The TWO REMAINING content steps are its second and no longer share one absence: step 7.1's replaced
       content is a SURFACE that core/paint/display_list.h has no kind for, while step 6's line boxes want the
       ENUMERATION core/paint/paint_order.h's residual (c) names and not a mark — the glyph kind exists and
       `PAINT_STEP_LINE_BOXES` below lays it. `PAINT_STEP_TABLE_BORDERS` is neither: the MARK now exists, and
       what that item still wants is the
       ENUMERATION of which boxes' borders it covers and in what order — CSS 2.1 §E.2's item is "all table
       borders (in tree order for separated borders)" and paint_order.h offers it ONCE carrying the table, so
       painting the table's own border here would be one box's ink where CSS 2.1 §E.2 asks for every box's. */
    case PAINT_STEP_COLUMN_GROUP_BACKGROUND:
    case PAINT_STEP_COLUMN_BACKGROUND:
    case PAINT_STEP_ROW_GROUP_BACKGROUND:
    case PAINT_STEP_ROW_BACKGROUND:
    case PAINT_STEP_TABLE_BORDERS:
    /* STILL NO MARK, AND THE TWO REMAINING CONTENT STEPS ARE NOT THE SAME ABSENCE. `PAINT_STEP_REPLACED_CONTENT`
       is CSS 2.1 §E.2's step 7.1 "the replaced content, atomically", which core/paint/paint_order.h calls a
       SURFACE rather than a mark and which this vocabulary has no word for. `PAINT_STEP_INLINE_LINE_BOXES` is
       step 6 — the line boxes an INLINE stacking context is in — and its sub-list is the same step 7.2.1
       `bp_step_7_2_1` performs below; what it waits on is the ENTRY into that walk for a box that is ON a line
       rather than one that establishes the lines. See box_paint.h's residual for the triple it needs. */
    case PAINT_STEP_INLINE_LINE_BOXES:
    case PAINT_STEP_REPLACED_CONTENT:
        return true;
    /* CSS 2.1 §E.2's STEP 7.2 — "Otherwise, for each line box of that element", whose sub-list is step 7.2.1's
       enumeration over the boxes in each of those line boxes. See `bp_line_boxes` for the two shapes of one
       formatting context and `bp_step_7_2_1` for the enumeration and for why it is performed here. */
    case PAINT_STEP_LINE_BOXES:
        return bp_line_boxes(st, el);
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
