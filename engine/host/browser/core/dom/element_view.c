/* CSSOM VIEW §6 — Extensions to the Element Interface. See element_view.h for the box model every member below
   branches over, for why a viewport-derived answer is concolic and a scroll position is not, and for which
   members of §6 are honestly absent. */
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <lexbor/dom/dom.h>

#include "check.h"
#include "quickjs.h"
#include "core/css/css_computed_value.h"
#include "core/dom/document.h"
#include "core/dom/element.h"
#include "core/dom/element_view.h"
#include "core/dom/node.h"
#include "core/frame/viewport.h"
#include "core/frame/window_proxy.h"
#include "core/geometry/dom_rect.h"
#include "core/geometry/dom_rect_list.h"
#include "core/idl_args.h"
#include "solver/concolic.h"

/* THE MEMBERS. `scrollTop` and `scrollLeft` are the two the IDL declares as `attribute` rather than `readonly
   attribute`, so their magic is a setter's magic as well as a getter's. */
typedef enum {
    EV_SCROLL_TOP, EV_SCROLL_LEFT,
    EV_SCROLL_WIDTH, EV_SCROLL_HEIGHT,
    EV_CLIENT_TOP, EV_CLIENT_LEFT,
    EV_CLIENT_WIDTH, EV_CLIENT_HEIGHT
} ElementViewMember;

static int g_id_set_scroll_top = -1, g_id_set_scroll_left = -1;
static int g_id_client_rects = -1, g_id_bounding_rect = -1;

/* EVERY MEMBER OF §6 OPENS WITH THE SAME FOUR QUESTIONS ABOUT THE ELEMENT AND ITS DOCUMENT, so they are asked
   once, here, rather than by each member — and asked of the ELEMENT'S node document, never of the running
   realm. "Let document be the element's node document" is the spec's own first step, and a member reached
   through one realm's Element.prototype on another realm's element (`iframe.contentDocument.body.clientWidth`
   is the ordinary way a page writes that) must answer for the element's document and not for the caller's.
   `dctx` is the realm whose ACTIVE document that is, which is NULL exactly when the spec's "if document is not
   the active document" step fires. */
typedef struct {
    lxb_dom_node_t *node;    /* the element */
    lxb_dom_node_t *doc;     /* its node document */
    JSContext      *dctx;    /* the realm whose active document `doc` is, or NULL */
    /* THE ELEMENT'S RELEVANT REALM — where a `[NewObject]` this member returns is created, and a DIFFERENT
       question from `dctx`. Web IDL makes a new platform object in the relevant realm of `this`, which for an
       element is the realm of its node document, so `iframe.contentDocument.body.getBoundingClientRect()`
       written through the PARENT's Element.prototype answers with an instance of the CHILD's DOMRect. A
       Document that is nobody's active document still has one, which is why the two fields cannot be merged:
       `new DOMParser().parseFromString(…, "text/html").body.getBoundingClientRect()` returns a real rectangle
       out of the realm that parsed it while `dctx` is correctly NULL. */
    JSContext      *rctx;
    bool            quirks;  /* §4.5's compat mode, read off the tree the parser built */
    bool            is_root; /* "the element is the root element" */
    bool            is_body; /* "the element is the body element" */
    bool            has_box; /* "the element has an associated box" — element_view.h's one predicate */
} EvTarget;

/* WEB IDL §3.7.5's BRAND CHECK. `Element.prototype.clientWidth` read off a plain object is a TypeError, and a
   page tells that apart from `undefined` — a feature detector that probes the descriptor and applies the getter
   reads the throw as "this is a real interface". Returns false with the TypeError pending. */
