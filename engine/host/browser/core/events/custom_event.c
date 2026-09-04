/* THE CustomEvent INTERFACE — DOM §2.4 Interface CustomEvent.
 *
 *     [Exposed=*]
 *     interface CustomEvent : Event {
 *       constructor(DOMString type, optional CustomEventInit eventInitDict = {});
 *       readonly attribute any detail;
 *       undefined initCustomEvent(DOMString type, optional boolean bubbles = false,
 *                                 optional boolean cancelable = false, optional any detail = null); // legacy
 *     };
 *     dictionary CustomEventInit : EventInit {
 *       any detail = null;
 *     };
 *
 * WHY IT EXISTS AND WHAT WAS BLOCKED ON IT. DOM §2.4 Interface CustomEvent states the interface's purpose in
 * one sentence — "Events using the CustomEvent interface can be used to carry custom data" — and that is the
 * whole of how an application talks to itself over the DOM. `el.dispatchEvent(new CustomEvent('thing', {detail:
 * payload}))` is the component-to-component channel of every widget library, every design system and every
 * micro-frontend loader shipped, and `CustomEvent` is on browser/platform_names.h's list, so solver/absent.c's
 * read hook LEFT IT ALONE and `new CustomEvent(...)` was a ReferenceError naming this component. An uncaught
 * ReferenceError at the top of a module ends that module, and everything it would have gone on to do is code no
 * run has ever reached.
 *
 * IT WAS RANKED BY EVIDENCE AND NOT BY THE GAP AUDITOR'S ORDERING, and the METHOD is the part that keeps: the
 * auditor prints absences per interface and Window's row is the longest, so working that list by size spends a
 * session on members no bundle touches. What a page can actually reach is a different question and it has a
 * derivation — take the global names the ENGINE ITSELF reports absent (every name in browser/platform_names.h
 * that answers false to `in globalThis`, which the engine can be asked for in one document) and count the
 * unambiguous construction sites for each in real frozen bundles: `new X(`, `window.X`, `X instanceof`. That
 * is a static census over bytes and never a run, so every row of it is a claim to CHECK by running one — which
 * is what a probe document with one `<script>` per member does, since an uncaught error per block leaves the
 * next block to run and each absence reports itself once. Run at the revision this component landed at, the
 * top of that ranking was CustomEvent, ahead of ResizeObserver and PerformanceObserver, and only one of its
 * construction sites stood behind a feature test. The NUMBERS are not repeated here — they belong to the
 * revision they were measured at — but the derivation can be re-run against any tree, which is the half that
 * cannot go stale.
 *
 * `detail` IS NOT DECORATION: it is the payload, and the handler reads nothing else. `e.detail.id` is the
 * ordinary shape, so an event without it does not lose a property quietly — it makes the read `undefined` and
 * sends the handler's next member access to a TypeError. DOM §2.4 Interface CustomEvent: "The detail
 * attribute must return the value it was initialized to."
 *
 * IT IS A REAL SUBCLASS: `CustomEvent.prototype.__proto__ === Event.prototype`, so `e instanceof Event` holds
 * and `initEvent` works on one. The base half is event_new_derived's.
 *
 * IT HAS A createEvent ROW, and that row was already in core/events/create_event.c's table with a NULL maker
 * and an assert over it whose message is `a createEvent row has no maker for an interface this realm exposes`
 * — this engine's own sentence and not any standard's, so it carries no quotation mark — so exposing
 * this interface without filling the row would abort on the first `document.createEvent('CustomEvent')`. The
 * row and the interface arrive together because that assert makes them.
 *
 * `[Exposed=*]` IS WIDER THAN THIS ENGINE'S ONE GLOBAL, and that is the IDL rather than a narrowing here: the
 * exposure set is every global, this agent builds Window, and a worker global would install the same
 * declarations through the same per-realm intrinsic.
 *
 * THE SLOT IS AN OWN PROPERTY UNDER A PRIVATE SYMBOL, for the reason event.c gives: a slot written as a
 * property write is captured by the COW delta, so the event's state time-travels with the flow that fired it,
 * and the symbol is a brand a page cannot forge. It matters here as much as it does for PopStateEvent's
 * `state` — `detail` is a whole object graph the page built, and two forked arms dispatching the same event
 * hold two of them.
 *
 * THE INTERFACE OBJECT IS A PER-REALM INTRINSIC, declared into realm.h's one list beside the prototype —
 * Web IDL §3.7.1 Interface object gives each realm its own for the same reason §3.7.3 Interface prototype
 * object gives each its own prototype. */
