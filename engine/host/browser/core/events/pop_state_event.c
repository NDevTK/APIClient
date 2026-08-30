/* THE PopStateEvent INTERFACE — HTML §7.2.7.2.
 *
 *     [Exposed=Window]
 *     interface PopStateEvent : Event {
 *       constructor(DOMString type, optional PopStateEventInit eventInitDict = {});
 *       readonly attribute any state;
 *       readonly attribute boolean hasUAVisualTransition;
 *     };
 *     dictionary PopStateEventInit : EventInit {
 *       any state = null;
 *       boolean hasUAVisualTransition = false;
 *     };
 *
 * WHY IT EXISTS AND WHAT WAS BLOCKED ON IT. §7.4.6.2's update-document-for-history-step-application fires
 * `popstate` "using PopStateEvent" and there was no such interface, so core/frame/history.c installed no `go`,
 * `back` or `forward` at all — a traversal that moved the current session history step without firing the event
 * is a page that believes it navigated and a router that never re-renders. Every client-side router listens for
 * this event: it is the ONLY notification a same-document back or forward gives, so with it absent the browser
 * half could push history entries and could never leave them. `history.back()` is also how a great deal of
 * shipped code closes a modal, leaves a wizard step, or returns from a detail view — code paths whose fetches
 * are exactly the surface this engine is looking for.
 *
 * `state` IS NOT DECORATION: it is what the handler branches on. `window.onpopstate = e => e.state ? render(e.state)
 * : boot()` is the ordinary shape, so an event without it does not lose a property quietly — it makes the test
 * `undefined` and sends every traversal down the wrong arm. §7.2.7.2: "The state attribute must return the value
 * it was initialized to. It represents the context information for the event, or null, if the state represented
 * is the initial state of the Document."
 *
 * `hasUAVisualTransition` IS A REAL QUESTION ABOUT THE USER AGENT, not a flag to default: "Returns true if the
 * user agent performed a visual transition for this navigation before dispatching this event. If true, the best
 * user experience will be given if the author synchronously updates the DOM to the post-navigation state." A
 * page reads it to decide whether to animate, so the answer decides which code runs. The caller answers it,
 * because it is a fact about what the user agent did and not about the event.
 *
 * IT IS A REAL SUBCLASS, like PageTransitionEvent: `PopStateEvent.prototype.__proto__ === Event.prototype`, so
 * `e instanceof Event` holds and `initEvent` works on one. The base half is event_new_derived's.
 *
 * THERE IS NO createEvent ROW, and that is DOM §4.5's table rather than a gap: it names BeforeUnloadEvent,
 * CompositionEvent, CustomEvent, DeviceMotionEvent, DeviceOrientationEvent, DragEvent, Event, FocusEvent,
 * HashChangeEvent, KeyboardEvent, MessageEvent, MouseEvent, StorageEvent, TextEvent, TouchEvent and UIEvent, and
 * not this one. `document.createEvent('PopStateEvent')` is step 3's NotSupportedError in every browser.
 *
 * THE SLOTS ARE OWN PROPERTIES UNDER A PRIVATE SYMBOL, for the reason event.c gives: a slot written as a
 * property write is captured by the COW delta, so the event's state time-travels with the flow that fired it,
 * and the symbol is a brand a page cannot forge. It matters more here than for most events — `state` is a whole
 * deserialized object graph, and two forked flows traversing to two different entries hold two of them.
 *
 * THE INTERFACE OBJECT IS A PER-REALM INTRINSIC, declared into realm.h's one list beside the prototype rather
 * than installed from a host's hand-written list of globals — §3.7 gives each realm its own interface OBJECT for
 * the same reason it gives each its own prototype. */
#include "check.h"
#include "quickjs.h"
#include "core/agent_state.h"
#include "core/events/event.h"
#include "core/events/pop_state_event.h"
#include "core/idl_args.h"
#include "core/idl_slots.h"
#include "core/realm.h"

static JSValue   g_key = JS_UNDEFINED;   /* the private Symbol this interface's own slots hang off */
static JSClassID g_pse_class;   /* the class exists for its per-REALM prototype slot; nothing wears it */
static int       g_ready;
static int       g_ctor_stepid = -1;

