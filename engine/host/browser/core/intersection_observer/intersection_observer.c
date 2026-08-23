/* INTERSECTION OBSERVER — §2.2's interface and §3.2's processing model. See intersection_observer.h for what
 * this is for, why delivery is a task and why every piece of state here is a JS value. */
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <lexbor/css/syntax/token.h>
#include <lexbor/css/syntax/tokenizer.h>
#include <lexbor/dom/dom.h>

#include "check.h"
#include "quickjs.h"
#include "quickjs-step.h"
#include "core/agent_state.h"
#include "core/css/css_computed_value.h"
#include "core/css/css_length.h"
#include "core/dom/document.h"
#include "core/dom/element.h"
#include "core/dom/element_view.h"
#include "core/dom/node.h"
#include "core/events/report_exception.h"
#include "core/frame/viewport.h"
#include "core/frame/window_proxy.h"
#include "core/geometry/dom_rect.h"
#include "core/idl_args.h"
#include "core/intersection_observer/intersection_observer.h"
#include "core/intersection_observer/intersection_observer_entry.h"
#include "core/layout/used_value.h"
#include "core/realm.h"
#include "core/timing/hr_time.h"
#include "solver/concolic.h"   /* §8.1.7.3's frameTimestamp is a MOMENT, and a moment can be unknown */

/* ---- the shapes ------------------------------------------------------------------------------------------
 *
 * §3.1.3's INTERNAL SLOTS, as one Array in an own slot on the observer. `IO_S_WINDOW` is not one of §3.1.3's
 * and is not state either: it is the observer's own REALM, recorded as the WindowProxy the realm is reachable
 * through, and it is here because §2.3 makes the entry's `time` "relative to the time origin of the global
 * object associated with the IntersectionObserver instance" while §3.2.10 walks a DOCUMENT'S list — and those
 * are two different realms for the ordinary case of an implicit-root observer constructed inside an iframe,
 * whose root is the TOP-LEVEL browsing context's document (§2.2) and whose global is the frame's. Recovered
 * with window_proxy_realm, which is how every other component asks that question.
 *
 * §3.1.2's IntersectionObserverRegistration record is an Array on the TARGET's wrapper, in a list of them. */
enum {
    IO_S_CALLBACK = 0, IO_S_TARGETS, IO_S_ENTRIES, IO_S_ROOT, IO_S_ROOT_MARGIN, IO_S_SCROLL_MARGIN,
    IO_S_THRESHOLDS, IO_S_DELAY, IO_S_TRACK_VISIBILITY, IO_S_WINDOW, IO_S_COUNT
};
enum { IOR_OBSERVER = 0, IOR_PREV_THRESHOLD, IOR_PREV_INTERSECTING, IOR_LAST_UPDATE, IOR_PREV_VISIBLE,
       IOR_COUNT };

/* §2.2's PARSED MARGIN — "a list of 4 pixel lengths or percentages", flat: index 2i says WHICH of the two the
   i-th side is and index 2i+1 carries the number. The sides are the CSS `margin` order the section states,
   "the amount the top, right, bottom and left edges, respectively, are offset by". */
enum { IO_MARGIN_TOP = 0, IO_MARGIN_RIGHT, IO_MARGIN_BOTTOM, IO_MARGIN_LEFT, IO_MARGIN_SIDES };
enum { IO_MARGIN_PX = 0, IO_MARGIN_PCT };
#define IO_MARGIN_LEN (IO_MARGIN_SIDES * 2)

typedef enum {
    IO_ROOT_MARGIN = 0,
    IO_SCROLL_MARGIN,
    IO_THRESHOLDS,
    IO_DELAY,
    IO_TRACK_VISIBILITY,
    IO_ROOT
} IoMember;

/* A RECTANGLE THE ENGINE IS STILL COMPUTING WITH. It stays in css_length.h's vocabulary — a number plus the
   environment facts it derives from — for the whole of §3.2.7 and §3.2.10, because every one of those steps is
   a C comparison that must not fork and must not throw the domain away either. It crosses viewport.h's one
   seam exactly once, where an entry's DOMRect is minted. See core/dom/element_view.h. */
typedef struct { CssPx x, y, w, h; } IoRect;

static JSClassID g_class;
static JSValue   g_state_key = JS_UNDEFINED;    /* the observer's own state slot */
static JSAtom    g_atom_state = JS_ATOM_NULL;
static JSValue   g_reg_key = JS_UNDEFINED;      /* §3.1.2's [[RegisteredIntersectionObservers]] on a target */
static JSAtom    g_atom_reg = JS_ATOM_NULL;
static JSAtom    g_atom_queued = JS_ATOM_NULL;  /* §3.1.1's IntersectionObserverTaskQueued flag */
static int       g_docobs_slot = -1;            /* this realm's document-observer list */
static int       g_notify_slot = -1;            /* this realm's §3.2.5 driver */
static int       g_notify_stepid = -1;
static int       g_id_ctor = -1, g_id_observe = -1, g_id_unobserve = -1;
static int       g_id_disconnect = -1, g_id_take = -1;
static int       g_ready;

/* ---- small list helpers (the same three every §4.3-shaped list needs) ------------------------------------- */

static uint32_t io_len(JSContext *ctx, JSValueConst arr)
{
    uint32_t n = 0;
    JSValue v = JS_GetPropertyStr(ctx, (JSValue)arr, "length");

    JS_ToUint32(ctx, &n, v);
    JS_FreeValue(ctx, v);
    return n;
}

static void io_push(JSContext *ctx, JSValueConst arr, JSValue v)
{
    JS_SetPropertyUint32(ctx, (JSValue)arr, io_len(ctx, arr), v);
}

static void io_set_len(JSContext *ctx, JSValueConst arr, uint32_t n)
{
    JS_SetPropertyStr(ctx, (JSValue)arr, "length", JS_NewUint32(ctx, n));
}

static double io_num_at(JSContext *ctx, JSValueConst arr, uint32_t i)
{
    JSValue v = JS_GetPropertyUint32(ctx, (JSValue)arr, i);
    double d = 0.0;

    DCHECK(JS_IsNumber(v), "an engine-built §3.1 list held something that is not a Number where one belongs");
    JS_ToFloat64(ctx, &d, v);
    JS_FreeValue(ctx, v);
    return d;
}

/* "a CLONE of" — a new list holding the same members, because §3.2.5 step 3.3 then EMPTIES the original. */
static JSValue io_clone(JSContext *ctx, JSValueConst arr)
{
    uint32_t n = io_len(ctx, arr), i;
    JSValue out = JS_NewArray(ctx);

    CHECK(!JS_IsException(out), "an Intersection Observer list clone could not be allocated");
    for (i = 0; i < n; i++)
        JS_SetPropertyUint32(ctx, out, i, JS_GetPropertyUint32(ctx, (JSValue)arr, i));
    return out;
}

/* THE OBSERVER'S OWN STATE. OWNED. */
static JSValue io_state(JSContext *ctx, JSValueConst obj)
{
    JSValue s;

    if (JS_GetClassID(obj) != g_class)
        return JS_ThrowTypeError(ctx, "not an IntersectionObserver");
    if (JS_GetOwnSlot(ctx, &s, obj, g_atom_state) <= 0)
        s = JS_UNDEFINED;
    DCHECK(JS_IsObject(s), "an IntersectionObserver carries no state — §3.2.1 writes every slot before the "
                           "object exists, so one without them was made somewhere else");
    return s;
}

/* THE OBSERVER'S OWN REALM — see the shapes comment. Never the walking realm. */
static JSContext *io_realm(JSContext *ctx, JSValueConst state)
{
    JSValue proxy = JS_GetPropertyUint32(ctx, (JSValue)state, IO_S_WINDOW);
    JSContext *rctx;

    DCHECK(window_proxy_is(proxy),
           "an IntersectionObserver's recorded global is not a WindowProxy — §3.2.1 records the constructing "
           "realm's and nothing else writes the slot");
    rctx = window_proxy_realm(ctx, proxy);
    JS_FreeValue(ctx, proxy);
    DCHECK(rctx != NULL,
           "an IntersectionObserver's realm is gone while the observer is still in a document's observer list "
           "— §3.3 keeps an observer alive while it is observing, and a realm that has been released has no "
           "targets left to observe");
    return rctx;
}

/* §3.1.2's list, off the TARGET's wrapper. `create` mints an empty one; without it an unobserved element stays
   an element with no extra slot. OWNED, JS_UNDEFINED when there is none. */
static JSValue io_reg_list(JSContext *ctx, JSValueConst wrap, int create)
{
    JSValue l;

    if (JS_GetOwnSlot(ctx, &l, wrap, g_atom_reg) > 0) {
        if (JS_IsObject(l)) return l;
        JS_FreeValue(ctx, l);
    }
    if (!create) return JS_UNDEFINED;
    l = JS_NewArray(ctx);
    CHECK(!JS_IsException(l), "an element's registered intersection observer list could not be allocated");
    JS_DefinePropertyValue(ctx, (JSValue)wrap, g_atom_reg, JS_DupValue(ctx, l), 0);
    return l;
}

/* §3.2.10 step 3.1's "the registration record whose observer property is equal to observer". OWNED,
   JS_UNDEFINED when the target carries none for this observer. */
static JSValue io_registration(JSContext *ctx, JSValueConst target, JSValueConst observer)
{
    JSValue list = io_reg_list(ctx, target, 0);
    uint32_t n, i;

    if (!JS_IsObject(list)) { JS_FreeValue(ctx, list); return JS_UNDEFINED; }
    n = io_len(ctx, list);
    for (i = 0; i < n; i++) {
        JSValue e = JS_GetPropertyUint32(ctx, list, i);
        JSValue ob = JS_GetPropertyUint32(ctx, e, IOR_OBSERVER);
        bool hit = JS_VALUE_GET_PTR(ob) == JS_VALUE_GET_PTR(observer);

        JS_FreeValue(ctx, ob);
        if (hit) { JS_FreeValue(ctx, list); return e; }
        JS_FreeValue(ctx, e);
    }
    JS_FreeValue(ctx, list);
    return JS_UNDEFINED;
}

/* ---- §3.1.1: the document's observers and its IntersectionObserverTaskQueued flag ------------------------- */

/* THIS REALM'S OBSERVER LIST. OWNED. It is per REALM and it holds the observers §3.2.10 step 1 and §3.2.5 step
   2 both ask for by the same words — "all IntersectionObservers whose root is in the DOM tree of document" —
   which is a LIVE query, so membership here is the registration and `io_root_in_tree` below is the query. */
static JSValue io_doc_list(JSContext *ctx)
{
    JSValue arr = realm_value_get(ctx, g_docobs_slot);

    DCHECK(JS_IsArray(arr),
           "a realm was asked for its Intersection Observer list before intersection_observer_install_proto "
           "built one — the list is a per-realm intrinsic and every realm goes through that one install");
    return arr;
}

/* WHICH DOCUMENT AN OBSERVER'S ROOT IS IN, as §2.2 defines it: the root's node document for an explicit root,
   and "the top-level browsing context's document node" — the IMPLICIT ROOT — for a null one. Answers the realm
   whose list the observer belongs on. */
static JSContext *io_root_document_realm(JSContext *ctx, JSValueConst root)
{
    JSValue top;
    JSContext *topctx;

    if (!JS_IsNull(root)) {
        lxb_dom_node_t *n = node_of(root);

        DCHECK(n != NULL, "an IntersectionObserver's root is neither null nor a node — §2.4 declares it "
                          "`(Element or Document)?` and the type is what refuses everything else");
        return document_realm_of(n);
    }
    top = window_proxy_top_navigable(ctx, document_window_proxy(ctx));
    DCHECK(window_proxy_materialized(top),
           "the top-level traversable is not materialized while a document inside it is constructing an "
           "implicit-root IntersectionObserver — this document is IN it, so it is the one navigable that "
           "cannot be deferred");
    topctx = window_proxy_realm(ctx, top);
    JS_FreeValue(ctx, top);
    return topctx;
}