#include <stdbool.h>

#include "check.h"
#include "quickjs.h"
#include "core/agent_state.h"
#include "core/events/custom_event.h"
#include "core/events/event.h"
#include "core/idl_args.h"
#include "core/idl_slots.h"
#include "core/realm.h"

static JSValue   g_key = JS_UNDEFINED;   /* the private Symbol this interface's own slot hangs off */
static JSClassID g_ce_class;   /* the class exists for its per-REALM prototype slot; nothing wears it */
static int       g_ready;
static int       g_ctor_stepid = -1;
static int       g_init_ce_id = -1;

static JSValue ce_proto(JSContext *ctx)
{
    JSValue proto = JS_GetClassProto(ctx, g_ce_class);

    DCHECK(!JS_IsNull(proto),
           "CustomEvent.prototype was asked for in a realm that never ran its per-realm install");
    return proto;   /* OWNED */
}

/* THIS EVENT'S OWN SLOT RECORD, or JS_UNDEFINED for anything that is not a CustomEvent — Web IDL §3.7
 * Interfaces' implementation-check, asked of something a page cannot forge. Returns OWNED. */
static JSValue ce_slots_of(JSContext *ctx, JSValueConst v)
{
    JSAtom k;
    JSValue slots;

    DCHECK(g_ready, "a CustomEvent brand was asked before custom_event_init ran");
    if (!JS_IsObject(v))
        return JS_UNDEFINED;
    k = JS_ValueToAtom(ctx, g_key);
    if (k == JS_ATOM_NULL)
        return JS_EXCEPTION;
    if (JS_GetOwnSlot(ctx, &slots, v, k) <= 0) slots = JS_UNDEFINED;
    JS_FreeAtom(ctx, k);
    if (!JS_IsObject(slots)) {
        JS_FreeValue(ctx, slots);
        return JS_UNDEFINED;
    }
    return slots;
}

/* DOM §2.4 Interface CustomEvent: "The detail attribute must return the value it was initialized to." */
static JSValue js_ce_get_detail(JSContext *ctx, JSValueConst this_val, int magic)
{
    JSValue slots = ce_slots_of(ctx, this_val), v;

    (void)magic;
    if (JS_IsException(slots))
        return slots;
    if (JS_IsUndefined(slots))
        return JS_ThrowTypeError(ctx, "CustomEvent.prototype.detail was read on something that is not a "
                                      "CustomEvent");
    v = JS_GetPropertyStr(ctx, slots, "detail");
    JS_FreeValue(ctx, slots);
    return v;
}

/* The one own slot, placed on an event whose Event half is already built. `detail` is a VALUE and is DUPPED:
   DOM §2.4 Interface CustomEvent initializes the attribute TO the value the page handed over rather than to a
   copy of it, so a page comparing `e.detail === payload` after a dispatch finds the same object. Returns -1
   with the throw live. */
static int ce_init_slots(JSContext *ctx, JSValueConst ev, JSValueConst detail)
{
    JSValue slots = idl_slots_new(ctx);
    JSAtom k = JS_ValueToAtom(ctx, g_key);

    if (JS_IsException(slots) || k == JS_ATOM_NULL) {
        JS_FreeValue(ctx, slots);
        if (k != JS_ATOM_NULL) JS_FreeAtom(ctx, k);
        return -1;
    }
    JS_SetPropertyStr(ctx, slots, "detail", JS_DupValue(ctx, detail));
    JS_SetProperty(ctx, (JSValue)ev, k, slots);
    JS_FreeAtom(ctx, k);
    return 0;
}

/* DOM §4.5 Interface Document's createEvent steps 6-8 overwrite type and isTrusted and unset the initialized
   flag afterwards, so what a maker owes is only a default instance of this interface — here, `detail` at the
   value CustomEventInit's `= null` gives it. */
