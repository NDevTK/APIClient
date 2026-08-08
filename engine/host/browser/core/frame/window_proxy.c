/* WindowProxy — HTML §7.2.5.1, and the reason it is not a formality here.
 *
 * WHAT IT IS. `window`, `self`, `parent`, `top`, `frames[i]`, `opener` and `iframe.contentWindow` are not
 * Window objects. Every one of them is a WindowProxy: an exotic object that FORWARDS to the current active
 * Window of a NAVIGABLE. The distinction exists because a navigable outlives the documents in it — navigate an
 * iframe and its Window is replaced, while the WindowProxy a page is holding stays the same object. HTML says
 * so in one line, and everything else in this file follows from it.
 *
 * WHY IT IS NOT A FORMALITY IN THIS ENGINE. With one navigable and no navigation, a WindowProxy that forwards
 * to the one global is observationally identical to that global, and I had it written down as exactly that —
 * a piece of machinery to build when there was a second window to point at. That is wrong, and TIME TRAVEL is
 * what makes it wrong:
 *
 *   - THE BINDING IS PER-FLOW. One forked arm navigates the frame and its `contentWindow` targets the new
 *     Window; its sibling never navigated and still targets the old one. Both hold the SAME proxy object,
 *     because that is what a proxy is for. So proxy -> Window is per-flow state, and it has to ride the COW
 *     delta like every other piece of shared state a flow writes — which is what cow_capture_host_record is,
 *     and which a global "the window is the global" cannot express at all.
 *   - SO IS THE SAME-ORIGIN CHECK. §7.2.5.1's [[Get]] is filtered by whether the ACCESSOR and the target's
 *     active document are same origin, and the target's origin is whatever the flow navigated it to. A check
 *     against a fixed origin answers for a world the flow is not in.
 *   - AND A FLOW HOLDING ONE MUST BE SUSPENDABLE. A parked flow resumes with its delta, so the proxy it holds
 *     must resolve through that delta rather than through whatever the engine last did. A binding kept
 *     anywhere else is read at resume time from the wrong world.
 *
 * CROSS-INSTANCE. One WASM instance is one document, so a proxy may name a Window in another instance. Then the
 * binding is not a JSValue at all — it is (document, world), and the world is the sending flow's decision
 * vector, because two arms of a fork that navigated a frame differently must not resolve to one Window.
 *
 * AND THE NAVIGABLE IS STILL THIS INSTANCE'S. A remote proxy is not a proxy this engine knows nothing about:
 * this engine CREATED that navigable, named it, nested it and can destroy it. What lives in the peer is the
 * ACTIVE DOCUMENT. The member table below is organised by exactly that line, and it is the difference between
 * `otherW.self` costing nothing and `otherW.self` parking a flow on a host that may never answer. */
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "core/frame/window_proxy.h"
#include "solver/cow.h"
#include "solver/world.h"
#include "solver/engine.h"
#include "solver/flow.h"
#include "core/idl_args.h"
#include <stdio.h>

/* §7.2.5.1's [[Window]] and the origin its same-origin check reads. BOTH are per-flow — see the file comment —
   which is the whole reason this is a record behind a class rather than a property on an object. */
typedef struct {
    JSValue window;    /* the navigable's active Window, or JS_UNDEFINED when the document is remote (owned) */
    char   *origin;    /* the active document's origin, for §7.2.5.1's same-origin check (owned) */
    /* THE NAVIGABLE'S OWN STATE — see the member table below for why it is here and not in the peer. All four
       are per-flow: a flow that closed the window or renamed it is the only one whose timeline contains that,
       and `parent`/`opener` are values a fork must carry. */
    JSValue parent;    /* the parent navigable's proxy (or the creator's Window); JS_UNDEFINED = top-level */
    JSValue opener;    /* §7.2.5's opener — the navigable that opened this one, or JS_NULL */
    char   *name;      /* §7.2.5's name: the BROWSING CONTEXT's, not the element's (owned; see proxy_of) */
    uint8_t closed;    /* the navigable has been destroyed (§4.8.5) or closed (§7.2.5.2) */
    /* WHICH DOCUMENT the navigable's active document IS — the same id the world registry names worlds by, so
       "is this remote?" is one comparison against one identity rather than a second naming scheme kept beside
       it. A proxy whose doc is this instance's has a live `window`; any other doc is in a peer instance and
       has none, and those two facts are asserted together because a proxy where they disagree would answer a
       cross-document read with THIS document's Window — the exact failure the same-origin check exists to
       prevent. */
    uint32_t doc;
} ProxyData;