/* §3.2.10 step 1 / §3.2.5 step 2's PREDICATE, asked of `ctx`'s document at the moment of the walk: is this
   observer's root still in its tree? An explicit root that has been removed from the document drops out of the
   list, which is observable — the observer stops being notified — and is why this is a query and not a flag
   set once at construction. */
static bool io_root_in_tree(JSContext *ctx, JSValueConst state)
{
    JSValue root = JS_GetPropertyUint32(ctx, (JSValue)state, IO_S_ROOT);
    lxb_dom_node_t *doc = document_root_node(ctx), *n;
    bool in;

    if (JS_IsNull(root)) {
        JS_FreeValue(ctx, root);
        /* THE IMPLICIT ROOT IS THE TOP-LEVEL BROWSING CONTEXT'S DOCUMENT, and registration put the observer on
           exactly that realm's list — so being on this list IS being in that tree, and the assert is what
           keeps that a derivation rather than an assumption. */
        DCHECK(window_proxy_is_top_level(document_window_proxy(ctx)),
               "an implicit-root IntersectionObserver is on a list belonging to a document that is not the "
               "top-level browsing context's — §2.2 makes the implicit root that document and nothing else");
        return true;
    }
    n = node_of(root);
    DCHECK(n != NULL, "an IntersectionObserver's root slot holds a non-null value that is not a node");
    in = n == doc || (lxb_dom_interface_node(n->owner_document) == doc && node_is_connected(n));
    JS_FreeValue(ctx, root);
    return in;
}

/* §3.2.4 "QUEUE AN INTERSECTION OBSERVER TASK for a document document":
     1. If document's IntersectionObserverTaskQueued flag is true, return.
     2. Set it to true.
     3. Queue a task ON THE INTERSECTIONOBSERVER TASK SOURCE associated with the document's event loop to
        notify intersection observers.
   THE FLAG IS WHAT MAKES MANY QUEUED ENTRIES IN ONE FRAME DELIVER ONE CALLBACK INVOCATION carrying all of
   them, which is the observable behaviour the whole design is for. It rides the list Array, so it is a property
   write the COW delta captures and two arms hold two flags.
   IT IS A TASK AND NOT A MICROTASK. §3.2.4 says "queue a task", and the event loop will not begin one until
   every outstanding microtask has run — choosing the other entry would reorder what the page observes. */
static void io_queue_task(JSContext *ctx)
{
    JSValue list = io_doc_list(ctx), f, fn;
    int set;

    f = JS_GetProperty(ctx, list, g_atom_queued);
    set = JS_ToBool(ctx, f);
    JS_FreeValue(ctx, f);
    if (set) { JS_FreeValue(ctx, list); return; }
    JS_SetProperty(ctx, list, g_atom_queued, JS_TRUE);
    JS_FreeValue(ctx, list);
    fn = realm_value_get(ctx, g_notify_slot);
    DCHECK(JS_IsFunction(ctx, fn),
           "an intersection observer task was queued in a realm that never built its notification driver");
    JS_EnqueueCallTask(ctx, fn, 0, NULL);
    JS_FreeValue(ctx, fn);
}

/* ---- §2.2 "parse a margin" -------------------------------------------------------------------------------- */

/* §2.2's PARSE A MARGIN, over CSS Syntax's own component values — lexbor's tokenizer, which is what
   core/css/css_at_rule_prelude.c and core/css/css_supports.c already read CSS text with, so a margin with a CSS comment
   between its components, and a `5px,10px`, are told apart by the same tokenizer the rest of this engine uses
   rather than by a split on spaces. The steps, in order:
     1. Parse a list of component values marginString, storing the result as tokens.
     2. Remove all whitespace tokens from tokens.
     3. If the length of tokens is greater than 4, return failure.
     4. If there are zero elements in tokens, set tokens to ["0px"].
     5. Replace each token in tokens: an ABSOLUTE LENGTH dimension token becomes an equivalent pixel length, a
        <percentage> token becomes an equivalent percentage, and anything else RETURNS FAILURE.
     6. Expand 1, 2 or 3 elements to 4 by the CSS `margin` rule.
     7. Return tokens.
   STEP 5 IS WHY THIS DOES NOT GO THROUGH css_length_parse: `2em` is a length dimension token and NOT an
   absolute one, so §2.2 makes it a failure — and that function CRASHES for a font-relative unit rather than
   refusing it, correctly, because its own caller has a validated declaration in hand and this one does not.
   css_length.h's `css_length_absolute_px` is the one table both of them read.
   Writes 8 numbers into `out` (see IO_MARGIN_*) and answers false for §2.2's failure, which the caller turns
   into §3.2.1's SyntaxError. */
static bool io_parse_margin(const char *s, size_t len, double out[IO_MARGIN_LEN])
{
    lxb_css_syntax_tokenizer_t *tkz;
    double kind[IO_MARGIN_SIDES], num[IO_MARGIN_SIDES];
    unsigned n = 0, i;
    bool ok = true;

    tkz = lxb_css_syntax_tokenizer_create();
    CHECK(tkz != NULL, "a CSS tokenizer could not be allocated for an Intersection Observer margin");
    CHECK(lxb_css_syntax_tokenizer_init(tkz) == LXB_STATUS_OK,
          "a CSS tokenizer could not be initialized for an Intersection Observer margin");
    lxb_css_syntax_tokenizer_buffer_set(tkz, (const lxb_char_t *)s, len);
    for (;;) {
        lxb_css_syntax_token_t *t = lxb_css_syntax_token(tkz);

        if (t == NULL) { ok = false; break; }                    /* the tokenizer's own allocation failure */
        if (t->type == LXB_CSS_SYNTAX_TOKEN__EOF) break;
        if (t->type == LXB_CSS_SYNTAX_TOKEN_WHITESPACE) {        /* step 2 */
            lxb_css_syntax_token_consume(tkz);
            continue;
        }
        if (n >= IO_MARGIN_SIDES) { ok = false; break; }         /* step 3 */
        if (t->type == LXB_CSS_SYNTAX_TOKEN_DIMENSION) {         /* step 5.1 */
            const lxb_css_syntax_token_string_t *u = lxb_css_syntax_token_dimension_string(t);
            double px;

            if (!css_length_absolute_px((const char *)u->data, u->length,
                                        lxb_css_syntax_token_dimension(t)->num.num, &px)) {
                ok = false;                                      /* step 5.3: a relative unit is a failure */
                break;
            }
            kind[n] = IO_MARGIN_PX;
            num[n] = px;
        } else if (t->type == LXB_CSS_SYNTAX_TOKEN_PERCENTAGE) { /* step 5.2 */
            kind[n] = IO_MARGIN_PCT;
            num[n] = lxb_css_syntax_token_percentage(t)->num;
        } else {
            ok = false;                                          /* step 5.3 */
            break;
        }
        n++;
        lxb_css_syntax_token_consume(tkz);
    }
    lxb_css_syntax_tokenizer_destroy(tkz);
    if (!ok) return false;
    if (n == 0) {                                                /* step 4: tokens becomes ["0px"] */
        kind[0] = IO_MARGIN_PX;
        num[0] = 0.0;
        n = 1;
    }
    /* Step 6 — one element duplicates three times, two duplicate each, three duplicate the second. */
    if (n == 1) { kind[1] = kind[2] = kind[3] = kind[0]; num[1] = num[2] = num[3] = num[0]; }
    else if (n == 2) { kind[2] = kind[0]; num[2] = num[0]; kind[3] = kind[1]; num[3] = num[1]; }
    else if (n == 3) { kind[3] = kind[1]; num[3] = num[1]; }
    for (i = 0; i < IO_MARGIN_SIDES; i++) {
        out[i * 2] = kind[i];
        out[i * 2 + 1] = num[i];
    }
    return true;
}

/* §2.2's `rootMargin` / `scrollMargin` GETTERS — "the result of serializing the elements of [[rootMargin]]
   space-separated, where pixel lengths serialize as the numeric value followed by `px` and percentages as the
   numeric value followed by `%`". CSSOM §6.7.2's number serialization is what css_length.h's two entries give,
   which is why "0px 0px 0px 0px" comes back and not "0.000000px". */
static JSValue io_serialize_margin(JSContext *ctx, JSValueConst margin)
{
    char buf[256];
    size_t at = 0;
    uint32_t i;

    buf[0] = '\0';
    for (i = 0; i < IO_MARGIN_SIDES; i++) {
        double kind = io_num_at(ctx, margin, i * 2), v = io_num_at(ctx, margin, i * 2 + 1);
        char *one = kind == IO_MARGIN_PCT ? css_length_serialize_pct(v) : css_length_serialize_px(v);
        size_t n;

        CHECK(one != NULL, "a margin component could not be serialized");
        n = strlen(one);
        CHECK(at + n + 2 <= sizeof buf,
              "an Intersection Observer margin serialized past the buffer four CSS lengths and three spaces "
              "fit in — every component is a number CSSOM §6.7.2 writes in its shortest round-tripping form "
              "plus at most two characters of unit, so a longer one means a component that is not a length");
        if (at) buf[at++] = ' ';
        memcpy(buf + at, one, n);
        at += n;
        buf[at] = '\0';
        free(one);
    }
    return JS_NewString(ctx, buf);
}

/* ---- §2.2's ROOT INTERSECTION RECTANGLE and §3.2.7's INTERSECTION ----------------------------------------- */

/* The rectangle of `ctx`'s viewport, at the client origin — §2.2's "if the intersection root is a document,
   it's the size of the document's viewport". The two dimensions carry the environment fact they derive from
   (viewport.h), so an entry's `rootBounds.height` is the same forkable question `innerHeight` is. */
static IoRect io_viewport_rect(JSContext *ctx)
{
    IoRect r;

    DCHECK(viewport_exists(ctx),
           "§2.2's root intersection rectangle was asked of a document NO NAVIGABLE IS PRESENTING — §3.2.10 "
           "note says this step can only be reached if the document is fully active, and update-the-rendering "
           "step 2 collects only fully active documents");
    r.x = css_px(0.0);
    r.y = css_px(0.0);
    r.w = viewport_icb_width(ctx);
    r.h = viewport_icb_height(ctx);
    return r;
}

/* CSSOM VIEW's "CONTENT CLIP" as §2.2 uses it — "an Element is defined as having a content clip if its computed
   style has overflow properties that cause its content to be clipped to the element's padding edge". The
   computed values are the cascade's (core/css/css_computed_value.h); everything but `visible` clips. */
static bool io_has_content_clip(lxb_dom_element_t *el)
{
    static const char *const AXES[2] = { "overflow-x", "overflow-y" };
    int i;

    for (i = 0; i < 2; i++) {
        char *v = css_computed_value(el, AXES[i]);
        bool clips;

        DCHECK(v != NULL, "the cascade produced no computed `overflow-x`/`overflow-y` for an element — every "
                          "property this engine asks for has an initial value in the UA sheet");
        clips = strcmp(v, "visible") != 0;
        free(v);
        if (clips) return true;
    }
    return false;
}

/* §6's get-the-bounding-box for `n`, in this engine's own vocabulary. */
static IoRect io_bounding_box(lxb_dom_node_t *n)
{
    CssPx b[4];
    IoRect r;

    element_view_bounding_box_px(lxb_dom_interface_element(n), b);
    r.x = b[0]; r.y = b[1]; r.w = b[2]; r.h = b[3];
    return r;
}

/* §2.2's ROOT INTERSECTION RECTANGLE, with §2.2's rootMargin dilation applied.
   THE DILATION IS UNCONDITIONAL HERE, and that is a derivation rather than a shortcut past §2.2's "these
   offsets are only applied when handling SAME-ORIGIN-DOMAIN targets". A target reaches this component only
   through `observe(Element target)`, whose argument is an element of THIS agent's heap — and an agent is one
   origin-keyed agent cluster (CLAUDE.md §Security), so a cross-origin-domain target is in another WASM instance
   and its element cannot cross. Every target this file can be given is same-origin-domain, which is also why
   `rootBounds` is never the null §2.3 gives a cross-origin-domain target. */