static JSValue pse_proto(JSContext *ctx)
{
    JSValue proto = JS_GetClassProto(ctx, g_pse_class);

    DCHECK(!JS_IsNull(proto),
           "PopStateEvent.prototype was asked for in a realm that never ran its per-realm install");
    return proto;   /* OWNED */
}

/* The two attributes "must return the value it was initialized to", read out of the one slot record. `magic` IS
   the member, so there is one body rather than two copies of the same brand check. */
enum { PSE_STATE = 0, PSE_HAS_UA_VISUAL_TRANSITION };
static const char *const PSE_SLOT_NAME[] = { "state", "hasUAVisualTransition" };

static JSValue js_pse_get(JSContext *ctx, JSValueConst this_val, int magic)
{
    JSAtom k;
    JSValue slots, v;

    DCHECK(g_ready, "a PopStateEvent attribute was read before pop_state_event_init ran");
    DCHECK(magic >= PSE_STATE && magic <= PSE_HAS_UA_VISUAL_TRANSITION,
           "a PopStateEvent accessor was installed with a magic this interface has no member for");
    if (!JS_IsObject(this_val))
        return JS_ThrowTypeError(ctx, "a PopStateEvent attribute was read on something that is not one");
    k = JS_ValueToAtom(ctx, g_key);
    if (k == JS_ATOM_NULL) return JS_EXCEPTION;
    if (JS_GetOwnSlot(ctx, &slots, this_val, k) <= 0) slots = JS_UNDEFINED;
    JS_FreeAtom(ctx, k);
    if (!JS_IsObject(slots)) {
        JS_FreeValue(ctx, slots);
        return JS_ThrowTypeError(ctx, "a PopStateEvent attribute was read on something that is not one");
    }
    v = JS_GetPropertyStr(ctx, slots, PSE_SLOT_NAME[magic]);
    JS_FreeValue(ctx, slots);
    return v;
}

/* The two own slots, placed on an event whose Event half is already built. `state` is a VALUE and is DUPPED:
   §7.4.6.2 hands over the very object `history.state` answers, and a page comparing `e.state === history.state`
   after a traversal must find them the same object — the standard initializes the attribute TO the history
   object's state rather than to a copy of it. Returns -1 with the throw live. */
static int pse_init_slots(JSContext *ctx, JSValueConst ev, JSValueConst state, bool has_ua_visual_transition)
{
    JSValue slots = idl_slots_new(ctx);
    JSAtom k = JS_ValueToAtom(ctx, g_key);

    if (JS_IsException(slots) || k == JS_ATOM_NULL) {
        JS_FreeValue(ctx, slots);
        if (k != JS_ATOM_NULL) JS_FreeAtom(ctx, k);
        return -1;
    }
    JS_SetPropertyStr(ctx, slots, "state", JS_DupValue(ctx, state));
    JS_SetPropertyStr(ctx, slots, "hasUAVisualTransition", JS_NewBool(ctx, has_ua_visual_transition));
    JS_SetProperty(ctx, (JSValue)ev, k, slots);
    JS_FreeAtom(ctx, k);
    return 0;
}

JSValue pop_state_event_new_to_fire(JSContext *ctx, JSValueConst state, bool has_ua_visual_transition)
{
    JSValue tv, ev;

    DCHECK(g_ready, "a popstate event was fired before pop_state_event_init declared the interface — HTML "
                    "§7.4.6.2 fires it USING PopStateEvent, so the interface has to exist before the traversal "
                    "can");
    tv = JS_NewString(ctx, "popstate");
    if (JS_IsException(tv))
        return tv;
    /* DOM's fire-an-event with none of the three flags set, because §7.4.6.2 sets none of them. It is TRUSTED:
       the user agent fired it, which is the difference a page reads off `isTrusted`. */
    ev = event_new_derived(ctx, pse_proto(ctx), tv, /*bubbles*/ false, /*cancelable*/ false,
                           /*composed*/ false, /*trusted*/ true);
    JS_FreeValue(ctx, tv);
    if (JS_IsException(ev))
        return ev;
    if (pse_init_slots(ctx, ev, state, has_ua_visual_transition) < 0) {
        JS_FreeValue(ctx, ev);
        return JS_EXCEPTION;
    }
    return ev;
}