static JSClassID g_proxy_class;
static JSRuntime *g_wp_rt;
/* THIS DOCUMENT'S ORIGIN — the ACCESSOR side of §7.2.5.1's same-origin check, and the reason the check can
   exist at all. A proxy carries the origin of the navigable's active document; the comparison needs the origin
   of whoever is reading, which is this instance's, and there is exactly one of those. */
static char *g_local_origin;
/* §7.2.5.1's member surface, shared by every proxy — which is also what makes `a.postMessage === b.postMessage`
   true. A WindowProxy has no own properties: it FORWARDS to its navigable, answering from the navigable's own
   state where that is what the member is, and suspending on the host where the member reads the active
   document. See the member table lower down for which is which. */
static JSValue g_proxy_proto = JS_UNINITIALIZED;

#define WP_OFF(f) (uint16_t)offsetof(ProxyData, f)
static const uint16_t PROXY_VALS[] = { WP_OFF(window), WP_OFF(parent), WP_OFF(opener) };
static const CowRecord PROXY_REC = { sizeof(ProxyData), PROXY_VALS, 3 };

/* THE CAPTURE IS IN THE ACCESSOR, as it is for every other component record: a record a flow has reached is one
   it may write, and there is then no write site to miss. `origin` is a POD pointer and `doc` a POD id inside the
   captured bytes — a navigation REPLACES them rather than mutating what they point at, so the byte copy is a
   complete description of the binding at that moment.
   THE STRINGS ARE THEREFORE NOT FREED ON NAVIGATION. A delta's saved bytes still name the old pointer, and
   freeing it would leave a parked flow resuming onto freed memory. They are owned by the PROXY and released
   with it, which costs one string per navigation and is the only arrangement in which a parked flow's binding
   stays readable. */
static ProxyData *proxy_of(JSValueConst v)
{
    ProxyData *p = JS_GetOpaque(v, g_proxy_class);
    if (p) cow_capture_host_record(v, p, &PROXY_REC);
    return p;
}

/* §7.2.5.1's SAME-ORIGIN CHECK, and the one rule that makes it more than a string compare: an OPAQUE origin is
   unique per spec, so it is same-origin with NOTHING — not even with another opaque one, and not with itself.
   A sandboxed frame serializes its origin as "null", and treating two "null"s as equal would let one sandboxed
   document script another. SECURITY.md states the same rule for the credentialed-read principal, and it is the
   same rule because it is the same concept. */
static bool proxy_same_origin(const ProxyData *p)
{
    DCHECK(g_local_origin != NULL, "the same-origin check ran before window_proxy_init named this document's "
                                   "origin — the accessor side of §7.2.5.1 has no value to compare against");
    if (!p->origin || !strcmp(p->origin, "null") || !strcmp(g_local_origin, "null")) return false;
    return !strcmp(p->origin, g_local_origin);
}

bool window_proxy_is(JSValueConst v)
{
    return g_proxy_class != 0 && JS_GetOpaque(v, g_proxy_class) != NULL;
}

/* Every string this proxy has ever held, so a delta that still names an older one resumes onto live memory.
   A list rather than a single slot for the reason the accessor's comment gives. */
typedef struct OwnedStr { char *s; struct OwnedStr *next; } OwnedStr;
static OwnedStr *g_strings;

static char *proxy_strdup(const char *s)
{
    OwnedStr *e;
    char *c;
    if (!s) return NULL;
    c = strdup(s);
    CHECK(c != NULL, "window proxy: OOM recording an origin");
    e = malloc(sizeof *e);
    CHECK(e != NULL, "window proxy: OOM recording an origin");
    e->s = c;
    e->next = g_strings;
    g_strings = e;
    return c;
}

static void proxy_finalizer(JSRuntime *rt, JSValue val)
{
    ProxyData *p = JS_GetOpaque(val, g_proxy_class);
    (void)rt;
    if (!p) return;
    JS_FreeValueRT(rt, p->window);
    JS_FreeValueRT(rt, p->parent);
    JS_FreeValueRT(rt, p->opener);
    free(p);   /* the strings are the component's, released at teardown — see proxy_of */
}