static bool ev_target(JSContext *ctx, JSValueConst this_val, EvTarget *t)
{
    lxb_dom_element_t *el = element_of_value(this_val);

    if (!el) {
        JS_ThrowTypeError(ctx, "a CSSOM VIEW Element member was reached on something that is not an Element");
        return false;
    }
    t->node = lxb_dom_interface_node(el);
    DCHECK(t->node->owner_document != NULL,
           "an element wrapper was reached whose node has no owner document — every node this engine mints "
           "belongs to the document that created it, so a null owner is a tree built outside the DOM layer");
    t->doc     = lxb_dom_interface_node(t->node->owner_document);
    t->dctx    = document_active_realm_of(t->doc);
    t->rctx    = document_realm_of(t->node);
    DCHECK(t->rctx != NULL,
           "a CSSOM VIEW §6 member was reached on an element whose node document has no realm — every Document "
           "a page can hold a node of was built by one, and the only trees without a record are the solver's "
           "own scratch parses, which are handed to nobody");
    t->quirks  = t->node->owner_document->compat_mode == LXB_DOM_DOCUMENT_CMODE_QUIRKS;
    t->is_root = document_document_element_of(t->doc) == t->node;
    t->is_body = document_body_of(t->doc) == t->node;
    t->has_box = element_view_has_box(t->node);
    /* "Let window be the value of document's defaultView attribute. If window is null, return zero." A
       Document this engine holds as a navigable's ACTIVE document is one whose realm has a Window, so the
       spec's null branch is unreachable here rather than unwritten — asserted at the one place both facts are
       in hand. A Document with no browsing context (createHTMLDocument, DOMParser, XHR's responseXML) is not
       any navigable's active document, so it leaves through the step above instead. */
    DCHECK(!t->dctx || window_proxy_is(document_window_proxy(t->dctx)),
           "§6's `let window be document's defaultView` found no Window on a Document that IS a navigable's "
           "active document — every such Document in this agent is presented by a navigable, and the navigable "
           "is what carries the WindowProxy");
    DCHECK(!t->has_box || (t->dctx && viewport_exists(t->dctx)),
           "an element was said to have an associated box while its document is not being presented — box "
           "existence is defined over exactly that (element_view.h), so the two answers have come apart");
    return true;
}

/* A member whose IDL type is `long`. Every length this component reports is a whole number of CSS pixels — it
   is the viewport's own size or it is zero — so a fraction here is a derivation that produced something the
   type cannot carry rather than a value to round. */
static JSValue ev_long(JSContext *ctx, double v)
{
    DCHECK(v == (double)(int32_t)v,
           "a CSSOM VIEW §6 Element member declared `long` computed a value that is not an integer");
    return JS_NewInt32(ctx, (int32_t)v);
}

/* A `long` member that reports the VIEWPORT: the modelled geometry as the EXAMPLE of a concolic, minted
   through viewport.h's one seam and keyed on the element's document — see element_view.h for why these are
   sources and the scroll positions are not. */
static JSValue ev_env_long(JSContext *ctx, const EvTarget *t, const char *member, double v)
{
    return viewport_env_value(t->dctx, member, ev_long(ctx, v));
}

/* THE `scrollX`/`scrollY` ATTRIBUTES' OWN ANSWER, which four of the steps below invoke by name: the viewport's
   scroll position, "or zero if there is no viewport". */
static double ev_window_scroll(JSContext *dctx, bool vertical)
{
    if (!viewport_exists(dctx)) return 0.0;
    return vertical ? viewport_scroll_y(dctx) : viewport_scroll_x(dctx);
}

bool element_view_has_box(const lxb_dom_node_t *n)
{
    const lxb_dom_node_t *a;
    JSContext *dctx;

    DCHECK(n && n->type == LXB_DOM_NODE_TYPE_ELEMENT,
           "the associated-box predicate was asked about a node that is not an element — only elements "
           "generate the principal boxes CSSOM VIEW §6 and HTML's `being rendered` are about");
    /* A node whose root is not a document has no boxes in any user agent. */
    if (!node_is_connected(n)) return false;
    /* And neither does one in a document nothing is presenting: a DOMParser document, a `<template>`'s
       contents owner and the document of a navigable that has been destroyed are all trees a user agent lays
       out nothing for. That is the same question viewport.h asks — is there a viewport — asked of the
       ELEMENT'S document. */
    if (!n->owner_document) return false;
    dctx = document_active_realm_of(lxb_dom_interface_node(n->owner_document));
    if (!dctx || !viewport_exists(dctx)) return false;
    /* AND THE COMPUTED `display`, which is the property this question was always about. This walk used to look
       for the `hidden` CONTENT ATTRIBUTE, which is one UA-stylesheet rule for one of the values `display` can
       take — so an author `display: none`, the rule for `<head>` and `<script>`, and a page's own
       `[hidden] { display: block }` overriding the UA rule were all invisible to it. The attribute is now
       decided where every other UA rule is (css_style_declaration.c's UA layer), and this asks the one
       question: an element whose computed display is `none` generates no box, an element whose computed
       display is `contents` generates none of its OWN (its children still do), and no descendant of a
       `display: none` ancestor generates one either. */
    for (a = n; a != NULL && a->type == LXB_DOM_NODE_TYPE_ELEMENT; a = a->parent) {
        char *d = css_computed_value(lxb_dom_interface_element((lxb_dom_node_t *)a), "display");
        bool boxless = d != NULL && (strcmp(d, "none") == 0 ||
                                     (a == n && strcmp(d, "contents") == 0));

        DCHECK(d != NULL, "the cascade produced no computed `display` for an element — the UA layer answers "
                          "`inline` for every element it does not name, so this cannot be unset");
        free(d);
        if (boxless) return false;
    }
    return true;
}