JSValue custom_event_new(JSContext *ctx)
{
    JSValue tv, ev;

    DCHECK(g_ready, "a CustomEvent was minted before custom_event_init declared the interface");
    tv = JS_NewString(ctx, "");
    if (JS_IsException(tv))
        return tv;
    /* DOM §2.5 Constructing events with none of the three flags set, and UNTRUSTED — DOM §4.5 Interface
       Document's createEvent step 7 sets isTrusted to false in any case, and an event a page's own factory call
       produced was never the user agent's. */
    ev = event_new_derived(ctx, ce_proto(ctx), tv, /*bubbles*/ false, /*cancelable*/ false,
                           /*composed*/ false, /*trusted*/ false);
    JS_FreeValue(ctx, tv);
    if (!JS_IsException(ev) && ce_init_slots(ctx, ev, JS_NULL) < 0) {
        JS_FreeValue(ctx, ev);
        ev = JS_EXCEPTION;
    }
    return ev;
}

/* ---- the legacy initialiser -------------------------------------------------------------------------------
 *
 * DOM §2.4 Interface CustomEvent's three steps, in order: "If this's dispatch flag is set, then return." —
 * "Initialize this with type, bubbles, and cancelable." — "Set this's detail attribute to detail."
 *
 * IT IS NOT A SECOND CONSTRUCTOR: it RE-INITIALISES an event that already exists, base half and own half,
 * which is why it reaches step 2 through core/events/event.c's initialise-an-existing-event rather than
 * rebuilding the object. That function answers false for exactly step 1's condition, so the early return is
 * read off it and is not a second test of the same flag.
 *
 * EVERY ARGUMENT IS READ WITHOUT AN `argc` GUARD, and that is the declaration rather than an omission: the
 * three optional positions carry the IDL's own `= false`, `= false` and `= null`, which Web IDL §3.6 Overload
 * resolution algorithm steps 15.4.1 and 16.1 PLACE, so the body reads the IDL's value instead of inventing one
 * from an absence. */
static JSValue js_ce_init(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    JSValue slots;

    (void)magic;
    DCHECK(argc == 4, "DOM §2.4 Interface CustomEvent's legacy initialiser reached its body at an arity its "
                      "declaration does not produce — all three of its optional positions carry a declared "
                      "default, so Web IDL §3.6 Overload resolution algorithm step 16.1 places one at every "
                      "position the page stopped short of and the count is 4 for every call");
    slots = ce_slots_of(ctx, this_val);
    if (JS_IsException(slots))
        return slots;
    if (JS_IsUndefined(slots))
        return JS_ThrowTypeError(ctx, "CustomEvent.prototype.initCustomEvent was called on something that is "
                                      "not a CustomEvent");
    if (!event_reinit(ctx, this_val, argv[0], JS_ToBool(ctx, argv[1]) != 0, JS_ToBool(ctx, argv[2]) != 0)) {
        JS_FreeValue(ctx, slots);
        return JS_UNDEFINED;   /* step 1: an event being dispatched right now is not re-initialised */
    }
    JS_SetPropertyStr(ctx, slots, "detail", JS_DupValue(ctx, argv[3]));   /* step 3 */
    JS_FreeValue(ctx, slots);
    return JS_UNDEFINED;
}

/* ---- the constructor ---------------------------------------------------------------------------------------
 *
 * `constructor(DOMString type, optional CustomEventInit eventInitDict = {})`. CustomEventInit INHERITS
 * EventInit, and Web IDL §3.2.17 Dictionary types converts a dictionary's members with the INHERITED ones
 * first (step 3's "in order from least to most derived") and each dictionary's own lexicographically among
 * themselves (step 4) — which is the order this list is in, and the order a page pins by throwing from one
 * member's getter and counting which others ran.
 *
 * `detail` CARRIES LEVEL 1 AND EventInit's THREE CARRY 0, because the level is the only place the inheritance
 * is written down. Both orders agree over these four — `detail` sorts after `composed` either way — so a table
 * stating one level for all of them would pass idl_dict_order_check and would go on passing until a member
 * sorting before `bubbles` was added to CustomEventInit, at which point the abort would name a row order that
 * was never wrong.
 *
 * `detail` IS DECLARED IDL_ANY WITH THE IDL'S OWN `= null` DEFAULT, so `new CustomEvent("x").detail` is null
 * and not undefined without this body deciding anything — the declaration carries it, which is what makes the
 * default checkable rather than a line in a body. */