static void proxy_gc_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func)
{
    ProxyData *p = JS_GetOpaque(val, g_proxy_class);
    if (!p) return;
    /* The Window holds the proxy (as `window`) and the proxy holds the Window: a cycle, and one the collector
       has to see or every navigable leaks its global. `parent` and `opener` close cycles of their own — a child
       names its creator's Window, which reaches the child through the element wrapper that holds it. */
    JS_MarkValue(rt, p->window, mark_func);
    JS_MarkValue(rt, p->parent, mark_func);
    JS_MarkValue(rt, p->opener, mark_func);
}

JSValue window_proxy_new(JSContext *ctx, JSValueConst window, const char *origin)
{
    JSValue obj;
    ProxyData *p;

    DCHECK(g_wp_rt != NULL, "a WindowProxy was minted before window_proxy_init ran");
    obj = JS_NewObjectClass(ctx, g_proxy_class);
    if (JS_IsException(obj)) return obj;
    JS_SetPrototype(ctx, obj, g_proxy_proto);
    p = calloc(1, sizeof *p);
    CHECK(p != NULL, "window proxy: OOM building a WindowProxy");
    p->window = JS_DupValue(ctx, window);
    p->origin = proxy_strdup(origin ? origin : "null");
    p->parent = JS_UNDEFINED;
    p->opener = JS_NULL;
    p->doc = world_local_doc();
    JS_SetOpaque(obj, p);
    return obj;
}

JSValue window_proxy_new_remote(JSContext *ctx, uint32_t doc, const char *origin, const char *name,
                                JSValueConst parent, JSValueConst opener)
{
    JSValue obj;
    ProxyData *p;

    DCHECK(g_wp_rt != NULL, "a WindowProxy was minted before window_proxy_init ran");
    /* A REMOTE PROXY THAT NAMES THIS DOCUMENT is a local one that lost its Window — it would resolve to
       JS_UNDEFINED for a navigable this instance can actually answer for. */
    DCHECK(doc != 0 && doc != world_local_doc(),
           "a remote WindowProxy was minted for this instance's own document — use window_proxy_new, which "
           "carries the live Window");
    obj = JS_NewObjectClass(ctx, g_proxy_class);
    if (JS_IsException(obj)) return obj;
    JS_SetPrototype(ctx, obj, g_proxy_proto);
    p = calloc(1, sizeof *p);
    CHECK(p != NULL, "window proxy: OOM building a remote WindowProxy");
    p->window = JS_UNDEFINED;   /* it lives in another instance; there is nothing local to hold */
    p->origin = proxy_strdup(origin ? origin : "null");
    p->name   = name && *name ? proxy_strdup(name) : NULL;
    p->parent = JS_DupValue(ctx, parent);
    p->opener = JS_DupValue(ctx, opener);
    p->doc = doc;
    JS_SetOpaque(obj, p);
    return obj;
}

bool window_proxy_is_remote(JSValueConst proxy)
{
    ProxyData *p = JS_GetOpaque(proxy, g_proxy_class);
    DCHECK(p != NULL, "something that is not a WindowProxy was asked whether it is remote");
    /* THE TWO FACTS MUST AGREE. Read together on every ask, because a proxy where they disagree hands a
       cross-document read this document's Window and nothing downstream can tell. */
    DCHECK((p->doc == world_local_doc()) == !JS_IsUndefined(p->window),
           "a WindowProxy's document id and its Window disagree — a local proxy must carry a live Window and a "
           "remote one must carry none, or a cross-document read is answered by the wrong document");
    return p->doc != world_local_doc();
}

uint32_t window_proxy_doc(JSValueConst proxy)
{
    ProxyData *p = JS_GetOpaque(proxy, g_proxy_class);
    DCHECK(p != NULL, "the document of something that is not a WindowProxy was asked for");
    return p->doc;
}

JSValue window_proxy_window(JSContext *ctx, JSValueConst proxy)
{
    ProxyData *p = proxy_of(proxy);
    DCHECK(p != NULL, "the Window of something that is not a WindowProxy was asked for");
    /* THE SEAM, and it crashes rather than answering. A proxy whose navigable lives in another WASM instance
       has no local Window to hand back, and handing back this document's would be a cross-document read that
       silently succeeded — the one failure mode the same-origin check exists to prevent. What has to be built
       is named in the message: the binding is (document, world), and resolving it is a host round trip because
       only the host knows which instance holds that document and which of its flows is in that world. */
    DCHECK(p->doc == world_local_doc(),
           "a WindowProxy names a navigable in ANOTHER WASM instance — build the cross-instance resolve: the "
           "flow SUSPENDS here (the same snapshot path as an await), the peer answers in its own scheduled turn "
           "under this flow's world, and the flow resumes with the value. The host owns the routing because "
           "only it knows which instance holds that document — window_proxy_doc names which one");
    return JS_DupValue(ctx, p->window);
}