/* CSSOM VIEW's "POTENTIALLY SCROLLABLE IN AN AXIS" — the branch four of §6's steps take and none of them could
   decide until a C caller could read a computed value. The definition's own three conditions, in its own order:
   the body has an ASSOCIATED BOX; the body's PARENT ELEMENT's computed `overflow-x`/`overflow-y` (whichever is
   in the given axis) is neither `visible` nor `clip`; and the body's OWN is neither. */
static bool ev_axis_clips_or_scrolls(const lxb_dom_node_t *el, bool vertical)
{
    char *v = css_computed_value(lxb_dom_interface_element((lxb_dom_node_t *)el),
                                 vertical ? "overflow-y" : "overflow-x");
    bool yes = v != NULL && strcmp(v, "visible") != 0 && strcmp(v, "clip") != 0;

    free(v);
    return yes;
}

static bool ev_potentially_scrollable(const EvTarget *t, bool vertical)
{
    const lxb_dom_node_t *parent = t->node->parent;

    DCHECK(t->is_body, "CSSOM VIEW states `potentially scrollable` for the BODY ELEMENT, and every step that "
                       "branches on it has already established that this element is one");
    if (!t->has_box) return false;
    DCHECK(parent != NULL && parent->type == LXB_DOM_NODE_TYPE_ELEMENT,
           "the body element has no parent element to read an overflow off — HTML makes the body element a "
           "CHILD OF THE ROOT ELEMENT by definition, and an element with an associated box is connected, so "
           "the two facts together leave no tree in which this holds");
    return ev_axis_clips_or_scrolls(parent, vertical) && ev_axis_clips_or_scrolls(t->node, vertical);
}

/* "…is not potentially scrollable IN AT LEAST ONE AXIS" — the shape `scrollTop`/`scrollLeft` ask for. The
   `scrollWidth`/`scrollHeight` step asks about ONE NAMED axis instead, which is why both spellings exist and
   why neither is written in terms of the other. */
static bool ev_not_potentially_scrollable_in_some_axis(const EvTarget *t)
{
    return !ev_potentially_scrollable(t, false) || !ev_potentially_scrollable(t, true);
}

/* CSSOM VIEW §6's step 1 for the four `client*` members is "if the element has no associated box OR IF THE BOX
   IS INLINE, return zero". An INLINE box is CSS Display's `inline flow` — a computed `display` of `inline` —
   and NOT `inline-block`, which has a padding edge and border widths and answers with them. Anything this
   engine cannot place in one of those two reaches step 3's DFAIL rather than a zero. */
static bool ev_box_is_inline(const EvTarget *t)
{
    char *d = css_computed_value(lxb_dom_interface_element(t->node), "display");
    bool inl = d != NULL && strcmp(d, "inline") == 0;

    free(d);
    return inl;
}

/* ---- §6's `scrollTop` and `scrollLeft` ------------------------------------------------------------------- */

/* THE GETTER, IN THE SPEC'S OWN STEP ORDER. Every terminal it can reach in this engine is the ORIGIN, and each
   is a derivation rather than a stand-in:
     step 2 and step 3 are the spec's own zero for a document that is not being presented;
     step 5 hands the root element the WINDOW's scroll position, which viewport.h derives as the ICB origin —
       the viewport's scrolling area IS the ICB, so there is one valid scroll position;
     step 6 hands a quirks-mode BODY the same window scroll position when it is not potentially scrollable in
       at least one axis — a condition this file could not decide until there was a computed-value entry to
       read the body's and its parent's `overflow-x`/`overflow-y` through;
     step 7 is the spec's own zero for an element with no associated box;
     step 8 is the element's OWN current scroll position, which is the origin because nothing has scrolled it:
       a scroll position moves only when §3.1's perform a scroll runs, and the setter below DFAILs rather than
       running it (element_view.h). */