static const IdlArgType CE_CTOR_ARGS[2] = { IDL_DOMSTRING, IDL_DICT };
static const IdlDictMember CE_INIT[] = {
    { "bubbles", IDL_BOOLEAN }, { "cancelable", IDL_BOOLEAN }, { "composed", IDL_BOOLEAN },
    { "detail", IDL_ANY, false, NULL, 1, NULL, IDL_DEFAULT_NULL, NULL },
};

static JSValue js_ce_ctor(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    JSValueConst init = argc > 1 ? argv[1] : JS_UNDEFINED;
    JSValue detail, ev;

    (void)magic;
    if (JS_IsUndefined(this_val))
        return JS_ThrowTypeError(ctx, "constructor CustomEvent requires 'new'");
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "CustomEvent constructor requires a type");
    detail = idl_dict_get(ctx, init, "detail");
    /* DOM §2.5 Constructing events with THIS interface's prototype — an event the PAGE constructs is
       untrusted. */
    ev = event_new_derived(ctx, ce_proto(ctx), argv[0],
                           idl_dict_bool(ctx, init, "bubbles"),
                           idl_dict_bool(ctx, init, "cancelable"),
                           idl_dict_bool(ctx, init, "composed"), /*trusted*/ false);
    if (!JS_IsException(ev) && ce_init_slots(ctx, ev, detail) < 0) {
        JS_FreeValue(ctx, ev);
        ev = JS_EXCEPTION;
    }
    JS_FreeValue(ctx, detail);
    return ev;
}

/* ---- install ------------------------------------------------------------------------------------------------
*/

void custom_event_init(JSContext *ctx)
{
    JSClassDef d = { "CustomEvent" };
    /* `initCustomEvent(type, bubbles, cancelable, detail)` — four arguments, three of them optional, each
       converted by the declaration. The two flags are `boolean` and the declaration says so: IDL_BOOLEAN is
       what makes Web IDL §3.2.3 boolean's conversion FORK over an unknown, where an IDL_ANY position would
       cross unconverted into a `JS_ToBool` whose ECMAScript §7.1.2 ToBoolean ( arg ) answer over the ordinary
       Object an unknown wears is "Return true" — so `e.initCustomEvent(t, cfg.bubbles)` would be a bubbling
       event in every world and the non-bubbling one would be deleted with nothing to say so. */
    static const IdlArgType INIT_CE_ARGS[4] = { IDL_DOMSTRING, IDL_BOOLEAN, IDL_BOOLEAN, IDL_ANY };

    DCHECK(!g_ready, "custom_event_init ran twice — the interface is declared once per AGENT");
    g_key = JS_NewSymbol(ctx, "customEventSlots", false);
    CHECK(!JS_IsException(g_key), "the CustomEvent slot key allocation failed");
    JS_NewClassID(JS_GetRuntime(ctx), &g_ce_class);
    JS_NewClass(JS_GetRuntime(ctx), g_ce_class, &d);
    g_init_ce_id = idl_method_id(ctx, INIT_CE_ARGS, 4, js_ce_init, 0);
    idl_optional_from(1);
    idl_arg_default(1, IDL_DEFAULT_FALSE, NULL);   /* Web IDL §3.6's steps 15.4.1 and 16.1: `= false` */
    idl_arg_default(2, IDL_DEFAULT_FALSE, NULL);
    idl_arg_default(3, IDL_DEFAULT_NULL, NULL);    /* Web IDL §3.6's steps 15.4.1 and 16.1: `= null` */
    g_ctor_stepid = idl_method_id_dict(ctx, CE_CTOR_ARGS, 2, CE_INIT,
                                       (int)(sizeof(CE_INIT) / sizeof(CE_INIT[0])), js_ce_ctor, 0);
    idl_optional_from(1);                       /* `optional CustomEventInit eventInitDict = {}` */
    g_ready = 1;
    /* WHAT THIS COMPONENT HOLDS FOR THE AGENT, DECLARED — and it names the `event` row, not this file.
       core/agent_state.h: a sub-component names the row whose RELEASE gives its slots back, which for every
       Event subclass is core/platform.c's `event` row — event_init calls this init and event_free calls this
       release. */
    agent_state_flag("event", &g_ready,
                     "DOM §2.4 Interface CustomEvent's declaration latch");
    agent_state_class("event", &g_ce_class,
                      "DOM §2.4 Interface CustomEvent's class, held for its per-realm prototype slot");
    agent_state_value("event", &g_key,
                      "the private Symbol DOM §2.4 Interface CustomEvent's slot record hangs off");
    agent_state_id("event", &g_ctor_stepid,
                   "DOM §2.4 Interface CustomEvent's `constructor(DOMString type, optional CustomEventInit "
                   "eventInitDict = {})`");
    agent_state_id("event", &g_init_ce_id,
                   "DOM §2.4 Interface CustomEvent's legacy `initCustomEvent(DOMString type, optional boolean "
                   "bubbles = false, optional boolean cancelable = false, optional any detail = null)`");
    realm_declare_intrinsic(custom_event_install_protos);
}