const char *window_proxy_name(JSValueConst proxy)
{
    ProxyData *p = JS_GetOpaque(proxy, g_proxy_class);
    DCHECK(p != NULL, "the name of something that is not a WindowProxy was asked for");
    /* A DESTROYED navigable has no name — the same answer the `name` getter gives, from the same two fields,
       so named access cannot find a frame the page has already removed. */
    return p->closed || !p->name ? "" : p->name;
}

const char *window_proxy_origin(JSValueConst proxy)
{
    ProxyData *p = JS_GetOpaque(proxy, g_proxy_class);
    DCHECK(p != NULL, "the origin of something that is not a WindowProxy was asked for");
    return p->origin;
}

void window_proxy_navigate(JSContext *ctx, JSValueConst proxy, JSValueConst window, const char *origin)
{
    ProxyData *p = proxy_of(proxy);   /* CAPTURED FIRST: the flow that navigates owns the change, not its siblings */
    DCHECK(p != NULL, "something that is not a WindowProxy was navigated");
    JS_FreeValue(ctx, p->window);
    p->window = JS_DupValue(ctx, window);
    p->origin = proxy_strdup(origin ? origin : "null");
    p->doc = world_local_doc();
}


/* §7.2.5.1's MEMBER SURFACE, and the one distinction that decides how each member is answered.
 *
 * A NAVIGABLE IS NOT ITS ACTIVE DOCUMENT. That sentence is the reason this interface exists at all — a proxy
 * outlives the documents in it — and it is also the reason most of the surface below never leaves this
 * instance. `closed`, `name`, `opener`, `parent`, `top` and the four self-references are properties of the
 * NAVIGABLE, and a navigable belongs to the instance that CREATED it: this one made the child, named it, knows
 * what it was nested in and knows whether it has been destroyed. Only a member that reads through to the
 * ACTIVE DOCUMENT — `length` counts the child's own child navigables — is a question for the peer.
 *
 * THAT SPLIT WAS THE WHOLE BUG. Every member here used to be a host request, so an iframe's `contentWindow`
 * could answer nothing at all until a host could run a second document; `otherW.self === otherW` — which is
 * true by definition, in any browser, about a navigable nobody has to look inside — parked its flow. Answering
 * the navigable's own state locally is not an optimisation, it is where the state actually is.
 *
 * WHY THE REMOTE HALF IS A STEP MACHINE. `otherWindow.length` must answer at its own call site, and the answer
 * is not available in this turn. There is exactly one shape in this engine that can suspend and answer at the
 * same call site: post the question on the flow's pending register, return JS_STEP_YIELD, and the scheduler
 * parks the flow — byte-identical, at any depth — while its siblings run. The host routes it to the instance
 * holding that document, because SECURITY.md makes the trusted zone the only thing that knows which that is.
 *
 * THE WORLD TRAVELS WITH IT. The question is not "what is document 7's `length`" but "what is it in THIS
 * FLOW'S world" — two arms of a fork that framed it differently must get different answers, which is the same
 * reason a fork re-issues an unanswered request under its own world rather than sharing the id.
 *
 * A LOCAL PROXY forwards everything to its Window with an ordinary read, in this turn: it IS the same document,
 * so its Window is authoritative for every member including the navigable's own. */