static IoRect io_root_bounds(JSContext *ctx, JSValueConst state)
{
    JSValue root = JS_GetPropertyUint32(ctx, (JSValue)state, IO_S_ROOT);
    JSValue margin = JS_GetPropertyUint32(ctx, (JSValue)state, IO_S_ROOT_MARGIN);
    IoRect r;
    CssPx off[IO_MARGIN_SIDES];
    uint32_t i;

    if (JS_IsNull(root)) {
        r = io_viewport_rect(ctx);                                  /* the implicit root */
    } else {
        lxb_dom_node_t *n = node_of(root);

        if (n->type == LXB_DOM_NODE_TYPE_DOCUMENT) {
            /* §2.2: "if the intersection root is a DOCUMENT, it's the size of the document's viewport." That
               document is THIS one — `io_root_in_tree` has already refused an observer whose Document root is
               not this realm's — so the viewport asked for is `ctx`'s, and the assert is what keeps that a
               derivation rather than a second lookup free to disagree. */
            DCHECK(n == document_root_node(ctx),
                   "§2.2's root intersection rectangle was asked for a DOCUMENT root that is not the document "
                   "being walked — §3.2.10 step 1 collects only observers whose root is in this tree");
            r = io_viewport_rect(ctx);
        } else if (io_has_content_clip(lxb_dom_interface_element(n))) {
            DFAIL("INTERSECTION OBSERVER §2.2's root intersection rectangle for an EXPLICIT ELEMENT ROOT WITH "
                  "A CONTENT CLIP is 'the element's PADDING AREA' — one box's padding extent (CSS 2.1 §8.1, "
                  "which core/layout/used_value.h's `used_value_padding_edge_px` already computes) at that "
                  "box's padding-edge POSITION, which is its border-box origin plus §8.1's leading border "
                  "width. core/dom/element_view.c has both halves for the BORDER area and neither is exported "
                  "for the padding one. BUILD the padding area beside `element_view_bounding_box_px`, in "
                  "element_view.c where §6's box model already is, and read it here — an unclipped root takes "
                  "get-the-bounding-box below and is already answered");
            r = io_bounding_box(n);
        } else {
            r = io_bounding_box(n);
        }
    }
    /* §2.2's dilation: the four values offset the top, right, bottom and left edges OUTWARD, and a percentage
       is "resolved relative to the WIDTH of the undilated rectangle" — all four of them, which is what the
       section says and is not the CSS `margin` rule for a vertical percentage. */
    for (i = 0; i < IO_MARGIN_SIDES; i++) {
        double kind = io_num_at(ctx, margin, i * 2), v = io_num_at(ctx, margin, i * 2 + 1);

        off[i] = kind == IO_MARGIN_PCT ? css_px_scale(r.w, v / 100.0) : css_px(v);
    }
    r.x = css_px_sub(r.x, off[IO_MARGIN_LEFT]);
    r.y = css_px_sub(r.y, off[IO_MARGIN_TOP]);
    r.w = css_px_add(r.w, css_px_add(off[IO_MARGIN_LEFT], off[IO_MARGIN_RIGHT]));
    r.h = css_px_add(r.h, css_px_add(off[IO_MARGIN_TOP], off[IO_MARGIN_BOTTOM]));
    JS_FreeValue(ctx, margin);
    JS_FreeValue(ctx, root);
    return r;
}

/* THE INTERSECTION OF TWO RECTANGLES, on the examples, carrying both operands' facts. A degenerate result is
   floored at zero rather than reported negative, which is what "the portion of target that intersects" means
   and what every edge comparison below then reads. */
static IoRect io_intersect(IoRect a, IoRect b)
{
    CssPx l = css_px_max(a.x, b.x);
    CssPx t = css_px_max(a.y, b.y);
    CssPx rr = css_px_min(css_px_add(a.x, a.w), css_px_add(b.x, b.w));
    CssPx bb = css_px_min(css_px_add(a.y, a.h), css_px_add(b.y, b.h));
    IoRect out;

    out.x = l;
    out.y = t;
    out.w = css_px_max(css_px(0.0), css_px_sub(rr, l));
    out.h = css_px_max(css_px(0.0), css_px_sub(bb, t));
    return out;
}

/* §3.2.7 "COMPUTE THE INTERSECTION of a target target and an intersection root root":
     1. Let intersectionRect be the result of getting the bounding box for target.
     2. Let container be the containing block of target.
     3. While container is not root: clip to a nested browsing context's viewport, map to container's space,
        apply scrollMargin to a scroll container's clip rect, apply a content clip or a css clip-path, and step
        to the next containing block.
     4. Map intersectionRect to the coordinate space of root.
     5. Update intersectionRect by intersecting it with the root intersection rectangle.
     6. Map intersectionRect to the coordinate space of the viewport of the document containing target.
   STEP 3's BODY IS FOUR CONDITIONS AND THIS ENGINE HAS NONE OF THEM, so the walk asks each one and CRASHES at
   the first container that answers yes, naming what that container needs. A container that answers no to all
   four contributes NOTHING to the rectangle — that is the loop body doing nothing, not a step being skipped —
   so an unclipped chain is a real derivation and not a shrug.
   STEPS 4 AND 6 ARE IDENTITIES IN THIS MODEL and are written as the derivation rather than omitted: every
   rectangle in this file is already in CLIENT COORDINATES (core/dom/element_view.c reports the border area
   there, subtracting the viewport's one valid scroll position), and the mapping between two boxes' spaces is
   the translation a TRANSFORM would carry — which element_view.c crashes for by name before any rectangle with
   a box in it reaches here. */
static IoRect io_compute_intersection(JSContext *ctx, lxb_dom_node_t *target, JSValueConst state, IoRect root)
{
    lxb_dom_element_t *container;
    JSValue rootv = JS_GetPropertyUint32(ctx, (JSValue)state, IO_S_ROOT);
    lxb_dom_node_t *rootn = JS_IsNull(rootv) ? NULL : node_of(rootv);
    JSContext *tctx = document_active_realm_of(lxb_dom_interface_node(target->owner_document));
    IoRect r = io_bounding_box(target);                                   /* step 1 */

    JS_FreeValue(ctx, rootv);
    /* A TARGET THAT GENERATES NO BOX HAS NO CONTAINING BLOCK, so step 2 has nothing to bind and step 3's walk
       cannot start — CSS 2.1 §10.1 is defined over BOXES, and core/dom/element_view.h's one predicate is what
       decides whether there is one. Step 1's rectangle is then §6's own zero rectangle (get-the-bounding-box
       step 2, "a DOMRect whose x, y, width and height members are zero", which every user agent computes
       identically for such an element), and steps 4 to 6 map and intersect it. That is the loop body doing
       nothing rather than a step being skipped, and it is the case a lazy-loading bundle spends most of its
       observations in: a target it has not inserted yet, or one its own CSS has set to `display: none`. */
    if (!element_view_has_box(target)) {
        r = io_intersect(r, root);                                        /* step 5 */
        return r;
    }
    container = used_value_containing_block(lxb_dom_interface_element(target));   /* step 2 */
    for (;;) {                                                            /* step 3 */
        if (container == NULL) {
            /* THE CHAIN HAS REACHED §10.1's INITIAL CONTAINING BLOCK, which is step 3.5's "if container is the
               root element of a browsing context, update container to be the browsing context's DOCUMENT".
               That document ends the walk when it IS the intersection root — the implicit root is the
               top-level browsing context's document (§2.2), and an explicit Document root is this one.
               Otherwise the next container is step 3.1's nested-browsing-context case. */
            if (rootn == NULL) {
                if (window_proxy_is_top_level(document_window_proxy(tctx))) break;
            } else if (rootn->type == LXB_DOM_NODE_TYPE_DOCUMENT) {
                if (rootn == lxb_dom_interface_node(target->owner_document)) break;
            } else {
                /* An ELEMENT root the chain never met — step 3.6 already refused that target and skipped to
                   step 11, so this walk was never entered for it. */
                DFAIL("INTERSECTION OBSERVER §3.2.7's walk reached the initial containing block without meeting "
                      "an ELEMENT intersection root — §3.2.10 step 3.6 skips a target that is not a descendant "
                      "of the root in the containing block chain, so this walk cannot have been entered");
            }
            DFAIL("INTERSECTION OBSERVER §3.2.7 step 3.1: the walk has reached 'the DOCUMENT of a NESTED "
                  "BROWSING CONTEXT', which clips the rectangle to that document's VIEWPORT and then continues "
                  "at the BROWSING CONTEXT CONTAINER — so the walk crosses a document boundary and the "
                  "rectangle crosses with it, into the parent's coordinate space. This engine has the second "
                  "document (an instance is an origin-keyed agent cluster) and has NO MAPPING between two "
                  "navigables' client frames: that mapping is the iframe element's own border-box origin "
                  "(core/layout/flow_position.h) plus CSS 2.1 §8.1's leading border and padding. BUILD it, "
                  "clip to core/frame/viewport.h's rectangle for the inner document, and continue the walk at "
                  "the container element — and note that a CROSS-ORIGIN parent is a different instance again, "
                  "so the mapping is a cross-instance read of the kind SECURITY.md's boundary already carries");
            break;
        }
        if (lxb_dom_interface_node(container) == rootn) break;            /* step 3's own condition */
        /* STEP 3.3 AND STEP 3.4 ARE ONE QUESTION IN THIS MODEL, because CSSOM VIEW defines a SCROLL CONTAINER
           by the same overflow properties a CONTENT CLIP is defined by — so `[[scrollMargin]]`, which §3.2.1
           parses and §2.2 serializes, is applied at exactly the container this crash stands on and nowhere
           else. A reader looking for where scrollMargin is APPLIED finds the answer here. */
        if (io_has_content_clip(container))
            DFAIL("INTERSECTION OBSERVER §3.2.7 steps 3.3 and 3.4: a container that is a SCROLL CONTAINER has "
                  "the observer's [[scrollMargin]] applied to its clip rect ('apply scroll margin to a "
                  "scrollport', §2.2), and a container with a CONTENT CLIP or a `clip-path` updates the "
                  "rectangle 'by applying container's clip' — CSSOM VIEW's clip to the PADDING EDGE, which is "
                  "one box's padding extent at its padding-edge position. core/layout/used_value.h computes "
                  "the extent (`used_value_padding_edge_px`) and core/layout/flow_position.h the border-box "
                  "origin; what is missing is the PADDING AREA as one rectangle, which §2.2's explicit-root "
                  "branch in this file names the same absence for. A `clip-path` needs the computed value "
                  "core/css/css_computed_value.c crashes for. BUILD the padding area in element_view.c beside "
                  "`element_view_bounding_box_px`, then apply the scroll margin to it and intersect here");
        /* STEP 3.2 — "map intersectionRect to the coordinate space of container" — is an IDENTITY in this
           model and is written as the derivation rather than omitted: every rectangle here is already in
           CLIENT coordinates (element_view.c reports the border area there), and what would make two boxes'
           spaces differ is a TRANSFORM, which element_view.c crashes for by name before any rectangle with a
           box in it reaches this function. */
        container = used_value_containing_block(container);               /* step 3.5 */
    }
    r = io_intersect(r, root);                                            /* step 5 */
    return r;                                       /* steps 4 and 6: identities, for step 3.2's reason */
}

/* §3.2.8 "COMPUTE THE VISIBILITY of a target":
     1. If the observer's trackVisibility attribute is false, return false.
     2..5. transformation matrix, opacity, filters, occlusion.
     6. Return true.
   Step 1 is a REAL computed answer and the whole of the algorithm for every observer that did not opt in,
   which is what makes `isVisible` false rather than a stub. */
