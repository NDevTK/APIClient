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
#include "core/dom/flat_tree.h"
#include "core/css/css_computed_style.h"
#include "core/css/css_computed_value.h"
#include "core/css/css_length.h"
#include "core/css/css_style_declaration.h"   /* the author matches the box tests read, asked resting */
#include "core/css/css_transform_matrix.h"
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
#include "core/layout/layout_job.h"
#include "core/layout/flow_position.h"
#include "core/layout/replaced_element.h"
#include "core/layout/scroll_container.h"
#include "core/layout/scrolling_area.h"
#include "core/layout/table_box.h"
#include "core/layout/used_value.h"
#include "solver/concolic.h"
#include "solver/metrics.h"
#include "solver/rest_unit.h"
#include "solver/reclaim.h"
#include <lexbor/core/base.h>   /* lexbor_malloc: the allocator the engine gives its refusal edge */

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
    EV_SCROLL_RELATIVE,       /* `scrollBy()`: the same steps with steps 3-4's addition ahead of them */
    EV_SCROLL_SET_TOP,        /* the `scrollTop` setter, whose steps 10 and 11 are the methods' */
    EV_SCROLL_SET_LEFT        /* the `scrollLeft` setter */
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
   `element_view_client_rects_ask` without a receiver to brand. */
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

/* WEB IDL §3.7.6 "Attributes"' AND §3.7.7 "Operations"' RECEIVER TEST IS STATED AT EACH DECLARATION AND
   PERFORMED BEFORE THIS RUNS, so this fills the four questions and decides nothing.

   `Element.prototype.clientWidth` read off a plain object is a TypeError, which a feature detector that probes
   the descriptor and applies the getter reads as "this is a real interface". It is not this function's to
   answer: the eight attributes and the six operations all declare idl_this_iface over core/dom/element.h's
   `element_is`, so Web IDL §3.7 Interfaces' implementation-check step 3 runs at one place for all fourteen.

   AND THE ORDER IS WHAT THE MOVE BUYS, WHICH A BODY-SIDE TEST CANNOT HAVE. §3.7.7's try-list asks the
   receiver at step 2.1.2.3, BEFORE step 2.1.4 computes the effective overload set, and §3.7.6's setter asks
   it — Web IDL §3.7.6 "Attributes": "If validThis is false and attribute was not specified with the
   [LegacyLenientThis] extended attribute, then throw a TypeError." — before it converts V. A test written
   here ran after §3.6 had converted every argument, so `Element.prototype.scrollTo.call({}, {valueOf(){…}})`
   ran the page's `valueOf` and only then refused, where a browser refuses with none of it having run.

   IT MAY NOT COME BACK AS AN ASSERT. A receiver is PAGE-SUPPLIED INPUT, so a DCHECK on it is an abort switch
   the page holds in dev and, in release where the DCHECK is compiled out, a dereference of the NULL the
   assert was standing on. The refusal that replaces it is §3.7.6's TypeError and lives in both builds. */