static JSValue ev_scroll_position(JSContext *ctx, const EvTarget *t, bool vertical)
{
    /* steps 2-3 */
    if (!t->dctx) return JS_NewFloat64(ctx, 0.0);
    /* step 4 */
    if (t->is_root && t->quirks) return JS_NewFloat64(ctx, 0.0);
    /* step 5 */
    if (t->is_root) return JS_NewFloat64(ctx, ev_window_scroll(t->dctx, vertical));
    /* step 6 */
    if (t->is_body && t->quirks && ev_not_potentially_scrollable_in_some_axis(t))
        return JS_NewFloat64(ctx, ev_window_scroll(t->dctx, vertical));
    /* step 7 */
    if (!t->has_box) return JS_NewFloat64(ctx, 0.0);
    /* step 8 */
    return JS_NewFloat64(ctx, 0.0);
}

/* THE SETTER — the same algorithm, and it RUNS rather than dropping the write. What makes the write a no-op is
   §4's scroll() clamping the requested position into a scrolling area that is exactly the viewport, which is
   the spec deciding and not this engine ignoring; viewport_scroll asserts it at the step that decides. */
static JSValue js_ev_set(JSContext *ctx, JSValueConst this_val, JSValueConst val, int magic)
{
    bool vertical = (magic == EV_SCROLL_TOP);
    /* UNKNOWN EXTERNAL INPUT REQUESTING A SCROLL POSITION IS NOT A FORK, and it is not read either. §4's
       scroll() clamps the request to max(0, min(x, scrolling area - viewport)), and with the scrolling area
       equal to the viewport that is the origin for EVERY value of the domain — so the algorithm's whole
       observable result (the viewport stays put, no scroll is performed, no scroll event is queued) is the same
       for the example as for every other value this could take, and there is no arm to explore. What the clamp
       would produce is the position the viewport already has, and that is what is passed on. viewport_scroll's
       own assert is what keeps that true: the day the clamp can land somewhere else, this value MATTERS and
       this branch has to fork. */
    bool unknown = concolic_is(val);
    EvTarget t;
    double v = 0.0;

    DCHECK(magic == EV_SCROLL_TOP || magic == EV_SCROLL_LEFT,
           "a CSSOM VIEW §6 setter was declared with a magic that is not one of the two members the IDL "
           "declares as a writable attribute");
    if (!ev_target(ctx, this_val, &t)) return JS_EXCEPTION;
    if (!unknown) {
        DCHECK(JS_IsNumber(val),
               "§6's scrollTop/scrollLeft setter was handed something that is not a number — its IDL type is "
               "`unrestricted double` and the declaration converts it, which is also what makes §3.2's "
               "normalization below reachable rather than a type error");
        JS_ToFloat64(ctx, &v, val);
        /* step 2 — §3.2's NORMALIZE NON-FINITE VALUES: Infinity, -Infinity and NaN all become 0. */
        if (!isfinite(v)) v = 0.0;
    }
    /* steps 3-6 */
    if (!t.dctx) return JS_UNDEFINED;
    /* step 7 */
    if (t.is_root && t.quirks) return JS_UNDEFINED;
    /* Steps 8 and 9 are the same operation — "invoke scroll() on window with scrollX as first argument and y
       as second" — over two different elements: the root element unconditionally, and a quirks-mode BODY only
       when it is "not potentially scrollable in at least one axis". The two arms genuinely differ (this one
       scrolls the window, step 10 terminates), and the condition between them is now decided rather than
       approximated by the body's box existence, which is only the first of the definition's three conditions. */
    if (t.is_root || (t.is_body && t.quirks && ev_not_potentially_scrollable_in_some_axis(&t))) {
        double req = unknown ? ev_window_scroll(t.dctx, vertical) : v;
        double x = vertical ? ev_window_scroll(t.dctx, false) : req;
        double y = vertical ? req : ev_window_scroll(t.dctx, true);

        viewport_scroll(t.dctx, x, y);
        return JS_UNDEFINED;
    }
    /* step 10 */
    if (!t.has_box) return JS_UNDEFINED;
    /* step 11 */
    DFAIL("CSSOM VIEW §6's scrollTop/scrollLeft setter's last step SCROLLS THE ELEMENT, and the step before it "
          "asks whether it has a scrolling box and whether it has any overflow — all three need the element's "
          "own geometry, which this engine has none of: the scrolling area §2 defines for an element is its "
          "padding edge extended by the margin edges of its descendants' boxes, and that is what a scroll "
          "position is clamped against. BUILD A LAYOUT that produces box geometry (viewport.c already models "
          "the initial containing block CSS 2.1 §10.3.3 resolves the root element's used width from), and make "
          "an element's scroll position per-flow state in the COW delta");
    return JS_UNDEFINED;
}