static bool io_compute_visibility(JSContext *ctx, JSValueConst state)
{
    JSValue tv = JS_GetPropertyUint32(ctx, (JSValue)state, IO_S_TRACK_VISIBILITY);
    bool track = JS_ToBool(ctx, tv) > 0;

    JS_FreeValue(ctx, tv);
    if (!track) return false;                                             /* step 1 */
    DFAIL("INTERSECTION OBSERVER §3.2.8 steps 2 to 5 decide whether a target is UNOCCLUDED, UNTRANSFORMED, "
          "UNFILTERED AND OPAQUE, and this engine can answer none of the four: step 2 needs §3.2.9's EFFECTIVE "
          "TRANSFORMATION MATRIX (a post-multiplication up the containing block chain of each box's computed "
          "`transform`, which core/css/css_computed_value.c's CSS_RESOLVED_TRANSFORM crash asks for and which "
          "core/dom/element_view.c's getClientRects step 3 names the same absence for); steps 3 and 4 need the "
          "computed `opacity` and `filter` of the target and of every element in its containing block chain; "
          "step 5 needs the ink overflow rectangles of the page's other content, which is a PAINT ORDER this "
          "engine does not build. BUILD the computed transform first — it unblocks §3.2.9, this step, and "
          "getClientRects' first constraint together — then opacity and filter, then step 5's occlusion. An "
          "observer that did NOT ask for `trackVisibility` never reaches here: step 1 answers it, above");
    /* A RELEASE BUILD CANNOT BUILD THE FOUR MISSING COMPONENTS, so it answers the one outcome this model can
       justify — steps 2 to 5 each RETURN FALSE when their condition is not met, and an engine that cannot see
       a transform, an opacity, a filter or an occluder cannot claim to have ruled them out. */
    return false;
}

/* ---- §3.2.6 and §3.2.10 ----------------------------------------------------------------------------------- */

/* The four numbers of a rectangle, crossing viewport.h's ONE SEAM to become what the page reads. This is the
   only place in this component where a geometry becomes a JS value. */
static JSValue io_rect_value(JSContext *rctx, IoRect r)
{
    return dom_rect_readonly_new_values(rctx,
                                        viewport_env_derived(r.x, JS_NewFloat64(rctx, r.x.px)),
                                        viewport_env_derived(r.y, JS_NewFloat64(rctx, r.y.px)),
                                        viewport_env_derived(r.w, JS_NewFloat64(rctx, r.w.px)),
                                        viewport_env_derived(r.h, JS_NewFloat64(rctx, r.h.px)));
}

/* §3.2.6 "QUEUE AN IntersectionObserverEntry":
     1. Construct an IntersectionObserverEntry, passing in time, rootBounds, boundingClientRect,
        intersectionRect, isIntersecting and target.
     2. Append it to observer's internal [[QueuedEntries]] slot.
     3. Queue an intersection observer task for document.
   The entry is minted in the OBSERVER'S realm — §2.3 makes its `time` relative to that global's time origin and
   Web IDL makes a [NewObject] there — while step 3's task is queued for the DOCUMENT this walk is for, which is
   `ctx`. Those are two different realms whenever an implicit-root observer was constructed inside an iframe. */
static void io_queue_entry(JSContext *ctx, JSValueConst observer, JSValueConst state, JSValueConst frame_ts,
                           IoRect root, IoRect target_rect, IoRect inter, bool is_intersecting,
                           bool is_visible, CssPx ratio, JSValueConst target)
{
    JSContext *rctx = io_realm(ctx, state);
    JSValue entries = JS_GetPropertyUint32(ctx, (JSValue)state, IO_S_ENTRIES);
    JSValue entry;
    double entry_time = 0;

    (void)observer;
    /* §2.3's `time` — the relative high resolution time of the frame's moment, in the OBSERVER's environment.
       It is flattened to a double here because that is what the entry's own component takes, and the flatten
       is SOUND rather than a collapse: io_update_target's step 3.2 crashes on an unknown frame timestamp
       before this walk is reached, so the only moments that get here are ones the clock computed. The day
       that seam is built this reads a value and so does the entry. */
    {
        JSValue t = hr_time_relative(rctx, frame_ts);

        DCHECK(JS_IsNumber(t),
               "§2.3's IntersectionObserverEntry time is not a number — §3.2.10 step 3.2 refuses an unknown "
               "frame timestamp above, so nothing unknown can have reached this derivation");
        JS_ToFloat64(rctx, &entry_time, t);
        JS_FreeValue(rctx, t);
    }
    entry = intersection_observer_entry_new(rctx, entry_time,
                                            io_rect_value(rctx, root), io_rect_value(rctx, target_rect),
                                            io_rect_value(rctx, inter), is_intersecting, is_visible,
                                            viewport_env_derived(ratio, JS_NewFloat64(rctx, ratio.px)),
                                            target);
    io_push(ctx, entries, entry);                                         /* step 2 */
    JS_FreeValue(ctx, entries);
    io_queue_task(ctx);                                                   /* step 3 */
}

/* §3.2.10 step 3.6's "target is a DESCENDANT OF THE INTERSECTION ROOT IN THE CONTAINING BLOCK CHAIN". */
static bool io_in_containing_chain(lxb_dom_node_t *target, lxb_dom_node_t *root)
{
    lxb_dom_element_t *c;

    /* A TARGET THAT GENERATES NO BOX IS IN NO CONTAINING BLOCK CHAIN AT ALL — §10.1 is defined over boxes — so
       it is not a descendant of the root in one, and step 3.6 skips it to step 11. */
    if (!element_view_has_box(target)) return false;
    c = used_value_containing_block(lxb_dom_interface_element(target));
    while (c != NULL) {
        if (lxb_dom_interface_node(c) == root) return true;
        c = used_value_containing_block(c);
    }
    return false;
}

/* §3.2.10 step 3, for ONE target of ONE observer. The step numbers are the section's own. */
static void io_update_target(JSContext *ctx, JSValueConst observer, JSValueConst state, JSValueConst target,
                             IoRect root, JSValueConst frame_ts)
{
    JSValue reg = io_registration(ctx, target, observer);                 /* step 3.1 */
    JSValue thresholds;
    lxb_dom_node_t *tn = node_of(target), *rootn;
    IoRect target_rect, inter;
    CssPx target_area, inter_area, ratio;
    double delay, last, prev_index, ts;
    uint32_t nth, k, threshold_index = 0;
    bool is_intersecting = false, is_visible = false, prev_intersecting, prev_visible, skipped = false;
    JSValue v;

    DCHECK(JS_IsObject(reg),
           "§3.2.10 step 3.1 found no registration record for a target in an observer's "
           "[[ObservationTargets]] — §3.2.2 appends the two together and §3.2.3 removes them together");
    v = JS_GetPropertyUint32(ctx, (JSValue)state, IO_S_DELAY);
    JS_ToFloat64(ctx, &delay, v);
    JS_FreeValue(ctx, v);
    last = io_num_at(ctx, reg, IOR_LAST_UPDATE);
    /* STEP 3.2's THROTTLE IS A COMPARISON, AND OVER AN UNKNOWN MOMENT IT IS A FORK THIS COMPONENT CANNOT ASK.
       `frameTimestamp` is HTML §8.1.7.3 step 1's last render opportunity time, a moment on the event loop's
       one virtual clock — and that clock becomes unknown external input the moment a timer set with an
       unknown `timeout` fires (core/timing/event_loop.h). `time - time of last update < delay` then has two
       real answers, "the target is skipped this frame" and "it is not", and both are programs the page can
       observe: an observer whose callback never runs and one whose does.
       IT CANNOT BE ASKED HERE, and that is a missing SEAM rather than a missing decision. §3.2.10 runs inside
       HTML §8.1.7.3's update-the-rendering step machine, and a fork needs a resume point the forking site
       owns — the machine's own `step_fork_run`, reached through its JSStepHdr. This walk is a plain C
       activation several calls below that machine and is handed no header, so there is nothing for the
       sibling to be rebuilt from. WHAT TO BUILD is the header down this path: `intersection_observer_update`
       and `io_update_target` take the update-the-rendering machine's JSStepHdr, this test becomes a
       `step_fork_run` over the difference (which is ECMAScript §13.8.2 The Subtraction Operator ( - ), run —
       event_loop_moment_plus's
       twin), and the walk's cursors (`s->ob`, and a per-target index this component does not yet park on)
       become stages the sibling re-enters at. Until then the honest state is a crash naming it, never a
       number picked for `frameTimestamp` — which would decide the throttle for every observer in the
       document from inside an accessor. */
    if (concolic_is(frame_ts))
        DFAIL("§3.2.10 step 3.2's throttle compares the frame timestamp against this registration's last "
              "update, and the frame timestamp is an UNKNOWN moment — the event loop's clock has been moved "
              "there by a timer whose `timeout` was unknown external input. Both outcomes of the comparison "
              "are real programs, so it is a FORK, and this walk has no seam to fork at: thread HTML "
              "§8.1.7.3's update-the-rendering JSStepHdr through intersection_observer_update and "
              "io_update_target and ask it with step_fork_run (see the paragraph at this line)");
    JS_ToFloat64(ctx, &ts, frame_ts);
    if (ts - last < delay) { JS_FreeValue(ctx, reg); return; }            /* step 3.2 */
    JS_SetPropertyUint32(ctx, reg, IOR_LAST_UPDATE, JS_NewFloat64(ctx, ts));         /* step 3.3 */

    /* step 3.4 */
    target_rect.x = target_rect.y = target_rect.w = target_rect.h = css_px(0.0);
    inter = target_rect;

    v = JS_GetPropertyUint32(ctx, (JSValue)state, IO_S_ROOT);
    rootn = JS_IsNull(v) ? NULL : node_of(v);
    JS_FreeValue(ctx, v);
    if (rootn != NULL && lxb_dom_interface_node(tn->owner_document) !=
                         (rootn->type == LXB_DOM_NODE_TYPE_DOCUMENT
                          ? rootn : lxb_dom_interface_node(rootn->owner_document)))
        skipped = true;                                                   /* step 3.5 */
    else if (rootn != NULL && rootn->type == LXB_DOM_NODE_TYPE_ELEMENT && !io_in_containing_chain(tn, rootn))
        skipped = true;                                                   /* step 3.6 */
    if (!skipped) {
        target_rect = io_bounding_box(tn);                                /* step 3.7 */
        inter = io_compute_intersection(ctx, tn, state, root);            /* step 3.8 */
    }
    target_area = css_px_mul(target_rect.w, target_rect.h);               /* step 3.9 */
    inter_area = css_px_mul(inter.w, inter.h);                            /* step 3.10 */
    /* STEP 3.11 — "isIntersecting is true if targetRect and rootBounds INTERSECT OR ARE EDGE-ADJACENT, even if
       the intersection has zero area (because rootBounds or targetRect have zero area)". The comparison is on
       the EXAMPLES: this is C and cannot fork, and the examples are what css_length.h says C compares.
       IT IS ASKED OF targetRect AND rootBounds and NOT of the intersection, which is the section's own wording
       and is what makes a zero-area target that touches the root report true — §2.3's `isIntersecting` prose
       states that case explicitly ("a transition from not-intersecting to intersecting with a zero-area
       intersection rect … or when the boundingClientRect has zero area"). */
    is_intersecting = target_rect.x.px <= root.x.px + root.w.px &&
                      target_rect.x.px + target_rect.w.px >= root.x.px &&
                      target_rect.y.px <= root.y.px + root.h.px &&
                      target_rect.y.px + target_rect.h.px >= root.y.px;
    /* step 3.12 */
    ratio = target_area.px != 0.0 ? css_px_div(inter_area, target_area)
                                  : css_px(is_intersecting ? 1.0 : 0.0);
    /* STEP 3.13 — "the index of the FIRST entry in observer.thresholds whose value is GREATER THAN
       intersectionRatio, or the LENGTH of thresholds if intersectionRatio is greater than or equal to the last
       entry". The list is sorted ascending by §3.2.1 step 7, so the first such index is the walk's first hit
       and the fall-through is the length. */
    thresholds = JS_GetPropertyUint32(ctx, (JSValue)state, IO_S_THRESHOLDS);
    nth = io_len(ctx, thresholds);
    DCHECK(nth > 0, "an IntersectionObserver's thresholds list is empty — §3.2.1 step 8 appends 0 to an empty "
                    "one, so the list always has at least one entry by the time the constructor returns");
    threshold_index = nth;
    for (k = 0; k < nth; k++)
        if (io_num_at(ctx, thresholds, k) > ratio.px) { threshold_index = k; break; }
    JS_FreeValue(ctx, thresholds);

    is_visible = io_compute_visibility(ctx, state);                       /* step 3.14 */
    prev_index = io_num_at(ctx, reg, IOR_PREV_THRESHOLD);                 /* step 3.15 */
    v = JS_GetPropertyUint32(ctx, reg, IOR_PREV_INTERSECTING);            /* step 3.16 */
    prev_intersecting = JS_ToBool(ctx, v) > 0;
    JS_FreeValue(ctx, v);
    v = JS_GetPropertyUint32(ctx, reg, IOR_PREV_VISIBLE);                 /* step 3.17 */
    prev_visible = JS_ToBool(ctx, v) > 0;
    JS_FreeValue(ctx, v);
    if ((double)threshold_index != prev_index || is_intersecting != prev_intersecting ||
        is_visible != prev_visible)                                       /* step 3.18 */
        io_queue_entry(ctx, observer, state, frame_ts, root, target_rect, inter, is_intersecting, is_visible,
                       ratio, target);
    JS_SetPropertyUint32(ctx, reg, IOR_PREV_THRESHOLD, JS_NewFloat64(ctx, (double)threshold_index)); /* 3.19 */
    JS_SetPropertyUint32(ctx, reg, IOR_PREV_INTERSECTING, JS_NewBool(ctx, is_intersecting));         /* 3.20 */
    JS_SetPropertyUint32(ctx, reg, IOR_PREV_VISIBLE, JS_NewBool(ctx, is_visible));                   /* 3.21 */
    JS_FreeValue(ctx, reg);
}