static void ev_target(JSValueConst this_val, EvTarget *t)
{
    lxb_dom_element_t *el = element_of_value(this_val);

    /* ON WHAT THE DECLARATION JUST READ, NEVER ON WHAT THE PAGE PASSED. `element_is` IS this read, so reaching
       here with no element means the member was installed or declared without stating its receiver interface —
       which is a fact about this file's own install block and not about the page. */
    DCHECK(el != NULL,
           "a CSSOM VIEW §6 Element member reached its body on a receiver that is not an Element — every one "
           "of them states element_is at its install or its declaration, so reaching here means one member "
           "was added without doing so");
    ev_target_of_element(el, t);
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
    return viewport_env_derived_long(px, ev_long(ctx, floor(px.px + 0.5)));
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

/* Whether `n` is in a document a viewport presents, the half of the subtree question no `display` decides. */
static bool ev_in_presented_document(const lxb_dom_node_t *n)
{
    JSContext *dctx;

    /* A node whose root is not a document has no boxes in any user agent. */
    if (!node_is_connected(n)) return false;
    /* And neither does one in a document nothing is presenting: a DOMParser document, a `<template>`'s
       contents owner and the document of a navigable that has been destroyed are all trees a user agent lays
       out nothing for. That is the same question viewport.h asks — is there a viewport — asked of the
       ELEMENT'S document. */
    if (!n->owner_document) return false;
    dctx = document_active_realm_of(lxb_dom_interface_node(n->owner_document));
    return dctx != NULL && viewport_exists(dctx);
}

/* Whether `n` is content of a replaced element, which CSS 2 §3.1 "Definitions" places outside the formatting
   model ("The content of replaced elements is not considered in the CSS rendering model"), so it generates no
   CSS box: HTML §15.4.1 "Embedded content"'s canvas fallback, or anything inside an `img` or `video`. The one
   content of a replaced element that has a box is an SVG element reached from an outermost `svg` through SVG
   elements only, whose SVG layout box CSSOM View §6 getClientRects() step 2 answers. A `foreignObject` (SVG 2
   §12.4 "The 'foreignObject' element") renders its children "using other user agent processes", so the walk
   restarts there: its content is in the formatting model if the `foreignObject` itself has a box. */
static bool ev_in_replaced_content(const lxb_dom_node_t *n)
{
    const lxb_dom_node_t *a;
    bool svg_chain = n->ns == LXB_NS_SVG;

    for (a = flat_tree_parent(n); a != NULL && a->type == LXB_DOM_NODE_TYPE_ELEMENT; a = flat_tree_parent(a)) {
        lxb_dom_element_t *el = lxb_dom_interface_element((lxb_dom_node_t *)a);
        size_t len = 0;
        const lxb_char_t *name = lxb_dom_element_local_name(el, &len);

        if (replaced_element_is(el) && !(a->ns == LXB_NS_SVG && svg_chain)) return true;
        if (a->ns == LXB_NS_SVG && len == 13 && memcmp(name, "foreignObject", 13) == 0) svg_chain = true;
        else svg_chain = svg_chain && a->ns == LXB_NS_SVG;
    }
    return false;
}

bool element_view_subtree_has_boxes(const lxb_dom_node_t *n)
{
    const lxb_dom_node_t *a;

    DCHECK(n && n->type == LXB_DOM_NODE_TYPE_ELEMENT,
           "css-display-3 §2.5's subtree question was asked about a node that is not an element — the two "
           "keywords it is stated over are values of `display`, and only an element has one");
    if (!ev_in_presented_document(n)) return false;
    /* AND THE COMPUTED `display`, which is the property this question was always about. This walk used to look
       for the `hidden` CONTENT ATTRIBUTE, which is one UA-stylesheet rule for one of the values `display` can
       take — so an author `display: none`, the rule for `<head>` and `<script>`, and a page's own
       `[hidden] { display: block }` overriding the UA rule were all invisible to it. The attribute is now
       decided where every other UA rule is (css_style_declaration.c's UA layer), and this asks css-display-3
       §2.5's ONE keyword that reaches a whole subtree: "the element and its descendants generate no boxes or
       text sequences". */
    for (a = n; a != NULL && a->type == LXB_DOM_NODE_TYPE_ELEMENT; a = flat_tree_parent(a)) {
        CssDisplay d = css_computed_display(lxb_dom_interface_element((lxb_dom_node_t *)a));
        bool none;

        none = css_display_spells(d, CSS_KEYWORD_NONE);
        if (none) return false;
    }
    return !ev_in_replaced_content(n);
}

/* The author matches element_view_has_box reads, each inclusive ancestor's for its computed `display`, asked
   under the resting drive on the entered job, so a page-sized sheet rests between rules rather than inside one
   box test. False when a build parked; the step rests and asks again. A node in no presented document reads
   no `display`, so nothing is asked for it. */
bool element_view_box_tests_ask(const lxb_dom_node_t *n)
{
    const lxb_dom_node_t *a;

    DCHECK(n != NULL && n->type == LXB_DOM_NODE_TYPE_ELEMENT,
           "a CSSOM VIEW §6 member's box tests were asked of no element — each member states element_is");
    if (!ev_in_presented_document(n)) return true;
    for (a = n; a != NULL && a->type == LXB_DOM_NODE_TYPE_ELEMENT; a = flat_tree_parent(a))
        if (!cssom_style_ask(lxb_dom_interface_element((lxb_dom_node_t *)a))) return false;
    return true;
}

bool element_view_has_box(const lxb_dom_node_t *n)
{
    CssDisplay d;
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
    d = css_computed_display(lxb_dom_interface_element((lxb_dom_node_t *)n));
    contents = css_display_spells(d, CSS_KEYWORD_CONTENTS);
    return !contents;
}

/* CSSOM VIEW's "POTENTIALLY SCROLLABLE IN AN AXIS" — the branch four of §6's steps take and none of them could
   decide until a C caller could read a computed value. The definition's own three conditions, in its own order:
   the body has an ASSOCIATED BOX; the body's PARENT ELEMENT's computed `overflow-x`/`overflow-y` (whichever is
   in the given axis) is neither `visible` nor `clip`; and the body's OWN is neither. */
static bool ev_axis_clips_or_scrolls(const lxb_dom_node_t *el, bool vertical)
{
    CssKeyword k = css_computed_keyword(lxb_dom_interface_element((lxb_dom_node_t *)el),
                                        vertical ? "overflow-y" : "overflow-x");

    return k != CSS_KEYWORD_VISIBLE && k != CSS_KEYWORD_CLIP;
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
    CssDisplay d = css_computed_display(lxb_dom_interface_element(t->node));
    bool inl = css_display_spells(d, CSS_KEYWORD_INLINE);

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
   own pair. CSSOM VIEW §2 "Terminology" is explicit that "when a method or an attribute is said to call
   another method or attribute, the user agent must invoke its INTERNAL API for that attribute", and §6's
   scroll members do exactly that at three steps: step 1's "or the element's current scroll position on the x
   axis otherwise", and `scrollBy`'s steps 3 and 4 ("add the value of scrollLeft to the left dictionary
   member"). Those callers need the NUMBER, so the derivation answers a double and the member wraps it —
   rather than the callers
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

/* Where §6's setter or scroll members end, once steps 1 to 9 have run: at a resolved exit, at §4's `scroll()`
   on the window (steps 8 and 9), or at steps 10 and 11 over the element's own scrolling box. */
typedef enum {
    EV_SCROLL_TO_RESOLVED,
    EV_SCROLL_TO_WINDOW,
    EV_SCROLL_TO_ELEMENT,
} EvScrollToArm;

/* The setter's and the scroll members' state, from step 1's requested position to step 11's scroll. */
typedef struct {
    LayoutJob *job;               /* owned, declared through layout_job_step_ops; NULL before the first activation */
    ScrollRequest x, y;           /* the requested position, resolved only by the step that consumes it */
    char behavior[16];            /* §4's ScrollBehavior keyword, one of the three the enumeration admits */
    ElementScrollingMeasure m;    /* step 10's scrolling box, measured; its nodes are the running flow's */
} EvScrollToState;

static void ev_scroll_to_behavior(EvScrollToState *s, const char *behavior)
{
    DCHECKF(strlen(behavior) < sizeof s->behavior,
            "§4's ScrollBehavior keyword `%s` does not fit the state's copy — the enumeration's three values are "
            "what §3.2.18 admits, and each is shorter than the copy", behavior);
    strcpy(s->behavior, behavior);
}

/* The `scrollTop`/`scrollLeft` setter's steps 1 to 9. Step 1's requested position is carried, not resolved,
   because steps 3-7 and step 10 each terminate without consuming it. */
static EvScrollToArm ev_set_steps(JSContext *ctx, const EvTarget *t, bool vertical, JSValueConst val,
                                  EvScrollToState *s)
{
    ScrollRequest req = { .unknown = concolic_is(val), .px = 0.0 };

    if (!req.unknown) {
        double v;

        DCHECK(JS_IsNumber(val),
               "§6's scrollTop/scrollLeft setter was handed something that is not a number — its IDL type is "
               "`unrestricted double` and the declaration converts it, which is also what makes §3.2's "
               "normalization below reachable rather than a type error");
        JS_ToFloat64(ctx, &v, val);
        /* step 2 — §3.2's normalize non-finite values: Infinity, -Infinity and NaN all become 0. */
        req.px = isfinite(v) ? v : 0.0;
    }
    ev_scroll_to_behavior(s, "auto");
    /* steps 3-6 */
    if (!t->dctx) return EV_SCROLL_TO_RESOLVED;
    /* step 7 */
    if (t->is_root && t->quirks) return EV_SCROLL_TO_RESOLVED;
    /* Steps 8 and 9 — "invoke scroll() on window with scrollX as first argument and y as second" — for the
       root element, and for a quirks-mode body "not potentially scrollable in at least one axis". §6 gives the
       setter no options dictionary, so §4's step 12 hands §3.1 its declared default `auto`. */
    if (t->is_root || (t->is_body && t->quirks && ev_not_potentially_scrollable_in_some_axis(t))) {
        s->x = vertical ? scroll_request_px(viewport_window_scroll(t->dctx, false)) : req;
        s->y = vertical ? req : scroll_request_px(viewport_window_scroll(t->dctx, true));
        return EV_SCROLL_TO_WINDOW;
    }
    /* Step 11 is "scroll the element to scrollLeft,y" for `scrollTop` and "to x,scrollTop" for `scrollLeft`:
       the axis this setter does not write comes from the element's own current position, through §2's
       internal-API rule. */
    s->x = vertical ? scroll_request_px(ev_scroll_position_px(t, false)) : req;
    s->y = vertical ? req : scroll_request_px(ev_scroll_position_px(t, true));
    return EV_SCROLL_TO_ELEMENT;
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

/* §6's `scroll()`, which `scrollTo()` is — "when the scrollTo() method is invoked, the user agent must act as
 * if the scroll() method was invoked with the same arguments" — and `scrollBy()`, the same steps with steps 3-4's
 * addition ahead of them (`ev_scroll_axis`), steps 1 to 9.
 *
 * The overload is already resolved and this reads that answer back off the converted argument count: §3.6
 * steps 3-4 decide `scroll` from the argument count alone (IDL_UNRESTRICTED_DOUBLE_OR_DICT), so two positions
 * are the numeric entry and one is the dictionary the declaration built, including for `el.scrollTo()`, whose
 * `optional ScrollToOptions options = {}` the machine materializes with every member at its default. */
static EvScrollToArm ev_scroll_steps(JSContext *ctx, const EvTarget *t, bool relative, int argc,
                                     JSValueConst *argv, EvScrollToState *s)
{
    JSValue left, top;

    /* Steps 1 and 2, whose only difference is where `left` and `top` come from: step 2's "let the left
       dictionary member of options have the value x" makes the two-argument form's arguments those members. */
    if (argc >= 2) {
        DCHECK(argc == 2, "§6's scroll members declare two positions and the conversion converts no more than a "
                          "member lists, so a body seeing a third is a declaration that grew without this");
        left = JS_DupValue(ctx, argv[0]);
        top  = JS_DupValue(ctx, argv[1]);
        ev_scroll_to_behavior(s, "auto");
    } else {
        JSValue behavior_v;
        const char *behavior;

        DCHECK(argc == 1 && JS_IsObject(argv[0]),
               "§6's scroll members reached their body at the dictionary arity with something that is not the "
               "engine-built options object — `optional ScrollToOptions options = {}` means an omitted argument "
               "is a dictionary carrying every member's default, which the argument machine materializes");
        left = idl_dict_get(ctx, argv[0], "left");
        top  = idl_dict_get(ctx, argv[0], "top");
        /* §6's step 11 scrolls with "the value of the behavior dictionary member of options"; its IDL
           default `"auto"` is placed by the conversion for a page that wrote none. */
        behavior_v = idl_dict_get(ctx, argv[0], "behavior");
        if (concolic_is(behavior_v))
            DFAIL("CSSOM VIEW §6's scroll members were handed unknown external input as `behavior`, a Web IDL "
                  "enumeration whose worlds are its three values, so none may be picked. Build the fork at "
                  "§3.2.18's conversion: core/idl_args.h's idl_concolic_rule answers IDL_CONCOLIC_FORKS for "
                  "IDL_ENUM, and `idl_enum_fork` is the ask this step machine can make");
        DCHECK(JS_IsString(behavior_v),
               "§4's ScrollToOptions declares `behavior` with the default \"auto\" and the conversion checks it "
               "against the ScrollBehavior enumeration, so the converted dictionary always carries a string");
        behavior = JS_ToCString(ctx, behavior_v);
        CHECK(behavior != NULL, "§4's ScrollBehavior keyword could not be read as a string");
        ev_scroll_to_behavior(s, behavior);
        JS_FreeCString(ctx, behavior);
        JS_FreeValue(ctx, behavior_v);
    }
    s->x = ev_scroll_axis(ctx, t, left, /*vertical*/ false, relative);
    s->y = ev_scroll_axis(ctx, t, top,  /*vertical*/ true,  relative);
    JS_FreeValue(ctx, left);
    JS_FreeValue(ctx, top);
    /* Steps 3 to 6 — the document is not the active document, or its window is null — are the one `dctx`. */
    if (!t->dctx) return EV_SCROLL_TO_RESOLVED;
    /* step 7 */
    if (t->is_root && t->quirks) return EV_SCROLL_TO_RESOLVED;
    /* Step 8 — "if the element is the root element, return the Promise returned by scroll() on window after the
       method is invoked with scrollX on window as first argument and y as second argument". The x this member
       was given is discarded, as the setter's step 8 words it; that is the spec's text, implemented as
       written. §2's internal-API rule makes this §4's algorithm and never the page-visible `window.scroll`. */
    if (t->is_root) {
        s->x = scroll_request_px(viewport_window_scroll(t->dctx, /*vertical*/ false));
        return EV_SCROLL_TO_WINDOW;
    }
    /* Step 9 — a quirks-mode body "not potentially scrollable in either axis" scrolls the window with options
       as the only argument. The condition is the either-axis one, not the setter's at-least-one-axis one. */
    if (t->is_body && t->quirks && ev_not_potentially_scrollable_in_either_axis(t)) return EV_SCROLL_TO_WINDOW;
    return EV_SCROLL_TO_ELEMENT;
}

/* The `scrollTop`/`scrollLeft` setter and `scroll()`/`scrollTo()`/`scrollBy()` as one step machine owning a
   layout job. EV_SCROLL_TO_TARGET asks the box tests' author matches under a resting drive (as EV_VIEW_STEP
   does), then runs steps 1 to 9 and step 10's first two disjuncts on the same job: "the element does not have
   any associated box" and "has no associated scrolling box" (css-overflow-3 §3.1's scroll container,
   core/layout/scroll_container.h). EV_SCROLL_TO_AREA measures that scrolling box, one rest unit of §2's
   scrolling-area walk per activation, for step 10's last disjunct, "the element has no overflow", and step 11's
   clamp, which read the one measurement. Steps 8 and 9's §4 `scroll()` on the window measures the viewport's
   box in the same stage and runs §4's steps 7 to 13 over it (core/frame/viewport.h). */
#define EV_SCROLL_TO_STAGES(X) \
    X(EV_SCROLL_TO_TARGET, "CSSOM VIEW §6 \"Extensions to the Element Interface\" scrollTop / scrollLeft setter " \
                           "and scroll() / scrollTo() / scrollBy() steps 1-10 (the box tests under a resting " \
                           "drive, then the steps on the same job)") \
    X(EV_SCROLL_TO_AREA, "CSSOM VIEW §6 scroll members step 10's overflow and step 11: CSSOM VIEW §6.1 " \
                         "\"Element Scrolling Members\" scroll an element to x,y, or steps 8-9's CSSOM VIEW §4 " \
                         "scroll() steps 7-13, over the scrolling box measured under a resting drive")
enum { IDL_STEP_STAGE_BASE(EV_SCROLL_TO_STAGES) EV_SCROLL_TO_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const EV_SCROLL_TO_STEPS[] = { EV_SCROLL_TO_STAGES(JS_STEP_STAGE_LABEL) NULL };

static void ev_scroll_to_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    EvScrollToState *s = st;

    v->tree(ctx, (void **)&s->job, NULL, 0, &layout_job_step_ops);
}

/* The member's completion: the setter returns undefined, and each scroll member's exits return a resolved
   Promise. */
static JSValue ev_scroll_to_result(JSContext *ctx, EvScrollKind kind)
{
    return kind == EV_SCROLL_SET_TOP || kind == EV_SCROLL_SET_LEFT ? JS_UNDEFINED : ev_resolved_promise(ctx);
}

static int js_ev_scroll_to_step(JSContext *ctx, JSStepHdr *hdr, void *st, int argc, JSValueConst *argv,
                                JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    EvScrollToState *s = st;
    EvScrollKind kind = (EvScrollKind)idl_step_magic(hdr);
    LayoutJobDrive d;
    bool more;
    int rc;

    (void)cb_result; (void)out_cb; (void)out_argc;
    STEP_DISPATCH(EV_SCROLL_TO_STAGES, hdr->stage, hdr->def->algorithm, JS_STEP_ABRUPT);

    STEP_ARM(EV_SCROLL_TO_TARGET);
    {
        lxb_dom_element_t *el = element_of_value(hdr->this_val);
        EvScrollToArm arm = EV_SCROLL_TO_RESOLVED;
        bool matched, measures = false, opened = false;
        EvTarget t;

        DCHECK(el != NULL, "a CSSOM VIEW §6 scroll member reached its body on a receiver that is not an Element "
                           "— the member states element_is at its declaration");
        if (s->job == NULL) s->job = layout_job_new();
        /* An entry the job's fork delivers to: its arm is recorded before the drive runs again. */
        rc = layout_job_fork(ctx, hdr, s->job);
        if (rc) return rc;
        rest_unit_begin(REST_UNIT_LAYOUT_DESCENDANTS);
        layout_job_enter(s->job);
        layout_job_drive_begin_resting(&d);
        matched = element_view_box_tests_ask(lxb_dom_interface_node(el));
        DCHECK(matched || layout_job_drive_spent(&d),
               "a CSSOM VIEW §6 scroll member's box tests parked an author match with the drive's allowance "
               "unspent, so its next activation would ask the same thing over the same state for ever");
        /* Steps 1 to 10 run inside the same drive, so the computed values they read are the job's held
           record's; a scrolling box whose layout defers parks the open, and the next activation repeats these
           steps, which read and write nothing but this state. */
        if (matched) {
            ev_target(hdr->this_val, &t);
            if (kind == EV_SCROLL_SET_TOP || kind == EV_SCROLL_SET_LEFT) {
                DCHECK(argc == 1, "an attribute setter is handed exactly the one converted value");
                arm = ev_set_steps(ctx, &t, kind == EV_SCROLL_SET_TOP, argv[0], s);
            } else {
                arm = ev_scroll_steps(ctx, &t, kind == EV_SCROLL_RELATIVE, argc, argv, s);
            }
            /* Steps 8 and 9 reach §4's scroll() on the window, whose step 4 aborts with no viewport and whose
               steps 5 to 13 run over the viewport's measured box. Otherwise step 10's first two disjuncts; the
               third is the measurement below. */
            if (arm == EV_SCROLL_TO_WINDOW) {
                measures = viewport_exists(t.dctx);
                if (measures) element_scrolling_measure_viewport(&s->m, t.dctx);
            } else {
                measures = arm == EV_SCROLL_TO_ELEMENT && t.has_box && scroll_container_is(el);
                while (measures && !opened && block_flow_drive_deferred(&d)) {
                    opened = element_scrolling_measure_element_ask(&s->m, el);
                    DCHECK(opened || layout_job_drive_spent(&d) || layout_job_drive_pending(&d),
                           "a scrolling box's open answered \"not yet\" with no question suspended and no "
                           "allowance spent, so its next activation would ask the same thing for ever");
                }
            }
        }
        layout_job_drive_end(&d);
        rest_unit_end();
        layout_job_leave(s->job);
        if (layout_job_declined(s->job)) {
            rc = layout_job_fork(ctx, hdr, s->job);
            return rc ? rc : JS_STEP_YIELD;
        }
        if (!matched || (measures && arm == EV_SCROLL_TO_ELEMENT && !opened)) {
            layout_job_rested(s->job);
            return JS_STEP_YIELD;
        }
        if (!measures) {
            *presult = ev_scroll_to_result(ctx, kind);
            return JS_STEP_DONE;
        }
        METRIC_ADD(scroll_area_asks, 1);
        hdr->stage = EV_SCROLL_TO_AREA;
    }

    STEP_ARM(EV_SCROLL_TO_AREA);
    rc = layout_job_fork(ctx, hdr, s->job);
    if (rc) return rc;
    layout_job_enter(s->job);
    more = element_scrolling_measure_next(&s->m);
    /* An element's box: §6's step 10, "or the element has no overflow", terminates, otherwise step 11. The
       window's box runs §4's steps 7 to 13. The requested position is resolved by the step that consumes it
       (core/dom/perform_scroll.h). */
    if (!more && s->m.box.el == NULL)
        viewport_scroll_measured(&s->m, s->x, s->y, s->behavior);
    else if (!more && element_scrolling_has_overflow(&s->m))
        element_scrolling_scroll_element(&s->m, scroll_request_resolve(s->x), scroll_request_resolve(s->y),
                                         s->behavior);
    layout_job_leave(s->job);
    if (layout_job_declined(s->job)) {
        rc = layout_job_fork(ctx, hdr, s->job);
        return rc ? rc : JS_STEP_YIELD;
    }
    if (more) {
        layout_job_rested(s->job);
        METRIC_ADD(scroll_area_rests, 1);
        return JS_STEP_YIELD;
    }
    *presult = ev_scroll_to_result(ctx, kind);
    return JS_STEP_DONE;
}

static const IdlStepDecl EV_SCROLL_TO_STEP = {
    js_ev_scroll_to_step, sizeof(EvScrollToState), ev_scroll_to_visit, NULL,
    "CSSOM VIEW §6 \"Extensions to the Element Interface\" scrollTop / scrollLeft setter and scroll() / "
    "scrollTo() / scrollBy()",
    EV_SCROLL_TO_STEPS
};


/* ---- §6's `scrollIntoView()` ------------------------------------------------------------------------------ */

/* One enumeration-valued member of §6's `scrollIntoView` argument, as the keyword its algorithm branches on.
 * Owned by the caller. §3.2.18's check has already run, so what arrives is a value the IDL lists or unknown
 * external input. An unknown keyword is a crash rather than a pass-through because `block` is an enumeration
 * whose worlds are its members: "start" and "end" align opposite edges, so picking either deletes a world.
 * The fork belongs at §3.2.18's conversion (core/idl_args.h's `idl_concolic_rule` answers IDL_CONCOLIC_FORKS
 * for IDL_ENUM; `idl_enum_fork` is the ask), which this member, now a step machine, can make. */
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
              "resolution site: this member is a step machine, so hand its `JSStepHdr` to this function and "
              "ask `step_fork_run` over the enumeration's values with outcome 0 the IDL's own "
              "DEFAULT — which is step_fork_run's one numbering rule, since outcome 0 is what a run with no "
              "forking policy takes. core/dom/abort.c states the same conversion for the same reason");
    DCHECK(JS_IsString(v),
           "§6's `scrollIntoView` read a dictionary member declared as a Web IDL ENUMERATION and found neither "
           "a string nor unknown external input — §3.2.18's conversion is part of the TYPE and rejects every "
           "other value with a TypeError before this body runs, and each of the three members declares a "
           "default, so an absent one arrives carrying that default rather than as undefined");
    s = JS_ToCString(ctx, v);
    CHECK(s != NULL, "a Web IDL enumeration value that is already a string could not be read as one");
    owned = reclaim_strdup(s);
    CHECK(owned != NULL, "a Web IDL enumeration keyword could not be copied");
    JS_FreeCString(ctx, s);
    (void)member;
    return owned;
}

/* §6's `scrollIntoView(arg)`, as a step machine that drives one layout job (core/layout/layout_job.h).
 *
 * Steps 1 to 4 are the four defaults the IDL also writes (`"auto"`, `"start"`, `"nearest"`, a null
 * container), so they are the initialisers below. Step 7 is the common termination: "if the element does not
 * have any associated box, or is not available to user-agent features, then return a resolved Promise and
 * abort the remaining steps". Its second disjunct is a property of skipped contents (css-contain-2 §4
 * "Suppressing An Element's Contents Entirely: the content-visibility property"), and this cascade carries no
 * `content-visibility`, so nothing is skipped; a DCHECK fires the day the property enters the cascade.
 * Step 9 is optional in the spec's own word and there is no user to attend. Step 10's promise is resolved
 * for core/dom/element_scrolling.h's reason: every scroll here is an instant one. */

/* Step 8 is §6.1's ancestor walk, one cursor unit per re-entry of EV_SIV_BOX: opening a scrolling box, a
 * run of REST_UNIT_LAYOUT_DESCENDANTS nodes of its scrolling area (cut short where a node's derivation rests
 * mid-node with its questions suspended on the job), or steps 2.2 to 2.4 over it
 * (core/dom/element_scrolling.h). Every layout question is asked through the job this state owns, so its
 * memo and placement record serve every later unit, and the machine rests between units with the job alive:
 * the flow is parked inside its own member call, where it writes nothing, so both stay valid (layout_job.h). */
#define EV_SIV_STAGES(X) \
    X(EV_SIV_ARGS, "CSSOM VIEW §6 \"Extensions to the Element Interface\" scrollIntoView(arg) steps 1-6 (the " \
                   "four defaults and the argument)") \
    X(EV_SIV_TARGET, "CSSOM VIEW §6 \"Extensions to the Element Interface\" scrollIntoView(arg) step 7 (the " \
                     "termination for an element with no box, its box tests' author matches asked under a " \
                     "resting drive)") \
    X(EV_SIV_BOX,  "CSSOM VIEW §6 \"Extensions to the Element Interface\" scrollIntoView(arg) step 8 (scroll a " \
                   "target into view: CSSOM VIEW §6.1 \"Element Scrolling Members\" step 2, one unit of its " \
                   "ancestor walk)")
enum { IDL_STEP_STAGE_BASE(EV_SIV_STAGES) EV_SIV_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const EV_SIV_STEPS[] = { EV_SIV_STAGES(JS_STEP_STAGE_LABEL) NULL };

typedef struct {
    ElementScrollingIntoView walk;   /* the ancestor walk's cursor: DOM nodes, which a fork shares by address */
    LayoutJob *job;                  /* owned, declared through layout_job_step_ops; NULL until step 7 */
    char       behavior[16];         /* §4's ScrollBehavior keyword, one of the three the enumeration admits */
    ScrollLogicalPosition block, inline_pos;   /* steps 1-6's answers, which step 8 begins the walk with */
    lxb_dom_element_t *container;    /* step 4's, a DOM node a fork shares by address; NULL for none */
} EvScrollIntoViewState;

static void ev_siv_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    EvScrollIntoViewState *s = st;

    v->tree(ctx, (void **)&s->job, NULL, 0, &layout_job_step_ops);
}

static int js_ev_scroll_into_view_step(JSContext *ctx, JSStepHdr *hdr, void *st, int argc, JSValueConst *argv,
                                       JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    EvScrollIntoViewState *s = st;
    bool more;
    int rc;

    (void)cb_result; (void)out_cb; (void)out_argc;
    STEP_DISPATCH(EV_SIV_STAGES, hdr->stage, hdr->def->algorithm, JS_STEP_ABRUPT);

    STEP_ARM(EV_SIV_ARGS);
    {
        ScrollLogicalPosition block = SCROLL_LOGICAL_START, inline_pos = SCROLL_LOGICAL_NEAREST;
        lxb_dom_element_t *container = NULL;   /* step 4 */
        const char *behavior = "auto";         /* step 1 */
        char *owned = NULL;

        DCHECK(argc == 1,
               "§6 declares `scrollIntoView(optional (boolean or ScrollIntoViewOptions) arg = {})` — one "
               "argument, with the default materialized by the argument machine — so a body seeing any other "
               "count is a declaration that grew without this");
        /* Step 5 against step 6, decided by which arm of `(boolean or ScrollIntoViewOptions)` the
           conversion took: §3.2.25 converts V to exactly one of the two, and IDL_BOOL_OR_DICT places it. */
        if (JS_IsBool(argv[0])) {
            /* Step 6: "Otherwise, if arg is false, then set block to "end"". An omitted argument never
               reaches here: §3.2.25 step 4 sends undefined and null to the dictionary arm. */
            if (!JS_ToBool(ctx, argv[0])) block = SCROLL_LOGICAL_END;
        } else if (concolic_is(argv[0])) {
            DFAIL("CSSOM VIEW §6's `scrollIntoView` reached its body with UNKNOWN EXTERNAL INPUT that the §3.2.25 "
                  "union placed on the BOOLEAN arm, and step 6 reads that boolean: `false` sets `block` to "
                  "\"end\" while `true` leaves it at \"start\", which are opposite alignments and two different "
                  "scroll positions. The union's ARM was already forked by the conversion (IDL_BOOL_OR_DICT is "
                  "IDL_CONCOLIC_FORKS); what is missing is the fork over the boolean's own VALUE, which belongs "
                  "at this stage: ask `step_fork_run` over two outcomes, outcome 0 being `true` because that is "
                  "the arm a run with no forking policy takes and it leaves every default in place");
        } else {
            JSValue v;

            DCHECK(JS_IsObject(argv[0]),
                   "§6's `scrollIntoView` reached the DICTIONARY arm of `(boolean or ScrollIntoViewOptions)` "
                   "with something that is neither the engine-built options object nor unknown external input "
                   "— `optional … arg = {}` means an omitted argument IS a dictionary carrying every member's "
                   "default, which the argument machine materializes");
            v = idl_dict_get(ctx, argv[0], "behavior");                         /* step 5.1 */
            owned = ev_scroll_keyword(ctx, v, "behavior");
            behavior = owned;
            JS_FreeValue(ctx, v);
            v = idl_dict_get(ctx, argv[0], "block");                            /* step 5.2 */
            { char *k = ev_scroll_keyword(ctx, v, "block");
              block = element_scrolling_logical_position(k); free(k); }
            JS_FreeValue(ctx, v);
            v = idl_dict_get(ctx, argv[0], "inline");                           /* step 5.3 */
            { char *k = ev_scroll_keyword(ctx, v, "inline");
              inline_pos = element_scrolling_logical_position(k); free(k); }
            JS_FreeValue(ctx, v);
            /* Step 5.4: "nearest" sets container to the element itself, because §6.1's step 2.4 stops the
               walk once the scrolling box is a shadow-including inclusive ancestor of container, and the
               first ancestor scrolling box already is one. */
            v = idl_dict_get(ctx, argv[0], "container");
            { char *k = ev_scroll_keyword(ctx, v, "container");

              DCHECK(strcmp(k, "all") == 0 || strcmp(k, "nearest") == 0,
                     "§6's `enum ScrollIntoViewContainer { \"all\", \"nearest\" }` admits two values and the "
                     "declaration's §3.2.18 check rejects every other, so a third here is the declaration and "
                     "this read having come apart");
              if (strcmp(k, "nearest") == 0) container = element_of_value(hdr->this_val);
              free(k); }
            JS_FreeValue(ctx, v);
        }
        DCHECKF(strlen(behavior) < sizeof s->behavior,
                "§4's ScrollBehavior keyword `%s` does not fit the state's copy — the enumeration's three values "
                "are what §3.2.18 admits, and each is shorter than the copy", behavior);
        strcpy(s->behavior, behavior);
        free(owned);
        s->block = block;
        s->inline_pos = inline_pos;
        s->container = container;
        s->job = layout_job_new();
        hdr->stage = EV_SIV_TARGET;
    }

    STEP_ARM(EV_SIV_TARGET);
    {
        LayoutJobDrive d;
        bool matched;
        EvTarget t;
        int rc;

        /* An entry the job's fork delivers to: its arm is recorded before the drive runs again. */
        rc = layout_job_fork(ctx, hdr, s->job);
        if (rc) return rc;
        /* Step 7 */
        DCHECK(!css_computed_models("content-visibility"),
               "css-contain-2's `content-visibility` is in this engine's cascade now, so CSSOM VIEW §6's "
               "`scrollIntoView` step 7 has a disjunct this member does not decide: \"or IS NOT AVAILABLE TO "
               "USER-AGENT FEATURES\", which css-contain-2 §4 \"Suppressing An Element's Contents Entirely: the "
               "content-visibility property\" states over SKIPPED CONTENTS. BUILD the flat-tree walk over the "
               "computed value and terminate here for a skipped element; §6's `checkVisibility` wants the same "
               "walk");
        rest_unit_begin(REST_UNIT_LAYOUT_DESCENDANTS);
        layout_job_enter(s->job);
        layout_job_drive_begin_resting(&d);
        matched = element_view_box_tests_ask(lxb_dom_interface_node(element_of_value(hdr->this_val)));
        DCHECK(matched || layout_job_drive_spent(&d),
               "CSSOM VIEW §6's `scrollIntoView` box tests parked an author match with the drive's allowance "
               "unspent, so the next activation would ask the same thing over the same state for ever");
        /* Read inside the job, so the box tests' cascade is held by the job's records for step 8's walk, and
           the walk is begun there too: its entry asserts the target's box over the same records. */
        if (matched) ev_target(hdr->this_val, &t);
        if (matched && t.has_box)
            element_scrolling_into_view_begin(&s->walk, lxb_dom_interface_element(t.node), s->block, s->inline_pos,
                                              s->container);
        layout_job_drive_end(&d);
        layout_job_leave(s->job);
        rest_unit_end();
        if (layout_job_declined(s->job)) {
            rc = layout_job_fork(ctx, hdr, s->job);
            return rc ? rc : JS_STEP_YIELD;
        }
        if (!matched) {
            layout_job_rested(s->job);
            return JS_STEP_YIELD;
        }
        if (!t.has_box) {
            *presult = ev_resolved_promise(ctx);
            return JS_STEP_DONE;
        }
        hdr->stage = EV_SIV_BOX;
    }

    STEP_ARM(EV_SIV_BOX);
    rc = layout_job_fork(ctx, hdr, s->job);
    if (rc) return rc;
    layout_job_enter(s->job);
    more = element_scrolling_into_view_next(&s->walk, s->behavior);
    layout_job_leave(s->job);
    if (layout_job_declined(s->job)) {
        rc = layout_job_fork(ctx, hdr, s->job);
        return rc ? rc : JS_STEP_YIELD;
    }
    if (more) {
        layout_job_rested(s->job);
        return JS_STEP_YIELD;
    }
    /* Steps 9 and 10 */
    *presult = ev_resolved_promise(ctx);
    return JS_STEP_DONE;
}

static const IdlStepDecl EV_SCROLL_INTO_VIEW_STEP = {
    js_ev_scroll_into_view_step, sizeof(EvScrollIntoViewState), ev_siv_visit, NULL,
    "CSSOM VIEW §6 \"Extensions to the Element Interface\" scrollIntoView(arg), over CSSOM VIEW §6.1 \"Element "
    "Scrolling Members\" scroll a target into view",
    EV_SIV_STEPS
};

/* ---- §6's `scrollWidth` and `scrollHeight` --------------------------------------------------------------- */

/* Which scrolling area §6's `scrollWidth`/`scrollHeight` returns at, once steps 2 to 6 have not answered
   from the viewport's size alone. */
typedef enum {
    EV_EXTENT_ANSWERED,   /* a step answered without walking a scrolling area */
    EV_EXTENT_VIEWPORT,   /* steps 4 and 5: max(the viewport's scrolling area, the viewport) */
    EV_EXTENT_ELEMENT,    /* step 7: the element's scrolling area */
} EvExtentArea;

/* Steps 2 to 7 up to the scrolling area each of steps 4, 5 and 7 walks: `*out` is the answer for
   EV_EXTENT_ANSWERED, and `*vp` is the viewport's own extent, the other operand of steps 4 and 5's max, for
   EV_EXTENT_VIEWPORT. */
static EvExtentArea ev_scroll_extent_early(JSContext *ctx, const EvTarget *t, bool vertical, JSValue *out,
                                           double *vp)
{
    bool presented;

    /* step 2 */
    if (!t->dctx) { *out = ev_long(ctx, 0.0); return EV_EXTENT_ANSWERED; }
    /* step 3: "the width of the viewport EXCLUDING the width of the scroll bar, if any, or zero if there is no
       viewport". This user agent renders no scroll bar, so the exclusion removes nothing. */
    presented = viewport_exists(t->dctx);
    *vp = !presented ? 0.0 : (vertical ? viewport_height(t->dctx) : viewport_width(t->dctx));
    /* Steps 4 and 5 return max(viewport scrolling area width, viewport width), or zero with no viewport. Step
       5's condition is per axis: "not potentially scrollable in the x axis" for `scrollWidth` and "in the y
       axis" for `scrollHeight`. */
    if ((t->is_root && !t->quirks) || (t->is_body && t->quirks && !ev_potentially_scrollable(t, vertical))) {
        if (!presented) { *out = ev_long(ctx, 0.0); return EV_EXTENT_ANSWERED; }
        return EV_EXTENT_VIEWPORT;
    }
    /* step 6 */
    if (!t->has_box) { *out = ev_long(ctx, 0.0); return EV_EXTENT_ANSWERED; }
    /* Step 7 — "return the width of the element's scrolling area": a right-most position over this element's
       padding edge and its descendants' boxes (core/layout/scrolling_area.h), not the padding edge's extent
       `clientWidth` reports. The two differ exactly when the element's content overflows it. */
    return EV_EXTENT_ELEMENT;
}

/* ---- §6's `clientWidth`, `clientHeight`, `clientTop` and `clientLeft` ------------------------------------- */

/* Steps 1 and 2, which read no layout: true with `*out` answered, false for step 3's padding edge. */
static bool ev_client_extent_direct(JSContext *ctx, const EvTarget *t, bool vertical, JSValue *out)
{
    /* Step 1: "If the element has no associated box OR IF THE BOX IS INLINE, return zero." Both halves are now
       decided from the computed `display` — the second one used to be waved through on the grounds that the two
       elements step 2 answers for can never be inline (CSS Display §2.7 blockifies the root, and §15.3.1 gives
       `body` `display: block`), which said nothing about the elements step 3 reaches. `span.clientWidth` is 0
       in every user agent, and this is where that is decided: an inline box has a padding edge in the box model
       and §6 declines to report it, so step 3 below must never be asked about one. */
    if (!t->has_box || ev_box_is_inline(t)) {
        *out = ev_long(ctx, 0.0);
        return true;
    }
    /* Step 2 — the ROOT ELEMENT outside quirks mode, and the BODY inside it, are answered with the VIEWPORT and
       never with their own box. `document.documentElement.clientWidth` is how most of the web asks how wide the
       viewport is. "Excluding the size of a rendered scroll bar (if any)" removes nothing: none is rendered. */
    if ((t->is_root && !t->quirks) || (t->is_body && t->quirks)) {
        DCHECK(t->dctx && viewport_exists(t->dctx),
               "a CSSOM VIEW §6 client extent reached its viewport branch for an element whose document is not "
               "being presented — step 1 admits only an element with an associated box, which is defined over "
               "exactly that");
        *out = ev_env_long(ctx, t, vertical ? "clientHeight" : "clientWidth",
                           vertical ? viewport_height(t->dctx) : viewport_width(t->dctx));
        return true;
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
       are that component's own subproblem and not this one's, and an ABSOLUTELY POSITIONED box is no longer
       among them: CSS 2.1 §10.3.7's constraint equation is built over the static position §9.4.1's own walk
       reads out, so this member answers a real number for one. What still crashes there is a box whose
       content this engine cannot measure, inside the intrinsic walk, naming what it met. */
    return false;
}

/* Every step is the step machine's, asked under a resting drive on the job the state owns: step 1's box test
   reads the author matches of the element and its ancestors, which rest between rules (element_view_box_tests_ask), and
   step 3's padding edge is laid out with its questions suspended across rests, as `scrollIntoView` does. */
#define EV_CLIENT_STAGES(X) \
    X(EV_CLIENT_MEASURE, "CSSOM VIEW §6 \"Extensions to the Element Interface\" clientWidth / clientHeight " \
                         "(steps 1-2, then step 3's padding edge, one resting drive per activation)")
enum { IDL_STEP_STAGE_BASE(EV_CLIENT_STAGES) EV_CLIENT_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const EV_CLIENT_STEPS[] = { EV_CLIENT_STAGES(JS_STEP_STAGE_LABEL) NULL };

typedef struct {
    LayoutJob *job;       /* owned, declared through layout_job_step_ops; NULL before the first activation */
    bool       measures;  /* steps 1 and 2 did not answer, so step 3 is being asked */
} EvClientState;

static void ev_client_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    EvClientState *s = st;

    v->tree(ctx, (void **)&s->job, NULL, 0, &layout_job_step_ops);
}

static int js_ev_client_step(JSContext *ctx, JSStepHdr *hdr, void *st, int argc, JSValueConst *argv,
                             JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    EvClientState *s = st;
    bool vertical = idl_step_magic(hdr) == EV_CLIENT_HEIGHT, done = false, direct = false;
    lxb_dom_element_t *el = element_of_value(hdr->this_val);
    LayoutJobDrive d;
    EvTarget t;
    CssPx px = css_px(0.0);
    int rc;

    (void)argc; (void)argv; (void)cb_result; (void)out_cb; (void)out_argc;
    STEP_DISPATCH(EV_CLIENT_STAGES, hdr->stage, hdr->def->algorithm, JS_STEP_ABRUPT);

    STEP_ARM(EV_CLIENT_MEASURE);
    DCHECK(el != NULL, "CSSOM VIEW §6's client extent reached its body on a receiver that is not an Element — the "
                       "member states element_is at its install");
    if (s->job == NULL) s->job = layout_job_new();
    /* An entry the job's fork delivers to: its arm is recorded before the drive runs again. */
    rc = layout_job_fork(ctx, hdr, s->job);
    if (rc) return rc;
    /* One activation is one unit, measured against the slice like each of `scrollIntoView`'s. */
    rest_unit_begin(REST_UNIT_LAYOUT_DESCENDANTS);
    layout_job_enter(s->job);
    layout_job_drive_begin_resting(&d);
    if (!element_view_box_tests_ask(lxb_dom_interface_node(el))) {
        DCHECK(layout_job_drive_spent(&d),
               "CSSOM VIEW §6's client extent's box tests parked an author match with the drive's allowance "
               "unspent, so its next activation would ask the same thing over the same state for ever");
    } else {
        /* Read inside the job, so the box tests' cascade is held by the job's records across activations. */
        ev_target(hdr->this_val, &t);
        direct = ev_client_extent_direct(ctx, &t, vertical, presult);
        if (!direct && !s->measures) {
            s->measures = true;
            METRIC_ADD(client_extent_asks, 1);
        }
    }
    while (s->measures && block_flow_drive_deferred(&d)) {
        if (used_value_padding_edge_ask(el, vertical, &px)) {
            done = true;
            break;
        }
        DCHECK(layout_job_drive_spent(&d) || layout_job_drive_pending(&d),
               "CSSOM VIEW §6's client extent answered \"not yet\" with no question suspended and no allowance "
               "spent, so its next activation would ask the same thing over the same state for ever");
    }
    layout_job_drive_end(&d);
    layout_job_leave(s->job);
    rest_unit_end();
    if (direct) return JS_STEP_DONE;
    if (layout_job_declined(s->job)) {
        rc = layout_job_fork(ctx, hdr, s->job);
        return rc ? rc : JS_STEP_YIELD;
    }
    if (!done) {
        layout_job_rested(s->job);
        METRIC_ADD(client_extent_rests, 1);
        return JS_STEP_YIELD;
    }
    *presult = ev_length_long(ctx, px);
    return JS_STEP_DONE;
}

static const IdlStepDecl EV_CLIENT_STEP = {
    js_ev_client_step, sizeof(EvClientState), ev_client_visit, NULL,
    "CSSOM VIEW §6 \"Extensions to the Element Interface\" clientWidth / clientHeight",
    EV_CLIENT_STEPS
};
static int g_id_client_width = -1, g_id_client_height = -1;

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

/* A matrix entry that is the concrete number `k` in every world. */
static bool ev_px_is(CssPx v, double k)
{
    return v.env == CSS_ENV_NONE && v.px == k;
}

/* §6's getClientRects() step 3's FIRST CONSTRAINT, PERFORMED — "Apply the transforms that apply to the element
   and its ancestors." css-transforms-1 §2 "The Transform Rendering Model" is the algorithm that constraint
   names without naming, and that section says so in its own words: "Transforms are also taken into account when
   computing client rectangles exposed via the Element Interface Extensions, namely getClientRects() and
   getBoundingClientRect(), which are specified in [CSSOM-VIEW]".
   THE RECTANGLE ARRIVES AND LEAVES IN THE INITIAL CONTAINING BLOCK'S SPACE, which is the whole reason this is a
   step of its own rather than four lines at each assembly arm. §2's current transformation matrix is composed
   "starting from the viewport coordinate system", so it maps a DOCUMENT coordinate — and the conversion to a
   CLIENT one is CSSOM VIEW §4's scroll subtraction, which must happen AFTER. The two used to be one expression
   at each arm and the order was invisible: a translation commutes with the subtraction so either order answers
   the same number, and a rotation does not, so the arm that is correct today would have gone silently wrong the
   day the matrix gained a linear part.
   A DOMRect IS AXIS-ALIGNED — Geometry Interfaces §3 gives it an `x`, a `y`, a `width` and a `height` and no
   angle — so what §6 can report of a mapped border area is the SMALLEST RECTANGLE ENCLOSING ITS FOUR MAPPED
   CORNERS, which is why a rotated box reports a rectangle wider than its own width in every user agent. The
   four comparisons go through core/css/css_length.h's `css_px_min`/`css_px_max` rather than through `<` on the
   examples, for that component's own stated reason: the loser at this viewport is the winner at another, so the
   answer carries BOTH operands' environment facts and a C branch on the example would delete an arm. */
static void ev_map_through_transforms(lxb_dom_element_t *el, CssPx *x, CssPx *y, CssPx *w, CssPx *h)
{
    CssTransformMatrix ctm = css_transform_matrix_current(el);
    CssPx cx[4], cy[4], min_x, min_y, max_x, max_y;
    unsigned i;

    /* An affine axis-aligned matrix that flips neither axis (the identity, a translation, a positive scale)
       maps the origin to the enclosing rectangle's corner and scales each extent, which is that rectangle exactly
       without `(y + h) - y`, a difference IEEE arithmetic need not return as `h`. */
    if (ev_px_is(ctm.b, 0.0) && ev_px_is(ctm.c, 0.0) && ev_px_is(ctm.p, 0.0) && ev_px_is(ctm.q, 0.0) &&
        ev_px_is(ctm.s, 1.0) && ctm.a.env == CSS_ENV_NONE && ctm.d.env == CSS_ENV_NONE && ctm.a.px >= 0.0 &&
        ctm.d.px >= 0.0 && w->px >= 0.0 && h->px >= 0.0) {
        css_transform_matrix_map(ctm, *x, *y, x, y);
        *w = css_px_scale(*w, ctm.a.px);
        *h = css_px_scale(*h, ctm.d.px);
        return;
    }
    css_transform_matrix_map(ctm, *x, *y, &cx[0], &cy[0]);
    css_transform_matrix_map(ctm, css_px_add(*x, *w), *y, &cx[1], &cy[1]);
    css_transform_matrix_map(ctm, css_px_add(*x, *w), css_px_add(*y, *h), &cx[2], &cy[2]);
    css_transform_matrix_map(ctm, *x, css_px_add(*y, *h), &cx[3], &cy[3]);
    min_x = max_x = cx[0];
    min_y = max_y = cy[0];
    for (i = 1; i < 4; i++) {
        min_x = css_px_min(min_x, cx[i]);
        max_x = css_px_max(max_x, cx[i]);
        min_y = css_px_min(min_y, cy[i]);
        max_y = css_px_max(max_y, cy[i]);
    }
    *x = min_x;
    *y = min_y;
    *w = css_px_sub(max_x, min_x);
    *h = css_px_sub(max_y, min_y);
}

/* §6's step 3 for one box fragment — "a DOMRect describing its border area": CSS 2 §8.1's border box, its two
   extents (core/layout/used_value.h) at its position (core/layout/flow_position.h), in the initial containing
   block's space. An inline box's border area is not an extent at a position (CSS 2 §10.3.1 and §10.6.1 say
   `width` and `height` do not apply), so its fragments come from `flow_inline_fragment_rects` instead. */
static void ev_border_area_px(lxb_dom_element_t *el, CssPx out[4])
{
    ElementViewFragments kind = element_view_fragment_kind(el);
    /* A table element's border area is its TABLE box's (`ev_table_fragments`), which is not at the element's
       origin when a caption is stacked above it. */
    FlowPoint o = kind == ELEMENT_VIEW_FRAGMENTS_TABLE ? flow_table_box_origin(el) : flow_border_box_origin(el);

    DCHECK(kind != ELEMENT_VIEW_FRAGMENTS_LINE_BOXES,
           "a border area was assembled as an extent at a position for an inline box, whose rectangles are its "
           "line fragments (`flow_inline_fragment_rects`)");
    out[0] = o.x;
    out[1] = o.y;
    out[2] = used_value_border_edge_px(el, false);
    out[3] = used_value_border_edge_px(el, true);
}

/* One rectangle of `el`'s from the initial containing block's space to client coordinates: §6's step 3's first
   constraint, css-transforms-1 §2's current transformation matrix, maps it ("starting from the viewport
   coordinate system", so before the conversion: a rotation does not commute with it), then CSSOM View §4's
   scroll position is subtracted. Applied to an element nothing transforms, the matrix is the identity. */
static void ev_client_rect_px(const EvTarget *t, lxb_dom_element_t *el, const CssPx in[4], CssPx out[4])
{
    CssPx x = in[0], y = in[1], w = in[2], h = in[3];

    ev_map_through_transforms(el, &x, &y, &w, &h);
    out[0] = css_px_sub(x, css_px(viewport_window_scroll(t->dctx, false)));
    out[1] = css_px_sub(y, css_px(viewport_window_scroll(t->dctx, true)));
    out[2] = w;
    out[3] = h;
}

/* §6's step 2's own question — "if the element HAS AN ASSOCIATED SVG LAYOUT BOX". The OUTERMOST `svg` element
   does not: it is a replaced element the CSS box model lays out, which is why every user agent answers its
   `getBoundingClientRect` out of a CSS border box. Its descendants do, and they have no margins, borders or
   paddings for §8.1's box model to be stated over. So the question is whether the element is in the SVG
   namespace UNDER another element that is. */
static bool ev_has_svg_layout_box(const lxb_dom_node_t *n)
{
    const lxb_dom_node_t *p = flat_tree_parent(n);

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
    CssDisplay d = css_computed_display(el);
    bool inl, table;

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
    inl = css_display_spells(d, CSS_KEYWORD_INLINE) && !replaced_element_of(el).replaced;
    table = css_display_spells(d, CSS_KEYWORD_TABLE) || css_display_spells(d, CSS_KEYWORD_INLINE_TABLE);
    if (inl) return ELEMENT_VIEW_FRAGMENTS_LINE_BOXES;
    if (table) return ELEMENT_VIEW_FRAGMENTS_TABLE;
    return ELEMENT_VIEW_FRAGMENTS_ONE;
}

/* §6's step 3's SECOND CONSTRAINT, PERFORMED — "if the element on which the method was invoked has a computed
   value for the display property of table or inline-table include both the table box and the caption box, if any,
   but not the anonymous container box". THE INLINE ARM IS GONE FROM HERE BECAUSE IT IS BUILT and the TABLE ARM IS
   GONE BECAUSE THIS IS IT: the crash that stood here ended "Emit that LIST here, the way the line-box arm above
   emits one rectangle per fragment", and the list is what this answers.
   IT IS A LIST OF ELEMENTS AND NOT OF RECTANGLES, because every box on it is one an element generates and the
   border area of each is the SAME derivation `ev_border_area_px` performs for any other box — one derivation over
   N boxes rather than a second assembly free to disagree with the first about what a border area is.
   THE ANONYMOUS CONTAINER THE CONSTRAINT EXCLUDES IS CSS 2.1 §17.4 "Tables in the visual formatting model"'s
   TABLE WRAPPER BOX — "the table generates a principal block box called the table wrapper box that contains the
   table box itself and any caption boxes (in document order)" — and excluding it costs nothing here rather than
   being a step performed: NO ELEMENT IN THE TREE NAMES THAT BOX (core/layout/table_wrapper.h says so in its own
   words), so a list keyed on elements cannot contain it.
   THE TABLE ELEMENT STANDS FOR THE TABLE BOX AND NOT FOR THE WRAPPER, which is CSS 2.1 §17.4's own declaration
   split and not a choice made here: "The computed values of properties 'position', 'float', 'margin-*', 'top',
   'right', 'bottom', and 'left' on the table element are used on the table wrapper box and not the table box; all
   other values of non-inheritable properties are used on the table box and not the table wrapper box." So the
   BORDER and PADDING `ev_border_area_px` reads off the table element are the TABLE BOX's, and its two extents are
   CSS 2.1 §17.5.2 "Table width algorithms: the 'table-layout' property"'s and CSS 2.1 §17.5.3 "Table height
   algorithms"' through core/layout/used_value.h — so the rectangle that comes back is the box this constraint
   names and not the one it excludes. core/layout/flow_position.c asserts the same split from the other side, over
   the same property list.
   A CAPTION NEEDS NO ENTRY OF ITS OWN EITHER, for CSS 2.1 §17.4's own reason: "The caption boxes are block-level
   boxes that retain their own content, padding, margin, and border areas, and are rendered as normal block boxes
   inside the table wrapper box", so the ordinary block-box derivation is the right one for it. The table box's
   own position is `flow_table_box_origin`, below the captions on the wrapper's stack.
   NAMED RESIDUAL — THE ORDER. Step 3 wants its rectangles "in content order" and CSSOM VIEW defines no such term;
   the only ordering the box structure carries is CSS 2.1 §17.4's own "(in document order)" over the WRAPPER's
   children, and a `<caption>` element may stand before or after the table element's internal boxes. WHAT IS NOT
   COVERED: this list is the table box followed by the caption boxes in document order, which is that order
   exactly when no caption precedes the first internal box and is narrower than step 3 otherwise. WHAT THE NEXT
   DIFF BUILDS: an entry beside core/layout/table_box.h's `table_box_captions` reporting the WRAPPER's CHILD BOXES
   in document order — the same list core/layout/block_flow.c's CSS 2.1 §9.4.1 stack needs, so it is one build
   with two consumers rather than a child scan written here, which would be a second copy of CSS 2.1 §17.2.1
   "Anonymous table objects"' classification. HOW ITS ABSENCE WOULD SHOW: `getClientRects()` on a `table` whose
   first child generates a caption box answers with that caption's rectangle SECOND where a browser answers with
   it first — an ordering visible in the returned DOMRectList and in nothing else.
   Answers the count and stores a newly allocated array of that many ELEMENTS at `*out`, which the caller owns and
   frees. The count is never zero — the table box is always on it. */
static size_t ev_table_fragments(lxb_dom_element_t *el, lxb_dom_element_t ***out)
{
    lxb_dom_element_t **caps = NULL, **frags;
    size_t ncaps, i;

    DCHECK(element_view_fragment_kind(el) == ELEMENT_VIEW_FRAGMENTS_TABLE,
           "§6's getClientRects() step 3's second constraint was asked for the fragments of an element whose "
           "computed `display` is neither `table` nor `inline-table` — the constraint is written over exactly "
           "those two values, and no other box structure in this model has a caption box to include");
    DCHECK(out != NULL,
           "§6's getClientRects() step 3's second constraint was asked with nowhere to put its boxes — a count "
           "alone names no box for the list to measure");
    ncaps = table_box_captions(el, &caps);
    frags = (lxb_dom_element_t **) lexbor_malloc((ncaps + 1) * sizeof *frags);
    CHECK(frags != NULL, "§6's getClientRects() step 3 could not allocate its fragment list");
    frags[0] = el;                      /* the TABLE BOX, which the split above makes this element's box */
    for (i = 0; i < ncaps; i++) frags[i + 1] = caps[i];
    free(caps);
    *out = frags;
    return ncaps + 1;
}

/* §6's getClientRects() steps 1 to 3, in css_length.h's vocabulary: stores a newly allocated array of client
   rectangles (x, y, width, height) at `*out`, which the caller frees, and answers its count. It is the internal
   algorithm (CSSOM View §2: a member "said to call another method" invokes the algorithm), so a page that
   overwrites `Element.prototype.getClientRects` cannot change what `getBoundingClientRect()` measures.
   Step 1 is a derivation: an element with no associated box (disconnected, in a document nothing presents, or
   `display: none`) generates no fragments in any user agent. Step 3's third constraint ("replace each anonymous
   block box with its child box(es)") never fires, because an element's own principal box is never anonymous. */
static size_t ev_client_rects_px(const EvTarget *t, CssPx (**out)[4])
{
    lxb_dom_element_t *el = lxb_dom_interface_element(t->node);
    ElementViewFragments kind;
    CssPx (*r)[4];
    CssPx b[4];
    size_t n, i;

    *out = NULL;
    /* step 1 */
    if (!t->has_box) return 0;
    /* step 2 */
    if (ev_has_svg_layout_box(t->node))
        DFAIL("CSSOM VIEW §6's getClientRects() step 2: an element with an associated SVG layout box answers "
              "with a single DOMRect describing \"the bounding box of the element as defined by the SVG "
              "specification\", SVG 2's object bounding box, which this engine does not lay out. Build it beside "
              "the CSS border box, in its own component");
    kind = element_view_fragment_kind(el);
    /* Step 3's count, "one for each box fragment", "in content order". An inline box has one per line box it
       spans, which `flow_inline_fragment_rects` reports in fill order: CSS 2 §9.4.2 distributes the run in
       document order and stacks the lines downward. */
    if (kind == ELEMENT_VIEW_FRAGMENTS_LINE_BOXES) {
        FlowRect *frags = NULL;

        n = flow_inline_fragment_rects(el, &frags);
        DCHECK(n > 0, "an inline box with a box reported no line fragment; flow_position.h answers its origin "
                      "from the first");
        r = lexbor_malloc(n * sizeof *r);
        CHECK(r != NULL, "§6's getClientRects() step 3 could not allocate its rectangle list");
        for (i = 0; i < n; i++) {
            b[0] = frags[i].x;
            b[1] = frags[i].y;
            b[2] = frags[i].width;
            b[3] = frags[i].height;
            ev_client_rect_px(t, el, b, r[i]);
        }
        free(frags);
        *out = r;
        return n;
    }
    /* Step 3's second constraint: the table box and the caption boxes `ev_table_fragments` names, each through
       the one border-area derivation: "both the table box and the caption box, if any, but not the anonymous
       container box". Gecko answers the same (nsLayoutUtils::AddBoxesForFrame over the table wrapper's
       children). Chrome answers one rectangle, the wrapper's border box with caption margins, for this and
       getBoundingClientRect(); no WPT test asserts either form, so the spec's list is kept. */
    if (kind == ELEMENT_VIEW_FRAGMENTS_TABLE) {
        lxb_dom_element_t **frags = NULL;

        n = ev_table_fragments(el, &frags);
        r = lexbor_malloc(n * sizeof *r);
        CHECK(r != NULL, "§6's getClientRects() step 3 could not allocate its rectangle list");
        for (i = 0; i < n; i++) {
            EvTarget ft;

            ev_target_of_element(frags[i], &ft);
            DCHECK(ft.rctx == t->rctx,
                   "§6's getClientRects() step 3 reached a caption box whose element's relevant realm is not the "
                   "table element's; CSS 2.1 §17.4's caption boxes are the table element's children, so this is "
                   "a caption taken from another tree");
            DCHECK(ft.has_box,
                   "§6's getClientRects() step 3 reached a caption that generates no box: core/layout/table_box.h "
                   "skips a `display: none` child before classifying it, so it and core/dom/element_view.h's box "
                   "predicate disagree");
            ev_border_area_px(frags[i], b);
            ev_client_rect_px(t, frags[i], b, r[i]);
        }
        free(frags);
        *out = r;
        return n;
    }
    DCHECK(kind == ELEMENT_VIEW_FRAGMENTS_ONE,
           "§6's getClientRects() step 3 reached a fragment kind with no arm; each member of core/dom/"
           "element_view.h's enumeration names a different count");
    r = lexbor_malloc(sizeof *r);
    CHECK(r != NULL, "§6's getClientRects() step 3 could not allocate its rectangle list");
    ev_border_area_px(el, b);
    ev_client_rect_px(t, el, b, r[0]);
    *out = r;
    return 1;
}

/* The layout §6's getClientRects() step 3 reads, asked under a step machine's resting drive: each fragment's
   position and its two extents. False ("not yet") when a question was deferred, with it suspended on the
   current job; true when every operand is answered, so `ev_client_rects_px` then runs over the answered memo.
   It asks what that function derives and nothing else, kind by kind. */
static bool ev_client_rects_ask(const EvTarget *t)
{
    lxb_dom_element_t *el = lxb_dom_interface_element(t->node);
    ElementViewFragments kind;
    FlowPoint o;
    CssPx w, h;

    if (!t->has_box || ev_has_svg_layout_box(t->node)) return true;
    /* Step 3's first constraint maps every rectangle through the transforms that apply (ev_client_rect_px). */
    if (!css_transform_matrix_current_ask(el)) return false;
    kind = element_view_fragment_kind(el);
    if (kind == ELEMENT_VIEW_FRAGMENTS_LINE_BOXES) {
        FlowRect *frags = NULL;
        size_t n = 0;
        bool done = flow_inline_fragment_rects_ask(el, &frags, &n);

        free(frags);
        return done;
    }
    if (kind == ELEMENT_VIEW_FRAGMENTS_TABLE) {
        lxb_dom_element_t **frags = NULL;
        size_t n = ev_table_fragments(el, &frags), i;
        bool done = flow_table_box_origin_ask(el, &o);

        /* The table box's extents are the element's, and each caption is a border area of its own. */
        for (i = 0; done && i < n; i++)
            done = (i == 0 || flow_border_box_origin_ask(frags[i], &o)) &&
                   used_value_padding_edge_ask(frags[i], false, &w) && used_value_padding_edge_ask(frags[i], true, &h);
        free(frags);
        return done;
    }
    return flow_border_box_origin_ask(el, &o) && used_value_padding_edge_ask(el, false, &w) &&
           used_value_padding_edge_ask(el, true, &h);
}

JSValue element_view_rect_mint(JSContext *rctx, const CssPx r[4])
{
    return dom_rect_new_values(rctx, ev_client_px(rctx, r[0]), ev_client_px(rctx, r[1]), ev_client_px(rctx, r[2]),
                               ev_client_px(rctx, r[3]));
}

static JSValue ev_client_rects_mint(const EvTarget *t, CssPx (*r)[4], size_t n)
{
    JSValue rects;
    size_t i;

    /* every object below is minted in the ELEMENT's relevant realm, never the caller's */
    rects = JS_NewArray(t->rctx);
    CHECK(!JS_IsException(rects), "the client-rect list could not be allocated");
    for (i = 0; i < n; i++)
        JS_SetPropertyUint32(t->rctx, rects, (uint32_t)i, element_view_rect_mint(t->rctx, r[i]));
    return dom_rect_list_new(t->rctx, rects);
}

/* The box tests and then step 3's layout, under the caller's resting drive on its entered job. */
bool element_view_client_rects_ask(lxb_dom_element_t *el, LayoutJobDrive *d)
{
    EvTarget t;

    DCHECK(el != NULL, "§6's getClientRects() was asked as an internal algorithm with no element");
    if (!element_view_box_tests_ask(lxb_dom_interface_node(el))) {
        DCHECK(layout_job_drive_spent(d),
               "CSSOM VIEW §6's client rectangles' box tests parked an author match with the drive's allowance "
               "unspent, so the next activation would ask the same thing over the same state for ever");
        return false;
    }
    /* Read inside the job, so the box tests' cascade is held by the job's records across activations. */
    ev_target_of_element(el, &t);
    while (block_flow_drive_deferred(d)) {
        if (ev_client_rects_ask(&t)) return true;
        DCHECK(layout_job_drive_spent(d) || layout_job_drive_pending(d),
               "CSSOM VIEW §6's client rectangles answered \"not yet\" with no question suspended and no "
               "allowance spent, so the next activation would ask the same thing over the same state for ever");
    }
    return false;
}

/* CSSOM View §9's text rule asks the Text node's own rectangles, so the boxes it reads are its parent's subtree's:
   a `display: contents` parent generates none itself and still lays out its text. */
static lxb_dom_node_t *ev_text_parent(const lxb_dom_node_t *text)
{
    DCHECK(text != NULL && text->type == LXB_DOM_NODE_TYPE_TEXT && flat_tree_parent(text) != NULL &&
               flat_tree_parent(text)->type == LXB_DOM_NODE_TYPE_ELEMENT,
           "CSSOM View §9's text rectangles were asked of a node that is not a Text node with an element parent; "
           "a Range in a document selects only those (its caller asks `range_in_document` first)");
    return flat_tree_parent(text);
}

bool element_view_text_rects_ask(const lxb_dom_node_t *text, uint32_t start, uint32_t end, LayoutJobDrive *d)
{
    lxb_dom_node_t *p = ev_text_parent(text);

    if (!element_view_box_tests_ask(p)) {
        DCHECK(layout_job_drive_spent(d),
               "CSSOM VIEW §9's text rectangles' box tests parked an author match with the drive's allowance "
               "unspent, so the next activation would ask the same thing over the same state for ever");
        return false;
    }
    if (!element_view_subtree_has_boxes(p)) return true;
    while (block_flow_drive_deferred(d)) {
        FlowRect *r = NULL;
        size_t n = 0;
        bool done = flow_text_fragment_rects_ask(text, start, end, &r, &n) &&
                    css_transform_matrix_current_ask(lxb_dom_interface_element(p));

        free(r);
        if (done) return true;
        DCHECK(layout_job_drive_spent(d) || layout_job_drive_pending(d),
               "CSSOM VIEW §9's text rectangles answered \"not yet\" with no question suspended and no allowance "
               "spent, so the next activation would ask the same thing over the same state for ever");
    }
    return false;
}

size_t element_view_text_rects_px(const lxb_dom_node_t *text, uint32_t start, uint32_t end, CssPx (**out)[4])
{
    lxb_dom_node_t *p = ev_text_parent(text);
    FlowRect *frags = NULL;
    CssPx (*r)[4] = NULL;
    CssPx b[4];
    EvTarget t;
    size_t n = 0, i;
    bool served;

    *out = NULL;
    if (!element_view_subtree_has_boxes(p)) return 0;
    ev_target_of_element(lxb_dom_interface_element(p), &t);
    served = flow_text_fragment_rects_ask(text, start, end, &frags, &n);
    DCHECK(served, "CSSOM View §9's text rectangles were read before element_view_text_rects_ask answered them");
    (void)served;
    if (n > 0) {
        r = lexbor_malloc(n * sizeof *r);
        CHECK(r != NULL, "CSSOM View §9's text rectangle list could not be allocated");
    }
    for (i = 0; i < n; i++) {
        b[0] = frags[i].x;
        b[1] = frags[i].y;
        b[2] = frags[i].width;
        b[3] = frags[i].height;
        /* "The transforms that apply to the ancestors are applied": the parent's, which the text is inside. */
        ev_client_rect_px(&t, lxb_dom_interface_element(p), b, r[i]);
    }
    free(frags);
    *out = r;
    return n;
}

size_t element_view_client_rects_px(lxb_dom_element_t *el, CssPx (**out)[4])
{
    EvTarget t;

    DCHECK(el != NULL, "§6's getClientRects() was invoked as an internal algorithm with no element");
    ev_target_of_element(el, &t);
    return ev_client_rects_px(&t, out);
}

/* One axis of the enclosing box (`o` 0 for x, 1 for y): its low edge and its extent. When one rectangle
   supplies both extremes, its own extent is the answer: `(y + h) - y` need not be `h` in IEEE arithmetic, and a
   browser's fixed-point layout returns `h`. The difference's facts are folded in at a zero example. */
static void ev_enclosing_axis(const CssPx (*r)[4], size_t n, size_t o, CssPx *lo, CssPx *extent)
{
    CssPx min = css_px(0.0), max = css_px(0.0), span;
    size_t i, imin = 0, imax = 0;

    for (i = 0; i < n; i++) {
        CssPx a = r[i][o], b = css_px_add(r[i][o], r[i][o + 2]);
        CssPx elo = css_px_min(a, b), ehi = css_px_max(a, b);

        if (i == 0 || elo.px < min.px) imin = i;
        if (i == 0 || ehi.px > max.px) imax = i;
        min = i == 0 ? elo : css_px_min(min, elo);
        max = i == 0 ? ehi : css_px_max(max, ehi);
    }
    span = css_px_sub(max, min);
    *lo = min;
    *extent = imin == imax && r[imin][o + 2].px >= 0.0 ? css_px_add(r[imin][o + 2], css_px_sub(span, span)) : span;
}

void element_view_enclosing_box_px(const CssPx (*r)[4], size_t n, CssPx out[4])
{
    DCHECK(r != NULL && n > 0, "the smallest rectangle enclosing a list was asked of an empty list");
    ev_enclosing_axis(r, n, 0, &out[0], &out[2]);
    ev_enclosing_axis(r, n, 1, &out[1], &out[3]);
}

/* §6's get the bounding box, steps 3 and 4, over a list of `n` > 0 client rectangles: "If all rectangles in
   list have zero width or height, return the first rectangle in list. Otherwise, return a DOMRect object
   describing the smallest rectangle that includes all of the rectangles in list of which the height or width
   is not zero." The tests run on the examples, as `css_px_min`/`css_px_max` do; every rectangle's extents are
   folded into all four results (`a - a` keeps `a`'s facts at a zero example), because which step answers and
   which rectangles step 4 encloses are functions of all of them. */
void element_view_bounding_box_of(const CssPx (*r)[4], size_t n, CssPx out[4])
{
    CssPx facts = css_px(0.0);
    CssPx (*kept)[4];
    bool step3 = true;
    size_t i, k, m = 0;

    DCHECK(n > 0, "get the bounding box's steps 3 and 4 were asked over an empty list; step 2 answers it");
    kept = lexbor_malloc(n * sizeof *kept);
    CHECK(kept != NULL, "§6's get the bounding box could not allocate its rectangle list");
    for (i = 0; i < n; i++) {
        facts = css_px_add(facts, css_px_add(css_px_sub(r[i][2], r[i][2]), css_px_sub(r[i][3], r[i][3])));
        if (r[i][2].px != 0.0 && r[i][3].px != 0.0) step3 = false;
        if (r[i][2].px == 0.0 && r[i][3].px == 0.0) continue;
        for (k = 0; k < 4; k++) kept[m][k] = r[i][k];
        m++;
    }
    if (step3) {
        for (k = 0; k < 4; k++) out[k] = css_px_add(r[0][k], facts);
    } else {
        DCHECK(m > 0, "step 4 was taken with no rectangle of a non-zero width and height kept");
        element_view_enclosing_box_px((const CssPx (*)[4])kept, m, out);
        for (k = 0; k < 4; k++) out[k] = css_px_add(out[k], facts);
    }
    free(kept);
}

/* §6's get the bounding box in css_length.h's vocabulary, for a caller that compares rectangles in C (see
   element_view.h). Step 2's zeros are the spec's own number for every user agent, so they stay concrete. */
void element_view_bounding_box_px(lxb_dom_element_t *el, CssPx out[4])
{
    EvTarget t;
    CssPx (*r)[4] = NULL;
    size_t n;

    DCHECK(el != NULL, "§6's get-the-bounding-box was invoked as an internal algorithm with no element");
    ev_target_of_element(el, &t);
    n = ev_client_rects_px(&t, &r);   /* step 1 */
    if (n == 0)                        /* step 2 */
        out[0] = out[1] = out[2] = out[3] = css_px(0.0);
    else
        element_view_bounding_box_of((const CssPx (*)[4])r, n, out);
    free(r);
}

void element_view_bounding_box_served_px(lxb_dom_element_t *el, CssPx out[4], const char *asker)
{
    LayoutJobServed s;

    layout_job_served_begin(&s, asker);
    element_view_bounding_box_px(el, out);
    layout_job_served_end(&s);
}

/* §6's get the bounding box over step 1's `n` client rectangles, minted. §6's `[NewObject]` holds because the
   rectangle is minted on every call. */
static JSValue ev_bounding_rect_mint(const EvTarget *t, CssPx (*r)[4], size_t n)
{
    CssPx b[4];

    if (n == 0) return dom_rect_new(t->rctx, 0.0, 0.0, 0.0, 0.0);   /* step 2 */
    element_view_bounding_box_of((const CssPx (*)[4])r, n, b);
    return dom_rect_new_values(t->rctx, ev_client_px(t->rctx, b[0]), ev_client_px(t->rctx, b[1]),
                               ev_client_px(t->rctx, b[2]), ev_client_px(t->rctx, b[3]));
}

/* §6's getClientRects() and getBoundingClientRect() as step machines, `clientWidth`'s shape: the layout step 3
   reads is asked under a resting drive on the job the state owns, so a box whose layout is wide rests between
   activations with its questions suspended. One activation is one unit, measured against the slice. The
   rectangles are derived over the answered memo inside the activation and minted after it. */
enum { EV_RECTS_LIST = 0, EV_RECTS_BOUNDING = 1 };
#define EV_RECTS_STAGES(X) \
    X(EV_RECTS_MEASURE, "CSSOM VIEW §6 \"Extensions to the Element Interface\" getClientRects() / " \
                        "getBoundingClientRect() (step 3's border areas, one resting drive per activation)")
enum { IDL_STEP_STAGE_BASE(EV_RECTS_STAGES) EV_RECTS_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const EV_RECTS_STEPS[] = { EV_RECTS_STAGES(JS_STEP_STAGE_LABEL) NULL };

typedef struct {
    LayoutJob *job;   /* owned, declared through layout_job_step_ops; NULL until the first activation */
} EvRectsState;

static void ev_rects_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    EvRectsState *s = st;

    v->tree(ctx, (void **)&s->job, NULL, 0, &layout_job_step_ops);
}

static int js_ev_rects_step(JSContext *ctx, JSStepHdr *hdr, void *st, int argc, JSValueConst *argv,
                            JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    EvRectsState *s = st;
    CssPx (*r)[4] = NULL;
    size_t n = 0;
    bool done;
    LayoutJobDrive d;
    EvTarget t;
    int rc;

    (void)argc; (void)argv; (void)cb_result; (void)out_cb; (void)out_argc;
    STEP_DISPATCH(EV_RECTS_STAGES, hdr->stage, hdr->def->algorithm, JS_STEP_ABRUPT);

    STEP_ARM(EV_RECTS_MEASURE);
    if (s->job == NULL) {
        s->job = layout_job_new();
        METRIC_ADD(client_rects_asks, 1);
    }
    /* An entry the job's fork delivers to: its arm is recorded before the drive runs again. */
    rc = layout_job_fork(ctx, hdr, s->job);
    if (rc) return rc;
    rest_unit_begin(REST_UNIT_LAYOUT_DESCENDANTS);
    layout_job_enter(s->job);
    layout_job_drive_begin_resting(&d);
    done = element_view_client_rects_ask(element_of_value(hdr->this_val), &d);
    if (done) {
        ev_target(hdr->this_val, &t);
        n = ev_client_rects_px(&t, &r);
    }
    layout_job_drive_end(&d);
    layout_job_leave(s->job);
    rest_unit_end();
    if (layout_job_declined(s->job)) {
        rc = layout_job_fork(ctx, hdr, s->job);
        return rc ? rc : JS_STEP_YIELD;
    }
    if (!done) {
        layout_job_rested(s->job);
        METRIC_ADD(client_rects_rests, 1);
        return JS_STEP_YIELD;
    }
    *presult = idl_step_magic(hdr) == EV_RECTS_BOUNDING ? ev_bounding_rect_mint(&t, r, n)
                                                       : ev_client_rects_mint(&t, r, n);
    free(r);
    return JS_STEP_DONE;
}

static const IdlStepDecl EV_RECTS_STEP = {
    js_ev_rects_step, sizeof(EvRectsState), ev_rects_visit, NULL,
    "CSSOM VIEW §6 \"Extensions to the Element Interface\" getClientRects() / getBoundingClientRect()",
    EV_RECTS_STEPS
};

/* ---- the members, the declaration and the per-realm install ---------------------------------------------- */

static JSValue ev_get_member(JSContext *ctx, const EvTarget *t, ElementViewMember member)
{
    switch (member) {
    case EV_SCROLL_TOP:     return ev_scroll_position(ctx, t, true);
    case EV_SCROLL_LEFT:    return ev_scroll_position(ctx, t, false);
    case EV_SCROLL_HEIGHT:
    case EV_SCROLL_WIDTH:
        DFAIL("CSSOM VIEW §6's `scrollWidth`/`scrollHeight` reached the plain getter; EV_VIEW_STEP answers them, "
              "walking step 7's scrolling area in its own stage");
        break;
    case EV_CLIENT_HEIGHT:
    case EV_CLIENT_WIDTH:
        DFAIL("CSSOM VIEW §6's `clientWidth`/`clientHeight` reached the plain getter; they are step machines "
              "(EV_CLIENT_STEP) installed through idl_install_accessor_step");
        break;
    case EV_CLIENT_TOP:     return ev_client_edge(ctx, t, true);
    case EV_CLIENT_LEFT:    return ev_client_edge(ctx, t, false);
    }
    DFAIL("a CSSOM VIEW §6 Element member was read with a magic no member of this file declares — the magic IS "
          "the member, so an unknown one means a name was installed without a case to answer it");
    return JS_UNDEFINED;
}

/* `scrollTop`, `scrollLeft`, `scrollWidth`, `scrollHeight`, `clientTop` and `clientLeft` as one step machine
   owning a layout job: the box tests every member opens with read the author matches of the element and its
   ancestors under a resting drive, so a page-sized sheet rests between rules and a selector the cascade cannot
   decide forks the flow (layout_job_fork); the member then runs on the same job, whose cascade record holds
   those matches for every element and property it asks.
   `scrollWidth`/`scrollHeight` walk §2's scrolling area (the element's for step 7, the viewport's for steps 4
   and 5) over the descendants in a stage of their own, one rest unit per activation under a resting drive, so
   a deep or wide subtree rests inside the walk.
   Named residual. Not covered: `scrollTop`/`scrollLeft`'s own layout (a scroll position) runs to completion
   after the box tests, so a descendant's style it reads is matched with no resting drive, cannot keep a fork
   and does not rest. Next diff: `scrollTop`/`scrollLeft` in a stage of their own. Absence shows as:
   selector_match_node's abort for a build with no resting drive under one of those members, or a rest unit
   overrun inside one. */
#define EV_VIEW_STAGES(X) \
    X(EV_VIEW_MEASURE, "CSSOM VIEW §6 \"Extensions to the Element Interface\" scrollTop / scrollLeft / " \
                       "scrollWidth / scrollHeight / clientTop / clientLeft (the box tests under a resting " \
                       "drive, then the member on the same job)") \
    X(EV_VIEW_AREA, "CSSOM VIEW §6 scrollWidth / scrollHeight steps 4, 5 and 7: CSSOM VIEW §2's scrolling " \
                    "area walked under a resting drive")
enum { IDL_STEP_STAGE_BASE(EV_VIEW_STAGES) EV_VIEW_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const EV_VIEW_STEPS[] = { EV_VIEW_STAGES(JS_STEP_STAGE_LABEL) NULL };

typedef struct {
    LayoutJob         *job;    /* owned, declared through layout_job_step_ops; NULL before the first activation */
    ScrollingAreaWalk  area;   /* EV_VIEW_AREA's cursor; its nodes are the running flow's, shared by address */
    double             vp;     /* steps 4 and 5's viewport extent, the max's other operand when area.viewport */
} EvViewState;

static void ev_view_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    EvViewState *s = st;

    v->tree(ctx, (void **)&s->job, NULL, 0, &layout_job_step_ops);
}

static int js_ev_view_step(JSContext *ctx, JSStepHdr *hdr, void *st, int argc, JSValueConst *argv,
                           JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    EvViewState *s = st;
    lxb_dom_element_t *el = element_of_value(hdr->this_val);
    LayoutJobDrive d;
    EvTarget t;
    bool matched, area = false;
    int rc;

    (void)argc; (void)argv; (void)cb_result; (void)out_cb; (void)out_argc;
    STEP_DISPATCH(EV_VIEW_STAGES, hdr->stage, hdr->def->algorithm, JS_STEP_ABRUPT);

    STEP_ARM(EV_VIEW_MEASURE);
    DCHECK(el != NULL, "a CSSOM VIEW §6 member reached its body on a receiver that is not an Element — the "
                       "member states element_is at its install");
    if (s->job == NULL) s->job = layout_job_new();
    /* An entry the job's fork delivers to: its arm is recorded before the drive runs again. */
    rc = layout_job_fork(ctx, hdr, s->job);
    if (rc) return rc;
    rest_unit_begin(REST_UNIT_LAYOUT_DESCENDANTS);
    layout_job_enter(s->job);
    layout_job_drive_begin_resting(&d);
    matched = element_view_box_tests_ask(lxb_dom_interface_node(el));
    DCHECK(matched || layout_job_drive_spent(&d),
           "a CSSOM VIEW §6 member's box tests parked an author match with the drive's allowance unspent, so its "
           "next activation would ask the same thing over the same state for ever");
    layout_job_drive_end(&d);
    rest_unit_end();
    if (matched) {
        ElementViewMember m = (ElementViewMember)idl_step_magic(hdr);

        ev_target(hdr->this_val, &t);
        if (m != EV_SCROLL_WIDTH && m != EV_SCROLL_HEIGHT)
            *presult = ev_get_member(ctx, &t, m);
        else switch (ev_scroll_extent_early(ctx, &t, m == EV_SCROLL_HEIGHT, presult, &s->vp)) {
        case EV_EXTENT_ANSWERED:
            break;
        case EV_EXTENT_VIEWPORT:
            DCHECK(document_root_node(t.dctx) != NULL &&
                   lxb_dom_interface_node(document_root_node(t.dctx)->owner_document) == t.doc,
                   "steps 4 and 5 walk the viewport of the realm whose active document is the element's node "
                   "document, so that realm's document element must belong to that document");
            scrolling_area_walk_viewport(&s->area, t.doc,
                                         m == EV_SCROLL_HEIGHT ? viewport_icb_height(t.dctx)
                                                               : viewport_icb_width(t.dctx),
                                         m == EV_SCROLL_HEIGHT);
            area = true;
            break;
        case EV_EXTENT_ELEMENT:
            scrolling_area_walk_element(&s->area, lxb_dom_interface_element(t.node), m == EV_SCROLL_HEIGHT);
            area = true;
            break;
        }
    }
    layout_job_leave(s->job);
    if (layout_job_declined(s->job)) {
        rc = layout_job_fork(ctx, hdr, s->job);
        return rc ? rc : JS_STEP_YIELD;
    }
    if (!matched) {
        layout_job_rested(s->job);
        return JS_STEP_YIELD;
    }
    if (!area) return JS_STEP_DONE;
    METRIC_ADD(scroll_area_asks, 1);
    hdr->stage = EV_VIEW_AREA;

    STEP_ARM(EV_VIEW_AREA);
    rc = layout_job_fork(ctx, hdr, s->job);
    if (rc) return rc;
    {
        size_t k = 0, n = rest_unit_items(REST_UNIT_LAYOUT_DESCENDANTS);
        bool done;

        /* The walk's drive, as core/dom/element_scrolling.c's into-view walk steps it: a node whose origin
           answers "not yet" leaves its questions suspended, and each activation computes them first, up to the
           drive's boxes, then retries the node. */
        rest_unit_begin(REST_UNIT_LAYOUT_DESCENDANTS);
        layout_job_enter(s->job);
        layout_job_drive_begin_resting(&d);
        while (k < n && !scrolling_area_walk_done(&s->area) && block_flow_drive_deferred(&d)) {
            if (scrolling_area_walk_step(&s->area)) k++;
            else if (layout_job_drive_spent(&d)) break;
        }
        layout_job_drive_end(&d);
        rest_unit_end();
        done = scrolling_area_walk_done(&s->area);
        if (done && !s->area.viewport) *presult = ev_length_long(ctx, scrolling_area_walk_extent(&s->area));
        else if (done) {
            /* Step 4's max(viewport scrolling area height, viewport height) is a real length, as the element
               arm's area is, and crosses its `long` by the same nearest-integer conversion
               (element_view_length_long); the viewport height is non-negative, so the tie-break is unreachable. */
            double h = fmax(scrolling_area_walk_extent(&s->area).px, s->vp);

            ev_target(hdr->this_val, &t);
            *presult = ev_env_long(ctx, &t, s->area.vertical ? "scrollHeight" : "scrollWidth", floor(h + 0.5));
        }
        layout_job_leave(s->job);
        if (layout_job_declined(s->job)) {
            rc = layout_job_fork(ctx, hdr, s->job);
            return rc ? rc : JS_STEP_YIELD;
        }
        if (!done) {
            layout_job_rested(s->job);
            METRIC_ADD(scroll_area_rests, 1);
            return JS_STEP_YIELD;
        }
    }
    return JS_STEP_DONE;
}

static const IdlStepDecl EV_VIEW_STEP = {
    js_ev_view_step, sizeof(EvViewState), ev_view_visit, NULL,
    "CSSOM VIEW §6 \"Extensions to the Element Interface\" scrollTop / scrollLeft / scrollWidth / scrollHeight / "
    "clientTop / clientLeft",
    EV_VIEW_STEPS
};
static int g_id_view[EV_CLIENT_LEFT + 1] = { -1, -1, -1, -1, -1, -1 };

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
    int id = idl_method_id_step(ctx, EV_SCROLL_ARGS, 2, VIEWPORT_SCROLL_TO_OPTIONS,
                                VIEWPORT_SCROLL_TO_OPTIONS_N, &EV_SCROLL_TO_STEP, (int)kind);

    /* §3.7.7's PROMISE RETURN TYPE. It is what makes `el.scrollTo(0)` — §3.2.17 step 1 refusing a value that
       is not undefined, null or an Object — a REJECTED promise rather than a throw, which is what a page
       wrapping the call in `.catch` is relying on. Its position here is only reading order: this used to say
       it stood before the optional index "for idl_returns_promise's own reason", and there is no such reason —
       see that function, whose ordering claim was false in both halves and is corrected there. */
    idl_returns_promise();
    /* WEB IDL §3.7 Interfaces' implementation-check step 3, asked at step 2.1.2.3 of §3.7.7's try-list —
       BEFORE step 2.1.4 computes the effective overload set, so `Element.prototype.scrollTo.call({}, x)`
       refuses without converting x. §3.7.7's `Try` opens before this and closes after the method steps, so
       the TypeError a foreign receiver gets is a REJECTED PROMISE for these two and not a throw. */
    idl_this_iface(element_is, "Element");
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
    g_id_set_scroll_top  = idl_setter_id_step(ctx, IDL_UNRESTRICTED_DOUBLE, false, &EV_SCROLL_TO_STEP,
                                              EV_SCROLL_SET_TOP);
    /* §3.7.6's SETTER asks the receiver BEFORE it converts the value — Web IDL §3.7.6 "Attributes": "If
       validThis is false and attribute was not specified with the [LegacyLenientThis] extended attribute,
       then throw a TypeError.", which stands above Web IDL §3.7.6 "Attributes": "Let idlValue be determined
       as follows:" and the per-type table that IS the conversion. A test in the body would run after that
       table, so `Element.prototype.scrollTop` set through a foreign receiver with `{valueOf(){…}}` would run
       the page's `valueOf` first. */
    idl_this_iface(element_is, "Element");
    g_id_set_scroll_left = idl_setter_id_step(ctx, IDL_UNRESTRICTED_DOUBLE, false, &EV_SCROLL_TO_STEP,
                                              EV_SCROLL_SET_LEFT);
    idl_this_iface(element_is, "Element");
    g_id_client_rects    = idl_method_id_step(ctx, NULL, 0, NULL, 0, &EV_RECTS_STEP, EV_RECTS_LIST);
    idl_this_iface(element_is, "Element");
    g_id_bounding_rect   = idl_method_id_step(ctx, NULL, 0, NULL, 0, &EV_RECTS_STEP, EV_RECTS_BOUNDING);
    idl_this_iface(element_is, "Element");
    g_id_scroll          = ev_declare_scroll(ctx, EV_SCROLL_ABSOLUTE);
    g_id_scroll_by       = ev_declare_scroll(ctx, EV_SCROLL_RELATIVE);
    for (int m = EV_SCROLL_TOP; m <= EV_CLIENT_LEFT; m++) {
        g_id_view[m] = idl_getter_id_step(ctx, &EV_VIEW_STEP, m);
        idl_this_iface(element_is, "Element");
    }
    g_id_client_width = idl_getter_id_step(ctx, &EV_CLIENT_STEP, EV_CLIENT_WIDTH);
    idl_this_iface(element_is, "Element");
    g_id_client_height = idl_getter_id_step(ctx, &EV_CLIENT_STEP, EV_CLIENT_HEIGHT);
    idl_this_iface(element_is, "Element");
    g_id_scroll_into_view = idl_method_id_step(ctx, EV_SCROLL_INTO_VIEW_ARGS, 1,
                                               EV_SCROLL_INTO_VIEW_OPTIONS,
                                               (int)(sizeof EV_SCROLL_INTO_VIEW_OPTIONS /
                                                     sizeof EV_SCROLL_INTO_VIEW_OPTIONS[0]),
                                               &EV_SCROLL_INTO_VIEW_STEP, 0);
    /* §3.7 Interfaces' implementation-check step 3 for the LAST of §6's six operations — declared beside its
       own type list rather than in ev_declare_scroll, because this member is not one of the two that helper
       makes and idl_this_iface names the member declared LAST. */
    idl_this_iface(element_is, "Element");
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
    /* Web IDL §3.7.6 "Attributes"' receiver test is declared with each getter (idl_this_iface over
       core/dom/element.h's `element_is`, as §6 is a `partial interface Element`), the same answer the
       operations below declare, so this file's attributes and its operations cannot come apart. */
    idl_install_accessor_step(ctx, proto, "scrollTop", g_id_view[EV_SCROLL_TOP], g_id_set_scroll_top);
    idl_install_accessor_step(ctx, proto, "scrollLeft", g_id_view[EV_SCROLL_LEFT], g_id_set_scroll_left);
    idl_install_accessor_step(ctx, proto, "scrollWidth", g_id_view[EV_SCROLL_WIDTH], -1);
    idl_install_accessor_step(ctx, proto, "scrollHeight", g_id_view[EV_SCROLL_HEIGHT], -1);
    idl_install_accessor_step(ctx, proto, "clientTop", g_id_view[EV_CLIENT_TOP], -1);
    idl_install_accessor_step(ctx, proto, "clientLeft", g_id_view[EV_CLIENT_LEFT], -1);
    idl_install_accessor_step(ctx, proto, "clientWidth", g_id_client_width, -1);
    idl_install_accessor_step(ctx, proto, "clientHeight", g_id_client_height, -1);
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
    for (int m = EV_SCROLL_TOP; m <= EV_CLIENT_LEFT; m++) g_id_view[m] = -1;
}