/* ---- §6's `scrollWidth` and `scrollHeight` --------------------------------------------------------------- */

static JSValue ev_scroll_extent(JSContext *ctx, const EvTarget *t, bool vertical)
{
    bool presented;
    double vp, area;

    /* step 2 */
    if (!t->dctx) return ev_long(ctx, 0.0);
    /* step 3: "the width of the viewport EXCLUDING the width of the scroll bar, if any, or zero if there is no
       viewport". This user agent renders no scroll bar — there is nothing to overflow the one box in the model
       — so the exclusion removes nothing and the answer is the viewport. */
    presented = viewport_exists(t->dctx);
    vp = !presented ? 0.0 : (vertical ? viewport_height(t->dctx) : viewport_width(t->dctx));
    /* Steps 4 and 5 both return MAX(VIEWPORT SCROLLING AREA, viewport), and the max is computed rather than
       assumed away: the scrolling area is viewport.h's own derivation, read from there so that a layout which
       grows it grows this answer with it. Today they are equal, which is why THIS IS THE ANSWER THAT IS NOT
       ZERO — `document.documentElement.scrollWidth` is 1280 in the top-level traversable and 300 in a child
       navigable, computed rather than shrugged at. */
    area = !presented ? 0.0 : (vertical ? viewport_scrolling_area_height(t->dctx)
                                        : viewport_scrolling_area_width(t->dctx));
    if (t->is_root && !t->quirks)
        return presented ? ev_env_long(ctx, t, vertical ? "scrollHeight" : "scrollWidth", fmax(area, vp))
                         : ev_long(ctx, 0.0);
    /* Step 5's condition is PER AXIS — "not potentially scrollable in the x axis" for `scrollWidth` and "in the
       y axis" for `scrollHeight` — which is why it is asked with this member's own axis and not with the
       at-least-one-axis form the scroll-position steps use. */
    if (t->is_body && t->quirks && !ev_potentially_scrollable(t, vertical))
        return presented ? ev_env_long(ctx, t, vertical ? "scrollHeight" : "scrollWidth", fmax(area, vp))
                         : ev_long(ctx, 0.0);
    /* step 6 */
    if (!t->has_box) return ev_long(ctx, 0.0);
    /* step 7 */
    DFAIL("CSSOM VIEW §6's scrollWidth/scrollHeight step 7 returns THE WIDTH OF THE ELEMENT'S SCROLLING AREA — "
          "§2's box, which is the element's padding edge extended by the margin edges of all of its "
          "descendants' boxes. Every term of that is layout, and this engine gives geometry to exactly one box, "
          "the initial containing block (element_view.h). BUILD A LAYOUT; there is no answer to derive from the "
          "viewport for an element that is not the root");
    return ev_long(ctx, 0.0);
}

/* ---- §6's `clientWidth`, `clientHeight`, `clientTop` and `clientLeft` ------------------------------------- */

