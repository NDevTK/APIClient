/* CSSOM VIEW §6 — Extensions to the Element Interface. See element_view.h for the box model every member below
   branches over, for why a viewport-derived answer is concolic and a scroll position is not, and for which
   members of §6 are honestly absent. */
#include <math.h>
#include <stdbool.h>
#include <stdint.h>

#include <lexbor/dom/dom.h>

#include "check.h"
#include "quickjs.h"
#include "core/dom/document.h"
#include "core/dom/element.h"
#include "core/dom/element_view.h"
#include "core/dom/node.h"
#include "core/frame/viewport.h"
#include "core/frame/window_proxy.h"
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
    size_t len = 0;

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
    /* §15.3.1's UA stylesheet rule for the `hidden` content attribute is `display: none`. */
    for (a = n; a; a = a->parent)
        if (a->type == LXB_DOM_NODE_TYPE_ELEMENT &&
            lxb_dom_element_get_attribute(lxb_dom_interface_element((lxb_dom_node_t *)a),
                                          (const lxb_char_t *)"hidden", 6, &len) != NULL)
            return false;
    return true;
}

/* ---- §6's `scrollTop` and `scrollLeft` ------------------------------------------------------------------- */

/* THE GETTER, IN THE SPEC'S OWN STEP ORDER. Every terminal it can reach in this engine is the ORIGIN, and each
   is a derivation rather than a stand-in:
     step 2 and step 3 are the spec's own zero for a document that is not being presented;
     step 5 hands the root element the WINDOW's scroll position, which viewport.h derives as the ICB origin —
       the viewport's scrolling area IS the ICB, so there is one valid scroll position;
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
    /* STEPS 6-8, AND WHY THE BRANCH BETWEEN THEM NEED NOT BE DECIDED. Step 6 hands the quirks-mode body the
       WINDOW's scroll position when the body is "not potentially scrollable", and steps 7-8 hand every other
       element its OWN — and this engine cannot decide the condition between them, because "potentially
       scrollable" reads the computed `overflow-x`/`overflow-y` of the body AND of its parent, and the
       `overflow` SHORTHAND a page actually writes is not in Lexbor's property registry for the cascade to
       resolve. It does not have to be decided while both answers are the same number, and the assert is what
       says so rather than a comment: the moment the viewport can be somewhere other than the ICB origin, the
       two arms disagree and this fires. */
    DCHECK(ev_window_scroll(t->dctx, vertical) == 0.0,
           "CSSOM VIEW §6's scrollTop/scrollLeft steps 6-8 answer the same number only while the viewport sits "
           "at the initial containing block origin: step 6 hands a quirks-mode body the WINDOW's scroll "
           "position and step 8 hands an element its own, which is the origin because nothing has scrolled "
           "one. The viewport can now be elsewhere, so `potentially scrollable` must be DECIDED here — build "
           "the computed `overflow` it reads (css_style_declaration.c resolves the cascade for longhands; the "
           "`overflow` shorthand is not a property Lexbor's registry carries)");
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
    /* steps 3-4 */
    if (!t.dctx) return JS_UNDEFINED;
    /* step 5 */
    if (t.is_root && t.quirks) return JS_UNDEFINED;
    /* Steps 6 and 7 are the same operation — "invoke scroll() on window with scrollX as first argument and y
       as second" — over two different elements, and step 7's own condition is the undecidable one the getter
       states. Here the two arms DIFFER (step 7 scrolls the window, step 8 terminates), so for a quirks-mode
       body that has a box it has to be decided, and it cannot be. */
    if (t.is_root || (t.is_body && t.quirks && !t.has_box)) {
        double req = unknown ? ev_window_scroll(t.dctx, vertical) : v;
        double x = vertical ? ev_window_scroll(t.dctx, false) : req;
        double y = vertical ? req : ev_window_scroll(t.dctx, true);

        viewport_scroll(t.dctx, x, y);
        return JS_UNDEFINED;
    }
    if (t.is_body && t.quirks)
        DFAIL("CSSOM VIEW §6's scrollTop/scrollLeft setter step 7 sends a quirks-mode BODY's scroll to the "
              "WINDOW only when the body is \"not potentially scrollable in at least one axis\", and step 8 "
              "otherwise scrolls the element itself — two different results. Deciding it needs the computed "
              "`overflow-x`/`overflow-y` of the body AND of its parent (the definition's own three conditions); "
              "css_style_declaration.c resolves the cascade but exposes it to no C caller, and the `overflow` "
              "SHORTHAND a page writes is not a property in Lexbor's registry. BUILD the computed-value read "
              "and the shorthand, then write both arms");
    /* step 8 */
    if (!t.has_box) return JS_UNDEFINED;
    /* step 9 */
    DFAIL("CSSOM VIEW §6's scrollTop/scrollLeft setter step 9 SCROLLS THE ELEMENT, and step 8 first asks "
          "whether it has a scrolling box and whether it has any overflow — all three need the element's own "
          "geometry, which this engine has none of: the scrolling area §2 defines for an element is its "
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
    if (t->is_body && t->quirks) {
        /* Step 5's condition is "the element is not POTENTIALLY SCROLLABLE in the x axis", whose first
           conjunct is "body has an associated box" — so a body with no box takes this branch decided, and one
           WITH a box needs the computed overflow this engine cannot read. The two arms differ (this one is the
           viewport, the other is the element's own scrolling area), so it has to be decided. */
        if (!t->has_box)
            return presented ? ev_env_long(ctx, t, vertical ? "scrollHeight" : "scrollWidth", fmax(area, vp))
                             : ev_long(ctx, 0.0);
        DFAIL("CSSOM VIEW §6's scrollWidth/scrollHeight step 5 answers a quirks-mode BODY with the VIEWPORT's "
              "scrolling area only when the body is \"not potentially scrollable\" in that axis, and otherwise "
              "with the body's OWN scrolling area. Deciding it needs the computed `overflow-x`/`overflow-y` of "
              "the body and of its parent; css_style_declaration.c resolves the cascade but exposes it to no C "
              "caller, and the `overflow` SHORTHAND is not a property in Lexbor's registry. BUILD the "
              "computed-value read and the shorthand");
    }
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
    /* Step 1: "If the element has no associated box OR IF THE BOX IS INLINE, return zero." The inline half is
       decided for the two elements step 2 answers for and asked of nothing else: CSS Display §2.7 BLOCKIFIES
       the root element's display type, so the root's box is never inline; and §15.3.1's UA stylesheet gives
       `body` `display: block`, which this engine's UA sheet also carries (css_style_declaration.c). An author
       rule making one of them inline is a computed value no C caller in this build can read — the same
       narrowing element_view_has_box states, and narrower than a laying-out browser rather than wider. */
    if (!t->has_box) return ev_long(ctx, 0.0);
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
          "the element's box. This engine gives geometry to exactly one box, the initial containing block "
          "(element_view.h), so an element that is neither the root element outside quirks mode nor the body "
          "inside it has no padding edge to measure. BUILD A LAYOUT — CSS 2.1 §10.3.3 resolves the root "
          "element's used width from the ICB viewport.c already models, which is where one starts");
    return ev_long(ctx, 0.0);
}

static JSValue ev_client_edge(JSContext *ctx, const EvTarget *t)
{
    /* step 1 */
    if (!t->has_box) return ev_long(ctx, 0.0);
    /* step 2 */
    DFAIL("CSSOM VIEW §6's clientTop/clientLeft step 2 returns THE UNSCALED COMPUTED VALUE OF THE "
          "border-top-width / border-left-width PROPERTY plus the width of any scrollbar rendered between that "
          "padding edge and that border edge. Neither term is available: a computed border width is 0 when "
          "border-*-style is `none` or `hidden` and a resolved length otherwise, and css_style_declaration.c "
          "resolves the cascade for the CSSOM's own reads while exposing it to no C caller. BUILD the "
          "computed-value read (the border-style interaction, `medium`, and length resolution) — the scrollbar "
          "term is 0 for as long as this user agent renders none");
    return ev_long(ctx, 0.0);
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
}

void element_view_free(void)
{
    /* The setter ids are the AGENT's, and the pool they live in goes with the runtime. */
    g_id_set_scroll_top = g_id_set_scroll_left = -1;
}