/* ---- the constructor ----------------------------------------------------------------------------------------
 *
 * `constructor(DOMString type, optional PopStateEventInit eventInitDict = {})`. PopStateEventInit INHERITS
 * EventInit, and Web IDL converts a dictionary's members with the INHERITED ones first and each level
 * lexicographically among itself — which is the order this list is in, and the order a page pins by throwing
 * from one member's getter. `hasUAVisualTransition` sorts before `state` within the derived dictionary, and both
 * sort after the base's three: a single sorted list would read `composed` between them.
 *
 * `state` IS DECLARED IDL_ANY WITH THE IDL'S OWN `= null` DEFAULT, so `new PopStateEvent("popstate").state` is
 * null and not undefined without this body deciding anything — the declaration carries it, which is what makes
 * the default checkable rather than a line in a body. */
static const IdlArgType PSE_CTOR_ARGS[2] = { IDL_DOMSTRING, IDL_DICT };
static const IdlDictMember PSE_INIT[] = {
    { "bubbles", IDL_BOOLEAN }, { "cancelable", IDL_BOOLEAN }, { "composed", IDL_BOOLEAN },
    { "hasUAVisualTransition", IDL_BOOLEAN, false, NULL, 1 },
    { "state", IDL_ANY, false, NULL, 1, NULL, IDL_DEFAULT_NULL, NULL },
};

static JSValue js_pse_ctor(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    JSValueConst init = argc > 1 ? argv[1] : JS_UNDEFINED;
    JSValue state, ev;

    (void)magic;
    if (JS_IsUndefined(this_val))
        return JS_ThrowTypeError(ctx, "constructor PopStateEvent requires 'new'");
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "PopStateEvent constructor requires a type");
    state = idl_dict_get(ctx, init, "state");
    /* DOM §2.5 "Constructing events" with THIS interface's prototype — an event the PAGE constructs is untrusted. */
    ev = event_new_derived(ctx, pse_proto(ctx), argv[0],
                           idl_dict_bool(ctx, init, "bubbles"),
                           idl_dict_bool(ctx, init, "cancelable"),
                           idl_dict_bool(ctx, init, "composed"), /*trusted*/ false);
    if (!JS_IsException(ev) &&
        pse_init_slots(ctx, ev, state, idl_dict_bool(ctx, init, "hasUAVisualTransition")) < 0) {
        JS_FreeValue(ctx, ev);
        ev = JS_EXCEPTION;
    }
    JS_FreeValue(ctx, state);
    return ev;
}

/* ---- install ------------------------------------------------------------------------------------------------ */

static const JSCFunctionListEntry js_pse_proto[] = {
    JS_CGETSET_MAGIC_DEF("state", js_pse_get, NULL, PSE_STATE),
    JS_CGETSET_MAGIC_DEF("hasUAVisualTransition", js_pse_get, NULL, PSE_HAS_UA_VISUAL_TRANSITION),
};