static JSValue ev_client_extent(JSContext *ctx, const EvTarget *t, bool vertical)
{
    /* Step 1: "If the element has no associated box OR IF THE BOX IS INLINE, return zero." Both halves are now
       decided from the computed `display` — the second one used to be waved through on the grounds that the two
       elements step 2 answers for can never be inline (CSS Display §2.7 blockifies the root, and §15.3.1 gives
       `body` `display: block`), which said nothing about the elements step 3 reaches. `span.clientWidth` is 0
       in every user agent and used to reach step 3's DFAIL here. */
    if (!t->has_box || ev_box_is_inline(t)) return ev_long(ctx, 0.0);
    /* Step 2 — the ROOT ELEMENT outside quirks mode, and the BODY inside it, are answered with the VIEWPORT and
       never with their own box. `document.documentElement.clientWidth` is how most of the web asks how wide the
       viewport is. "Excluding the size of a rendered scroll bar (if any)" removes nothing: none is rendered. */
    if ((t->is_root && !t->quirks) || (t->is_body && t->quirks)) {
        DCHECK(t->dctx && viewport_exists(t->dctx),
               "a CSSOM VIEW §6 client extent reached its viewport branch for an element whose document is not "
               "being presented — step 1 admits only an element with an associated box, which is defined over "
               "exactly that");
        return ev_env_long(ctx, t, vertical ? "clientHeight" : "clientWidth",
                           vertical ? viewport_height(t->dctx) : viewport_width(t->dctx));
    }
    /* step 3 */
    DFAIL("CSSOM VIEW §6's clientWidth/clientHeight step 3 returns THE UNSCALED WIDTH OF THE PADDING EDGE of "
          "the element's box — the used `width` plus `padding-left` plus `padding-right` — for an element that "
          "is neither the root element outside quirks mode nor the body inside it. THE LAYOUT THIS USED TO ASK "
          "FOR NOW EXISTS AND IS core/layout/used_value.h, and it already answers two of those three terms "
          "whenever they are absolute lengths, so this is no longer `BUILD A LAYOUT`: what is missing is the "
          "used `width` for the ordinary `width: auto` box, which is CSS 2.1 §10.3.3's constraint equation, "
          "which is blocked on the `border-*-width` longhands lexbor's property registry does not carry — the "
          "same ones clientTop/clientLeft below is waiting on, and its DFAIL states the build order. BUILD "
          "those, then §10.3.3, and this step becomes three reads of used_value_px");
    return ev_long(ctx, 0.0);
}

static JSValue ev_client_edge(JSContext *ctx, const EvTarget *t)
{
    /* step 1 */
    if (!t->has_box || ev_box_is_inline(t)) return ev_long(ctx, 0.0);
    /* step 2 */
    DFAIL("CSSOM VIEW §6's clientTop/clientLeft step 2 returns THE UNSCALED COMPUTED VALUE OF THE "
          "border-top-width / border-left-width PROPERTY plus the width of any scrollbar rendered between that "
          "padding edge and that border edge. The scrollbar term is 0 for as long as this user agent renders "
          "none, so the whole answer is that computed value, and css_computed_value.c is now the C entry that "
          "would carry it — what is missing is BELOW it. Lexbor's property registry has no `border-top-width` "
          "and no `border-top-style` (it carries the `border-top` SHORTHAND and `border-*-color`, and nothing "
          "else of the border), so both longhands reach the cascade as unknown declarations with raw text and "
          "no grammar, and `border-width: 1px 2px`, `border-style: solid`, `border-top: 1px solid red` and "
          "`border: 1px solid red` all set them through shorthands css_shorthand.c does not expand. BUILD, in "
          "this order: those four expansions (the four-side 1-to-4 rotation, and the any-order triple lexbor "
          "already parses into a typed struct for `border` and `border-top`), then the two longhands' computed "
          "value in css_computed_value.c — CSS Backgrounds §border-width makes it 0 when the matching "
          "border-*-style computes to `none` or `hidden`, and an absolute length otherwise, with "
          "thin/medium/thick the UA's own 1px/3px/5px");
    return ev_long(ctx, 0.0);
}

/* ---- §6's `getClientRects()` and `getBoundingClientRect()` ----------------------------------------------- */

/* §6's getClientRects() STEPS, AS THE INTERNAL ALGORITHM. §2 is explicit that a member "said to call another
   method or attribute" invokes the algorithm and not the page-visible member, and getBoundingClientRect's step
   1 is exactly such a call — so a page that overwrites `Element.prototype.getClientRects` cannot change what
   `getBoundingClientRect()` measures.
   STEP 1 IS THE ONE THIS ENGINE ANSWERS FOR REAL, and it is a DERIVATION and not a stand-in: an element with
   no associated box generates no fragments in ANY user agent, so the empty list is the whole domain of the
   answer rather than one point picked out of it. element_view.h's one predicate is what decides it, so this
   branch covers a disconnected element, an element of a document nothing is presenting (a DOMParser parse, a
   `<template>`'s contents, the document of a destroyed navigable) and anything whose computed `display` is
   `none` — which is most of what a lazy-loading bundle measures before it inserts anything. */