uint32_t intersection_observer_count(JSContext *ctx)
{
    JSValue list = io_doc_list(ctx);
    uint32_t n = io_len(ctx, list);

    JS_FreeValue(ctx, list);
    return n;
}

void intersection_observer_update(JSContext *ctx, uint32_t i, JSValueConst frame_ts)
{
    JSValue list = io_doc_list(ctx), observer, state, targets;
    IoRect root;
    uint32_t n, k;

    DCHECK(i < io_len(ctx, list), "§3.2.10 step 2 was asked for an observer past the end of the document's "
                                  "list — the count and the walk are one snapshot");
    observer = JS_GetPropertyUint32(ctx, list, i);
    JS_FreeValue(ctx, list);
    state = io_state(ctx, observer);
    DCHECK(!JS_IsException(state), "a document's Intersection Observer list holds something that is not an "
                                   "IntersectionObserver");
    if (!io_root_in_tree(ctx, state)) {                                   /* step 1's own predicate */
        JS_FreeValue(ctx, state);
        JS_FreeValue(ctx, observer);
        return;
    }
    root = io_root_bounds(ctx, state);                                    /* step 2.1 */
    targets = JS_GetPropertyUint32(ctx, state, IO_S_TARGETS);
    n = io_len(ctx, targets);
    /* STEP 2.2 — "for each target … PROCESSED IN THE SAME ORDER THAT observe() WAS CALLED on each target",
       which is the order §3.2.2 step 4 appended them in and therefore index order. */
    for (k = 0; k < n; k++) {
        JSValue target = JS_GetPropertyUint32(ctx, targets, k);

        io_update_target(ctx, observer, state, target, root, frame_ts);
        JS_FreeValue(ctx, target);
    }
    JS_FreeValue(ctx, targets);
    JS_FreeValue(ctx, state);
    JS_FreeValue(ctx, observer);
}

bool intersection_observer_pending(JSContext *ctx)
{
    JSValue list = io_doc_list(ctx);
    uint32_t n = io_len(ctx, list), i, k, nt;
    bool pending = false;

    for (i = 0; i < n && !pending; i++) {
        JSValue observer = JS_GetPropertyUint32(ctx, list, i);
        JSValue state = io_state(ctx, observer), targets;

        DCHECK(!JS_IsException(state), "a document's Intersection Observer list holds something that is not "
                                       "an IntersectionObserver");
        if (!io_root_in_tree(ctx, state)) {
            JS_FreeValue(ctx, state);
            JS_FreeValue(ctx, observer);
            continue;
        }
        targets = JS_GetPropertyUint32(ctx, state, IO_S_TARGETS);
        nt = io_len(ctx, targets);
        for (k = 0; k < nt && !pending; k++) {
            JSValue target = JS_GetPropertyUint32(ctx, targets, k);
            JSValue reg = io_registration(ctx, target, observer);

            /* §3.4.2's second condition, derived: an entry has never been queued for this target exactly while
               its previousThresholdIndex is still the −1 §3.2.2 set. */
            if (JS_IsObject(reg) && io_num_at(ctx, reg, IOR_PREV_THRESHOLD) < 0.0) pending = true;
            JS_FreeValue(ctx, reg);
            JS_FreeValue(ctx, target);
        }
        JS_FreeValue(ctx, targets);
        JS_FreeValue(ctx, state);
        JS_FreeValue(ctx, observer);
    }
    JS_FreeValue(ctx, list);
    return pending;
}

/* ---- §3.2.5 NOTIFY INTERSECTION OBSERVERS ----------------------------------------------------------------- */

#define IO_NOTIFY_STAGES(X) \
    X(IO_NOTIFY_CALLBACK, "INTERSECTION OBSERVER §3.2.5 step 3.5 (invoking observer's callback with " \
                          "« queue, observer » and observer as the callback this value), one observer per rest") \
    X(IO_NOTIFY_REPORT,   "INTERSECTION OBSERVER §3.2.5 step 3.5's \"if this throws an exception, REPORT THE " \
                          "EXCEPTION\", which is HTML §8.1.4.6 (it fires an `error` event at the global)")
enum { IO_NOTIFY_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const IO_NOTIFY_STEPS[] = { IO_NOTIFY_STAGES(JS_STEP_STAGE_LABEL) NULL };

typedef struct JSIoNotify {
    JSStepHdr hdr;        /* FIRST — the driver writes the def and the operand bounds through it */
    JSValue   notify;     /* step 2's list of observers, UNDEFINED before step 1 has run */
    uint32_t  i;
    JSValue   cur;        /* the observer whose callback is in flight (owned) */
    JSValue   records;    /* step 3.2's clone, held across the park because it IS the argument */
    uint8_t   phase;      /* step_call_run's own */
    /* HAVE THE OWNED FIELDS BEEN PLACED YET. Its own byte and not a JS_IsUndefined test, because a step state
       arrives js_mallocz'd and a ZEROED JSValue is the INTEGER 0 — so every value on a fresh state reads as
       already set, and "have I begun" would always answer yes. */
    uint8_t   started;
    uint8_t   reporting;
    JSValue   exc;
    ReportExceptionWork rep;
    JSValue   cb[4];      /* [this, callback, queue, observer] */
} JSIoNotify;

static void js_io_notify_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSIoNotify *s = st;
    int k;

    v->val(ctx, &s->notify);
    v->val(ctx, &s->cur);
    v->val(ctx, &s->records);
    v->val(ctx, &s->exc);
    report_exception_work_visit(ctx, &s->rep, v);
    STEP_CB_FOREACH(s->cb, k) v->val(ctx, &s->cb[k]);
}

static JSValue js_io_notify_fini(JSContext *ctx, void *st, bool take_result)
{
    JSIoNotify *s = st;

    (void)take_result;
    /* §8.1.4.6 step 5's FLAG, if a report was abandoned holding it. Not a reference, so no declaration names it
       and the visit above is what releases the record's references. */
    report_exception_work_unlock(ctx, &s->rep);
    return JS_UNDEFINED;
}

static int js_io_notify_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    JSIoNotify *s = st;
    int r, k;

    DCHECK(s->hdr.stage == IO_NOTIFY_CALLBACK || s->hdr.stage == IO_NOTIFY_REPORT,
           "notify intersection observers resumed into a stage §3.2.5 does not have");
    if (!s->started) {
        JSValue list;

        /* EVERY OWNED FIELD IS PLACED BEFORE THE FIRST THING THAT CAN FAIL — the failure path tears the state
           down through `fini`, which frees exactly what the state holds. */
        s->notify = s->cur = s->records = s->exc = JS_UNDEFINED;
        STEP_CB_FOREACH(s->cb, k) s->cb[k] = JS_UNDEFINED;
        report_exception_work_start(&s->rep);
        s->reporting = 0;
        s->started = 1;
        s->i = 0;
        /* STEP 1: "set document's IntersectionObserverTaskQueued flag to false", BEFORE the walk — so an
           observation queued by one of these callbacks schedules a NEW task rather than joining the batch
           being delivered. */
        list = io_doc_list(ctx);
        JS_SetProperty(ctx, list, g_atom_queued, JS_FALSE);
        s->notify = io_clone(ctx, list);                               /* step 2 */
        JS_FreeValue(ctx, list);
    }
    for (;;) {
        JSValue ignored = JS_UNDEFINED, cbfn, ostate;

        if (s->reporting) {
            r = report_exception_run(ctx, &s->rep, s->exc, cb_result, out_cb, out_argc);
            cb_result = JS_UNDEFINED;
            if (r > 0) return r;
            s->reporting = 0;
            STEP_GOTO(s->hdr.stage, IO_NOTIFY_CALLBACK, &s->phase, NULL);
            JS_FreeValue(ctx, s->exc);
            s->exc = JS_UNDEFINED;
        }
        if (JS_IsUndefined(s->cur)) {
            /* STEP 3: the next observer with something to deliver. Step 3.1 skips an observer whose
               [[QueuedEntries]] is empty, and steps 3.2 and 3.3 take and clear the queue. */
            for (;;) {
                JSValue observer, entries;
                JSContext *rctx;

                if (s->i >= io_len(ctx, s->notify)) {
                    JS_FreeValue(ctx, s->notify);
                    s->notify = JS_UNDEFINED;
                    JS_FreeValue(ctx, cb_result);
                    return JS_STEP_DONE;
                }
                observer = JS_GetPropertyUint32(ctx, s->notify, s->i++);
                ostate = io_state(ctx, observer);
                DCHECK(!JS_IsException(ostate),
                       "a document's Intersection Observer list holds something that is not an "
                       "IntersectionObserver");
                if (!io_root_in_tree(ctx, ostate)) {                   /* step 2's own predicate */
                    JS_FreeValue(ctx, ostate);
                    JS_FreeValue(ctx, observer);
                    continue;
                }
                entries = JS_GetPropertyUint32(ctx, ostate, IO_S_ENTRIES);
                if (io_len(ctx, entries) == 0) {                       /* step 3.1 */
                    JS_FreeValue(ctx, entries);
                    JS_FreeValue(ctx, ostate);
                    JS_FreeValue(ctx, observer);
                    continue;
                }
                /* THE ARGUMENT IS BUILT IN THE OBSERVER'S REALM, because Web IDL converts the callback's
                   `sequence<IntersectionObserverEntry>` in the callback's own realm and the entries are that
                   realm's objects already. */
                rctx = io_realm(ctx, ostate);
                s->records = io_clone(rctx, entries);                  /* step 3.2 */
                JS_FreeValue(ctx, entries);
                JS_SetPropertyUint32(ctx, ostate, IO_S_ENTRIES, JS_NewArray(ctx));   /* step 3.3 */
                JS_FreeValue(ctx, ostate);
                s->cur = observer;
                break;
            }
        }
        /* STEP 3.5 — the page's callback, with `this` = observer and « queue, observer ». */
        {
            JSValueConst args[2];

            ostate = io_state(ctx, s->cur);
            DCHECK(!JS_IsException(ostate), "the notify walk lost its observer's state mid-callback");
            cbfn = JS_GetPropertyUint32(ctx, ostate, IO_S_CALLBACK);   /* step 3.4 */
            JS_FreeValue(ctx, ostate);
            args[0] = s->records;
            args[1] = s->cur;
            r = step_call_run(ctx, &s->phase, s->cb, 4, cbfn, s->cur, 2, args, cb_result, &ignored,
                              out_cb, out_argc);
            JS_FreeValue(ctx, cbfn);
            cb_result = JS_UNDEFINED;
            if (r > 0) return JS_STEP_CALL;
        }
        /* "If this throws an exception, REPORT THE EXCEPTION": the walk goes on to the next observer. Without
           it one throwing callback would tear the task down and every observer behind it would silently never
           be notified. */
        if (JS_IsException(ignored)) {
            ignored = JS_UNDEFINED;
            s->exc = JS_GetException(ctx);
            s->reporting = 1;
            STEP_GOTO(s->hdr.stage, IO_NOTIFY_REPORT, &s->phase, NULL);
        }
        JS_FreeValue(ctx, ignored);   /* IntersectionObserverCallback returns undefined; anything else goes */
        JS_FreeValue(ctx, s->cur);
        JS_FreeValue(ctx, s->records);
        s->cur = s->records = JS_UNDEFINED;
    }
}

