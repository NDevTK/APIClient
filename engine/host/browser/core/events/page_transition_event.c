/* THE PageTransitionEvent INTERFACE — HTML §7.2.7.6.
 *
 *     [Exposed=Window]
 *     interface PageTransitionEvent : Event {
 *       constructor(DOMString type, optional PageTransitionEventInit eventInitDict = {});
 *       readonly attribute boolean persisted;
 *     };
 *     dictionary PageTransitionEventInit : EventInit { boolean persisted = false; };
 *
 * WHY IT EXISTS AND WHAT WAS BLOCKED ON IT. `onpageshow` and `onpagehide` are already installed as event
 * handler IDL attributes, so a bundle can register for both and NOTHING could ever fire at them: §7.5.9's
 * unload fires `pagehide` and §7.4.6's reactivation fires `pageshow`, and both do it "using
 * PageTransitionEvent", which did not exist. A handler that never runs is a whole branch of a single-page app
 * that never runs with it — the pagehide handler is where a bundle flushes analytics, posts a beacon and tears
 * down a session, which is exactly the request surface this engine is looking for.
 *
 * `persisted` IS NOT DECORATION EITHER: it is the one bit a handler branches on ("am I being frozen into the
 * back/forward cache, or am I going away"), so an event without it collapses two code paths into one.
 *
 * IT IS A REAL SUBCLASS, like ErrorEvent: `PageTransitionEvent.prototype.__proto__ === Event.prototype`, so
 * `e instanceof Event` holds and `initEvent` works on one. The base half is event_new_derived's.
 *
 * THE SLOT IS AN OWN PROPERTY UNDER A PRIVATE SYMBOL, for the reason event.c gives: a slot written as a
 * property write is captured by the COW delta, so the event's state time-travels with the flow that fired it,
 * and the symbol is a brand a page cannot forge.
 *
 * THE INTERFACE OBJECT IS A PER-REALM INTRINSIC, declared into realm.h's one list beside the prototype rather
 * than installed from a host's hand-written list of globals — §3.7 gives each realm its own interface OBJECT
 * for the same reason it gives each its own prototype, and a component missing from one host's copy is
 * silently absent in that realm with nothing to say so. */
#include "check.h"
#include "quickjs.h"
#include "core/agent_state.h"
#include "core/events/event.h"
#include "core/events/page_transition_event.h"
#include "core/idl_args.h"
#include "core/idl_slots.h"
#include "core/realm.h"

static JSValue   g_key = JS_UNDEFINED;   /* the private Symbol this interface's own slot hangs off */
static JSClassID g_pte_class;   /* the class exists for its per-REALM prototype slot; nothing wears it */
static int       g_ready;
static int       g_ctor_stepid = -1;

static JSValue pte_proto(JSContext *ctx)
{
    JSValue proto = JS_GetClassProto(ctx, g_pte_class);

    DCHECK(!JS_IsNull(proto),
           "PageTransitionEvent.prototype was asked for in a realm that never ran its per-realm install");
    return proto;   /* OWNED */
}

/* §7.2.7.6: "The persisted attribute must return the value it was initialized to." */
static JSValue js_pte_get_persisted(JSContext *ctx, JSValueConst this_val, int magic)
{
    JSAtom k;
    JSValue slots, v;

    (void)magic;
    DCHECK(g_ready, "a PageTransitionEvent attribute was read before page_transition_event_init ran");
    if (!JS_IsObject(this_val))
        return JS_ThrowTypeError(ctx, "a PageTransitionEvent attribute was read on something that is not one");
    k = JS_ValueToAtom(ctx, g_key);
    if (k == JS_ATOM_NULL) return JS_EXCEPTION;
    if (JS_GetOwnSlot(ctx, &slots, this_val, k) <= 0) slots = JS_UNDEFINED;
    JS_FreeAtom(ctx, k);
    if (!JS_IsObject(slots)) {
        JS_FreeValue(ctx, slots);
        return JS_ThrowTypeError(ctx, "a PageTransitionEvent attribute was read on something that is not one");
    }
    v = JS_GetPropertyStr(ctx, slots, "persisted");
    JS_FreeValue(ctx, slots);
    return v;
}

/* The one own slot, placed on an event whose Event half is already built. Returns -1 with the throw live. */
static int pte_init_slot(JSContext *ctx, JSValueConst ev, bool persisted)
{
    JSValue slots = idl_slots_new(ctx);
    JSAtom k = JS_ValueToAtom(ctx, g_key);

    if (JS_IsException(slots) || k == JS_ATOM_NULL) {
        JS_FreeValue(ctx, slots);
        if (k != JS_ATOM_NULL) JS_FreeAtom(ctx, k);
        return -1;
    }
    JS_SetPropertyStr(ctx, slots, "persisted", JS_NewBool(ctx, persisted));
    JS_SetProperty(ctx, (JSValue)ev, k, slots);
    JS_FreeAtom(ctx, k);
    return 0;
}