static JSValue ev_client_rects(JSContext *ctx, const EvTarget *t)
{
    (void)ctx;
    /* step 1 */
    if (!t->has_box) return dom_rect_list_new(t->rctx, JS_NewArray(t->rctx));
    /* Steps 2 and 3 are two different geometries and this engine has neither. Step 2 wants the SVG bounding box
       with the transforms of the element and its ancestors applied; step 3 wants one DOMRect per BOX FRAGMENT
       describing its border area, in content order, again with transforms applied, with the table/caption boxes
       included for a `display: table` element and every anonymous block box replaced by its children. */
    DFAIL("CSSOM VIEW §6's getClientRects() steps 2 and 3 describe the element's BOX FRAGMENTS — one DOMRect per "
          "fragment's border area (or the SVG bounding box), in content order, with the transforms of the "
          "element and its ancestors applied. This engine gives geometry to exactly one box, the initial "
          "containing block (element_view.h), and it has no fragments at all: there is no line box, so an "
          "inline element spanning two lines cannot be two rectangles, and no answer for a single one is "
          "derivable from the viewport for an element that is not the root. BUILD A LAYOUT that produces box "
          "fragments — CSS 2.1 §10.3.3 resolves the root element's used width from the ICB viewport.c already "
          "models, which is where one starts. The two constraints' own inputs are half here already: the "
          "computed `display` is css_computed_value.c's (CSS Display §2.7's blockification included), and the "
          "computed `transform` is not — it needs the transform-function grammar and the reference box "
          "css-transforms §3.2's matrix reduction resolves a percentage translation against");
    /* A RELEASE BUILD CANNOT BUILD THE LAYOUT, so it answers what every other §6 member here answers past its
       own DFAIL: the value for the case this engine does model. */
    return dom_rect_list_new(t->rctx, JS_NewArray(t->rctx));
}

/* §6's GET THE BOUNDING BOX steps. Step 1 is the call above; step 2 is the answer for an element that generates
   no box, and it is the spec's OWN number — "a DOMRect object whose x, y, width and height members are zero" —
   for every user agent, which is what makes it a computed value and not a zero standing in for one this engine
   does not have. It stays CONCRETE for viewport.h's reason: a domain of one point has no arm to explore, and a
   concolic there would model an ignorance this engine does not have. */
static JSValue ev_bounding_rect(JSContext *ctx, const EvTarget *t)
{
    JSValue list = ev_client_rects(ctx, t), len;
    uint32_t n = 0;

    DCHECK(JS_IsObject(list), "§6's get-the-bounding-box step 1 got no list back from getClientRects — that "
                              "algorithm answers a DOMRectList on every path it has");
    len = JS_GetPropertyStr(t->rctx, list, "length");
    JS_ToUint32(t->rctx, &n, len);
    JS_FreeValue(t->rctx, len);
    JS_FreeValue(t->rctx, list);
    /* step 2 */
    if (n == 0) return dom_rect_new(t->rctx, 0.0, 0.0, 0.0, 0.0);
    /* Steps 3 and 4. They are unreachable while step 1 above cannot produce a non-empty list, and they are
       written as the assertion that says so rather than as a comment: the moment a layout makes that list real,
       this is the line that has to decide between them. */
    DFAIL("CSSOM VIEW §6's get-the-bounding-box steps 3 and 4 are the two ways a NON-EMPTY client-rect list is "
          "reduced: step 3 returns the FIRST rectangle when every one of them has a zero width or a zero "
          "height, and step 4 otherwise returns the smallest rectangle enclosing all of those whose width and "
          "height are not both zero. Neither is written, because getClientRects() above cannot yet produce a "
          "list to reduce — WRITE BOTH here when it can, and note that the union in step 4 is over the "
          "rectangles' edges, so it goes through Geometry Interfaces §3's derived edges rather than around them");
    return dom_rect_new(t->rctx, 0.0, 0.0, 0.0, 0.0);
}

static JSValue js_ev_client_rects(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    EvTarget t;

    (void)argc; (void)argv; (void)magic;
    if (!ev_target(ctx, this_val, &t)) return JS_EXCEPTION;
    return ev_client_rects(ctx, &t);
}