enum {
    /* §7.2.5's four names for THIS navigable's own proxy. `window`, `self` and `frames` are on §7.2.5.1's
       cross-origin whitelist; `globalThis` is the global object, which IS the proxy, and is same-origin only —
       the distinction costs nothing here because both answers are the same object. */
    WP_WINDOW, WP_SELF, WP_FRAMES, WP_GLOBALTHIS,
    WP_PARENT, WP_TOP, WP_OPENER, WP_CLOSED, WP_NAME,
    WP_LENGTH,   /* the ACTIVE DOCUMENT's — the one member below that leaves this instance */
    WP_MEMBER_N
};
static const char *const PROXY_MEMBER[WP_MEMBER_N] = {
    "window", "self", "frames", "globalThis", "parent", "top", "opener", "closed", "name", "length"
};
/* §7.2.5.1's CROSS-ORIGIN PROPERTY NAMES — the fixed list a WindowProxy exposes whatever the origins are:
   window, self, location, close, closed, focus, blur, frames, length, top, opener, parent, postMessage.
   Everything else is same-origin ONLY, and reaching it across origins is a SecurityError.
 *
 * THE LIST IS DECLARED BESIDE THE MEMBERS rather than checked at each read, because the failure mode of the
 * alternative is silence: this surface had NO check at all, and it was safe only by the accident that the
 * members installed so far happen to be nearly the whole allowlist. The first member added without one —
 * `document`, `location`'s same-origin half, anything reached through them — would have leaked a cross-origin
 * document with nothing to say so. A member declared here cannot be added without answering the question. */
static const bool PROXY_CROSS_ORIGIN[WP_MEMBER_N] = {
    true,  /* window     */ true,  /* self       */ true,  /* frames     */
    false, /* globalThis — the global OBJECT, and §7.2.5.1 does not list it */
    true,  /* parent     */ true,  /* top        */ true,  /* opener     */
    true,  /* closed     */
    false, /* name — a browsing context's name is NOT on the list; a cross-origin read of it is a SecurityError */
    true,  /* length     */
};

/* §7.2.5.1: a read the origins do not permit is a SecurityError, and it is thrown at the READ rather than
   answered with undefined — a page distinguishes the two, and undefined would say "this window has no such
   member" about one it cannot see. */
static bool proxy_read_permitted(const ProxyData *p, int magic)
{
    return PROXY_CROSS_ORIGIN[magic] || proxy_same_origin(p);
}

/* §7.2.5's `top`: the TOP-LEVEL traversable's proxy. Walked rather than stored, because a navigable's parent
   chain is the only place the answer lives and a cached one goes stale the moment a frame is reparented. */
static JSValue proxy_top(JSContext *ctx, JSValueConst self)
{
    JSValueConst cur = self;

    for (;;) {
        ProxyData *q = JS_GetOpaque(cur, g_proxy_class);
        if (!q || JS_IsUndefined(q->parent)) return JS_DupValue(ctx, cur);
        /* THE CHAIN LEAVES THE PROXIES at this instance's own Window, which is not one — it is the global, and
           the global answers `top` for itself. Following it is what makes a grandchild's `top` this document
           rather than its parent frame. */
        if (!window_proxy_is(q->parent)) return JS_GetPropertyStr(ctx, q->parent, "top");
        cur = q->parent;
    }
}

static JSValue proxy_member_get(JSContext *ctx, JSValueConst this_val, int magic)
{
    ProxyData *p = proxy_of(this_val);   /* CAPTURED: `closed` and `name` are per-flow state a read may precede */

    DCHECK(magic >= 0 && magic < WP_MEMBER_N,
           "a WindowProxy member was declared with a magic this component does not name");
    if (!p) return JS_UNDEFINED;

    /* THE LOCAL CASE: the same document, so its own Window is the authority for every member. A local proxy
       can still be cross-origin — this instance is one document, but a proxy it holds may name one it
       navigated elsewhere — so the check comes first either way. */
    if (!proxy_read_permitted(p, magic))
        return JS_ThrowDOMException(ctx, "SecurityError",
                                    "the origins do not permit reading this member of that Window");
    if (p->doc == world_local_doc()) {
        DCHECK(!JS_IsUndefined(p->window),
               "a local WindowProxy carries no Window — its document id and its binding disagree");
        return JS_GetPropertyStr(ctx, p->window, PROXY_MEMBER[magic]);
    }

    switch (magic) {
    case WP_WINDOW: case WP_SELF: case WP_FRAMES: case WP_GLOBALTHIS:
        return JS_DupValue(ctx, this_val);
    case WP_PARENT:
        /* §7.2.5: the parent navigable's proxy, or THIS one when there is no parent. */
        return JS_IsUndefined(p->parent) ? JS_DupValue(ctx, this_val) : JS_DupValue(ctx, p->parent);
    case WP_TOP:
        return proxy_top(ctx, this_val);
    case WP_OPENER:
        return JS_DupValue(ctx, p->opener);
    case WP_CLOSED:
        return JS_NewBool(ctx, p->closed);
    case WP_NAME:
        /* §7.2.5: a destroyed navigable has no name, and the spec files assert exactly that — the empty string,
           not the name it had. */
        return JS_NewString(ctx, p->closed || !p->name ? "" : p->name);
    default:
        DFAIL("a WindowProxy member with no local answer reached the local switch — WP_LENGTH and anything "
              "added beside it read the ACTIVE DOCUMENT and must be declared on the step machine below");
        return JS_UNDEFINED;
    }
}