void pop_state_event_init(JSContext *ctx)
{
    JSClassDef d = { "PopStateEvent" };

    DCHECK(!g_ready, "pop_state_event_init ran twice — the interface is declared once per AGENT");
    g_key = JS_NewSymbol(ctx, "popStateEventSlots", false);
    CHECK(!JS_IsException(g_key), "the PopStateEvent slot key allocation failed");
    JS_NewClassID(JS_GetRuntime(ctx), &g_pse_class);
    JS_NewClass(JS_GetRuntime(ctx), g_pse_class, &d);
    g_ctor_stepid = idl_method_id_dict(ctx, PSE_CTOR_ARGS, 2, PSE_INIT,
                                       (int)(sizeof(PSE_INIT) / sizeof(PSE_INIT[0])), js_pse_ctor, 0);
    idl_optional_from(1);                       /* `optional PopStateEventInit eventInitDict = {}` */
    g_ready = 1;
    /* WHAT THIS COMPONENT HOLDS FOR THE AGENT, DECLARED — AND IT NAMES THE `event` ROW, NOT THIS FILE.
       core/agent_state.h: a sub-component names the row whose RELEASE gives its slots back, which for every
       Event subclass is core/platform.c's `event` row — event_init calls this init and event_free calls this
       release. Nothing here was declared at all, so the pairing's own arm — does anybody release this? — was
       never asked about any of these. */
    agent_state_flag("event", &g_ready,
                     "HTML §7.2.7.2 The PopStateEvent interface's declaration latch");
    agent_state_class("event", &g_pse_class,
                      "HTML §7.2.7.2 The PopStateEvent interface's class, held for its per-realm prototype slot");
    agent_state_value("event", &g_key,
                      "the private Symbol HTML §7.2.7.2 The PopStateEvent interface's slot record hangs off");
    agent_state_id("event", &g_ctor_stepid,
                   "HTML §7.2.7.2 The PopStateEvent interface's `constructor(DOMString type, optional "
                   "PopStateEventInit eventInitDict = {})`");
    realm_declare_intrinsic(pop_state_event_install_protos);
}

void pop_state_event_install_protos(JSContext *ctx)
{
    JSValue proto, prev, base, ctor, global;

    DCHECK(g_ready, "a realm asked for PopStateEvent before pop_state_event_init declared it");
    prev = JS_GetClassProto(ctx, g_pse_class);
    DCHECK(JS_IsNull(prev), "pop_state_event_install_protos ran twice in one realm");
    JS_FreeValue(ctx, prev);
    base = event_proto(ctx);
    proto = JS_NewObjectProto(ctx, base);
    JS_FreeValue(ctx, base);
    CHECK(!JS_IsException(proto), "PopStateEvent.prototype could not be allocated");
    idl_interface_tag(ctx, proto, "PopStateEvent");
    JS_SetPropertyFunctionList(ctx, proto, js_pse_proto, (int)(sizeof(js_pse_proto) / sizeof(js_pse_proto[0])));
    JS_SetClassProto(ctx, g_pse_class, JS_DupValue(ctx, proto));

    /* §3.7.1's interface object, on THIS realm's global — one `PopStateEvent` per realm, whose `prototype` is
       the prototype this same install just built. */
    ctor = idl_step_constructor(ctx, "PopStateEvent", 1, g_ctor_stepid);
    CHECK(!JS_IsException(ctor), "the PopStateEvent interface object could not be allocated");
    JS_SetConstructor(ctx, ctor, proto);
    JS_FreeValue(ctx, proto);
    global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, "PopStateEvent", ctor);
    JS_FreeValue(ctx, global);
}

/* THE RUNTIME, NOT A REALM — core/platform.h's release column, reached through event_free. What this
   gives back is the AGENT's: a private Symbol, a class id and this interface's member declarations; every
   prototype it built is in some realm's class-proto slot and goes with that realm. */
void pop_state_event_free(JSRuntime *rt)
{
    /* NOT `if (!g_ready) return;`. core/events/event.c's event_init calls this component's init on the ONE
       declaration pass and its event_free — which has already asserted its own latch — calls this release
       unconditionally, so the test could never be true and what it could do was hide a release that left the
       latch set. */
    DCHECK(g_ready, "HTML §7.2.7.2 The PopStateEvent interface was released in an agent that never declared it — "
                    "event_init declares every Event subclass on the one unconditional pass");
    JS_FreeValueRT(rt, g_key);   /* the prototypes are the REALMS' — each is released with its context */
    g_key = JS_UNDEFINED;
    g_ready = 0;
    /* core/agent_state.h's one policy: a class id is given back like every other slot, because the id doubles
       as the init latch and a carried one names a class in a runtime that is gone. Nothing WEARS this class —
       it exists for its per-realm prototype slot, and every event in this engine is minted by
       core/events/event.c's event_make_proto through JS_NewObjectProto — so there is no finalizer and no
       gc_mark here to owe the JS_GetAnyOpaque the zeroing costs a component whose objects do wear one. */
    g_pse_class = 0;
    g_ctor_stepid = -1;
}