static JSValue js_ev_bounding_rect(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    EvTarget t;

    (void)argc; (void)argv; (void)magic;
    if (!ev_target(ctx, this_val, &t)) return JS_EXCEPTION;
    return ev_bounding_rect(ctx, &t);
}

/* ---- the members, the declaration and the per-realm install ---------------------------------------------- */

static JSValue js_ev_get(JSContext *ctx, JSValueConst this_val, int magic)
{
    EvTarget t;

    if (!ev_target(ctx, this_val, &t)) return JS_EXCEPTION;
    switch ((ElementViewMember)magic) {
    case EV_SCROLL_TOP:     return ev_scroll_position(ctx, &t, true);
    case EV_SCROLL_LEFT:    return ev_scroll_position(ctx, &t, false);
    case EV_SCROLL_HEIGHT:  return ev_scroll_extent(ctx, &t, true);
    case EV_SCROLL_WIDTH:   return ev_scroll_extent(ctx, &t, false);
    case EV_CLIENT_HEIGHT:  return ev_client_extent(ctx, &t, true);
    case EV_CLIENT_WIDTH:   return ev_client_extent(ctx, &t, false);
    case EV_CLIENT_TOP:
    case EV_CLIENT_LEFT:    return ev_client_edge(ctx, &t);
    }
    DFAIL("a CSSOM VIEW §6 Element member was read with a magic no member of this file declares — the magic IS "
          "the member, so an unknown one means a name was installed without a case to answer it");
    return JS_UNDEFINED;
}

void element_view_init(JSContext *ctx)
{
    DCHECK(g_id_set_scroll_top < 0,
           "element_view_init ran twice — §6's two setters are declared once per AGENT, and a second "
           "declaration would mint them per realm");
    /* `attribute unrestricted double scrollTop` — UNRESTRICTED is what makes §3.2's normalize-non-finite step
       reachable at all: a plain `double` would reject NaN and the infinities at the boundary, and
       `el.scrollTop = NaN` is a scroll to 0 rather than a TypeError. */
    g_id_set_scroll_top  = idl_setter_id(ctx, IDL_UNRESTRICTED_DOUBLE, false, js_ev_set, EV_SCROLL_TOP);
    g_id_set_scroll_left = idl_setter_id(ctx, IDL_UNRESTRICTED_DOUBLE, false, js_ev_set, EV_SCROLL_LEFT);
    g_id_client_rects    = idl_method_id(ctx, NULL, 0, js_ev_client_rects, 0);
    g_id_bounding_rect   = idl_method_id(ctx, NULL, 0, js_ev_bounding_rect, 0);
}

void element_view_install(JSContext *ctx, JSValueConst proto)
{
    DCHECK(g_id_set_scroll_top >= 0,
           "§6's members were installed in a realm before element_view_init declared their setters — the "
           "component declares once per agent and installs from the cached ids");
    idl_install_accessor(ctx, proto, "scrollTop",    js_ev_get, EV_SCROLL_TOP,    g_id_set_scroll_top);
    idl_install_accessor(ctx, proto, "scrollLeft",   js_ev_get, EV_SCROLL_LEFT,   g_id_set_scroll_left);
    idl_install_accessor(ctx, proto, "scrollWidth",  js_ev_get, EV_SCROLL_WIDTH,  -1);
    idl_install_accessor(ctx, proto, "scrollHeight", js_ev_get, EV_SCROLL_HEIGHT, -1);
    idl_install_accessor(ctx, proto, "clientTop",    js_ev_get, EV_CLIENT_TOP,    -1);
    idl_install_accessor(ctx, proto, "clientLeft",   js_ev_get, EV_CLIENT_LEFT,   -1);
    idl_install_accessor(ctx, proto, "clientWidth",  js_ev_get, EV_CLIENT_WIDTH,  -1);
    idl_install_accessor(ctx, proto, "clientHeight", js_ev_get, EV_CLIENT_HEIGHT, -1);
    idl_install_method(ctx, proto, "getClientRects", 0, g_id_client_rects);
    idl_install_method(ctx, proto, "getBoundingClientRect", 0, g_id_bounding_rect);
}

void element_view_free(void)
{
    /* The member ids are the AGENT's, and the pool they live in goes with the runtime. */
    g_id_set_scroll_top = g_id_set_scroll_left = -1;
    g_id_client_rects = g_id_bounding_rect = -1;
}