JSValue page_transition_event_new(JSContext *ctx, const char *type, bool persisted)
{
    JSValue tv, ev;

    DCHECK(g_ready, "a page transition event was fired before page_transition_event_init declared the "
                    "interface — HTML §7.2.7.6 fires it USING PageTransitionEvent, so the interface has to "
                    "exist before §7.5.9 can");
    DCHECK(type != NULL && *type,
           "fire a page transition event was given no eventName — the algorithm is named-by-its-caller "
           "(§7.5.9's `pagehide`, §7.4.6's `pageshow`) and there is no default");
    tv = JS_NewString(ctx, type);
    if (JS_IsException(tv))
        return tv;
    /* §7.2.7.6's four initialisers and no others: `persisted`, `cancelable` TRUE and `bubbles` TRUE — both
       historical, and neither observable except that a handler reads them. It is NOT composed, and it IS
       trusted: the user agent fired it. */
    ev = event_new_derived(ctx, pte_proto(ctx), tv, /*bubbles*/ true, /*cancelable*/ true,
                           /*composed*/ false, /*trusted*/ true);
    JS_FreeValue(ctx, tv);
    if (JS_IsException(ev))
        return ev;
    if (pte_init_slot(ctx, ev, persisted) < 0) {
        JS_FreeValue(ctx, ev);
        return JS_EXCEPTION;
    }
    return ev;
}

/* ---- the constructor ----------------------------------------------------------------------------------------
 *
 * `constructor(DOMString type, optional PageTransitionEventInit eventInitDict = {})`. PageTransitionEventInit
 * INHERITS EventInit, and Web IDL converts a dictionary's members with the INHERITED ones first and each level
 * lexicographically among itself — which is the order this list is in, and the order a page pins by throwing
 * from one member's getter. `persisted` sorts before `composed`, so a single sorted list would read the
 * derived dictionary's member before the base's: THE LEVEL is what makes this list the spec's read order. */
static const IdlArgType PTE_CTOR_ARGS[2] = { IDL_DOMSTRING, IDL_DICT };
static const IdlDictMember PTE_INIT[] = {
    { "bubbles", IDL_BOOLEAN }, { "cancelable", IDL_BOOLEAN }, { "composed", IDL_BOOLEAN },
    { "persisted", IDL_BOOLEAN, false, NULL, 1 },
};

static JSValue js_pte_ctor(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    JSValueConst init = argc > 1 ? argv[1] : JS_UNDEFINED;
    JSValue ev;

    (void)magic;
    if (JS_IsUndefined(this_val))
        return JS_ThrowTypeError(ctx, "constructor PageTransitionEvent requires 'new'");
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "PageTransitionEvent constructor requires a type");
    /* §2.2's constructor steps with THIS interface's prototype — an event the PAGE constructs is untrusted. */
    ev = event_new_derived(ctx, pte_proto(ctx), argv[0],
                           idl_dict_bool(ctx, init, "bubbles"),
                           idl_dict_bool(ctx, init, "cancelable"),
                           idl_dict_bool(ctx, init, "composed"), /*trusted*/ false);
    if (JS_IsException(ev))
        return ev;
    if (pte_init_slot(ctx, ev, idl_dict_bool(ctx, init, "persisted")) < 0) {
        JS_FreeValue(ctx, ev);
        return JS_EXCEPTION;
    }
    return ev;
}

/* ---- install ------------------------------------------------------------------------------------------------ */

static const JSCFunctionListEntry js_pte_proto[] = {
    JS_CGETSET_MAGIC_DEF("persisted", js_pte_get_persisted, NULL, 0),
};