static const JSTrampStepDef js_io_notify_def = {
    sizeof(JSIoNotify), js_io_notify_step, js_io_notify_fini, 0,
    /* Step 3.5 reports rather than propagates, so the callback's abrupt completion is this machine's own VALUE
       and must be delivered back to step() instead of tearing the task down. */
    .catches_abrupt = 1, .visit = js_io_notify_visit,
    .algorithm = "INTERSECTION OBSERVER §3.2.5 notify intersection observers",
    .steps = IO_NOTIFY_STEPS
};

/* ---- §2.2's ATTRIBUTES ------------------------------------------------------------------------------------ */

static JSValue js_io_get(JSContext *ctx, JSValueConst this_val, int magic)
{
    JSValue state = io_state(ctx, this_val), v;

    if (JS_IsException(state)) return JS_EXCEPTION;
    switch ((IoMember)magic) {
    case IO_ROOT:             v = JS_GetPropertyUint32(ctx, state, IO_S_ROOT); break;
    case IO_THRESHOLDS:       v = JS_GetPropertyUint32(ctx, state, IO_S_THRESHOLDS); break;
    case IO_DELAY:            v = JS_GetPropertyUint32(ctx, state, IO_S_DELAY); break;
    case IO_TRACK_VISIBILITY: v = JS_GetPropertyUint32(ctx, state, IO_S_TRACK_VISIBILITY); break;
    case IO_ROOT_MARGIN:
    case IO_SCROLL_MARGIN: {
        JSValue m = JS_GetPropertyUint32(ctx, state,
                                         magic == IO_ROOT_MARGIN ? IO_S_ROOT_MARGIN : IO_S_SCROLL_MARGIN);

        v = io_serialize_margin(ctx, m);
        JS_FreeValue(ctx, m);
        break;
    }
    default:
        JS_FreeValue(ctx, state);
        DFAIL("an IntersectionObserver attribute was read with a magic no member of §2.2 declares — the magic "
              "IS the member, so an unknown one means a name was installed without a case to answer it");
        return JS_UNDEFINED;
    }
    JS_FreeValue(ctx, state);
    return v;
}

/* ---- §3.2.2, §3.2.3, disconnect and takeRecords ----------------------------------------------------------- */

/* §3.2.2 "OBSERVE A TARGET Element":
     1. If target is in observer's [[ObservationTargets]] slot, return.
     2. Let intersectionObserverRegistration be a record with observer set to observer,
        previousThresholdIndex set to -1, previousIsIntersecting false and previousIsVisible false.
     3. Append it to target's [[RegisteredIntersectionObservers]] slot.
     4. Add target to observer's [[ObservationTargets]] slot. */
static JSValue js_io_observe(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    JSValue state = io_state(ctx, this_val), targets, list, rec;
    uint32_t n, i;

    (void)argc; (void)magic;
    if (JS_IsException(state)) return JS_EXCEPTION;
    DCHECK(argc >= 1 && element_is(argv[0]),
           "observe reached its body without the Element its declaration requires — `observe(Element target)` "
           "is a REQUIRED interface-typed argument, so the declared type is what makes everything else a "
           "TypeError before step 1");
    targets = JS_GetPropertyUint32(ctx, state, IO_S_TARGETS);
    n = io_len(ctx, targets);
    for (i = 0; i < n; i++) {                                             /* step 1 */
        JSValue t = JS_GetPropertyUint32(ctx, targets, i);
        bool same = JS_VALUE_GET_PTR(t) == JS_VALUE_GET_PTR(argv[0]);

        JS_FreeValue(ctx, t);
        if (same) {
            JS_FreeValue(ctx, targets);
            JS_FreeValue(ctx, state);
            return JS_UNDEFINED;
        }
    }
    rec = JS_NewArray(ctx);                                               /* step 2 */
    CHECK(!JS_IsException(rec), "an IntersectionObserverRegistration could not be allocated");
    JS_SetPropertyUint32(ctx, rec, IOR_OBSERVER, JS_DupValue(ctx, this_val));
    /* §3.1.2's previousThresholdIndex is "a number between -1 and the length of thresholds (inclusive)", and
       −1 is what §3.4.2's "no entry has yet been queued" is read off — see intersection_observer.h. */
    JS_SetPropertyUint32(ctx, rec, IOR_PREV_THRESHOLD, JS_NewFloat64(ctx, -1.0));
    JS_SetPropertyUint32(ctx, rec, IOR_PREV_INTERSECTING, JS_FALSE);
    /* §3.1.2's lastUpdateTime, which §3.2.2 does not name and §3.2.10 step 3.2 reads. Zero is the time origin,
       so the first frame's `time - 0 < delay` is false for the `delay: 0` every observer that did not opt in
       has — which is what makes an un-delayed observer report on the very first frame. */
    JS_SetPropertyUint32(ctx, rec, IOR_LAST_UPDATE, JS_NewFloat64(ctx, 0.0));
    JS_SetPropertyUint32(ctx, rec, IOR_PREV_VISIBLE, JS_FALSE);
    list = io_reg_list(ctx, argv[0], 1);
    io_push(ctx, list, rec);                                              /* step 3 */
    JS_FreeValue(ctx, list);
    io_push(ctx, targets, JS_DupValue(ctx, argv[0]));                     /* step 4 */
    JS_FreeValue(ctx, targets);
    JS_FreeValue(ctx, state);
    return JS_UNDEFINED;
}

/* §3.2.3 "UNOBSERVE A TARGET Element" — remove the registration record whose observer is this from target's
   list, if present, and remove target from this's [[ObservationTargets]], if present. */
static JSValue js_io_unobserve(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    JSValue state = io_state(ctx, this_val), targets, list;
    uint32_t n, i, out = 0;

    (void)argc; (void)magic;
    if (JS_IsException(state)) return JS_EXCEPTION;
    DCHECK(argc >= 1 && element_is(argv[0]),
           "unobserve reached its body without the Element its declaration requires — `unobserve(Element "
           "target)` is a REQUIRED interface-typed argument");
    list = io_reg_list(ctx, argv[0], 0);
    if (JS_IsObject(list)) {
        n = io_len(ctx, list);
        for (i = 0; i < n; i++) {
            JSValue e = JS_GetPropertyUint32(ctx, list, i);
            JSValue ob = JS_GetPropertyUint32(ctx, e, IOR_OBSERVER);
            bool drop = JS_VALUE_GET_PTR(ob) == JS_VALUE_GET_PTR(this_val);

            JS_FreeValue(ctx, ob);
            if (drop) { JS_FreeValue(ctx, e); continue; }
            JS_SetPropertyUint32(ctx, list, out++, e);
        }
        if (out != n) io_set_len(ctx, list, out);
    }
    JS_FreeValue(ctx, list);
    targets = JS_GetPropertyUint32(ctx, state, IO_S_TARGETS);
    n = io_len(ctx, targets);
    for (i = 0, out = 0; i < n; i++) {
        JSValue t = JS_GetPropertyUint32(ctx, targets, i);

        if (JS_VALUE_GET_PTR(t) == JS_VALUE_GET_PTR(argv[0])) { JS_FreeValue(ctx, t); continue; }
        JS_SetPropertyUint32(ctx, targets, out++, t);
    }
    if (out != n) io_set_len(ctx, targets, out);
    JS_FreeValue(ctx, targets);
    JS_FreeValue(ctx, state);
    return JS_UNDEFINED;
}

/* §2.2's `disconnect()` — for each target in [[ObservationTargets]]: remove the registration record whose
   observer is this from that target's list, and remove the target from [[ObservationTargets]].
   IT DOES NOT CLEAR [[QueuedEntries]], which `takeRecords()` does and this does not: §2.2 states two different
   algorithms and a disconnected observer whose entries were already queued is still notified for them. */
static JSValue js_io_disconnect(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    JSValue state = io_state(ctx, this_val), targets;
    uint32_t n, k;

    (void)argc; (void)argv; (void)magic;
    if (JS_IsException(state)) return JS_EXCEPTION;
    targets = JS_GetPropertyUint32(ctx, state, IO_S_TARGETS);
    n = io_len(ctx, targets);
    for (k = 0; k < n; k++) {
        JSValue t = JS_GetPropertyUint32(ctx, targets, k);
        JSValue l = io_reg_list(ctx, t, 0);
        uint32_t ln, i, out = 0;

        if (JS_IsObject(l)) {
            ln = io_len(ctx, l);
            for (i = 0; i < ln; i++) {
                JSValue e = JS_GetPropertyUint32(ctx, l, i);
                JSValue ob = JS_GetPropertyUint32(ctx, e, IOR_OBSERVER);
                bool drop = JS_VALUE_GET_PTR(ob) == JS_VALUE_GET_PTR(this_val);

                JS_FreeValue(ctx, ob);
                if (drop) { JS_FreeValue(ctx, e); continue; }
                JS_SetPropertyUint32(ctx, l, out++, e);
            }
            if (out != ln) io_set_len(ctx, l, out);
        }
        JS_FreeValue(ctx, l);
        JS_FreeValue(ctx, t);
    }
    io_set_len(ctx, targets, 0);
    JS_FreeValue(ctx, targets);
    JS_FreeValue(ctx, state);
    return JS_UNDEFINED;
}

/* §2.2's `takeRecords()` — let queue be a copy of [[QueuedEntries]], clear the slot, return queue. */
static JSValue js_io_take_records(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    JSValue state = io_state(ctx, this_val), entries, out;

    (void)argc; (void)argv; (void)magic;
    if (JS_IsException(state)) return JS_EXCEPTION;
    entries = JS_GetPropertyUint32(ctx, state, IO_S_ENTRIES);
    out = io_clone(ctx, entries);
    JS_FreeValue(ctx, entries);
    JS_SetPropertyUint32(ctx, state, IO_S_ENTRIES, JS_NewArray(ctx));
    JS_FreeValue(ctx, state);
    return out;
}

/* ---- §3.2.1 "INITIALIZE A NEW IntersectionObserver" -------------------------------------------------------- */

typedef struct { uint8_t unused; } JSIoCtorState;
static void js_io_ctor_visit(JSContext *ctx, void *st, JSStepVisit *v) { (void)ctx; (void)st; (void)v; }

#define IO_CTOR_STAGES(X) \
    X(IO_CTOR_BUILD = IDL_STEP_FIRST, \
      "INTERSECTION OBSERVER §3.2.1 initialize a new IntersectionObserver (parse both margins, sort and " \
      "range-check the thresholds, clamp the delay for a trackVisibility observer)")