/* §7.2.5's `name` SETTER. It renames the BROWSING CONTEXT, which is why `frameW.name = "B"` leaves the
   element's `name` content attribute alone — the spec files assert that pair together, and an implementation
   that reflected one into the other would pass neither. A destroyed navigable has nothing to rename. */
static JSValue proxy_name_set(JSContext *ctx, JSValueConst this_val, JSValueConst v, int magic)
{
    ProxyData *p = proxy_of(this_val);
    const char *n;

    (void)magic;
    if (!p) return JS_UNDEFINED;
    /* §7.2.5.1's write side is the same rule: `name` is not a cross-origin property, so setting it across
       origins is a SecurityError rather than a silent no-op. */
    if (!proxy_read_permitted(p, WP_NAME))
        return JS_ThrowDOMException(ctx, "SecurityError",
                                    "the origins do not permit setting the name of that Window");
    if (p->doc == world_local_doc()) {
        if (JS_SetPropertyStr(ctx, p->window, "name", JS_DupValue(ctx, v)) < 0) return JS_EXCEPTION;
        return JS_UNDEFINED;
    }
    if (p->closed) return JS_UNDEFINED;
    n = JS_ToCString(ctx, v);
    if (!n) return JS_EXCEPTION;
    p->name = proxy_strdup(n);
    JS_FreeCString(ctx, n);
    return JS_UNDEFINED;
}

/* §7.2.5.2's `close()`. A REAL STATE CHANGE, never a no-effect: the navigable is closed and `closed` reports it
   from that point on, which is exactly what a page tests before touching a popup again. §7.2.5.2 returns early
   for a navigable that is not a TOP-LEVEL traversable, so `iframe.contentWindow.close()` does nothing — a frame
   is removed by removing its element, not by closing its window. */
static JSValue proxy_close(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    ProxyData *p = proxy_of(this_val);

    (void)argc; (void)argv; (void)magic;
    if (!p) return JS_UNDEFINED;
    if (p->doc == world_local_doc()) {
        JSAtom a = JS_NewAtom(ctx, "close");
        JSValue r = JS_Invoke(ctx, p->window, a, 0, NULL);
        JS_FreeAtom(ctx, a);
        if (JS_IsException(r)) return r;
        JS_FreeValue(ctx, r);
        return JS_UNDEFINED;
    }
    if (JS_IsUndefined(p->parent)) p->closed = 1;
    return JS_UNDEFINED;
}

void window_proxy_close(JSContext *ctx, JSValueConst proxy)
{
    ProxyData *p = proxy_of(proxy);
    (void)ctx;
    DCHECK(p != NULL, "something that is not a WindowProxy was destroyed");
    p->closed = 1;
}

typedef struct { uint32_t req; } ProxyGetState;

static void proxy_get_visit(JSContext *ctx, void *st, JSStepVisit *v) { (void)ctx; (void)st; (void)v; }