void page_transition_event_init(JSContext *ctx)
{
    JSClassDef d = { "PageTransitionEvent" };

    DCHECK(!g_ready, "page_transition_event_init ran twice — the interface is declared once per AGENT");
    g_key = JS_NewSymbol(ctx, "pageTransitionEventSlots", false);
    CHECK(!JS_IsException(g_key), "the PageTransitionEvent slot key allocation failed");
    JS_NewClassID(JS_GetRuntime(ctx), &g_pte_class);
    JS_NewClass(JS_GetRuntime(ctx), g_pte_class, &d);
    g_ctor_stepid = idl_method_id_dict(ctx, PTE_CTOR_ARGS, 2, PTE_INIT,
                                       (int)(sizeof(PTE_INIT) / sizeof(PTE_INIT[0])), js_pte_ctor, 0);
    idl_optional_from(1);                       /* `optional PageTransitionEventInit eventInitDict = {}` */
    g_ready = 1;
    /* WHAT THIS COMPONENT HOLDS FOR THE AGENT, DECLARED — AND IT NAMES THE `event` ROW, NOT THIS FILE.
       core/agent_state.h: a sub-component names the row whose RELEASE gives its slots back, which for every
       Event subclass is core/platform.c's `event` row — event_init calls this init and event_free calls this
       release. Nothing here was declared at all, so the pairing's own arm — does anybody release this? — was
       never asked about any of these. */
    agent_state_flag("event", &g_ready,
                     "HTML §7.2.7.6 The PageTransitionEvent interface's declaration latch");
    agent_state_class("event", &g_pte_class,
                      "HTML §7.2.7.6 The PageTransitionEvent interface's class, held for its per-realm prototype "
                      "slot");
    agent_state_value("event", &g_key,
                      "the private Symbol HTML §7.2.7.6 The PageTransitionEvent interface's slot record hangs off");
    agent_state_id("event", &g_ctor_stepid,
                   "HTML §7.2.7.6 The PageTransitionEvent interface's `constructor(DOMString type, optional "
                   "PageTransitionEventInit eventInitDict = {})`");
    realm_declare_intrinsic(page_transition_event_install_protos);
}

void page_transition_event_install_protos(JSContext *ctx)
{
    JSValue proto, prev, base, ctor, global;

    DCHECK(g_ready, "a realm asked for PageTransitionEvent before page_transition_event_init declared it");
    prev = JS_GetClassProto(ctx, g_pte_class);
    DCHECK(JS_IsNull(prev), "page_transition_event_install_protos ran twice in one realm");
    JS_FreeValue(ctx, prev);
    base = event_proto(ctx);
    proto = JS_NewObjectProto(ctx, base);
    JS_FreeValue(ctx, base);
    CHECK(!JS_IsException(proto), "PageTransitionEvent.prototype could not be allocated");
    idl_interface_tag(ctx, proto, "PageTransitionEvent");
    JS_SetPropertyFunctionList(ctx, proto, js_pte_proto, (int)(sizeof(js_pte_proto) / sizeof(js_pte_proto[0])));
    JS_SetClassProto(ctx, g_pte_class, JS_DupValue(ctx, proto));

    /* §3.7.1's interface object, on THIS realm's global — one `PageTransitionEvent` per realm, whose
       `prototype` is the prototype this same install just built. */
    ctor = idl_step_constructor(ctx, "PageTransitionEvent", 1, g_ctor_stepid);
    CHECK(!JS_IsException(ctor), "the PageTransitionEvent interface object could not be allocated");
    JS_SetConstructor(ctx, ctor, proto);
    JS_FreeValue(ctx, proto);
    global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, "PageTransitionEvent", ctor);
    JS_FreeValue(ctx, global);
}

/* THE RUNTIME, NOT A REALM — core/platform.h's release column, reached through event_free. What this
   gives back is the AGENT's: a private Symbol, a class id and this interface's member declarations; every
   prototype it built is in some realm's class-proto slot and goes with that realm. */
void page_transition_event_free(JSRuntime *rt)
{
    /* NOT `if (!g_ready) return;`. core/events/event.c's event_init calls this component's init on the ONE
       declaration pass and its event_free — which has already asserted its own latch — calls this release
       unconditionally, so the test could never be true and what it could do was hide a release that left the
       latch set. */
    DCHECK(g_ready, "HTML §7.2.7.6 The PageTransitionEvent interface was released in an agent that never declared "
                    "it — event_init declares every Event subclass on the one unconditional pass");
    JS_FreeValueRT(rt, g_key);   /* the prototypes are the REALMS' — each is released with its context */
    g_key = JS_UNDEFINED;
    g_ready = 0;
    /* core/agent_state.h's one policy: a class id is given back like every other slot, because the id doubles
       as the init latch and a carried one names a class in a runtime that is gone. Nothing WEARS this class —
       it exists for its per-realm prototype slot, and every event in this engine is minted by
       core/events/event.c's event_make_proto through JS_NewObjectProto — so there is no finalizer and no
       gc_mark here to owe the JS_GetAnyOpaque the zeroing costs a component whose objects do wear one. */
    g_pte_class = 0;
    g_ctor_stepid = -1;
}
