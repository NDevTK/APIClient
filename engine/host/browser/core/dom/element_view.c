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
#include "core/css/css_length.h"
#include "core/css/css_transform.h"
#include "core/dom/document.h"
#include "core/dom/element.h"
#include "core/dom/element_scrolling.h"
#include "core/dom/element_view.h"
#include "core/dom/node.h"
#include "core/dom/perform_scroll.h"
#include "core/frame/viewport.h"
#include "core/frame/window_proxy.h"
#include "core/geometry/dom_rect.h"
#include "core/geometry/dom_rect_list.h"
#include "core/idl_args.h"
#include "core/layout/flow_position.h"
#include "core/layout/replaced_element.h"
#include "core/layout/scroll_container.h"
#include "core/layout/scrolling_area.h"
#include "core/layout/used_value.h"
#include "solver/concolic.h"

/* THE MEMBERS. `scrollTop` and `scrollLeft` are the two the IDL declares as `attribute` rather than `readonly
   attribute`, so their magic is a setter's magic as well as a getter's. */
typedef enum {
    EV_SCROLL_TOP, EV_SCROLL_LEFT,
    EV_SCROLL_WIDTH, EV_SCROLL_HEIGHT,
    EV_CLIENT_TOP, EV_CLIENT_LEFT,
    EV_CLIENT_WIDTH, EV_CLIENT_HEIGHT
} ElementViewMember;

/* §6's THREE SCROLL METHODS ARE TWO ALGORITHMS, which is what these two name. `scrollTo()` is not a third:
   "when the scrollTo() method is invoked, the user agent must act as if the scroll() method was invoked with
   the same arguments", so it shares this magic AND the declaration, exactly as viewport.c's `pageXOffset`
   shares `scrollX`'s — one body, no second answer to keep in step. */
typedef enum {
    EV_SCROLL_ABSOLUTE = 0,   /* `scroll()` and `scrollTo()` */
    EV_SCROLL_RELATIVE        /* `scrollBy()`: the same steps with steps 3-4's addition ahead of them */
} EvScrollKind;

static int g_id_set_scroll_top = -1, g_id_set_scroll_left = -1;
static int g_id_client_rects = -1, g_id_bounding_rect = -1;
static int g_id_scroll = -1, g_id_scroll_by = -1, g_id_scroll_into_view = -1;

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

/* The four questions, over an ELEMENT — §2's internal-algorithm entry, which §9's Range members reach through
   `element_view_client_rects` without a receiver to brand. */
static void ev_target_of_element(lxb_dom_element_t *el, EvTarget *t)
{
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
}

/* WEB IDL §3.7.6 Attributes' BRAND CHECK. `Element.prototype.clientWidth` read off a plain object is a
   TypeError, and a
   page tells that apart from `undefined` — a feature detector that probes the descriptor and applies the getter
   reads the throw as "this is a real interface". Returns false with the TypeError pending. */
static bool ev_target(JSContext *ctx, JSValueConst this_val, EvTarget *t)
{
    lxb_dom_element_t *el = element_of_value(this_val);

    if (!el) {
        JS_ThrowTypeError(ctx, "a CSSOM VIEW Element member was reached on something that is not an Element");
        return false;
    }
    ev_target_of_element(el, t);
    return true;
}

/* A member whose IDL type is `long`, handed a value that is ALREADY an integer number of CSS pixels — the
   viewport's own size, a border width css-backgrounds-3 §3.3 has snapped, or one of §6's own zeros. A fraction
   here is a derivation that produced something the type cannot carry rather than a value to round; the one
   member of §6 whose answer is a real length goes through the conversion below instead. */
static JSValue ev_long(JSContext *ctx, double v)
{
    DCHECK(v == trunc(v),
           "a CSSOM VIEW §6 Element member declared `long` computed a value that is not an integer");
    DCHECK(v >= -2147483648.0 && v <= 2147483647.0,
           "a CSSOM VIEW §6 Element member declared `long` computed a whole number of CSS pixels that an IDL "
           "`long` cannot carry. CSS Values §5 puts no upper bound on a <length> and Web IDL puts one on the "
           "type §6 reports it through, so the two disagree for a box wider than 2^31 CSS pixels and the spec "
           "says nothing about the disagreement. A real user agent never meets it because its layout carries a "
           "coordinate in a SATURATING fixed-point type whose range is far smaller (Blink's LayoutUnit tops out "
           "near 33 million CSS pixels), which is a decision about the layout's own arithmetic and not about "
           "this member. BUILD that coordinate type in core/layout, where the used values are computed, so the "
           "saturation happens once for every geometry rather than at each member that reports one");
    return JS_NewInt32(ctx, (int32_t)v);
}

/* A `long` MEMBER WHOSE ANSWER IS A REAL LENGTH — §6's step 3 for `clientWidth` and `clientHeight`, and step 2
   for `clientTop` and `clientLeft`. A padding edge is a used value in CSS pixels and NOTHING snaps it:
   css-backgrounds-3 §3.3 snaps a border WIDTH and no other term of the box model, so `padding: 0.5px` makes a
   padding edge that is not an integer. A snapped border width is a whole number of DEVICE pixels, which is a
   whole number of CSS pixels only where the device pixel ratio is an integer — so `clientTop` is a real length
   too, and the two members answer through one conversion rather than through a rounding one of them assumes it
   never needs.
   WHAT THE SPEC SAYS AND WHERE IT STOPS. §6 declares all six extents `long` and its own Changes section records
   that as deliberate ("the scrollWidth, scrollHeight, clientTop, clientLeft, clientWidth and clientHeight IDL
   attributes on Element were changed back to return integers"), so the TYPE is the normative statement: the
   value reported is a whole number of CSS pixels. What the spec does not state anywhere is the conversion —
   §3.2, its only WebIDL-values rule, is normalize-non-finite and is about the scroll setters' INPUT. So the
   question left open is not whether to convert but which integer to convert to, and there are only three
   answers: toward zero, away from zero, or to the nearest.
   IT IS THE NEAREST, AND THAT IS A DERIVATION FROM WHAT THE MEMBER IS FOR. `clientWidth` is a MEASUREMENT — a
   page reads it to find out how wide a box is — and rounding to the nearest integer is the only one of the
   three whose error is bounded by half a CSS pixel in both directions. Truncation is a systematic UNDERSTATEMENT
   that grows with the number of fractional terms (a `padding: 0.5px` on each side of a box loses a whole pixel,
   in the same direction, every time), and a bias no user agent has is a fidelity bug and not a conservative
   choice.
   AND THE TIE IS NOT A DECISION HERE, WHICH IS WHY THIS FILE DOES NOT MAKE ONE. Round-half-up and
   round-half-away-from-zero are the same function on non-negative numbers, and a padding edge is a content box
   floored at zero plus two paddings CSS 2.1 §8.4 forbids to be negative. The assert below is what keeps that
   true rather than a remark that it happens to be: the day something hands this a negative length the two
   tie-breaks separate, and the crash stands at the line where the choice would have to be made.
   AND THE ROUNDING IS OVER THE EXAMPLE, WHICH IS WHY THE DOMAIN OUTLIVES IT. A padding edge whose box is sized
   by CSS 2.1 §10.3.3's constraint equation is derived from the INITIAL CONTAINING BLOCK, so it carries the
   viewport's domain (css_length.h) — `document.body.clientWidth < 768` is the same responsive gate as
   `innerWidth < 768` and a bare integer there deletes the mobile arm. §6's `long` is the EXAMPLE the concolic
   carries, minted through viewport.h's one seam, which hands back the plain integer for a box whose size the
   author's own declarations determined.
   THE CONVERSION IS EXPORTED AND THE NON-NEGATIVITY IS NOT, which is the whole of the split between the two
   functions below. The rounding rule and the mint are the same question §7's four offset members ask, so they
   are stated once; the assertion that the tie-break cannot be reached belongs to §6's own six extents, which
   are distances between parallel edges of one box. §7's `offsetTop` is a COORDINATE DIFFERENCE and can be
   negative, so it takes the shared conversion without this assert and the tie-break becomes a stated choice
   (element_view.h) rather than an unreachable one. */
JSValue element_view_length_long(JSContext *ctx, CssPx px)
{
    DCHECK(isfinite(px.px),
           "a CSSOM VIEW length member declared `long` was handed a length that is not FINITE — every operand "
           "of every used value in this engine is a finite number of CSS pixels, so an infinity or a NaN here "
           "is a derivation that lost an operand rather than a value to convert");
    return viewport_env_derived(px, ev_long(ctx, floor(px.px + 0.5)));
}

static JSValue ev_length_long(JSContext *ctx, CssPx px)
{
    DCHECK(px.px >= 0.0,
           "a CSSOM VIEW §6 length member was handed a NEGATIVE length. A padding edge is a content box floored "
           "at zero (css-sizing §5) plus two paddings CSS 2.1 §8.4 forbids to be negative, and a border width "
           "is a non-negative <length> css-values §6 snapped towards zero; a SCROLLING AREA's extent is §2's "
           "extreme taken over that same padding edge, so it is at least as large again. A negative one is "
           "therefore a derivation that lost an operand — and it would also make this conversion's tie-break "
           "observable, which the derivation above says it is not for THIS section's members");
    return element_view_length_long(ctx, px);
}

/* A `long` member that reports the VIEWPORT: the modelled geometry as the EXAMPLE of a concolic, minted
   through viewport.h's one seam and keyed on the element's document — see element_view.h for why these are
   sources and the scroll positions are not. */
static JSValue ev_env_long(JSContext *ctx, const EvTarget *t, const char *member, double v)
{
    return viewport_env_value(t->dctx, member, ev_long(ctx, v));
}

/* THE `scrollX`/`scrollY` ATTRIBUTES' OWN ANSWER, which four of the steps below invoke by name, is
   `viewport_window_scroll` — the component that owns the derivation owns the member's "or zero if there is no
   viewport" with it. It was a private static here, and a THIRD caller (CSSOM VIEW §10's `pageX`/`pageY` step 2,
   which invokes the attribute in those words) is what said so: §2 requires an algorithm that calls an attribute
   to invoke its internal API, and an internal API each caller re-states is not one API. */