static int proxy_get_step(JSContext *ctx, JSStepHdr *hdr, void *st, int argc, JSValueConst *argv,
                          JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    ProxyGetState *s = st;
    JSValueConst answer;
    int magic = idl_step_magic(hdr);
    ProxyData *p;

    (void)argc; (void)argv; (void)out_cb; (void)out_argc;
    JS_FreeValue(ctx, cb_result);
    DCHECK(magic >= 0 && magic < WP_MEMBER_N,
           "a WindowProxy member was declared with a magic this component does not name");

    p = JS_GetOpaque(hdr->this_val, g_proxy_class);
    if (!p) { *presult = JS_UNDEFINED; return JS_STEP_DONE; }
    if (!proxy_read_permitted(p, magic)) {
        JS_ThrowDOMException(ctx, "SecurityError",
                             "the origins do not permit reading this member of that Window");
        return JS_STEP_ABRUPT;
    }

    /* THE LOCAL CASE, answered in this turn: an ordinary read of the navigable's own Window. */
    if (p->doc == world_local_doc()) {
        DCHECK(!JS_IsUndefined(p->window),
               "a local WindowProxy carries no Window — its document id and its binding disagree");
        *presult = JS_GetPropertyStr(ctx, p->window, PROXY_MEMBER[magic]);
        return JS_IsException(*presult) ? JS_STEP_ABRUPT : JS_STEP_DONE;
    }
    /* A DESTROYED NAVIGABLE HAS NO ACTIVE DOCUMENT to ask, and §7.2.5 answers for it here rather than parking
       a flow on an instance the host has already torn down. */
    if (p->closed) { *presult = JS_NewInt32(ctx, 0); return JS_STEP_DONE; }

    if (s->req == 0) {
        char op[256];
        Flow *f = flow_running();
        DCHECK(f != NULL, "a cross-document read was issued outside a flow — there would be nothing to suspend");
        /* (document, world, member), and the document by NAME: a `uint32_t doc` is this instance's handle into
           its own name table and means a different document in the peer's. The world is the flow's, because the
           answer is only true in it. */
        snprintf(op, sizeof op, "windowproxy.get\t%s\t%s:%u\t%s",
                 world_doc_name(p->doc), world_doc_name(f->world.doc), f->world.serial, PROXY_MEMBER[magic]);
        s->req = engine_host_request(ctx, op);
        return JS_STEP_YIELD;   /* park; siblings run until the peer answers */
    }
    if (!engine_host_answered(s->req, &answer))
        return JS_STEP_YIELD;
    *presult = engine_host_take(ctx, s->req);
    s->req = 0;
    return JS_STEP_DONE;
}

static const IdlStepDecl PROXY_GET_DECL = { proxy_get_step, sizeof(ProxyGetState), proxy_get_visit, NULL };

void window_proxy_install_members(JSContext *ctx)
{
    int i;
    DCHECK(g_wp_rt != NULL, "WindowProxy members were installed before window_proxy_init ran");
    for (i = 0; i < WP_MEMBER_N; i++) {
        if (i == WP_LENGTH)
            idl_install_accessor_step(ctx, g_proxy_proto, PROXY_MEMBER[i],
                                      idl_getter_id_step(ctx, &PROXY_GET_DECL, i), -1);
        else if (i == WP_NAME)
            idl_install_accessor(ctx, g_proxy_proto, PROXY_MEMBER[i], proxy_member_get, i,
                                 idl_setter_id(ctx, IDL_DOMSTRING, false, proxy_name_set, i));
        else
            idl_install_accessor(ctx, g_proxy_proto, PROXY_MEMBER[i], proxy_member_get, i, -1);
    }
    idl_install_method(ctx, g_proxy_proto, "close", 0, idl_method_id(ctx, NULL, 0, proxy_close, 0));
}

void window_proxy_init(JSContext *ctx, const char *origin)
{
    JSClassDef d = { "WindowProxy", .finalizer = proxy_finalizer, .gc_mark = proxy_gc_mark };
    JSRuntime *rt = JS_GetRuntime(ctx);

    DCHECK(g_wp_rt == NULL || g_wp_rt == rt, "WindowProxy was installed into a second runtime");
    if (g_wp_rt == rt) return;
    g_wp_rt = rt;
    free(g_local_origin);
    g_local_origin = strdup(origin ? origin : "null");
    CHECK(g_local_origin != NULL, "window proxy: OOM recording this document's origin");
    JS_NewClassID(rt, &g_proxy_class);
    JS_NewClass(rt, g_proxy_class, &d);
    g_proxy_proto = JS_NewObject(ctx);
    CHECK(!JS_IsException(g_proxy_proto), "the WindowProxy prototype could not be allocated");
}

JSValueConst window_proxy_proto(void)
{
    DCHECK(g_wp_rt != NULL, "the WindowProxy prototype was asked for before window_proxy_init ran");
    return g_proxy_proto;
}

void window_proxy_free(JSContext *ctx)
{
    OwnedStr *e = g_strings;
    if (!g_wp_rt) return;
    while (e) { OwnedStr *n = e->next; free(e->s); free(e); e = n; }
    g_strings = NULL;
    JS_FreeValue(ctx, g_proxy_proto);
    g_proxy_proto = JS_UNINITIALIZED;
    free(g_local_origin);
    g_local_origin = NULL;
    g_wp_rt = NULL;
}