void custom_event_install_protos(JSContext *ctx)
{
    JSValue proto, prev, base, ctor, global;

    DCHECK(g_ready, "a realm asked for CustomEvent before custom_event_init declared it");
    prev = JS_GetClassProto(ctx, g_ce_class);
    DCHECK(JS_IsNull(prev), "custom_event_install_protos ran twice in one realm");
    JS_FreeValue(ctx, prev);
    base = event_proto(ctx);
    proto = JS_NewObjectProto(ctx, base);
    JS_FreeValue(ctx, base);
    CHECK(!JS_IsException(proto), "CustomEvent.prototype could not be allocated");
    idl_interface_tag(ctx, proto, "CustomEvent");
    idl_install_accessor(ctx, proto, "detail", js_ce_get_detail, 0, -1);
    idl_install_method(ctx, proto, "initCustomEvent", g_init_ce_id);
    JS_SetClassProto(ctx, g_ce_class, JS_DupValue(ctx, proto));

    /* Web IDL §3.7.1 Interface object, on THIS realm's global — one `CustomEvent` per realm, whose `prototype`
       is the prototype this same install just built. */
    ctor = idl_step_constructor(ctx, "CustomEvent", g_ctor_stepid);
    CHECK(!JS_IsException(ctor), "the CustomEvent interface object could not be allocated");
    JS_SetConstructor(ctx, ctor, proto);
    JS_FreeValue(ctx, proto);
    global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, "CustomEvent", ctor);
    JS_FreeValue(ctx, global);
}

/* THE RUNTIME, NOT A REALM — core/platform.h's release column, reached through event_free. What this gives
   back is the AGENT's: a private Symbol, a class id and this interface's member declarations; every prototype
   it built is in some realm's class-proto slot and goes with that realm. */
void custom_event_free(JSRuntime *rt)
{
    /* NOT `if (!g_ready) return;`. core/events/event.c's event_init calls this component's init on the ONE
       declaration pass and its event_free — which has already asserted its own latch — calls this release
       unconditionally, so the test could never be true and what it could do was hide a release that left the
       latch set. */
    DCHECK(g_ready, "DOM §2.4 Interface CustomEvent was released in an agent that never declared it — "
                    "event_init declares every Event subclass on the one unconditional pass");
    JS_FreeValueRT(rt, g_key);   /* the prototypes are the REALMS' — each is released with its context */
    g_key = JS_UNDEFINED;
    g_ready = 0;
    /* core/agent_state.h's one policy: a class id is given back like every other slot, because the id doubles
       as the init latch and a carried one names a class in a runtime that is gone. Nothing WEARS this class —
       it exists for its per-realm prototype slot, and every event in this engine is minted by
       core/events/event.c through JS_NewObjectProto — so there is no finalizer and no gc_mark here. */
    g_ce_class = 0;
    g_ctor_stepid = -1;
    g_init_ce_id = -1;
}