enum { IO_CTOR_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const IO_CTOR_STEPS[] = { IO_CTOR_STAGES(JS_STEP_STAGE_LABEL) NULL };

/* One margin option, parsed into a fresh 8-number Array or a SyntaxError. */
static JSValue io_ctor_margin(JSContext *ctx, JSValueConst options, const char *name)
{
    JSValue v = JS_GetPropertyStr(ctx, (JSValue)options, name), arr;
    double parsed[IO_MARGIN_LEN];
    const char *s;
    size_t len;
    uint32_t i;
    bool ok;

    DCHECK(JS_IsString(v),
           "an IntersectionObserverInit margin member reached §3.2.1 unconverted — §2.4 declares both "
           "`DOMString` with a `= \"0px\"` default, so the conversion places a string on every path");
    s = JS_ToCStringLen(ctx, &len, v);
    CHECK(s != NULL, "a margin option's string could not be read");
    ok = io_parse_margin(s, len, parsed);
    JS_FreeCString(ctx, s);
    JS_FreeValue(ctx, v);
    if (!ok)
        return JS_ThrowSyntaxError(ctx, "IntersectionObserver: %s is not a valid margin", name);
    arr = JS_NewArray(ctx);
    CHECK(!JS_IsException(arr), "a parsed margin could not be allocated");
    for (i = 0; i < IO_MARGIN_LEN; i++)
        JS_SetPropertyUint32(ctx, arr, i, JS_NewFloat64(ctx, parsed[i]));
    return arr;
}

static int js_io_ctor_step(JSContext *ctx, JSStepHdr *hdr, void *st, int argc, JSValueConst *argv,
                           JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    JSValue obj, state, proto, options, root, margin, scroll_margin, thresholds, v;
    JSContext *rootctx;
    double delay = 0.0;
    uint32_t nth, i, j;
    bool track;

    (void)st; (void)out_cb; (void)out_argc;
    JS_FreeValue(ctx, cb_result);
    DCHECK(hdr->stage == IO_CTOR_BUILD,
           "the IntersectionObserver constructor resumed at a stage §3.2.1 does not have");
    if (JS_IsUndefined(hdr->this_val))
        return JS_ThrowTypeError(ctx, "constructor IntersectionObserver requires 'new'"), -1;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "IntersectionObserver requires a callback"), -1;
    DCHECK(JS_IsFunction(ctx, argv[0]),
           "IntersectionObserver's callback reached the body unconverted — §2.2 declares it an "
           "IntersectionObserverCallback, and Web IDL §3.2.19's brand test is what makes a non-callable a "
           "TypeError before step 1");
    /* `optional IntersectionObserverInit options = {}`. A DECLARED DICTIONARY POSITION IS CONVERTED EVEN WHEN
       THE PAGE STOPPED SHORT OF IT (core/idl_args.c states the rule at the count it extends), so
       `new IntersectionObserver(cb)` arrives here with the object built and §2.4's six defaults already placed
       on it. This is the assertion of that and NOT a substitute for it: a body that built its own empty object
       would be re-deriving the IDL's `= {}` and then, member by member, the IDL's six defaults. */
    DCHECK(argc >= 2 && JS_IsObject(argv[1]),
           "IntersectionObserver's options reached the body without the object the dictionary conversion "
           "builds — §2.4 gives every one of its six members a default, and the body reads them rather than "
           "inventing them");
    options = argv[1];

    /* STEPS 3 AND 4 — both margins, each a SyntaxError of its own, and BEFORE anything is allocated so a
       failure leaves nothing behind. */
    margin = io_ctor_margin(ctx, options, "rootMargin");
    if (JS_IsException(margin)) return -1;
    scroll_margin = io_ctor_margin(ctx, options, "scrollMargin");
    if (JS_IsException(scroll_margin)) { JS_FreeValue(ctx, margin); return -1; }

    /* STEP 5: "let thresholds be a list equal to options.threshold". The union already resolved its arm —
       §2.4's `(double or sequence<double>)` — so what arrives is either a Number or an Array of them. */
    v = JS_GetPropertyStr(ctx, (JSValue)options, "threshold");
    thresholds = JS_NewArray(ctx);
    CHECK(!JS_IsException(thresholds), "an IntersectionObserver's thresholds list could not be allocated");
    if (JS_IsArray(v)) {
        nth = io_len(ctx, v);
        for (i = 0; i < nth; i++)
            JS_SetPropertyUint32(ctx, thresholds, i, JS_GetPropertyUint32(ctx, v, i));
    } else {
        DCHECK(JS_IsNumber(v),
               "IntersectionObserverInit.threshold reached §3.2.1 as neither a Number nor an Array — §2.4 "
               "declares `(double or sequence<double>) threshold = 0` and IDL_DOUBLE_OR_SEQUENCE is what "
               "resolves the arm, so a third shape means the type was not declared");
        JS_SetPropertyUint32(ctx, thresholds, 0, JS_DupValue(ctx, v));
        nth = 1;
    }
    JS_FreeValue(ctx, v);
    /* STEP 6: "if ANY value in thresholds is less than 0.0 or greater than 1.0, throw a RangeError." Before
       the sort, which is what makes the error independent of the order the page wrote them in. */
    for (i = 0; i < nth; i++) {
        double d = io_num_at(ctx, thresholds, i);

        if (!(d >= 0.0 && d <= 1.0)) {          /* written so a NaN is refused by the same test */
            JS_FreeValue(ctx, thresholds);
            JS_FreeValue(ctx, margin);
            JS_FreeValue(ctx, scroll_margin);
            JS_ThrowRangeError(ctx, "IntersectionObserver: threshold values must be between 0.0 and 1.0");
            return -1;
        }
    }
    /* STEP 7: sort ascending. An insertion sort over the list itself — the comparison is on engine-built
       Numbers (Web IDL §3.2.7 `double`'s conversion already refused anything else), so nothing here runs the
       page's code and
       the list is small by construction. */
    for (i = 1; i < nth; i++) {
        double d = io_num_at(ctx, thresholds, i);

        for (j = i; j > 0 && io_num_at(ctx, thresholds, j - 1) > d; j--)
            JS_SetPropertyUint32(ctx, thresholds, j,
                                 JS_GetPropertyUint32(ctx, thresholds, j - 1));
        JS_SetPropertyUint32(ctx, thresholds, j, JS_NewFloat64(ctx, d));
    }
    if (nth == 0) {                                                       /* step 8 */
        JS_SetPropertyUint32(ctx, thresholds, 0, JS_NewFloat64(ctx, 0.0));
        nth = 1;
    }
    /* §2.2 declares `thresholds` a `FrozenArray<double>`, and Web IDL §3.2.27 Frozen arrays makes that a
       frozen Array — so
       it is frozen HERE, once, rather than copied on every read. */
    CHECK(idl_freeze_array(ctx, thresholds) == 0,
          "an IntersectionObserver's thresholds FrozenArray could not be frozen");

    /* STEPS 10 TO 12. Step 11 clamps a trackVisibility observer's delay up to 100, and step 12 as printed in
       the section reads "set this's internal [[delay]] slot to options.delay to delay" — a sentence with two
       objects. It is the CLAMPED value: step 11 exists to raise it and would have no effect at all otherwise,
       which is what makes this a reading of the text rather than a choice between two. */
    v = JS_GetPropertyStr(ctx, (JSValue)options, "delay");
    DCHECK(JS_IsNumber(v), "IntersectionObserverInit.delay reached §3.2.1 unconverted — §2.4 declares it "
                           "`long` with a `= 0` default");
    JS_ToFloat64(ctx, &delay, v);
    JS_FreeValue(ctx, v);
    v = JS_GetPropertyStr(ctx, (JSValue)options, "trackVisibility");
    track = JS_ToBool(ctx, v) > 0;
    JS_FreeValue(ctx, v);
    if (track && delay < 100.0) delay = 100.0;                            /* step 11 */

    root = JS_GetPropertyStr(ctx, (JSValue)options, "root");
    DCHECK(JS_IsNull(root) || node_of(root) != NULL,
           "IntersectionObserverInit.root reached §3.2.1 as neither null nor a node — §2.4 declares it "
           "`(Element or Document)? root = null` and the declared type is what refuses everything else");

    proto = JS_GetClassProto(ctx, g_class);
    DCHECK(!JS_IsNull(proto), "an IntersectionObserver was constructed in a realm that never ran its install");
    obj = JS_NewObjectProtoClass(ctx, proto, g_class);
    JS_FreeValue(ctx, proto);
    if (JS_IsException(obj)) {
        JS_FreeValue(ctx, root);
        JS_FreeValue(ctx, margin);
        JS_FreeValue(ctx, scroll_margin);
        JS_FreeValue(ctx, thresholds);
        return -1;
    }
    state = JS_NewArray(ctx);
    CHECK(!JS_IsException(state), "an IntersectionObserver's state could not be allocated");
    JS_SetPropertyUint32(ctx, state, IO_S_CALLBACK, JS_DupValue(ctx, argv[0]));   /* step 2 */
    JS_SetPropertyUint32(ctx, state, IO_S_TARGETS, JS_NewArray(ctx));
    JS_SetPropertyUint32(ctx, state, IO_S_ENTRIES, JS_NewArray(ctx));
    JS_SetPropertyUint32(ctx, state, IO_S_ROOT, root);
    JS_SetPropertyUint32(ctx, state, IO_S_ROOT_MARGIN, margin);
    JS_SetPropertyUint32(ctx, state, IO_S_SCROLL_MARGIN, scroll_margin);
    JS_SetPropertyUint32(ctx, state, IO_S_THRESHOLDS, thresholds);
    JS_SetPropertyUint32(ctx, state, IO_S_DELAY, JS_NewFloat64(ctx, delay));      /* step 12 */
    JS_SetPropertyUint32(ctx, state, IO_S_TRACK_VISIBILITY, JS_NewBool(ctx, track));  /* step 13 */
    JS_SetPropertyUint32(ctx, state, IO_S_WINDOW, JS_DupValue(ctx, document_window_proxy(ctx)));
    JS_DefinePropertyValue(ctx, obj, g_atom_state, state, 0);

    /* §3.2.10 step 1 and §3.2.5 step 2 both walk "all IntersectionObservers whose root is in the DOM tree of
       document", so the observer joins THAT document's list — which for a null root is the TOP-LEVEL browsing
       context's (§2.2's implicit root) and NOT the constructing realm's. `io_root_in_tree` is what keeps
       membership a live query afterwards. */
    {
        JSValue rv = JS_GetPropertyUint32(ctx, state, IO_S_ROOT), list;

        rootctx = io_root_document_realm(ctx, rv);
        JS_FreeValue(ctx, rv);
        list = io_doc_list(rootctx);
        io_push(rootctx, list, JS_DupValue(ctx, obj));
        JS_FreeValue(rootctx, list);
    }
    *presult = obj;
    return 0;
}

static const IdlStepDecl js_io_ctor_decl = {
    js_io_ctor_step, sizeof(JSIoCtorState), js_io_ctor_visit, NULL,
    "INTERSECTION OBSERVER §3.2.1 initialize a new IntersectionObserver", IO_CTOR_STEPS
};

/* §2.4's `(Element or Document)?` — the union's NARROWING, beside the class the brand is stated with. Both arms
   are Nodes to this engine's class system, so the class refuses everything that is not a node and this refuses
   every node that is neither of the two arms. */
static bool io_root_is(JSValueConst v)
{
    lxb_dom_node_t *n;

    if (element_is(v)) return true;
    n = node_of(v);
    return n != NULL && n->type == LXB_DOM_NODE_TYPE_DOCUMENT;
}

/* ---- declaration and installation -------------------------------------------------------------------------- */