bool element_view_subtree_has_boxes(const lxb_dom_node_t *n)
{
    const lxb_dom_node_t *a;
    JSContext *dctx;

    DCHECK(n && n->type == LXB_DOM_NODE_TYPE_ELEMENT,
           "css-display-3 §2.5's subtree question was asked about a node that is not an element — the two "
           "keywords it is stated over are values of `display`, and only an element has one");
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
       decided where every other UA rule is (css_style_declaration.c's UA layer), and this asks css-display-3
       §2.5's ONE keyword that reaches a whole subtree: "the element and its descendants generate no boxes or
       text sequences". */
    for (a = n; a != NULL && a->type == LXB_DOM_NODE_TYPE_ELEMENT; a = a->parent) {
        char *d = css_computed_value(lxb_dom_interface_element((lxb_dom_node_t *)a), "display");
        bool none;

        DCHECK(d != NULL, "the cascade produced no computed `display` for an element — the UA layer answers "
                          "`inline` for every element it does not name, so this cannot be unset");
        none = strcmp(d, "none") == 0;
        free(d);
        if (none) return false;
    }
    return true;
}

bool element_view_has_box(const lxb_dom_node_t *n)
{
    char *d;
    bool contents;

    DCHECK(n && n->type == LXB_DOM_NODE_TYPE_ELEMENT,
           "the associated-box predicate was asked about a node that is not an element — only elements "
           "generate the principal boxes CSSOM VIEW §6 and HTML's `being rendered` are about");
    /* THE SUBTREE HALF FIRST, and it is the same walk rather than a copy of it: everything that stops boxes
       being generated ANYWHERE below `n` also stops `n`'s own. */
    if (!element_view_subtree_has_boxes(n)) return false;
    /* AND css-display-3 §2.5's OTHER KEYWORD, which reaches exactly one box: `contents` means "the element
       itself does not generate any boxes, but its children and pseudo-elements still generate boxes and text
       sequences as normal", so it is asked of `n` alone and never of an ancestor. */
    d = css_computed_value(lxb_dom_interface_element((lxb_dom_node_t *)n), "display");
    DCHECK(d != NULL, "the cascade produced no computed `display` for an element — the UA layer answers "
                      "`inline` for every element it does not name, so this cannot be unset");
    contents = strcmp(d, "contents") == 0;
    free(d);
    return !contents;
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

/* "…is not potentially scrollable in EITHER AXIS" — a THIRD spelling, and a different condition from the one
   above rather than another way of writing it. §6's `scroll()`, `scrollTo()` and `scrollBy()` are the members
   that ask it (and §5's `scrollingElement`, which this engine answers elsewhere), while the
   `scrollTop`/`scrollLeft` setter beside them asks the at-least-one form — the spec really does write the two
   differently at two adjacent steps, so a body potentially scrollable in exactly ONE axis takes the window
   branch from the setter and NOT from the method. Reading either spelling as the other is a body that scrolls
   the window where it should have gone on to step 10, or the reverse; they are written apart here so that
   neither can be derived from the other by accident. */
static bool ev_not_potentially_scrollable_in_either_axis(const EvTarget *t)
{
    return !ev_potentially_scrollable(t, false) && !ev_potentially_scrollable(t, true);
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

/* THE GETTER, IN THE SPEC'S OWN STEP ORDER. Every terminal it can reach is either the spec's own zero, the
   VIEWPORT's stored position, or an element's — and each is a real answer rather than a stand-in. IT USED TO
   READ THAT EVERY TERMINAL IS THE ORIGIN, which CSSOM VIEW §3.1's arrival retired for the two window terminals
   and left standing for the element one:
     step 2 and step 3 are the spec's own zero for a document that is not being presented;
     step 5 hands the root element the WINDOW's scroll position, which is REAL PER-FLOW STATE now
       (core/frame/viewport.h) rather than the ICB origin this used to derive — so this terminal is not the
       origin at all once a page has scrolled, and the sentence at the head of this comment is narrower than it
       reads for exactly that step and step 6;
     step 6 hands a quirks-mode BODY the same window scroll position when it is not potentially scrollable in
       at least one axis — a condition this file could not decide until there was a computed-value entry to
       read the body's and its parent's `overflow-x`/`overflow-y` through;
     step 7 is the spec's own zero for an element with no associated box;
     step 8 is the element's OWN current scroll position, which is STILL the origin because nothing has
       scrolled it: a scroll position moves only when §3.1 Scrolling's perform a scroll runs, and every §6
       member that could reach it for an ELEMENT — the setter below, `scroll()`/`scrollTo()`/`scrollBy()` and
       `scrollIntoView()` — converges on §6.1's one perform-a-scroll step, which hands §3.1 an element box and
       aborts there for want of a store (core/dom/perform_scroll.c, and element_view.h for why that must stay
       one site). */
static double ev_scroll_position_px(const EvTarget *t, bool vertical)
{
    /* steps 2-3 */
    if (!t->dctx) return 0.0;
    /* step 4 */
    if (t->is_root && t->quirks) return 0.0;
    /* step 5 */
    if (t->is_root) return viewport_window_scroll(t->dctx, vertical);
    /* step 6 */
    if (t->is_body && t->quirks && ev_not_potentially_scrollable_in_some_axis(t))
        return viewport_window_scroll(t->dctx, vertical);
    /* step 7 */
    if (!t->has_box) return 0.0;
    /* step 8 */
    return 0.0;
}

/* THE ATTRIBUTE, over the derivation above — and the two are separate for the reason viewport.h gives for its
   own pair. §2 Terminology is explicit that "when a method or an attribute is said to call another method or
   attribute, the user agent must invoke its INTERNAL API for that attribute", and §6's scroll members do
   exactly that at three steps: step 1's "or the element's current scroll position on the x axis otherwise",
   and `scrollBy`'s steps 3 and 4 ("add the value of scrollLeft to the left dictionary member"). Those callers
   need the NUMBER, so the derivation answers a double and the member wraps it — rather than the callers
   reading a JSValue back out of the member, which is the shape that lets a page's override decide what an
   engine algorithm measures. */
static JSValue ev_scroll_position(JSContext *ctx, const EvTarget *t, bool vertical)
{
    return JS_NewFloat64(ctx, ev_scroll_position_px(t, vertical));
}

double element_view_scroll_position(lxb_dom_element_t *el, bool vertical)
{
    EvTarget t;

    DCHECK(el != NULL, "§6's scrollTop/scrollLeft getter was invoked as an internal algorithm with no element");
    ev_target_of_element(el, &t);
    return ev_scroll_position_px(&t, vertical);
}

/* "THE ELEMENT HAS NO OVERFLOW" — the third disjunct of the setter's step 10, derived from §2's own edges
   rather than from a property. The scrolling area's beginning edges ARE the element's two leading padding
   edges and its ending edges are the extreme over the trailing ones, so the area equals the padding box on an
   axis exactly when no descendant's margin edge lies outside it — and the element overflows when either axis
   is larger. Both operands are the same box's, so nothing here compares two different rectangles.
   THE COMPARISON RUNS ON THE EXAMPLE, which is css_length.h's stated layering and not a shortcut past it: both
   extents may be functions of the initial containing block, so which is larger is a question the environment
   could answer either way, and it is decided on the modelled viewport exactly as CSS 2.1 §10.3.3's own
   slack test is. */
static bool ev_has_overflow(const EvTarget *t)
{
    lxb_dom_element_t *el = lxb_dom_interface_element(t->node);

    DCHECK(t->has_box, "the overflow question was asked about an element that generates no box — §2's scrolling "
                       "area is stated over a box's padding edge, and the caller's own step has already "
                       "terminated for an element that has none");
    return scrolling_area_extent_px(el, false).px > used_value_padding_edge_px(el, false).px ||
           scrolling_area_extent_px(el, true).px > used_value_padding_edge_px(el, true).px;
}

/* ONE AXIS OF A §6 SCROLL MEMBER'S REQUESTED POSITION, CARRIED RATHER THAN DECIDED — the value every step
   between the READ and the SCROLL passes along without looking at it.
   IT IS A PAIR AND NOT A `double` BECAUSE THE READ AND THE USE ARE DIFFERENT STEPS, AND §6 PUTS TERMINATIONS
   BETWEEN THEM. The setter reads its position at step 1 and consumes it at step 8, 9 or 11; the methods read
   theirs at step 1.3/1.4 or 2.2 and consume it at the same three. Between those, steps 3-7 and step 10 end the
   algorithm having never looked at the number — and step 10's three disjuncts ("the element does not have any
   associated box, the element has no associated scrolling box, or the element has no overflow") are the COMMON
   case rather than an edge, since every element whose scrolling area equals its padding box terminates there.
   A member that resolved an UNKNOWN request where it READ one would therefore be answering a question §6 does
   not ask on most of its own paths, and answering it with a crash that names an unbuilt mechanism the
   algorithm was never going to reach.
   So an unknown TRAVELS to the one step that consumes it, and is decided there or not at all. */
/* THE TYPE AND THE DECISION BOTH MOVED TO core/dom/perform_scroll.h, AND THE MOVE IS WHAT KEEPS THEM ONE.
   They were written here because §6 was the only section with scroll members in this engine; CSSOM VIEW §4
   "Extensions to the Window Interface" declares the same six over the same two dictionary members for the
   VIEWPORT, and a second copy of the decision about an unknown requested position is the shape this engine has
   already got wrong once — two sites, one substituting and one crashing. That incident, the obligation the
   crash names, and the correction of the predecessor who recorded that obligation as a FORK all moved with it
   and are stated there; none of them was dropped. The request now lives beside §3.1's perform a scroll, which
   is the step every one of the seven members reaches. */

/* §6's LAST TWO STEPS FOR AN ELEMENT THAT IS NOT THE ROOT AND IS NOT A QUIRKS-MODE BODY — step 10's three-way
   termination and step 11's SCROLL THE ELEMENT. They are ONE statement here because §6 writes the same two
   sentences twice: the `scrollTop`/`scrollLeft` setter's steps 10 and 11, and `scroll()`'s steps 10 and 11.
   The two differ only in WHICH TWO NUMBERS step 11 is handed — "scroll the element to scrollLeft,y" for the
   setter and "scroll the element to x,y" for the method — and in the behavior keyword, so those are the
   arguments and the steps are one function.
   ALL THREE OF STEP 10's DISJUNCTS ARE NOW DECIDED, and none of them is a stand-in: box existence is
   element_view.h's one predicate, the ASSOCIATED SCROLLING BOX is css-overflow-3 §3.1's scroll container
   (core/layout/scroll_container.h), and OVERFLOW is §2's scrolling area against the element's own padding box
   (ev_has_overflow, over core/layout/scrolling_area.h). Step 11 then RUNS — core/dom/element_scrolling.h's
   scroll an element to x,y clamps into that same scrolling area and terminates at §6.1's own resolved-Promise
   exit when the clamped position is the one the element already has, which is every element whose slack is
   zero. What it cannot do is MOVE one, and that crash lives at the one site that performs a scroll rather than
   here: §DFAIL's failure mode is a crash whose text outlives the thing it names, and a copy at each algorithm
   that reaches it is exactly the copy nobody deletes.
   THE REQUESTED POSITIONS ARRIVE UNRESOLVED AND ARE RESOLVED BETWEEN THE TWO STEPS, which is the ORDER of §6
   and not a convenience: step 10 terminates without consuming either coordinate, so an UNKNOWN request that
   this element was never going to scroll on has no question to answer here. Resolving them in the caller —
   which is what the setter did — asked `scroll_request_resolve` on behalf of every element that terminates at
   step 10, which is nearly all of them. */
static void ev_scroll_the_element_or_terminate(const EvTarget *t, ScrollRequest x, ScrollRequest y,
                                              const char *behavior)
{
    /* Step 10 — "if the element does not have any associated box, the element has no associated scrolling box,
       or the element has no overflow, terminate these steps". */
    if (!t->has_box) return;
    if (!scroll_container_is(lxb_dom_interface_element(t->node))) return;
    if (!ev_has_overflow(t)) return;
    /* step 11 */
    element_scrolling_scroll_element(lxb_dom_interface_element(t->node), scroll_request_resolve(x),
                                     scroll_request_resolve(y), behavior);
}

/* THE SETTER — the same algorithm, and it RUNS rather than dropping the write. What decides the write is §4's
   scroll() clamping the requested position into §2's scrolling area of the viewport, which is the spec deciding
   and not this engine ignoring. TWO SENTENCES HAVE BEEN RETIRED HERE IN TURN and are restated in capitals so
   neither is re-derived: THE AREA IS EXACTLY THE VIEWPORT, SO THE CLAMP LANDS EVERY REQUEST WHERE IT ALREADY IS
   (true while the area was the initial containing block), and then WHERE THE CLAMP LANDS SOMEWHERE ELSE §4's
   STEP 10 CRASHES NAMING §3.1's PERFORM A SCROLL. §3.1 is written; step 10 aborts or the viewport MOVES. */

static JSValue js_ev_set(JSContext *ctx, JSValueConst this_val, JSValueConst val, int magic)
{
    bool vertical = (magic == EV_SCROLL_TOP);
    /* STEP 1's "let y be the given value" — CARRIED and not resolved, because steps 3-7 and step 10 each
       terminate without consuming it. See `ScrollRequest` above for why the decision travels to the scroll. */
    ScrollRequest req;
    EvTarget t;
    double v = 0.0;

    req.unknown = concolic_is(val);
    req.px = 0.0;

    DCHECK(magic == EV_SCROLL_TOP || magic == EV_SCROLL_LEFT,
           "a CSSOM VIEW §6 setter was declared with a magic that is not one of the two members the IDL "
           "declares as a writable attribute");
    if (!ev_target(ctx, this_val, &t)) return JS_EXCEPTION;
    if (!req.unknown) {
        DCHECK(JS_IsNumber(val),
               "§6's scrollTop/scrollLeft setter was handed something that is not a number — its IDL type is "
               "`unrestricted double` and the declaration converts it, which is also what makes §3.2's "
               "normalization below reachable rather than a type error");
        JS_ToFloat64(ctx, &v, val);
        /* step 2 — §3.2's NORMALIZE NON-FINITE VALUES: Infinity, -Infinity and NaN all become 0. */
        if (!isfinite(v)) v = 0.0;
        req.px = v;
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
        /* §4's `scroll()` IS INVOKED WITH THE REQUEST, NOT WITH A NUMBER, and the request travels unresolved.
           This used to resolve it here on the ground that the scroll is performed on this arm whatever the
           position is — which is true of a realm that HAS a viewport and false of one that does not: §4's own
           step 4 returns before step 7 reads either coordinate, so a decision made in front of the call was a
           crash on a call that consumes nothing. The one site that decides an unknown is now §4's step 7
           itself (core/frame/viewport.c). */
        ScrollRequest x = vertical ? scroll_request_px(viewport_window_scroll(t.dctx, false)) : req;
        ScrollRequest y = vertical ? req : scroll_request_px(viewport_window_scroll(t.dctx, true));

        /* §6 gives the setter's steps 8 and 9 no options dictionary to carry a `ScrollBehavior`, so §4's step
           12 hands §3.1 the `auto` its own declaration defaults to. */
        viewport_scroll(t.dctx, x, y, "auto");
        return JS_UNDEFINED;
    }
    /* STEPS 10 AND 11, which are the same two the scroll members below reach — stated once. Step 11 is
       "scroll the element to scrollLeft,y" for the `scrollTop` setter and "to x,scrollTop" for `scrollLeft`'s:
       the axis this setter does NOT write comes from the element's own current position through §2's
       internal-API rule, exactly as the root-element arm above takes the window's other coordinate.
       AN UNKNOWN REQUESTED POSITION IS NOT DECIDED HERE, and that is step 10's own doing rather than a
       softening: the request is handed over UNRESOLVED and step 10's three disjuncts run first, so an element
       with no box, no scrolling box or no overflow terminates exactly as §6 says and the unknown is never
       asked a question. Only step 11 consumes it, through the one site the arm above uses. */
    ev_scroll_the_element_or_terminate(&t,
                                       vertical ? scroll_request_px(ev_scroll_position_px(&t, false)) : req,
                                       vertical ? req : scroll_request_px(ev_scroll_position_px(&t, true)),
                                       "auto");
    return JS_UNDEFINED;
}

/* ---- §6's `scroll()`, `scrollTo()` and `scrollBy()` ------------------------------------------------------ */

/* "RETURN A RESOLVED PROMISE" — §6's own answer at four of the scroll members' exits, and there is no engine
   primitive for one. A promise is a capability plus a CALL of its resolving function, and that call goes
   through JS_CallAsFlow rather than JS_Call because a resolving function must run on a flow base: this is a C
   activation, and delivering into one is what every other host settle in this engine avoids.
   THE RESOLVE CANNOT THROW, which is why the failure is an assert and not a swallow: the capability is fresh,
   nothing has attached a reaction to it, and the value is `undefined` — so there is no `then` getter of the
   page's for the resolve to reach and no arm of it that runs anything. */
static JSValue ev_resolved_promise(JSContext *ctx)
{
    JSValue funcs[2], promise = JS_NewPromiseCapability(ctx, funcs);
    int r;

    CHECK(!JS_IsException(promise),
          "CSSOM VIEW §6's scroll members answer with a Promise on every path they have, and this one's "
          "capability could not be allocated — a member that answers with neither a promise nor a throw is a "
          "call a page can only hang on");
    r = JS_CallAsFlow(ctx, funcs[0], JS_UNDEFINED);
    DCHECK(r >= 0, "resolving a FRESH promise capability with `undefined` completed abruptly — nothing of the "
                   "page's is reachable from there, so a throw means the capability is not the one this just "
                   "created");
    JS_FreeValue(ctx, funcs[0]);
    JS_FreeValue(ctx, funcs[1]);
    return promise;
}

/* ONE AXIS OF §6's SCROLL MEMBERS' REQUESTED POSITION, resolved to the coordinate the later steps use.
 *
 * THREE WAYS IT ARRIVES AND EACH IS THE SPEC'S OR THIS ENGINE'S — none of them a zero standing in for a number:
 *   ABSENT is step 1's own "or the ELEMENT'S CURRENT SCROLL POSITION on the x axis otherwise", which is the
 *     derivation above and not a zero. A page writing `el.scrollTo({top: 40})` is asking for the x it already
 *     has, and answering 0 there would be a horizontal scroll it did not request.
 *   A NUMBER is the coordinate the page computed, with §3.2 "WebIDL values"' NORMALIZE NON-FINITE VALUES
 *     applied here — "if x is one of the three special floating point literal values (Infinity, -Infinity or
 *     NaN), then x must be changed to the value 0". It is the member's own step and not the declaration's: the
 *     IDL type is `unrestricted double` precisely SO THAT those three reach the algorithm, which is why
 *     `el.scrollTo(NaN, 0)` is a scroll to the origin rather than a TypeError.
 *   UNKNOWN EXTERNAL INPUT is CARRIED, neither read nor substituted — see `ScrollRequest`, and see
 *     `scroll_request_resolve` for the one site that decides one and for why that decision is a crash.
 *     WHAT STOOD HERE WAS THE RETIRED ARGUMENT ITSELF, and it is restated in capitals so that nobody
 *     re-derives it: §6.1's CLAMP LANDS EVERY VALUE OF THE DOMAIN ON THE POSITION THE SCROLLING BOX ALREADY
 *     HAS, SO THE TWO WORLDS ARE OBSERVATIONALLY THE SAME AND THERE IS NO ARM. That was true while §2's
 *     scrolling area was exactly the viewport, and this site's own sentence named the assert in
 *     core/frame/viewport.c that kept it true, and said in its own words that the day a clamp could land
 *     elsewhere this branch would have to fork instead. THE DAY ARRIVED: §2's viewport row extended the area
 *     past the ICB and §3.1's perform a scroll MOVES the box, and that assert is gone (viewport.c states what
 *     retired it). So
 *     the substitution had become this engine deciding that a page's `el.scrollTo({top: h})` had no effect,
 *     over a domain most of whose members land somewhere else — a SILENT wrong answer where the setter one
 *     algorithm over already crashed, because the fix that retired the premise repaired the setter and left
 *     the sentence that cited it standing here.
 *
 * `relative` is `scrollBy`'s steps 3 and 4 — "add the value of scrollLeft to the left dictionary member". An
 * ABSENT member needs no addition, and that is a derivation rather than a skipped step: adding the current
 * position to an absent member gives the current position, which is exactly what step 1 then defaults an
 * absent member to. The two readings of the sentence coincide, which is why this is one function. */
static ScrollRequest ev_scroll_axis(JSContext *ctx, const EvTarget *t, JSValueConst member, bool vertical,
                                    bool relative)
{
    /* §6's read IS §4's read, over a different box's current position — the three arms and §3.2's normalize
       are stated once (core/dom/perform_scroll.h) and what this file supplies is WHOSE position `current` is.
       The paragraph above lists those arms; the reason they are not written here is that §4 asks the identical
       question of the viewport and one question with two bodies is free to answer differently. */
    return scroll_request_member(ctx, member, ev_scroll_position_px(t, vertical), relative);
}

/* §6's `scroll()`, which `scrollTo()` IS — "when the scrollTo() method is invoked, the user agent must act as
 * if the scroll() method was invoked with the same arguments", so the two share one declaration and there is no
 * second body that could ever answer differently. `scrollBy` is the same algorithm with steps 3-4's addition
 * ahead of it, which `ev_scroll_axis` performs, and then §6's own "return the Promise returned by scroll()
 * after the method is invoked with options as the only argument" — the same steps, reached with the same two
 * numbers, which is why it is this body under a magic rather than a second one.
 *
 * THE OVERLOAD IS ALREADY RESOLVED and this body reads that answer back off the CONVERTED ARGUMENT COUNT, which
 * is the machine's own output rather than a second resolution: §3.6 steps 3-4 decide `scroll` from the argument
 * count ALONE (see IDL_UNRESTRICTED_DOUBLE_OR_DICT), so a body seeing two positions is seeing the numeric entry
 * and a body seeing one is seeing the dictionary the declaration built — including for `el.scrollTo()`, whose
 * `optional ScrollToOptions options = {}` the machine materializes with every member at its default.
 *
 * WHAT THIS ENGINE ACTUALLY DOES WITH THE REQUEST is §4's clamp and then a real scroll: the root element and a
 * quirks-mode body route to the VIEWPORT's scroll() (steps 8 and 9), which clamps into §2's scrolling area and
 * MOVES the viewport unless step 10 aborts (core/frame/viewport.c, and core/dom/perform_scroll.h for §3.1).
 * THAT USED TO END AT THE POSITION IT ALREADY HAS, BECAUSE THE SCROLLING AREA WAS EXACTLY THE VIEWPORT — §2's
 * viewport row retired the premise and §3.1 retired the conclusion. Every other element reaches step 10's
 * termination or the crash step 11 names. */
static JSValue js_ev_scroll(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    bool relative = (magic == EV_SCROLL_RELATIVE);
    JSValue left, top, behavior_v = JS_UNDEFINED;
    const char *behavior = "auto";
    EvTarget t;
    ScrollRequest x, y;

    DCHECK(magic == EV_SCROLL_ABSOLUTE || magic == EV_SCROLL_RELATIVE,
           "a CSSOM VIEW §6 scroll member was declared with a magic that is neither of the two algorithms — "
           "`scroll`/`scrollTo` is one and `scrollBy` is the other, and the magic IS which");
    if (!ev_target(ctx, this_val, &t)) return JS_EXCEPTION;
    /* Steps 1 and 2, whose ONLY difference is where `left` and `top` come from. Step 2's "let the left
       dictionary member of options have the value x" makes the two-argument form's arguments those very
       members, so they are read as such and the rest of the algorithm has one shape. */
    if (argc >= 2) {
        DCHECK(argc == 2, "§6's scroll members declare two positions and the conversion converts no more than a "
                          "member lists, so a body seeing a third is a declaration that grew without this");
        left = JS_DupValue(ctx, argv[0]);
        top  = JS_DupValue(ctx, argv[1]);
    } else {
        DCHECK(argc == 1 && JS_IsObject(argv[0]),
               "§6's scroll members reached their body at the dictionary arity with something that is not the "
               "engine-built options object — `optional ScrollToOptions options = {}` means an omitted argument "
               "IS a dictionary carrying every member's default, which the argument machine materializes");
        left = idl_dict_get(ctx, argv[0], "left");
        top  = idl_dict_get(ctx, argv[0], "top");
        /* §6's step 11 hands §6.1 "the value of the BEHAVIOR dictionary member of options", so the member the
           declaration converted is read HERE rather than being described as unread. `= "auto"` is its IDL
           default, which the conversion places for a page that wrote none, so the two-argument overload above
           — whose step 2 builds `options` out of its two numbers and nothing else — keeps the initialiser. */
        behavior_v = idl_dict_get(ctx, argv[0], "behavior");
        behavior = JS_IsString(behavior_v) ? JS_ToCString(ctx, behavior_v) : "auto";
        CHECK(behavior != NULL, "§4's ScrollBehavior keyword could not be read as a string");
    }
    x = ev_scroll_axis(ctx, &t, left, /*vertical*/ false, relative);
    y = ev_scroll_axis(ctx, &t, top,  /*vertical*/ true,  relative);
    JS_FreeValue(ctx, left);
    JS_FreeValue(ctx, top);
    /* Steps 3 to 6 — "let document be the element's node document; if document is not the active document …
       let window be the value of document's defaultView attribute; if window is null …". Both absences are the
       one `dctx` the four questions already answered, and the second is asserted unreachable there rather than
       being written as a branch this engine can never take. */
    if (!t.dctx) goto resolved;
    /* step 7 */
    if (t.is_root && t.quirks) goto resolved;
    /* STEP 8 — "if the element is the root element, return the Promise returned by scroll() on window after the
       method is invoked with SCROLLX ON WINDOW as first argument and Y as second argument". The x this member
       was given is DISCARDED, and that is the SPEC'S OWN TEXT rather than a transcription slip — it is quoted
       here verbatim because it reads like one. The sentence is the `scrollTop` setter's step 8 word for word,
       where discarding x is principled (that setter is writing one coordinate, so the other has to come from
       the window), and the scroll members carry it unchanged. It is implemented AS WRITTEN, because the spec
       is what this half of the engine traces to and a member that quietly "fixed" it would be a fidelity gap
       invented here rather than one inherited: a page whose `documentElement.scrollTo(x, y)` moved
       horizontally in this engine and nowhere else is a divergence with nothing to point at.
       §2 Terminology is why this is `viewport_scroll` and not the page-visible `window.scroll`: "when a method
       or an attribute is said to call another method or attribute, the user agent must invoke its INTERNAL API
       for that attribute", so a page overriding `window.scroll` cannot change what this does. */
    if (t.is_root) {
        /* ONLY `y` IS CONSUMED ON THIS ARM, so only `y` is resolved: step 8 DISCARDS x (see the paragraph
           above, which quotes it), and resolving a request no step reads would crash on an unknown `left` that
           this member was never going to scroll by. `el.scrollTo({left: h, top: 0})` on the root element is
           exactly that call. */
        viewport_scroll(t.dctx, scroll_request_px(viewport_window_scroll(t.dctx, /*vertical*/ false)),
                        y, behavior);
        goto resolved;
    }
    /* STEP 9 — "if the element is the body element, document is in quirks mode, and the element is not
       potentially scrollable in either axis, return the Promise returned by scroll() on window after the method
       is invoked with OPTIONS as the only argument". Both coordinates are used here where step 8 dropped one,
       and §4's own step 1 then defaults an absent member to the VIEWPORT's current position — which for a
       quirks-mode body that is not potentially scrollable is the same number step 1 above already defaulted to
       (the `scrollTop` getter's step 6 answers such a body with the window's position), so the two defaults
       agree and there is nothing to re-derive.
       AND THE CONDITION IS THE `EITHER AXIS` ONE, which is NOT the setter's: §6 writes "not potentially
       scrollable in either axis" here and "not potentially scrollable in at least one axis" at the setter's
       step 9, and a body potentially scrollable in exactly one axis is on opposite sides of the two. */
    if (t.is_body && t.quirks && ev_not_potentially_scrollable_in_either_axis(&t)) {
        viewport_scroll(t.dctx, x, y, behavior);
        goto resolved;
    }
    /* Steps 10 and 11, which are the setter's own last two — stated once. An element that terminates at step
       10 has consumed neither coordinate, and that is §6's own answer for an element with no overflow rather
       than a request dropped on the floor: there is no position for it to have but the one it has. */
    ev_scroll_the_element_or_terminate(&t, x, y, behavior);
    /* ONE EXIT FOR THE KEYWORD'S OWNERSHIP. §6 gives this member five places to return a resolved Promise and
       the `behavior` member is read before the first of them, so the release is stated once — a `return` added
       to any of those steps later cannot skip it. */
resolved:
    if (JS_IsString(behavior_v)) JS_FreeCString(ctx, behavior);
    JS_FreeValue(ctx, behavior_v);
    return ev_resolved_promise(ctx);
}

/* ---- §6's `scrollIntoView()` ------------------------------------------------------------------------------ */

/* ONE ENUM-VALUED PIECE OF §6's `scrollIntoView` ARGUMENT, as the keyword its algorithm branches on. OWNED.
 * The declaration has already run §3.2.18's enumeration check, so what arrives is one of the values the IDL
 * lists — or unknown external input, which is the one case this cannot answer and says so.
 * WHY A CONCOLIC IS A CRASH HERE AND NOT A PASS-THROUGH, AND WHY IT IS A DIFFERENT QUESTION FROM THE ONE THE
 * SCROLL MEMBERS BESIDE THIS ONE ASK. What stood here contrasted the two by saying that an unknown `left`/`top`
 * needs no arm because §6.1's clamp lands every value of the domain where the box already is — a derivation
 * that §2's viewport row and §3.1's perform a scroll retired, and that those members no longer make: an
 * unknown POSITION is now CARRIED to the step that scrolls and decided there (`ScrollRequest`,
 * `scroll_request_resolve`). So the contrast is not between a real derivation and a missing one. It is
 * between a VALUE and a QUESTION, which is CLAUDE.md's own line and the reason the two owe different
 * mechanisms: a scroll position is a value with a domain whose fork belongs at the page's own branch over it,
 * while `block` is an ENUMERATION whose domain is finite and declared, so its worlds are the members
 * themselves. `block: "start"` and `block: "end"` align opposite edges of the target against opposite edges of
 * the scrolling box, and on a box with real slack one of them is the position the box already has and the
 * other is not — one terminates and the other performs a scroll. Picking either would delete a world, and
 * §3.2.18's own conversion is where that fork belongs (core/idl_args.h's `idl_concolic_rule` answers
 * IDL_CONCOLIC_FORKS for IDL_ENUM and `idl_enum_fork` is the ask); what this dictionary member lacks is a
 * `JSStepHdr` to ask it from, which is what makes this member a step machine (JS_CFUNC_STEP_DEF). */
static char *ev_scroll_keyword(JSContext *ctx, JSValueConst v, const char *member)
{
    const char *s;
    char *owned;

    if (concolic_is(v))
        DFAIL("CSSOM VIEW §6 \"Extensions to the Element Interface\"'s `scrollIntoView` was handed UNKNOWN "
              "EXTERNAL INPUT as one of its enumerated arguments, and its step 5 reads that value to choose an "
              "ALIGNMENT: `block` of \"start\" aligns the target's beginning edge with the scrolling box's and "
              "\"end\" aligns the ending edges, which are two different scroll positions and not two spellings "
              "of one. So neither arm may be picked and both must run. BUILD the fork at the value's own "
              "resolution site: make this member a step machine (`JS_CFUNC_STEP_DEF`) so it carries a "
              "`JSStepHdr`, and ask `step_fork_run` over the enumeration's values with outcome 0 the IDL's own "
              "DEFAULT — which is step_fork_run's one numbering rule, since outcome 0 is what a run with no "
              "forking policy takes. core/dom/abort.c states the same conversion for the same reason");
    DCHECK(JS_IsString(v),
           "§6's `scrollIntoView` read a dictionary member declared as a Web IDL ENUMERATION and found neither "
           "a string nor unknown external input — §3.2.18's conversion is part of the TYPE and rejects every "
           "other value with a TypeError before this body runs, and each of the three members declares a "
           "default, so an absent one arrives carrying that default rather than as undefined");
    s = JS_ToCString(ctx, v);
    CHECK(s != NULL, "a Web IDL enumeration value that is already a string could not be read as one");
    owned = strdup(s);
    CHECK(owned != NULL, "a Web IDL enumeration keyword could not be copied");
    JS_FreeCString(ctx, s);
    (void)member;
    return owned;
}

/* §6's `scrollIntoView(arg)` STEPS, IN ORDER. Steps 1 to 4 are the four defaults the IDL also writes, and they
 * are the initialisers below rather than a second statement of them — `= "auto"`, `= "start"`, `= "nearest"`
 * and a null container are what the algorithm says and what the declaration places.
 *
 * STEP 7 IS THE REAL TERMINATION AND IT IS THE COMMON CASE, which is why this member is answerable at all: "if
 * the element does not have any associated box, or is not available to user-agent features, then return a
 * resolved Promise and abort the remaining steps". The first disjunct is element_view.h's one predicate, and it
 * covers a disconnected element, an element of a document nothing is presenting, a `<template>`'s contents and
 * anything whose computed `display` is `none` — most of what a lazy-loading bundle calls this on.
 * THE SECOND DISJUNCT IS DERIVED AND ASSERTED, NOT ASSUMED. "Available to user-agent features" is a property of
 * SKIPPED CONTENTS, which css-contain-2 §4 "Suppressing An Element's Contents Entirely: the content-visibility
 * property" states for the `hidden` value — "the skipped contents must not be accessible to user-agent
 * features, such as find-in-page, tab-order navigation, etc." — and contents are skipped only under a computed
 * `content-visibility` of `hidden` or an unrevealed `auto`. This engine's cascade
 * carries no such property at all — core/css/css_computed_value.h's own entry crashes for a property it does
 * not model — so no element's contents are skipped and every element with a box is available. The DCHECK below
 * is the two-sided form of that: the day `content-visibility` enters the cascade (HTML §15.3.1's UA rule for
 * `hidden=until-found` is the first declaration that would put it there, and core/css/css_style_declaration.c
 * already declines to emit it for want of a property to emit into), it fires and this branch has to be written.
 *
 * STEP 9 — "optionally perform some other action that brings the element to the user's attention" — is
 * OPTIONAL in the spec's own word and there is no user to attend, which is a headless answer the spec licenses
 * rather than a stub: nothing is elided, because the sentence permits doing nothing.
 *
 * THE PROMISE IS RESOLVED FOR core/dom/element_scrolling.h's REASON, which is a derivation and not a shrug —
 * and it is a DIFFERENT derivation now. Every scroll this user agent performs is an INSTANT one, because §3.1
 * step 5's smooth arm is gated on honoring css-overflow-3 §4.1's `scroll-behavior` and this cascade declares no
 * such property, so each perform-a-scroll in the ancestor walk has resolved its own promise before it returns.
 * WHAT STOOD HERE WAS THAT EVERY PATH EITHER PERFORMS NO SCROLL — THE VIEWPORT HAVING ONE VALID POSITION AND AN
 * ELEMENT WHOSE SCROLLING AREA EQUALS ITS PADDING BOX HAVING ONE TOO — OR CRASHES; the viewport half of that is
 * retired. So `ancestorPromises` holds only resolved promises and §6.1's scroll a target into view STEP 5 —
 * "Resolve scrollPromise when all Promises in ancestorPromises have settled." — is satisfied at creation.
 * IT WAS CITED HERE AS STEP 4, WHICH IS OFF BY ONE: that algorithm's steps are the ancestorPromises set, the
 * ancestor loop, the new promise, the return-and-run-in-parallel, and the resolve — five, with the resolve
 * last. The neighbouring step 4 is the one that RETURNS scrollPromise, so the wrong number named the step that
 * hands the promise out rather than the one that settles it. */
static JSValue js_ev_scroll_into_view(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
                                      int magic)
{
    ScrollLogicalPosition block = SCROLL_LOGICAL_START, inline_pos = SCROLL_LOGICAL_NEAREST;
    lxb_dom_element_t *container = NULL;   /* step 4 */
    char *behavior = strdup("auto");       /* step 1 */
    EvTarget t;

    (void)magic;
    CHECK(behavior != NULL, "§6's `scrollIntoView` could not hold its own default `behavior` keyword");
    if (!ev_target(ctx, this_val, &t)) { free(behavior); return JS_EXCEPTION; }
    DCHECK(argc == 1,
           "§6 declares `scrollIntoView(optional (boolean or ScrollIntoViewOptions) arg = {})` — one argument, "
           "with the default materialized by the argument machine — so a body seeing any other count is a "
           "declaration that grew without this");
    /* STEP 5 AGAINST STEP 6, decided by which arm of `(boolean or ScrollIntoViewOptions)` the conversion took.
       §3.2.25 converts V to exactly one of the two types and IDL_BOOL_OR_DICT places what it converted, so the
       question here is the union's own OUTPUT and not a shape test performed a second time on the page's
       value — which is what `JS_IsObject` on a concolic would have been. */
    if (JS_IsBool(argv[0])) {
        /* Step 6 — "Otherwise, if arg is false, then set block to "end"". `true` sets nothing, so
           `el.scrollIntoView(true)` and `el.scrollIntoView()` agree on every value §6 computes — BY TWO
           DIFFERENT ROUTES, and the sentence that stood here named only one of them. An OMITTED argument does
           not reach this branch at all: `optional (boolean or ScrollIntoViewOptions) arg = {}` makes the
           position a dictionary, and Web IDL §3.2.25 Union types step 4 sends its `undefined` (and an
           explicit `null`) to the dictionary arm below, where steps 1-4's "auto"/"start"/"nearest"/null are
           what ScrollIntoViewOptions' own declared defaults place. They agreed by ACCIDENT while the union
           read undefined as a non-object and handed this branch a `false`, which set `block` to "end" — the
           opposite alignment — for every `el.scrollIntoView()` there has ever been. */
        if (!JS_ToBool(ctx, argv[0])) block = SCROLL_LOGICAL_END;
    } else if (concolic_is(argv[0])) {
        free(behavior);
        DFAIL("CSSOM VIEW §6's `scrollIntoView` reached its body with UNKNOWN EXTERNAL INPUT that the §3.2.25 "
              "union placed on the BOOLEAN arm, and step 6 reads that boolean: `false` sets `block` to \"end\" "
              "while `true` leaves it at \"start\", which are opposite alignments and two different scroll "
              "positions. The union's ARM was already forked by the conversion (IDL_BOOL_OR_DICT is "
              "IDL_CONCOLIC_FORKS); what is missing is the fork over the boolean's own VALUE, which belongs at "
              "this step. BUILD it the way `ev_scroll_keyword` names — this member as a step machine "
              "(`JS_CFUNC_STEP_DEF`) with `step_fork_run` over two outcomes, outcome 0 being `true` because "
              "that is the arm a run with no forking policy takes and it is the one that leaves every default "
              "in place");
    } else {
        JSValue v;

        DCHECK(JS_IsObject(argv[0]),
               "§6's `scrollIntoView` reached the DICTIONARY arm of `(boolean or ScrollIntoViewOptions)` with "
               "something that is neither the engine-built options object nor unknown external input — "
               "`optional … arg = {}` means an omitted argument IS a dictionary carrying every member's "
               "default, which the argument machine materializes");
        /* Step 5.1 */
        v = idl_dict_get(ctx, argv[0], "behavior");
        free(behavior);
        behavior = ev_scroll_keyword(ctx, v, "behavior");
        JS_FreeValue(ctx, v);
        /* Step 5.2 */
        v = idl_dict_get(ctx, argv[0], "block");
        { char *k = ev_scroll_keyword(ctx, v, "block");
          block = element_scrolling_logical_position(k); free(k); }
        JS_FreeValue(ctx, v);
        /* Step 5.3 */
        v = idl_dict_get(ctx, argv[0], "inline");
        { char *k = ev_scroll_keyword(ctx, v, "inline");
          inline_pos = element_scrolling_logical_position(k); free(k); }
        JS_FreeValue(ctx, v);
        /* Step 5.4 — "if the container dictionary member of options is 'nearest', set container to THE
           ELEMENT". The container is the element itself and not its parent: §6.1's step 2.4 stops the ancestor
           walk once the scrolling box is a shadow-including inclusive ancestor of `container`, and the FIRST
           ancestor scrolling box already is one — which is exactly what "nearest" means. */
        v = idl_dict_get(ctx, argv[0], "container");
        { char *k = ev_scroll_keyword(ctx, v, "container");

          DCHECK(strcmp(k, "all") == 0 || strcmp(k, "nearest") == 0,
                 "§6's `enum ScrollIntoViewContainer { \"all\", \"nearest\" }` admits two values and the "
                 "declaration's §3.2.18 check rejects every other, so a third here is the declaration and this "
                 "read having come apart");
          if (strcmp(k, "nearest") == 0) container = lxb_dom_interface_element(t.node);
          free(k); }
        JS_FreeValue(ctx, v);
    }
    /* STEP 7 */
    DCHECK(!css_computed_models("content-visibility"),
           "css-contain-2's `content-visibility` is in this engine's cascade now, so CSSOM VIEW §6's "
           "`scrollIntoView` step 7 has a disjunct this member does not decide: \"or IS NOT AVAILABLE TO "
           "USER-AGENT FEATURES\", which css-contain-2 §4 \"Suppressing An Element's Contents Entirely: the "
           "content-visibility property\" states over SKIPPED CONTENTS — an element inside a subtree whose computed "
           "`content-visibility` is `hidden`, or `auto` while not relevant to the user. Until the property "
           "existed, every element's value was the initial `visible` and nothing was skipped, which is why this "
           "member could answer the disjunct by derivation. BUILD the flat-tree walk over the computed value "
           "and terminate here for a skipped element; §6's `checkVisibility` wants the same walk");
    if (!t.has_box) { free(behavior); return ev_resolved_promise(ctx); }
    /* STEP 8 */
    element_scrolling_scroll_target_into_view(lxb_dom_interface_element(t.node), behavior, block, inline_pos,
                                              container);
    free(behavior);
    /* STEPS 9 AND 10 */
    return ev_resolved_promise(ctx);
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
    /* Step 7 — "return the width of the element's scrolling area", which is §2's own term and is
       core/layout/scrolling_area.h's: a right-most POSITION over this element's padding edge and every one of
       its descendants' margin edges, and NOT the padding edge's extent `clientWidth` above reports. The two
       differ exactly when the element's content overflows it, which is the state a page asks this member
       about. The extent is a length like `clientWidth`'s and takes the same conversion, so a box whose chain
       bottoms out in the initial containing block reports a `long` carrying the viewport's domain. */
    return ev_length_long(ctx, scrolling_area_extent_px(lxb_dom_interface_element(t->node), vertical));
}

/* ---- §6's `clientWidth`, `clientHeight`, `clientTop` and `clientLeft` ------------------------------------- */

static JSValue ev_client_extent(JSContext *ctx, const EvTarget *t, bool vertical)
{
    /* Step 1: "If the element has no associated box OR IF THE BOX IS INLINE, return zero." Both halves are now
       decided from the computed `display` — the second one used to be waved through on the grounds that the two
       elements step 2 answers for can never be inline (CSS Display §2.7 blockifies the root, and §15.3.1 gives
       `body` `display: block`), which said nothing about the elements step 3 reaches. `span.clientWidth` is 0
       in every user agent, and this is where that is decided: an inline box has a padding edge in the box model
       and §6 declines to report it, so step 3 below must never be asked about one. */
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
    /* Step 3 — "the unscaled width of the padding edge excluding the width of any rendered scrollbar between
       the padding edge and the border edge". The scrollbar term is zero for the reason every other member here
       reads the same way: this user agent renders none. UNSCALED is the other half of the step and it is
       satisfied by construction rather than by undoing anything — a used value IS in CSS pixels, and §2.2's two
       zooms are what would scale it into device pixels for painting. What is left is the padding edge itself,
       which core/layout/used_value.h computes: `width: auto` reaches CSS 2.1 §10.3.3's constraint equation
       there and is solved against §10.1's containing block, so an ordinary `div` answers a real number here —
       one that carries the ICB's domain, which is why the conversion below hands the pair to viewport.h rather
       than returning an integer. A FLOATED OR INLINE-BLOCK box answers here too, through CSS 2.2 §10.3.5's
       shrink-to-fit over core/layout/intrinsic_size.h's measurement of its own text. The arms that still crash
       are that component's own subproblem and not this one's — an absolutely positioned box's width needs
       §10.3.7's static position, and a box whose content this engine cannot measure crashes inside the
       intrinsic walk naming what it met. */
    return ev_length_long(ctx, used_value_padding_edge_px(lxb_dom_interface_element(t->node), vertical));
}

/* CSSOM VIEW §6's `clientTop`/`clientLeft`, in the spec's own two steps. Step 2 is "return the unscaled
   COMPUTED VALUE of the border-top-width / border-left-width property plus the width of any scrollbar rendered
   between the top padding edge and the top border edge, ignoring any transforms that apply to the element and
   its ancestors."
   THE SCROLLBAR TERM IS ZERO AND THAT IS A DERIVATION, not an omission: this user agent renders no scroll bar
   — element_view.h states the box model, and every other §6 member here reads the same fact the same way — so
   the whole answer is that computed value. It is a REAL one now: core/css/css_computed_value.c derives
   `border-*-width` over the four shorthand expansions core/css/css_shorthand.c added, with CSS 2.1 §8.5.1's
   rule that a `none` or `hidden` border style makes it 0 whatever width was declared. So `border-left: 4px
   solid` reports 4, `border-left-width: 4px` with no style reports 0 (the initial style is `none`, which is
   the same number every user agent gives), and an element with no border declaration at all reports 0 for the
   same reason rather than the initial `medium`.
   AND IT IS A WHOLE NUMBER OF DEVICE PIXELS, WHICH IS NOT THE SAME AS A WHOLE NUMBER OF CSS PIXELS.
   css-backgrounds-3 §3.3 makes the computed value "snapped as a border width", so css_length.h has already
   rounded it to a whole number of DEVICE pixels — one CSS pixel each at the modelled ratio of 1, and two
   thirds of one at 1.5, which is why this goes through the same conversion `clientWidth` does rather than
   through a `long` that assumes it never has to round. THE RATIO IS ALSO WHAT THE ANSWER DERIVES FROM:
   viewport.h makes `devicePixelRatio` a PICKED environment fact, so `el.clientTop` is the retina gate asked
   through a member, and the seam mints its domain from the fact the length carries. */
static JSValue ev_client_edge(JSContext *ctx, const EvTarget *t, bool vertical)
{
    CssLength len;

    /* step 1 */
    if (!t->has_box || ev_box_is_inline(t)) return ev_long(ctx, 0.0);
    /* step 2 */
    len = css_computed_length(lxb_dom_interface_element(t->node),
                              vertical ? "border-top-width" : "border-left-width");
    DCHECK(len.kind == CSS_LENGTH_ABSOLUTE,
           "§6's clientTop/clientLeft read a `border-*-width` whose computed value is not an absolute length. "
           "css-backgrounds-3 §3.3's `Computed value:` line is `absolute length, snapped as a border width` "
           "and every arm of that derivation produces one, so a percentage or a keyword here is a "
           "computed-value rule that did not run");
    return ev_length_long(ctx, len.px);
}

/* ---- §6's `getClientRects()` and `getBoundingClientRect()` ----------------------------------------------- */

/* A CLIENT COORDINATE, which is the frame this member reports in and NOT the frame core/layout/flow_position.h
   answers in. §10's `clientX` is "the x-coordinate of the position where the event occurred relative to the
   ORIGIN OF THE VIEWPORT", §11.1 says the same of `getBoxQuads` ("expressed relative to the layout viewport",
   "similar to getClientRects()"), and §4's `scrollX` is "the x-coordinate, RELATIVE TO THE INITIAL CONTAINING
   BLOCK ORIGIN, of the left of the viewport" — so the conversion is one subtraction and it belongs to the
   member, not to the layout. It subtracts the viewport's REAL stored scroll position (core/frame/viewport.h),
   which is what makes a client rectangle move as a page scrolls. IT USED TO SUBTRACT A DERIVED ZERO, on the
   argument that THE VIEWPORT'S SCROLLING AREA IS THE ICB SO THERE IS ONE VALID SCROLL POSITION, and was written
   as the subtraction rather than skipped precisely so that the day the area outgrew the ICB this line would
   already be right. It did, and it was.
   THE MINT IS viewport.h's ONE SEAM, for the reason element_view.h states: a coordinate whose containing-block
   chain bottoms out in the ICB carries the viewport's domain, and `rect.top < 600` is the same environment gate
   `innerHeight < 600` is. CONSUMES nothing; the caller owns the returned value. */
static JSValue ev_client_px(JSContext *ctx, CssPx v)
{
    return viewport_env_derived(v, JS_NewFloat64(ctx, v.px));
}

/* §6's step 3 FOR ONE BOX FRAGMENT — "a DOMRect describing its border area". A border area is CSS 2 §8.1's
   border box, so it is that box's two EXTENTS (core/layout/used_value.h) at its POSITION
   (core/layout/flow_position.h), and the two are asked in that order because the extent is the position's own
   operand: §9.4.1 stacks a box below its preceding siblings' heights, so a component that cannot measure a box
   cannot place the next one either, and the crash the extent raises is the earlier subproblem. */
static void ev_border_area_px(const EvTarget *t, CssPx out[4])
{
    lxb_dom_element_t *el = lxb_dom_interface_element(t->node);
    lxb_dom_element_t *transformed = css_transform_applied_self_or_ancestor(el);
    CssPx w, h, x, y;
    FlowPoint o;

    /* STEP 3'S FIRST CONSTRAINT, ASKED BEFORE THE RECTANGLE IS BUILT — "Apply the transforms that apply to the
       element and its ancestors."
       THE CONSTRAINT IS SATISFIED, NOT SKIPPED, WHEN NOTHING IS TRANSFORMED. css-transforms-1 §2 Terminology
       makes a TRANSFORMED ELEMENT one "with a computed value other than none for the transform property", and
       §3's `Applies to:` line restricts even that to TRANSFORMABLE elements; core/css/css_transform.h asks
       both of this element and of every one of its ancestors, out of each element's own computed value. When
       none of them answers, the transformation matrix this step would map the border area through is the
       IDENTITY, and mapping a rectangle through the identity is the rectangle — so the four numbers below ARE
       the constrained ones. That is a derivation over the whole chain and not the silence it replaced: this
       member used to crash for EVERY element because the cascade had no `transform` value at all to read, so
       an untransformed `div` and a rotated one were one unanswerable case.
       WHAT IS LEFT IS THE MATRIX, and it is reached only by an element that really is transformed. */
    if (transformed != NULL)
        DFAIL("CSSOM VIEW §6 \"Extensions to the Element Interface\"'s getClientRects() step 3 states its first "
              "constraint as \"Apply the transforms that apply to the element and its ancestors\", and one of "
              "them IS transformed — core/css/css_transform.h found a transformable element at or above this "
              "one whose computed `transform` is not `none`. Reporting the border area below would drop an "
              "author's own declaration, which is a WRONG rectangle rather than an absent one. This is not the "
              "scroll-bar term every other member here reads as zero: no scroll bar is a UA CHOICE this model "
              "makes, and a transform is a declaration the page wrote. WHAT TO BUILD, IN ORDER: "
              "core/css/css_computed_value.c crashes for the COMPUTED value of a <transform-list> and names "
              "css-transforms-1 §7 \"The Transform Functions\"' grammar; then §3.2 \"Resolved value of "
              "transform\"'s reduction of a list to one 4x4 matrix; then THIS step, which post-multiplies the "
              "matrices of the element and of every ancestor and maps the border area's four corners through "
              "the product — the result is the AXIS-ALIGNED bounding box of those corners, which is why a "
              "rotated box reports a rectangle wider than its own width");
    /* AN INLINE BOX'S BORDER AREA IS NOT AN EXTENT AT A POSITION and cannot be assembled the way the block
       arm below assembles one: CSS 2 §10.3.1 "Inline, non-replaced elements" says the `width` property "does
       not apply", and §10.6.1 says the same of `height` while making the box's own content area a function of
       its font — so `used_value_border_edge_px` has no answer for either axis. Both numbers come out of the
       FRAGMENT instead (core/layout/flow_position.h), which is the same list `getClientRects` enumerates. */
    if (element_view_fragment_kind(el) == ELEMENT_VIEW_FRAGMENTS_LINE_BOXES) {
        FlowRect *frags = NULL;
        size_t n = flow_inline_fragment_rects(el, &frags);
        FlowRect first = frags[0];

        free(frags);
        if (n > 1)
            DFAIL("CSSOM VIEW §6's get-the-bounding-box was asked in css_length.h's vocabulary for an inline "
                  "box that CSS 2 §9.4.2 \"Inline formatting contexts\" split across SEVERAL line boxes — "
                  "\"when an inline box exceeds the width of a line box, it is split into several boxes and "
                  "these boxes are distributed across several line boxes\" — so §6's steps 3 and 4 must CHOOSE "
                  "between them, and this entry has one rectangle to answer with. The choice is the same one "
                  "`ev_bounding_rect` above crashes for and it is the SAME BUILD: step 3 returns the first "
                  "rectangle when every one has a zero width or height, step 4 otherwise returns the smallest "
                  "rectangle enclosing those that do not — both COMPARISONS over rectangles whose numbers are "
                  "concolics derived from the initial containing block, so neither may be a C branch on the "
                  "example (that deletes the arm a responsive bundle explores) and both go through Geometry "
                  "Interfaces §3's NaN-safe derived edges. BUILD it ONCE, over CssPx, and let both entries "
                  "call it — this entry and that one are two vocabularies for one algorithm and a second copy "
                  "of the choice would be two answers");
        out[0] = css_px_sub(first.x, css_px(viewport_window_scroll(t->dctx, false)));
        out[1] = css_px_sub(first.y, css_px(viewport_window_scroll(t->dctx, true)));
        out[2] = first.width;
        out[3] = first.height;
        return;
    }
    w = used_value_border_edge_px(el, false);
    h = used_value_border_edge_px(el, true);
    o = flow_border_box_origin(el);
    x = css_px_sub(o.x, css_px(viewport_window_scroll(t->dctx, false)));
    y = css_px_sub(o.y, css_px(viewport_window_scroll(t->dctx, true)));
    out[0] = x; out[1] = y; out[2] = w; out[3] = h;
}

/* THE SAME RECTANGLE, MINTED. The four numbers stop here in css_length.h's vocabulary and cross to the page
   through viewport.h's one seam, which is why the derivation above answers a `CssPx[4]` and this answers a
   DOMRect: a caller that has to COMPARE two rectangles (Intersection Observer §3.2.10 steps 9 to 12) must do it
   on the examples, in C, with the environment facts still attached, and a caller that has to hand one to the
   page must mint it. Two callers, one derivation, no second answer. */
static JSValue ev_border_area(const EvTarget *t)
{
    CssPx b[4];

    ev_border_area_px(t, b);
    return dom_rect_new_values(t->rctx, ev_client_px(t->rctx, b[0]), ev_client_px(t->rctx, b[1]),
                               ev_client_px(t->rctx, b[2]), ev_client_px(t->rctx, b[3]));
}

/* §6's step 2's own question — "if the element HAS AN ASSOCIATED SVG LAYOUT BOX". The OUTERMOST `svg` element
   does not: it is a replaced element the CSS box model lays out, which is why every user agent answers its
   `getBoundingClientRect` out of a CSS border box. Its descendants do, and they have no margins, borders or
   paddings for §8.1's box model to be stated over. So the question is whether the element is in the SVG
   namespace UNDER another element that is. */
static bool ev_has_svg_layout_box(const lxb_dom_node_t *n)
{
    const lxb_dom_node_t *p = n->parent;

    return n->ns == LXB_NS_SVG && p != NULL && p->type == LXB_DOM_NODE_TYPE_ELEMENT && p->ns == LXB_NS_SVG;
}

/* §6's step 3's own count — "one for each BOX FRAGMENT". An element generates ONE principal box; it is more
   than one FRAGMENT in exactly two ways, and step 3 names both. An INLINE box is split by the line boxes of
   §9.4.2's inline formatting context, so `span.getClientRects().length` is the number of lines it spans. A
   `table` or `inline-table` is step 3's own second constraint: "include both the table box and the caption box,
   if any, but not the anonymous container box". A fragmentation context would be a third, and there is none —
   this engine has no multicol and no pages, so the principal box of every other element is one fragment and
   that is a derivation rather than an assumption.
   IT IS EXPORTED BECAUSE §7's `offsetWidth`/`offsetHeight` ASK THE SAME QUESTION — "all fragments generated by
   the element's principal box" — and would otherwise carry a second copy of this derivation, free to disagree
   with this one about which boxes are split. The two sections' CRASHES stay apart, because their texts do. */
ElementViewFragments element_view_fragment_kind(lxb_dom_element_t *el)
{
    char *d = css_computed_value(el, "display");
    bool inl, table;

    DCHECK(d != NULL, "the cascade produced no computed `display` for an element");
    /* THE INLINE ARM IS TWO CONJUNCTS AND `display` IS ONLY THE FIRST, which is CSS 2.2 §9.4.2's own
       split-an-inline-box sentence read with §9.2.2 beside it. What §6's step 3 counts once per LINE BOX is
       the box "when an inline box exceeds the width of a line box, it is SPLIT into several boxes and these
       boxes are distributed across several line boxes" — an inline BOX. A REPLACED element keeps a computed
       `display` of `inline` and is not one: §9.2.2 makes it an atomic inline-level box that "participate[s] in
       [its] inline formatting context as a SINGLE OPAQUE BOX", which css-text-3 §5.5 names in the same breath
       as an `inline-block` ("each replaced element or other atomic inline"), so an `img` is ONE fragment
       however many lines the paragraph around it has. It is also the pair CSS 2.1 §10.3.1's and §10.6.1's
       titles are written over — "Inline, NON-REPLACED elements" — which is why core/layout/used_value.c asserts
       the identical conjunction and core/layout/scrolling_area.c splits on the identical predicate: one fact
       answered from three places, and this was the one place that answered it with half the question. */
    inl = strcmp(d, "inline") == 0 && !replaced_element_of(el).replaced;
    table = strcmp(d, "table") == 0 || strcmp(d, "inline-table") == 0;
    free(d);
    if (inl) return ELEMENT_VIEW_FRAGMENTS_LINE_BOXES;
    if (table) return ELEMENT_VIEW_FRAGMENTS_TABLE;
    return ELEMENT_VIEW_FRAGMENTS_ONE;
}

/* §6's step 3's SECOND CONSTRAINT — the one fragment shape this engine cannot enumerate. THE INLINE ARM IS
   GONE FROM HERE BECAUSE IT IS BUILT, and this comment says so because the crash that stood here instructed
   the reader to build it: core/layout/flow_position.h's `flow_inline_fragment_rects` answers every border area
   of a non-replaced inline box, out of `line_box_inline_fragments`' per-line fragments, out of
   `text_run_measure_line_offset`'s per-item position and css-text-4 §7.3 "Default Text Alignment: the
   text-align-all property"'s alignment. `ev_client_rects` below emits one rect per line box the box spans. */
static void ev_require_enumerable_fragments(const EvTarget *t)
{
    ElementViewFragments kind = element_view_fragment_kind(lxb_dom_interface_element(t->node));

    if (kind == ELEMENT_VIEW_FRAGMENTS_TABLE)
        DFAIL("CSSOM VIEW §6's getClientRects() step 3's SECOND CONSTRAINT: an element whose computed `display` "
              "is `table` or `inline-table` contributes 'both the TABLE BOX and the CAPTION BOX, if any, but "
              "not the anonymous container box' — two fragments out of a box structure CSS 2.1 §17.2.1 "
              "Anonymous table objects generates. THE STRUCTURE IS BUILT and this line used to say it was not: "
              "core/layout/table_box.h answers §17.2.1's first two stages, and `table_box_captions` answers "
              "exactly the CAPTION BOX half of the sentence above — the anonymous container this step excludes "
              "is CSS 2.1 §17.4 Tables in the visual formatting model's table wrapper box, which nothing here "
              "has to produce. ITS EXTENTS ARE NOT §10's EITHER AND BOTH AXES ARE NOW ANSWERED — this line "
              "used to name the block axis as the one still missing, and then told its reader to BUILD it. "
              "CSS 2.1 §17.5.2 Table width algorithms: the 'table-layout' property owns the table's WIDTH "
              "and CSS 2.1 §17.5.3 Table height algorithms owns its HEIGHT, and BOTH are components "
              "(core/layout/table_width.h, core/layout/table_height.h) that core/layout/used_value.c routes "
              "a table box to on the declared arm and the `auto` arm alike — each section takes the "
              "declaration as an INPUT to its own comparison rather than as the used value, so there is no "
              "declared-height arm left for it to crash in. WHAT IS STILL MISSING IS THIS STEP'S OWN "
              "ENUMERATION AND NOT AN EXTENT: one ELEMENT's entry has to answer SEVERAL rectangles — the "
              "table box's, which is the single-fragment arm below, and one per element `table_box_captions` "
              "reports, each of which CSS 2.1 §17.4 Tables in the visual formatting model makes an ordinary "
              "block-level box — \"The caption boxes are block-level boxes that retain their own content, "
              "padding, margin, and border areas, and are rendered as normal block boxes inside the table "
              "wrapper box\". Emit that LIST here, the way "
              "the line-box arm above emits one rectangle per fragment");
}

/* §6's getClientRects() STEPS, AS THE INTERNAL ALGORITHM. §2 is explicit that a member "said to call another
   method or attribute" invokes the algorithm and not the page-visible member, and getBoundingClientRect's step
   1 is exactly such a call — so a page that overwrites `Element.prototype.getClientRects` cannot change what
   `getBoundingClientRect()` measures, and neither can it change what §9's Range members measure.
   STEP 1 IS THE ONE THIS ENGINE ANSWERS FOR REAL, and it is a DERIVATION and not a stand-in: an element with
   no associated box generates no fragments in ANY user agent, so the empty list is the whole domain of the
   answer rather than one point picked out of it. element_view.h's one predicate is what decides it, so this
   branch covers a disconnected element, an element of a document nothing is presenting (a DOMParser parse, a
   `<template>`'s contents, the document of a destroyed navigable) and anything whose computed `display` is
   `none` — which is most of what a lazy-loading bundle measures before it inserts anything. */
static JSValue ev_client_rects(JSContext *ctx, const EvTarget *t)
{
    JSValue rects;

    (void)ctx;   /* every object below is minted in the ELEMENT's relevant realm, never the caller's */
    /* step 1 */
    if (!t->has_box) return dom_rect_list_new(t->rctx, JS_NewArray(t->rctx));
    /* step 2 */
    if (ev_has_svg_layout_box(t->node))
        DFAIL("CSSOM VIEW §6's getClientRects() step 2: an element with an associated SVG LAYOUT BOX answers "
              "with a single DOMRect describing 'the bounding box of the element AS DEFINED BY THE SVG "
              "SPECIFICATION' — SVG 2's own object bounding box over the element's geometry, which is a "
              "different computation from CSS 2 §8.1's border box and not a special case of it (a `<path>` has "
              "no margins and no borders; its bounds are its own segments). This engine lays out no SVG at all. "
              "BUILD SVG 2's bounding box beside the CSS one, in its own component, since none of §10's used "
              "values applies to it");
    /* step 3, whose second constraint is decided first */
    ev_require_enumerable_fragments(t);
    /* Step 3's THIRD constraint — "replace each anonymous block box with its child box(es) and repeat" — never
       fires for a list this engine produces, because an ELEMENT's own principal box is never anonymous: CSS 2
       §9.2.1.1 generates one only around block-level children of an inline-containing block container, and it
       belongs to no element. The list below is therefore already in the constraint's final form. */
    rects = JS_NewArray(t->rctx);
    CHECK(!JS_IsException(rects), "the client-rect list could not be allocated");
    /* STEP 3'S COUNT — "one for each BOX FRAGMENT", "IN CONTENT ORDER". For an inline box that is one per line
       box it spans, which is the whole reason this member answers a list; `flow_inline_fragment_rects` reports
       them in the order the fill assigned the items, which IS content order because CSS 2 §9.4.2 distributes
       the run in document order and stacks the lines downward. */
    if (element_view_fragment_kind(lxb_dom_interface_element(t->node)) == ELEMENT_VIEW_FRAGMENTS_LINE_BOXES) {
        FlowRect *frags = NULL;
        size_t n = flow_inline_fragment_rects(lxb_dom_interface_element(t->node), &frags), i;

        for (i = 0; i < n; i++) {
            CssPx x = css_px_sub(frags[i].x, css_px(viewport_window_scroll(t->dctx, false)));
            CssPx y = css_px_sub(frags[i].y, css_px(viewport_window_scroll(t->dctx, true)));

            JS_SetPropertyUint32(t->rctx, rects, (uint32_t)i,
                                 dom_rect_new_values(t->rctx, ev_client_px(t->rctx, x),
                                                     ev_client_px(t->rctx, y),
                                                     ev_client_px(t->rctx, frags[i].width),
                                                     ev_client_px(t->rctx, frags[i].height)));
        }
        free(frags);
        return dom_rect_list_new(t->rctx, rects);
    }
    JS_SetPropertyUint32(t->rctx, rects, 0, ev_border_area(t));
    return dom_rect_list_new(t->rctx, rects);
}

JSValue element_view_client_rects(lxb_dom_element_t *el)
{
    EvTarget t;

    DCHECK(el != NULL, "§6's getClientRects() was invoked as an internal algorithm with no element");
    ev_target_of_element(el, &t);
    return ev_client_rects(t.rctx, &t);
}

/* §6's GET THE BOUNDING BOX, ANSWERED IN css_length.h's VOCABULARY — the entry a caller takes when the
   rectangle is an OPERAND rather than a result. See element_view.h.
   IT IS THE SAME FOUR STEPS `ev_bounding_rect` performs and it takes the same two roads, which is the only way
   this can be a second ENTRY without being a second ANSWER: step 1's list is empty exactly when the element has
   no associated box, so step 2's "a DOMRect object whose x, y, width and height members are zero" is reached by the
   one predicate element_view.h states and not by counting a list that was built to be counted; and a list of
   one is what steps 3 and 4 both answer with, which is the derivation `ev_bounding_rect` writes out. What is
   deliberately NOT reachable here is that function's multi-fragment crash — the two gates below fire first, for
   the only two ways §6 says a list of more than one arises, so a caller of this entry meets the earlier
   subproblem by its own name rather than the later one by a count. */
void element_view_bounding_box_px(lxb_dom_element_t *el, CssPx out[4])
{
    EvTarget t;

    DCHECK(el != NULL, "§6's get-the-bounding-box was invoked as an internal algorithm with no element");
    ev_target_of_element(el, &t);
    if (!t.has_box) {                       /* steps 1 and 2 */
        out[0] = out[1] = out[2] = out[3] = css_px(0.0);
        return;
    }
    if (ev_has_svg_layout_box(t.node))      /* step 2 of getClientRects, which step 1 called */
        DFAIL("CSSOM VIEW §6's getClientRects() step 2: an element with an associated SVG LAYOUT BOX answers "
              "with a single DOMRect describing 'the bounding box of the element AS DEFINED BY THE SVG "
              "SPECIFICATION', which is SVG 2's own object bounding box and not a special case of CSS 2 §8.1's "
              "border box. This engine lays out no SVG at all. BUILD SVG 2's bounding box beside the CSS one, "
              "in its own component");
    ev_require_enumerable_fragments(&t);
    ev_border_area_px(&t, out);
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
    /* step 2 */
    if (n == 0) {
        JS_FreeValue(t->rctx, list);
        return dom_rect_new(t->rctx, 0.0, 0.0, 0.0, 0.0);
    }
    /* STEPS 3 AND 4 AGREE FOR A ONE-FRAGMENT LIST, and that is a derivation rather than a shortcut past the
       branch between them. Step 3 returns "the first rectangle in list" when every rectangle has a zero width
       or height; step 4 otherwise returns "the smallest rectangle that includes all of the rectangles in list
       of which the height or width is not zero" — and the smallest rectangle enclosing exactly one rectangle
       IS that rectangle. So a list of one answers with its own single member under either step, with no
       comparison performed, which is also what keeps this file's rule that nothing here branches in C on a
       concolic: a border area derived from the initial containing block carries the viewport's domain, and
       `width == 0` asked of one would be a C branch over unknown input. §6's `[NewObject]` is satisfied
       because the list above is minted fresh on every call. */
    if (n == 1) {
        JSValue only = JS_GetPropertyUint32(t->rctx, list, 0);

        DCHECK(dom_rect_is(only), "§6's get-the-bounding-box read a client-rect list whose one member is not a "
                                  "DOMRect — getClientRects answers §4's list over rectangles and nothing else");
        JS_FreeValue(t->rctx, list);
        return only;
    }
    JS_FreeValue(t->rctx, list);
    DFAIL("CSSOM VIEW §6's get-the-bounding-box steps 3 and 4 must CHOOSE for a list of more than one "
          "rectangle: step 3 returns the first when every one of them has a zero width or a zero height, step 4 "
          "otherwise returns the smallest rectangle enclosing those whose width and height are not zero. Both "
          "the choice and the union are COMPARISONS over the rectangles' own numbers, and a border area derived "
          "from the initial containing block is a concolic whose domain is the viewport's — so writing them "
          "needs a decision this file has never had to make, and the answer is NOT a C branch on the example "
          "(that would delete the arm a responsive bundle explores). The union in step 4 goes through Geometry "
          "Interfaces §3's derived edges, whose own NaN-safe minimum already states the rule for an unknown "
          "operand: yield the concolic with the real derivation run on the examples, never fork. WRITE BOTH "
          "over that primitive when a multi-fragment list exists — an inline box across line boxes, or a "
          "`table` and its caption, which are the two ways step 3 above says one arises");
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
    case EV_CLIENT_TOP:     return ev_client_edge(ctx, &t, true);
    case EV_CLIENT_LEFT:    return ev_client_edge(ctx, &t, false);
    }
    DFAIL("a CSSOM VIEW §6 Element member was read with a magic no member of this file declares — the magic IS "
          "the member, so an unknown one means a name was installed without a case to answer it");
    return JS_UNDEFINED;
}

/* §4's `ScrollToOptions`, WHICH §6's THREE MEMBERS TAKE — declared where they are declared, because a
   dictionary is part of the TYPE of the argument that names it and this is the only member list that carries
   it today. §4 states it as two dictionaries:

       enum ScrollBehavior { "auto", "instant", "smooth" };
       dictionary ScrollOptions { ScrollBehavior behavior = "auto"; };
       dictionary ScrollToOptions : ScrollOptions { unrestricted double left; unrestricted double top; };

   §3.2.17 Dictionary types reads the INHERITED members first and each dictionary's own lexicographically among
   themselves, which is `behavior` (ScrollOptions, level 0), then `left` and `top` (level 1) — the order the
   `level` column states and the order a page's getters observe.
   NEITHER `left` NOR `top` HAS A DEFAULT, and that is the whole of how §6 step 1 can say "or the element's
   current scroll position on the x axis otherwise": a member with no `= …` does not EXIST on the converted
   dictionary when the page did not write it, so absence is a state the body can read. Giving either one a
   `= 0` here would turn `el.scrollTo({top: 40})` into a horizontal scroll to the origin.
   `behavior` IS DECLARED AND NOT YET READ, AND THAT IS NOT A FIELD NOBODY WROTE — it is a CONVERSION whose
   whole observable effect happens before any step consumes the value. §3.2.18's enumeration check is part of
   the TYPE, so `el.scrollTo({behavior: "bogus"})` is a rejected promise in this engine exactly as it is in a
   browser, while a declaration that left the member out would accept it silently. The step that consumes the
   value is §3.1 Scrolling's perform-a-scroll behavior argument, which is inside the crash
   `ev_scroll_the_element_or_terminate` names — so the member arrives with the algorithm that reads it, and
   until then the type is the entire member. */
/* THE ENUM AND THE DICTIONARY MOVED TO core/frame/viewport.h, WHICH IS THE COMPONENT THAT OWNS §4. The
   paragraph above already said what the move follows: "§4 states it as two dictionaries", and this file was
   only "the only member list that carries it today". §4's own three Window scroll members carry it now too, so
   a copy here would be one dictionary with two definitions — free to disagree about the READ ORDER §3.2.17
   fixes, which is the order a page's getters observe. */

/* §6's TWO OVERLOADS AS ONE DECLARATION — the longest type list the effective overload set has, with the
   position the entries split at carrying that split as its type:

       Promise<undefined> scroll(optional ScrollToOptions options = {});
       Promise<undefined> scroll(unrestricted double x, unrestricted double y);

   §3.6 steps 3-4 remove the dictionary entry the moment a second argument is passed, which is why position 0
   is one row rather than a shape test in the body — see IDL_UNRESTRICTED_DOUBLE_OR_DICT. */
static const IdlArgType EV_SCROLL_ARGS[2] = { IDL_UNRESTRICTED_DOUBLE_OR_DICT, IDL_UNRESTRICTED_DOUBLE };

/* §6's `dictionary ScrollIntoViewOptions : ScrollOptions`, in §3.2.17's OWN READ ORDER — the inherited
   dictionary's members first (`behavior`, level 0) and then this one's own LEXICOGRAPHICALLY among themselves
   (`block`, `container`, `inline`), which is the order a page's getters observe and is NOT the order the IDL
   block lists them in. Every one of the four declares a DEFAULT, which is what lets `scrollIntoView`'s body
   read each member as a value rather than testing for absence — §6's steps 1 to 4 set the same four values,
   and the declaration placing them is the same statement made once.
   `inline` IS A DICTIONARY MEMBER NAMED WITH A RESERVED WORD, and that is the IDL's own spelling: Web IDL
   identifiers are not ECMAScript ones, and the property a page writes really is `{inline: "center"}`. */
/* THE `ScrollLogicalPosition` LIST IS core/dom/element_scrolling.h's AND IS NOT RESTATED HERE. §3.2.18's
   enumeration check reads it and §6.1's alignment switch is written over the same four keywords in the same
   order, so a second array would be one enumeration with two definitions — free to disagree about an order
   nothing else states, which is exactly how a keyword the declaration accepts becomes one the algorithm maps
   to the wrong arm. */
IDL_ENUM_VALUES(EV_SCROLL_INTO_VIEW_CONTAINER, "all", "nearest");
static const IdlDictMember EV_SCROLL_INTO_VIEW_OPTIONS[] = {
    { "behavior",  IDL_ENUM, false, VIEWPORT_SCROLL_BEHAVIOR,            0, NULL, IDL_DEFAULT_STRING, "auto" },
    { "block",     IDL_ENUM, false, SCROLL_LOGICAL_POSITIONS,      1, NULL, IDL_DEFAULT_STRING, "start" },
    { "container", IDL_ENUM, false, EV_SCROLL_INTO_VIEW_CONTAINER, 1, NULL, IDL_DEFAULT_STRING, "all" },
    { "inline",    IDL_ENUM, false, SCROLL_LOGICAL_POSITIONS,      1, NULL, IDL_DEFAULT_STRING, "nearest" },
};
/* `optional (boolean or ScrollIntoViewOptions) arg = {}` — ONE position whose type IS the union, so §3.2.25's
   arm is resolved by the conversion machine and the body reads its OUTPUT (see IDL_BOOL_OR_DICT). It is not an
   overload: §6 declares `scrollIntoView` once, with one argument. */
static const IdlArgType EV_SCROLL_INTO_VIEW_ARGS[1] = { IDL_BOOL_OR_DICT };

/* ONE OF §6's THREE SCROLL METHODS, DECLARED — the same argument shape and the same dictionary for all of
   them, differing only in which of the two algorithms the magic selects. */
static int ev_declare_scroll(JSContext *ctx, EvScrollKind kind)
{
    int id = idl_method_id_dict(ctx, EV_SCROLL_ARGS, 2, VIEWPORT_SCROLL_TO_OPTIONS,
                                VIEWPORT_SCROLL_TO_OPTIONS_N,
                                js_ev_scroll, (int)kind);

    /* §3.7.7's PROMISE RETURN TYPE. It is what makes `el.scrollTo(0)` — §3.2.17 step 1 refusing a value that
       is not undefined, null or an Object — a REJECTED promise rather than a throw, which is what a page
       wrapping the call in `.catch` is relying on. Its position here is only reading order: this used to say
       it stood before the optional index "for idl_returns_promise's own reason", and there is no such reason —
       see that function, whose ordering claim was false in both halves and is corrected there. */
    idl_returns_promise();
    /* THE DICTIONARY ENTRY DECLARES POSITION 0 OPTIONAL (`optional ScrollToOptions options = {}`), so
       `el.scrollTo()` is a legal call… */
    idl_optional_from(0);
    /* …AND THE NUMERIC ENTRY DECLARES NEITHER OF ITS TWO POSITIONS OPTIONAL, which is a different list of
       optionality values for the same declaration and is why §3.6 step 15.3 needs both. Without it
       `el.scrollTo(1, undefined)` would read `undefined` at position 1 as an ABSENT optional and default y to
       the current position, where the surviving entry owes it ToNumber(undefined) and then §3.2's normalized
       0. `nargs` is this member's own "there are none". */
    idl_overload_split_optional_from(2);
    return id;
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
    g_id_scroll          = ev_declare_scroll(ctx, EV_SCROLL_ABSOLUTE);
    g_id_scroll_by       = ev_declare_scroll(ctx, EV_SCROLL_RELATIVE);
    g_id_scroll_into_view = idl_method_id_dict(ctx, EV_SCROLL_INTO_VIEW_ARGS, 1,
                                               EV_SCROLL_INTO_VIEW_OPTIONS,
                                               (int)(sizeof EV_SCROLL_INTO_VIEW_OPTIONS /
                                                     sizeof EV_SCROLL_INTO_VIEW_OPTIONS[0]),
                                               js_ev_scroll_into_view, 0);
    /* §3.7.7's PROMISE RETURN TYPE — `Promise<undefined> scrollIntoView(...)`, so a conversion that throws
       (§3.2.18 refusing `{block: "middle"}`) is a REJECTED promise and not a throw, which is what a page
       wrapping the call in `.catch` relies on. Its position before the optional index is reading order and
       nothing else — the "idl_returns_promise's own reason" this used to cite does not exist, and the false
       ordering claim is corrected at that function. */
    idl_returns_promise();
    idl_optional_from(0);
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
    idl_install_method(ctx, proto, "getClientRects", g_id_client_rects);
    idl_install_method(ctx, proto, "getBoundingClientRect", g_id_bounding_rect);
    /* §3.7.7's `length` is §3.6's OWN NUMBER for an overloaded operation: the smallest argument-list length
       over the effective overload set's entries, which for these three is ZERO — `scroll(optional
       ScrollToOptions options = {})` can be called with nothing. `Element.prototype.scrollTo.length` is 0 in a
       browser for exactly that reason, and a feature detector reading it is reading the overload set.
       `scrollTo` IS `scroll` — one declaration under two names, per §6 — so there is no second body to keep in
       step and no second place for the two to disagree. */
    idl_install_method(ctx, proto, "scroll", g_id_scroll);
    idl_install_method(ctx, proto, "scrollTo", g_id_scroll);
    idl_install_method(ctx, proto, "scrollBy", g_id_scroll_by);
    /* §3.7.7's `length` is 0 — the one argument is `optional`, so `Element.prototype.scrollIntoView.length` is
       0 in a browser and a feature detector reading it is reading the declaration. */
    idl_install_method(ctx, proto, "scrollIntoView", g_id_scroll_into_view);
}

void element_view_free(void)
{
    /* The member ids are the AGENT's, and the pool they live in goes with the runtime. */
    g_id_set_scroll_top = g_id_set_scroll_left = -1;
    g_id_client_rects = g_id_bounding_rect = -1;
    g_id_scroll = g_id_scroll_by = g_id_scroll_into_view = -1;
}