void intersection_observer_init(JSContext *ctx)
{
    /* §2.4's IntersectionObserverInit, MEMBER FOR MEMBER AND DEFAULT FOR DEFAULT, in Web IDL §3.2.17
       Dictionary types' LEXICOGRAPHIC read order. Every one of the six carries a default, so a body reads the IDL's value on
       every path and never invents one. */
    static const IdlDictMember INIT_MEMBERS[] = {
        { "delay",           IDL_LONG,               false, NULL, 0, NULL, IDL_DEFAULT_ZERO },
        { "root",            IDL_INTERFACE_NULLABLE, false, NULL, 0, NULL, IDL_DEFAULT_NULL },
        { "rootMargin",      IDL_DOMSTRING,          false, NULL, 0, NULL, IDL_DEFAULT_STRING, "0px" },
        { "scrollMargin",    IDL_DOMSTRING,          false, NULL, 0, NULL, IDL_DEFAULT_STRING, "0px" },
        { "threshold",       IDL_DOUBLE_OR_SEQUENCE, false, NULL, 0, NULL, IDL_DEFAULT_ZERO },
        { "trackVisibility", IDL_BOOLEAN,            false, NULL, 0, NULL, IDL_DEFAULT_FALSE },
    };
    static const IdlArgType CTOR_ARGS[2] = { IDL_CALLBACK, IDL_DICT };
    static const IdlArgType OBSERVE_ARGS[1] = { IDL_INTERFACE };
    JSClassDef d = { "IntersectionObserver" };

    if (g_ready) return;   /* one AGENT, one class */
    JS_NewClassID(JS_GetRuntime(ctx), &g_class);
    JS_NewClass(JS_GetRuntime(ctx), g_class, &d);
    intersection_observer_entry_init(ctx);

    g_state_key = JS_NewSymbol(ctx, "intersectionObserverState", false);
    CHECK(!JS_IsException(g_state_key), "the IntersectionObserver state slot key allocation failed");
    g_atom_state = JS_ValueToAtom(ctx, g_state_key);
    g_reg_key = JS_NewSymbol(ctx, "registeredIntersectionObservers", false);
    CHECK(!JS_IsException(g_reg_key), "the registered intersection observer list slot key allocation failed");
    g_atom_reg = JS_ValueToAtom(ctx, g_reg_key);
    g_atom_queued = JS_NewAtom(ctx, "queued");
    CHECK(g_atom_state != JS_ATOM_NULL && g_atom_reg != JS_ATOM_NULL && g_atom_queued != JS_ATOM_NULL,
          "an Intersection Observer slot key could not be interned");

    g_notify_stepid = JS_RegisterStepDef(JS_GetRuntime(ctx), &js_io_notify_def);
    CHECK(g_notify_stepid >= 0, "no step id for §3.2.5's notification driver");
    g_notify_slot = realm_value_declare(ctx, "§3.2.5 notifyIntersectionObservers");
    g_docobs_slot = realm_value_declare(ctx, "§3.1.1 the document's IntersectionObservers");

    g_id_ctor = idl_method_id_step(ctx, CTOR_ARGS, 2, INIT_MEMBERS,
                                   (int)(sizeof(INIT_MEMBERS) / sizeof(INIT_MEMBERS[0])),
                                   &js_io_ctor_decl, 0);
    idl_iface_brand(node_class_id());
    idl_iface_narrow(io_root_is);        /* §2.4's `(Element or Document)?` */
    idl_optional_from(1);                /* `optional IntersectionObserverInit options = {}` */
    g_id_observe = idl_method_id(ctx, OBSERVE_ARGS, 1, js_io_observe, 0);
    idl_iface_brand(node_class_id());
    idl_iface_narrow(element_is);        /* `observe(Element target)` */
    g_id_unobserve = idl_method_id(ctx, OBSERVE_ARGS, 1, js_io_unobserve, 0);
    idl_iface_brand(node_class_id());
    idl_iface_narrow(element_is);        /* `unobserve(Element target)` */
    g_id_disconnect = idl_method_id(ctx, NULL, 0, js_io_disconnect, 0);
    g_id_take = idl_method_id(ctx, NULL, 0, js_io_take_records, 0);

    realm_declare_intrinsic(intersection_observer_install_proto);
    g_ready = 1;

    /* WHAT THIS COMPONENT HOLDS FOR THE AGENT, DECLARED — core/agent_state.h, whose whole subject is that a
       release which frees a value and KEEPS the handle is invisible to every detector this tree has: the only
       reader of a stale id is this component's own next `_init`, and by then the agent that wrote it is gone.
       This row is on core/platform.h's release column, so the two-sided check in platform.c requires the
       declaration to exist and asserts every slot below is back at its pre-init value the moment the column
       ends. `g_ready` is the latch this init consults first and is therefore the one that must not survive.
       THE ENTRY INTERFACE'S OWN SLOTS ARE DECLARED UNDER THIS SAME COMPONENT NAME, from its `_init`: §2.3 is a
       second interface but not a second platform row — one declare, one install and one release cover both,
       which is what core/platform.c's witness table already says by mapping `IntersectionObserverEntry` to
       this component. */
    agent_state_flag("intersection_observer", &g_ready, "the declaration latch");
    agent_state_class("intersection_observer", &g_class, "§2.2's IntersectionObserver class");
    agent_state_value("intersection_observer", &g_state_key, "the observer's state-slot key");
    agent_state_atom("intersection_observer", &g_atom_state, "the observer's state-slot key, interned");
    agent_state_value("intersection_observer", &g_reg_key,
                      "§3.1.2's [[RegisteredIntersectionObservers]] slot key");
    agent_state_atom("intersection_observer", &g_atom_reg,
                     "§3.1.2's [[RegisteredIntersectionObservers]] slot key, interned");
    agent_state_atom("intersection_observer", &g_atom_queued,
                     "§3.1.1's IntersectionObserverTaskQueued flag name");
    agent_state_id("intersection_observer", &g_notify_stepid, "§3.2.5's notification machine");
    agent_state_id("intersection_observer", &g_notify_slot,
                   "the per-realm slot §3.2.5's driver is held in");
    agent_state_id("intersection_observer", &g_docobs_slot,
                   "the per-realm slot §3.1.1's document observer list is held in");
    agent_state_id("intersection_observer", &g_id_ctor, "§3.2.1's constructor declaration");
    agent_state_id("intersection_observer", &g_id_observe, "§3.2.2's observe declaration");
    agent_state_id("intersection_observer", &g_id_unobserve, "§3.2.3's unobserve declaration");
    agent_state_id("intersection_observer", &g_id_disconnect, "§2.2's disconnect declaration");
    agent_state_id("intersection_observer", &g_id_take, "§2.2's takeRecords declaration");
}

void intersection_observer_install_proto(JSContext *ctx)
{
    JSValue proto, prev, list;

    DCHECK(g_class != 0, "a realm asked for IntersectionObserver.prototype before the interface was declared");
    prev = JS_GetClassProto(ctx, g_class);
    DCHECK(JS_IsNull(prev), "intersection_observer_install_proto ran twice in one realm");
    JS_FreeValue(ctx, prev);
    /* §3.1.1's per-DOCUMENT state, built EAGERLY with the realm: the observer list and, riding it, the
       IntersectionObserverTaskQueued flag. Built lazily on first use it would be created inside whichever flow
       happened to observe first, which puts a baseline object in one flow's delta. */
    list = JS_NewArray(ctx);
    CHECK(!JS_IsException(list), "a realm's Intersection Observer list could not be allocated");
    JS_SetProperty(ctx, list, g_atom_queued, JS_FALSE);
    realm_value_set(ctx, g_docobs_slot, list);
    {
        /* THE NOTIFICATION DRIVER IS THIS REALM'S. A function object carries the realm it was minted in, and
           this one runs §3.2.5 for THIS document — a driver held in one static would notify every document's
           observers out of whichever realm built it first. It is a step function object nobody installs, so a
           page can neither see it nor replace it. */
        JSValue fn = JS_NewCFunction2(ctx, NULL, "notifyIntersectionObservers", 0, JS_CFUNC_step,
                                      g_notify_stepid);

        CHECK(!JS_IsException(fn), "§3.2.5's notification driver could not be allocated");
        realm_value_set(ctx, g_notify_slot, fn);
    }
    proto = JS_NewObject(ctx);
    CHECK(!JS_IsException(proto), "IntersectionObserver.prototype could not be allocated");
    idl_interface_tag(ctx, proto, "IntersectionObserver");
    idl_install_accessor(ctx, proto, "root", js_io_get, IO_ROOT, -1);
    idl_install_accessor(ctx, proto, "rootMargin", js_io_get, IO_ROOT_MARGIN, -1);
    idl_install_accessor(ctx, proto, "scrollMargin", js_io_get, IO_SCROLL_MARGIN, -1);
    idl_install_accessor(ctx, proto, "thresholds", js_io_get, IO_THRESHOLDS, -1);
    idl_install_accessor(ctx, proto, "delay", js_io_get, IO_DELAY, -1);
    idl_install_accessor(ctx, proto, "trackVisibility", js_io_get, IO_TRACK_VISIBILITY, -1);
    idl_install_method(ctx, proto, "observe", 1, g_id_observe);
    idl_install_method(ctx, proto, "unobserve", 1, g_id_unobserve);
    idl_install_method(ctx, proto, "disconnect", 0, g_id_disconnect);
    idl_install_method(ctx, proto, "takeRecords", 0, g_id_take);
    JS_SetClassProto(ctx, g_class, proto);
}

void intersection_observer_install(JSContext *ctx, JSValueConst global)
{
    JSValue ctor, proto;

    intersection_observer_entry_install(ctx, global);
    DCHECK(g_id_ctor >= 0, "IntersectionObserver was installed before its constructor was declared");
    ctor = idl_step_constructor(ctx, "IntersectionObserver", 1, g_id_ctor);
    CHECK(!JS_IsException(ctor), "the IntersectionObserver interface object could not be allocated");
    proto = JS_GetClassProto(ctx, g_class);
    DCHECK(!JS_IsNull(proto),
           "IntersectionObserver was installed in a realm that never ran its prototype install");
    JS_SetConstructor(ctx, ctor, proto);
    JS_FreeValue(ctx, proto);
    JS_SetPropertyStr(ctx, (JSValue)global, "IntersectionObserver", ctor);
}

/* NEITHER OF THIS COMPONENT'S CLASSES HAS A FINALIZER OR A gc_mark, WHICH IS WHY RESETTING THE CLASS IDS BELOW
 * IS SAFE — core/agent_state.h's cost, answered rather than assumed.
 *
 * That header's obligation is exact: a finalizer and a gc_mark run in the collection AFTER this column, so
 * neither may read a static this release resets, and four components were reached that way. Both `JSClassDef`s
 * here are `{ "IntersectionObserver" }` and `{ "IntersectionObserverEntry" }` — every other field is a zero, so
 * `rt->class_array[id].finalizer` and `.gc_mark` are NULL and the collector has nothing of this component's to
 * dispatch to. There is no lookup to get wrong.
 *
 * AND NOTHING IS HELD WHERE THE COLLECTOR CANNOT SEE IT. This component keeps no C record and calls
 * JS_SetOpaque nowhere: an observer's ten slots, an entry's eight, and an element's registration list are OWN
 * PROPERTIES (JS_DefinePropertyValue), and the property walk in mark_children and free_object is
 * UNCONDITIONAL — it runs before the class hook is consulted and does not depend on one existing. So the
 * WindowProxy an observer records for its own realm, its callback, its targets and its queued entries are all
 * marked through ordinary property edges and their references are subtracted by the ordinary free. The two
 * per-realm values (the document's observer list and §3.2.5's driver) are the REALM's and go with its context;
 * what is agent state about them is the SLOT NUMBER, which is declared and reset here. */
void intersection_observer_free(JSRuntime *rt)
{
    /* NOT `if (!g_ready) return;`. This is a row on core/platform.h's release column and the declare pass that
       pairs with it is unconditional, so the release is too — and a release reached in an agent that never
       declared is the thing to CRASH on rather than the thing to skip. */
    DCHECK(g_ready, "Intersection Observer was released in an agent that never declared it");
    intersection_observer_entry_free(rt);
    JS_FreeValueRT(rt, g_state_key);
    JS_FreeValueRT(rt, g_reg_key);
    g_state_key = g_reg_key = JS_UNDEFINED;
    JS_FreeAtomRT(rt, g_atom_state);
    JS_FreeAtomRT(rt, g_atom_reg);
    JS_FreeAtomRT(rt, g_atom_queued);
    g_atom_state = g_atom_reg = g_atom_queued = JS_ATOM_NULL;
    /* The per-realm values are the REALMS' and go with their contexts; the ids and the step registration name
       a runtime that is going away with this one, and the CLASS ID names a registration in it — kept, it would
       hand a second agent in one process a class registered in a runtime that no longer exists, which is
       core/agent_state.h's dom_rect defect exactly. */
    g_docobs_slot = g_notify_slot = g_notify_stepid = -1;
    g_id_ctor = g_id_observe = g_id_unobserve = g_id_disconnect = g_id_take = -1;
    g_class = 0;
    g_ready = 0;
}
