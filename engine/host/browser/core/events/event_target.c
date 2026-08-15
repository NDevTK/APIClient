/* EVENTTARGET — DOM §2.7, over the objects this engine already hands the page.
 *
 * WPT NAMED THIS ONE. Every DOM test loads testharness.js, which declares its tests and then waits for the LOAD
 * EVENT before reporting — so with no event loop, 175 of 175 dom/nodes files ran the harness perfectly and
 * produced nothing at all. It is also how ordinary pages are written: the half of a bundle that runs on
 * DOMContentLoaded is the half that touches the DOM and calls the API.
 *
 * WHERE THE LISTENERS LIVE. On the TARGET, as an ordinary own property under a private key the page cannot
 * reach. That is not a shortcut — it is what makes registration per-flow for free: a listener added in one arm
 * of a fork is a property write like any other, so the COW delta captures it and the sibling never sees it.
 * A side table keyed by object pointer would have needed its own delta kind and its own swap, for the same
 * result.
 *
 * HOW A LISTENER RUNS. Not by JS_Call from C — a listener body is the page's code and holds loops, awaits and
 * concolic branches, so calling it from a C activation is the drive-to-completion the engine aborts on. Each
 * listener is dispatched through the promise machinery, which is how every other page callback in this engine
 * reaches a flow: the reaction runs as a call-root flow, preemptible and forkable like anything else. */
#include <stdbool.h>
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "core/idl_slots.h"
#include "quickjs-step.h"
#include "core/idl_args.h"
#include "core/realm.h"
#include "core/dom/abort.h"
#include "core/events/event.h"
#include "core/events/event_path.h"
#include "core/events/report_exception.h"

/* The two shapes every DOM member in this file has. Spelled once so a member declares its IDL, not a bitmask. */
static const IdlArgType IDL_1STR[1] = { IDL_DOMSTRING };
static const IdlArgType IDL_2STR[2] = { IDL_DOMSTRING, IDL_DOMSTRING };
#include "core/events/event_target.h"

/* The private key the listener map hangs off. A SYMBOL, so a page enumerating its own objects cannot see it and
   cannot collide with it — the same reason the platform uses internal slots. */
/* `g_ready` rather than testing g_key: a static JSValue is ZERO-initialised, and zero is not JS_UNDEFINED — the
   tag is part of the value. Asking JS_IsUndefined of it answers "no" before anything has run, which fired the
   ran-twice assert on the FIRST call. A JSValue's emptiness is not the allocator's default. */
static JSValue g_key;
static int g_ready;
/* §2.9's PROPAGATION PATH IS THE TREE'S QUESTION, not this component's. Naming node.c here made EVERY host that
   installs events link the whole DOM and lexbor with it, which is why the streams gate could not build with a
   real AbortSignal in it. The tree registers the walk; a host that registers none has a path of one, which is
   exactly §2.7's own get the parent and exactly right for a host with no document — and, for the same reason,
   no shadow trees, which is the default every question below answers with. */
static const EventTargetTree *g_tree;
/* DOM §2.9's ACTIVATION BEHAVIOUR, declared by whoever owns the element — see event_target.h. */
static bool (*g_has_activation)(JSContext *ctx, JSValueConst el);
static int (*g_run_activation)(JSContext *ctx, JSValueConst el, JSValueConst ev,
                               uint8_t *phase, uint32_t *req);
/* HTML §8.1.7's handler map key, and the MARKER that holds the handler's place in a listener list — see the
   event-handler section below. Declared here because event_target_init mints them. */
static JSValue g_handler_key;
static JSValue g_handler_marker;
/* The DISPATCH_PAIR step declaration — the one door a C caller has into the §2.9 machine. The FUNCTION OBJECT
   is minted per fire, in the FIRING REALM, and is never installed anywhere the page can reach: a C function
   runs in the realm that DEFINED it (js_call_c_function does `ctx = p->u.cfunc.realm`), so one object held in a
   static would have carried the agent realm's ctx into every child document's dispatch — and dispatch_path
   reads §7.6's window off that ctx, so a child document's `load` would have propagated to the ROOT window. */
static int g_dispatch_pair_stepid = -1;
/* The ids JS_RegisterStepDef handed this runtime for add/removeEventListener. `type` is a Web IDL DOMString,
   so it is ToString on whatever the page passed and cannot be a JS_ToCString from C. */
static int g_add_stepid = -1, g_remove_stepid = -1, g_dispatch_stepid = -1;
/* §2.7's INTERFACE PROTOTYPE OBJECT. addEventListener, removeEventListener and dispatchEvent live HERE and
   nowhere else: every interface that inherits EventTarget — Node, AbortSignal, MessagePort, BroadcastChannel,
   Window — reaches them by CHAINING to it. They used to be installed onto each of those prototypes in turn,
   which is five copies of three members and, worse, a lie the corpus checks directly: `Node.prototype` is not
   where `addEventListener` is declared, `EventTarget.prototype` is, and `document instanceof EventTarget` is
   false when the interface does not exist at all.
   IT IS PER REALM, in quickjs's own per-context class-proto slot — the same place window.c keeps
   Window.prototype and bar_prop.c keeps BarProp.prototype, and for the same reason: js_call_c_function does
   `ctx = p->u.cfunc.realm`, so a member installed once answers every realm's question with the DEFINING realm's
   ctx forever. Here that is not an identity nicety, it is a wrong ANSWER — §3.6's [Global] rule resolves an
   unqualified `addEventListener('load', f)` against the RELEVANT GLOBAL, which this file reads off `ctx`, so a
   shared prototype registered every iframe's listeners on the ROOT window. A class id is what gives the slot a
   key; the class also brands `new EventTarget()`. */
static JSClassID g_et_class;
static JSValue idl_add_or_remove(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic);
static void event_target_install(JSContext *ctx);

void event_target_init(JSContext *ctx)
{
    JSClassDef d = { "EventTarget" };

    DCHECK(!g_ready, "event_target_init ran twice — one instance is one document");
    g_key = JS_NewSymbol(ctx, "eventListeners", false);
    CHECK(!JS_IsException(g_key), "the event-listener key allocation failed");
    g_handler_key = JS_NewSymbol(ctx, "eventHandlers", false);
    g_handler_marker = JS_NewObject(ctx);
    CHECK(!JS_IsException(g_handler_key) && !JS_IsException(g_handler_marker),
          "the event-handler key or marker allocation failed");
    g_ready = 1;
    if (g_add_stepid < 0) {
        /* (DOMString type, EventListener? callback, optional (AddEventListenerOptions or boolean) options) —
           removeEventListener's third is (EventListenerOptions or boolean), which is the same union with only
           `capture` in it, and reading a member the IDL does not declare there simply never happens. */
        static const IdlArgType ADD_ARGS[3] = { IDL_DOMSTRING, IDL_ANY, IDL_DICT_OR_BOOL_FIRST };
        static const IdlDictMember ADD_OPTS[] = {   /* `capture` FIRST: it is what the bare boolean means */
            { "capture", IDL_BOOLEAN }, { "once", IDL_BOOLEAN },
            /* `passive` is IDL_ANY and NOT IDL_BOOLEAN, and that is the declaration doing its job: a boolean
               member with a `= false` default converts an ABSENT one to false, and §2.7's flatten more options
               needs to know whether the page WROTE it — an absent `passive` is null and is then filled from the
               default passive value, which for a wheel listener on the window is TRUE. The body does the
               ToBoolean, which runs none of the page's code. */
            { "passive", IDL_ANY },
            /* `AbortSignal signal` — IDL_ANY because §3.2's brand is a private slot record and not the class
               opaque IDL_INTERFACE tests; the body performs the interface check, and says so. */
            { "signal", IDL_ANY },
        };
        static const IdlDictMember REMOVE_OPTS[] = { { "capture", IDL_BOOLEAN } };
        g_add_stepid    = idl_method_id_dict(ctx, ADD_ARGS, 3, ADD_OPTS,
                                             (int)(sizeof(ADD_OPTS) / sizeof(ADD_OPTS[0])),
                                             idl_add_or_remove, 0);
        idl_optional_from(2);   /* §2.7: `addEventListener(type, callback, optional options)` */
        g_remove_stepid = idl_method_id_dict(ctx, ADD_ARGS, 3, REMOVE_OPTS,
                                             (int)(sizeof(REMOVE_OPTS) / sizeof(REMOVE_OPTS[0])),
                                             idl_add_or_remove, 1);
        idl_optional_from(2);   /* §2.7: `removeEventListener(type, callback, optional options)` */
    }
    JS_NewClassID(JS_GetRuntime(ctx), &g_et_class);
    JS_NewClass(JS_GetRuntime(ctx), g_et_class, &d);
    /* §2.7's PROTOTYPE IS A PER-REALM INTRINSIC LIKE EVERY OTHER ONE, and it goes in the same list — which is
       why this call is FIRST: the registry installs in declaration order, and every interface that inherits
       EventTarget chains to this realm's prototype while building its own. It was the one component whose
       install was hand-copied into each host's realm builder, which is the exact failure core/realm.h exists
       to end; a host cannot now forget it, because there is no line to forget. */
    realm_declare_intrinsic(event_target_install);
}

/* §2.7's prototype FOR THIS REALM. Owned — the caller frees. */
JSValue event_target_proto(JSContext *ctx)
{
    JSValue proto = JS_GetClassProto(ctx, g_et_class);
    DCHECK(JS_IsObject(proto),
           "EventTarget.prototype was asked for in a realm that never ran event_target_install — a realm whose "
           "intrinsics are half-built answers §2.7 with nothing");
    return proto;
}

void event_target_chain(JSContext *ctx, JSValueConst proto)
{
    JSValue etp;

    DCHECK(JS_IsObject(proto), "an interface prototype that inherits EventTarget is not an object");
    etp = event_target_proto(ctx);
    JS_SetPrototype(ctx, (JSValue)proto, etp);
    JS_FreeValue(ctx, etp);
}

/* §2.7 declares `constructor()`, so EventTarget IS constructible — `new EventTarget()` is a plain event target,
   which is what a page uses to give an ordinary object a listener list. It honours new.target's prototype, so a
   subclass gets its own. */
static JSValue js_event_target_ctor(JSContext *ctx, JSValueConst new_target, int argc, JSValueConst *argv)
{
    JSValue proto = JS_GetPropertyStr(ctx, new_target, "prototype"), obj;
    if (JS_IsException(proto)) return proto;
    /* §3.7.1: a subclass's `prototype` wins; without one it is THIS REALM's — and `ctx` is the CONSTRUCTOR's
       realm, which is the realm the interface object was installed in, so the two agree by construction. */
    if (!JS_IsObject(proto)) {
        JS_FreeValue(ctx, proto);
        proto = event_target_proto(ctx);
    }
    obj = JS_NewObjectProtoClass(ctx, proto, g_et_class);
    JS_FreeValue(ctx, proto);
    (void)argc; (void)argv;
    return obj;
}

void event_target_install_interface(JSContext *ctx, JSValueConst global)
{
    JSValue ctor = JS_NewCFunction2(ctx, js_event_target_ctor, "EventTarget", 0, JS_CFUNC_constructor, 0);
    JSValue proto = event_target_proto(ctx);
    int i;
    /* Web IDL §3.7.3's [Global] RULE REACHES THE INHERITED INTERFACES TOO. Window is declared [Global], and the
       rule is about the OBJECT, not about one interface: every member of every interface in the global's
       inheritance chain is an OWN property of the global. Window includes EventTarget, so
       `window.hasOwnProperty("addEventListener")` is true in every browser, and it is the SAME function object
       as `EventTarget.prototype.addEventListener` — the member is one declaration placed twice, never a second
       one. Reaching it up the chain is observably different: a page that copies the global's own property
       names, or reads a descriptor off `window`, sees nothing there. */
    static const char *const GLOBAL_MEMBERS[3] = { "addEventListener", "removeEventListener", "dispatchEvent" };

    CHECK(!JS_IsException(ctor), "the EventTarget interface object could not be allocated");
    JS_SetConstructor(ctx, ctor, proto);
    for (i = 0; i < 3; i++) {
        JSAtom a = JS_NewAtom(ctx, GLOBAL_MEMBERS[i]);
        JSValue fn = JS_GetProperty(ctx, proto, a);
        CHECK(JS_IsFunction(ctx, fn), "§2.7's prototype is missing a member the global must carry its own "
                                      "reference to — the two lists are one declaration read twice");
        /* Web IDL §3.7.6's flags for an operation, and all three are asserted by the corpus: writable,
           ENUMERABLE and configurable. An IDL member is enumerable — that is what makes a for-in over a
           platform object list the platform's own names — and only [LegacyUnforgeable] takes configurable
           away, which none of these three carry. */
        JS_DefinePropertyValue(ctx, (JSValue)global, a, fn, JS_PROP_C_W_E);
        JS_FreeAtom(ctx, a);
    }
    JS_FreeValue(ctx, proto);
    JS_SetPropertyStr(ctx, (JSValue)global, "EventTarget", ctor);
}

void event_target_set_tree(const EventTargetTree *tree)
{
    DCHECK(tree != NULL && tree->get_parent != NULL && tree->default_passive_target != NULL &&
           tree->root != NULL && tree->shadow_root_mode != NULL && tree->is_window != NULL &&
           tree->is_slot != NULL && tree->is_assigned_slottable != NULL &&
           tree->is_shadow_including_inclusive_ancestor != NULL,
           "half a tree was registered with the events layer — §2.9's walk, §2.7's default passive value and the "
           "six shadow facts the walk composes are all tree questions, and one answered without the others is a "
           "component that silently never runs. It is ALL of them or none: a tree that answers the walk but not "
           "the shadow terms would build a path that crosses boundaries without retargeting at them");
    g_tree = tree;
}

void event_target_set_activation(bool (*has)(JSContext *ctx, JSValueConst el),
                                 int (*run)(JSContext *ctx, JSValueConst el, JSValueConst ev,
                                            uint8_t *phase, uint32_t *req))
{
    DCHECK((has != NULL) == (run != NULL),
           "half an activation behaviour was registered — a predicate with nothing to perform picks an "
           "activation target the dispatch then cannot run, and a performer with no predicate is never picked");
    g_has_activation = has;
    g_run_activation = run;
}

void event_target_free(JSContext *ctx)
{
    if (!g_ready)
        return;
    JS_FreeValue(ctx, g_key);
    JS_FreeValue(ctx, g_handler_key);
    JS_FreeValue(ctx, g_handler_marker);
    /* THE PROTOTYPE IS NOT RELEASED HERE: each realm's is held by that realm's class-proto slot and goes with
       the realm. Neither is the dispatcher — there is no lasting one to hold. */
    g_key = g_handler_key = g_handler_marker = JS_UNDEFINED;
    g_ready = 0;
}

/* THE RELEVANT GLOBAL OBJECT, WHICH IS THE RUNNING REALM'S — asked of the realm rather than remembered.
   A module-static held it, set by every realm's install, so the LAST document installed was the window every
   realm answered with: materializing a same-origin popup made the OPENER's unqualified `addEventListener(...)`
   register on the popup's global. That is the same defect as the API base URL having been one string for every
   realm, and the same defect as `window.name` having had two sources — a per-realm fact answered per agent.
   There is nothing to set now, so there is nothing a host can forget to set; the DCHECK that caught a host
   forgetting goes with the state it was guarding.
   BORROWED. A realm owns its global for the realm's whole life, so this needs no reference of its own — the
   dup and free below are how quickjs spells "read it without taking one". */
static JSValueConst event_target_global(JSContext *ctx)
{
    JSValue g = JS_GetGlobalObject(ctx);
    JS_FreeValue(ctx, g);
    return g;
}

/* WEB IDL §3.6's [Global] RULE: an operation on the Window interface called with an undefined `this` uses the
   RELEVANT GLOBAL OBJECT. That is not a nicety — `addEventListener('load', init)` written unqualified is how a
   great deal of real code registers, and a bare call has an undefined this-binding, so without this rule every
   one of those listeners was registered on nothing at all and silently never fired. It is applied at the shared
   entry because that is where the receiver arrives; a non-global interface reached with undefined would
   otherwise be an immediate TypeError, so there is nothing here for this to take away. */
static JSValueConst event_target_receiver(JSContext *ctx, JSValueConst this_val)
{
    if (JS_IsObject(this_val)) return this_val;
    return event_target_global(ctx);
}

/* The map of type -> listener array on `target`, created on first use. NULL only on allocation failure. */
static JSValue listener_map(JSContext *ctx, JSValueConst target, int create)
{
    JSAtom k;
    JSValue map;

    DCHECK(g_ready, "an event-listener map was asked for before the key existed");
    k = JS_ValueToAtom(ctx, g_key);
    if (k == JS_ATOM_NULL)
        return JS_UNDEFINED;
    /* AN OWN SLOT, never a property LOOKUP. `window` is the global object, and a miss on the global is the
       solver's absent-state seam: it mints a concolic for the name a page read and did not define. That is
       right for the page's own reads and wrong for an internal slot — the map came back as a concolic, every
       listener was stored on it, and window.addEventListener silently registered nothing. An internal slot is
       by definition an own slot, so it is read as one. */
    if (JS_GetOwnSlot(ctx, &map, target, k) <= 0)
        map = JS_UNDEFINED;
    if (!JS_IsObject(map) && create) {
        JS_FreeValue(ctx, map);
        map = idl_slots_new(ctx);
        if (!JS_IsException(map))
            JS_SetProperty(ctx, (JSValue)target, k, JS_DupValue(ctx, map));
    }
    JS_FreeAtom(ctx, k);
    return map;
}

/* §2.7 A LISTENER IS NOT A CALLBACK — it is a RECORD: {callback, capture, once, passive}. Storing the bare
   callback made three of those four unrepresentable, so `once` fired every time (a real bundle's one-shot
   init ran on every event), `capture` had no phase, and the dedup key was wrong — the spec's key is
   (type, callback, capture), so the same function registered once capturing and once bubbling is TWO
   listeners and used to be silently one. The record is an engine-built null-prototyped object, so reading it
   back from C runs none of the page's code. */
static JSValue listener_record(JSContext *ctx, JSValueConst cb, bool capture, bool once, bool passive)
{
    JSValue rec = JS_NewObjectProto(ctx, JS_NULL);
    CHECK(!JS_IsException(rec), "event listeners: OOM recording a registration — a dropped listener is page "
                                "code that never runs");
    JS_SetPropertyStr(ctx, rec, "cb", JS_DupValue(ctx, cb));
    JS_SetPropertyStr(ctx, rec, "capture", JS_NewBool(ctx, capture));
    JS_SetPropertyStr(ctx, rec, "once", JS_NewBool(ctx, once));
    JS_SetPropertyStr(ctx, rec, "passive", JS_NewBool(ctx, passive));
    /* §2.7's REMOVED FIELD, and it is the fifth field for a reason the spec states in a note: dispatch walks a
       CLONE of the list so that a listener added mid-walk does not run, "note that removal still has an effect
       due to the removed field". Without it, removing a listener from inside a dispatch removed it from the live
       list and the snapshot ran it anyway — the one thing the clone must NOT preserve. */
    JS_SetPropertyStr(ctx, rec, "removed", JS_FALSE);
    return rec;
}

/* §2.7 "remove an event listener" step 2: SET REMOVED, then drop it. The two halves are one operation, and the
   first is what a dispatch already holding a snapshot of this list observes. */
static void listener_mark_removed(JSContext *ctx, JSValueConst rec)
{
    JS_SetPropertyStr(ctx, (JSValue)rec, "removed", JS_TRUE);
}

static bool rec_flag(JSContext *ctx, JSValueConst rec, const char *name)
{
    JSValue v = JS_GetPropertyStr(ctx, rec, name);
    bool b = JS_ToBool(ctx, v);
    JS_FreeValue(ctx, v);
    return b;
}

/* The record's callback, or the handler MARKER — an owned value either way. */
static JSValue rec_cb(JSContext *ctx, JSValueConst rec)
{
    return JS_GetPropertyStr(ctx, rec, "cb");
}

/* Does this record register `cb` with this capture flag? §2.7's identity, and the whole of it. */
static bool rec_matches(JSContext *ctx, JSValueConst rec, JSValueConst cb, bool capture)
{
    JSValue c = rec_cb(ctx, rec);
    bool same = JS_VALUE_GET_TAG(c) == JS_VALUE_GET_TAG(cb) && JS_VALUE_GET_PTR(c) == JS_VALUE_GET_PTR(cb);
    JS_FreeValue(ctx, c);
    return same && rec_flag(ctx, rec, "capture") == capture;
}

/* The live list for (target, type), created on demand. OWNED. */
static JSValue listener_list(JSContext *ctx, JSValueConst target, const char *type, int create)
{
    JSValue map = listener_map(ctx, target, create), arr;

    if (!JS_IsObject(map)) { JS_FreeValue(ctx, map); return JS_UNDEFINED; }
    arr = JS_GetPropertyStr(ctx, map, type);
    if (!JS_IsArray(arr) && create) {
        JS_FreeValue(ctx, arr);
        arr = JS_NewArray(ctx);
        JS_SetPropertyStr(ctx, map, type, JS_DupValue(ctx, arr));
    }
    JS_FreeValue(ctx, map);
    return arr;
}

static uint32_t arr_len(JSContext *ctx, JSValueConst arr)
{
    JSValue v = JS_GetPropertyStr(ctx, arr, "length");
    uint32_t n = 0;
    JS_ToUint32(ctx, &n, v);
    JS_FreeValue(ctx, v);
    return n;
}

static JSValue remove_listener_with_type(JSContext *ctx, JSValueConst this_val, JSValueConst cb, const char *type,
                                         bool capture);

/* §2.7 step 6's ABORT ALGORITHM, as a closure over exactly the four things "remove an event listener" needs.
   It is an ALGORITHM and not an `abort` listener: §3.2 runs the algorithms BEFORE it fires `abort`, so a
   listener registered with a signal is already gone by the time the page's own `abort` handler runs — and an
   algorithm is invisible to the page, which cannot remove it or see it in a listener list.
   The captures are the REGISTRATION's identity, which is (target, type, callback, capture) and nothing else:
   holding the RECORD instead would keep a listener the page has since removed and re-added alive as a second
   entry to delete. */
static JSValue js_listener_signal_abort(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
                                        int magic, JSValue *func_data)
{
    const char *type = JS_ToCString(ctx, func_data[1]);   /* a real string: the registration converted it */

    (void)this_val; (void)argc; (void)argv; (void)magic;
    if (type) {
        remove_listener_with_type(ctx, func_data[0], func_data[2], type, JS_ToBool(ctx, func_data[3]));
        JS_FreeCString(ctx, type);
    }
    return JS_UNDEFINED;
}

static JSValue listener_signal_algorithm(JSContext *ctx, JSValueConst target, const char *type,
                                         JSValueConst cb, bool capture)
{
    JSValueConst data[4];
    JSValue t = JS_NewString(ctx, type), fn;

    data[0] = target; data[1] = t; data[2] = cb; data[3] = JS_NewBool(ctx, capture);
    fn = JS_NewCFunctionData(ctx, js_listener_signal_abort, 0, 0, 4, data);
    JS_FreeValue(ctx, t);
    CHECK(!JS_IsException(fn), "the abort algorithm for a signal-scoped listener could not be allocated — a "
                               "dropped one is a listener that outlives the signal that owns it");
    return fn;
}

/* §2.7's DEFAULT PASSIVE VALUE, given an event type and an EventTarget. The four types are this file's list —
   they are named in the spec, not derived — and the target test is the tree's. Both halves must hold. */
static bool default_passive_value(JSContext *ctx, const char *type, JSValueConst target)
{
    static const char *const PASSIVE_BY_DEFAULT[] = { "touchstart", "touchmove", "wheel", "mousewheel", NULL };
    int i;

    for (i = 0; PASSIVE_BY_DEFAULT[i]; i++)
        if (!strcmp(type, PASSIVE_BY_DEFAULT[i]))
            return g_tree != NULL && g_tree->default_passive_target(ctx, target);
    return false;
}

/* The listener-list work, once `type` is a real string. Split from the coercion so the part that CAN reach the
   page's code is a request and the part that cannot is ordinary C.
   `passive` is a TRISTATE, because §2.7's flatten more options makes it one: -1 means the page did not say, and
   step 4 of "add an event listener" then fills it from the default passive value. Collapsing it to false at the
   dictionary read would make `{passive:false}` and `{}` the same registration, which for a wheel listener on
   the window is exactly the difference the flag exists to express. */
static JSValue add_listener_with_type(JSContext *ctx, JSValueConst this_val, JSValueConst cb, const char *type,
                                      bool capture, bool once, int passive, JSValueConst signal)
{
    JSValue arr;
    uint32_t len, i;

    /* §2.7 step 2: a listener registered with an ALREADY-ABORTED signal is not registered at all. */
    if (abort_signal_is(ctx, signal) && abort_signal_aborted(ctx, signal))
        return JS_UNDEFINED;
    if (passive < 0)
        passive = default_passive_value(ctx, type, this_val);
    arr = listener_list(ctx, this_val, type, 1);
    if (!JS_IsArray(arr)) { JS_FreeValue(ctx, arr); return JS_UNDEFINED; }
    len = arr_len(ctx, arr);
    /* "If the event listener list already contains a listener whose type, callback and capture are the same,
       do nothing." The flags of the EXISTING one win — a second add does not change `once`. */
    for (i = 0; i < len; i++) {
        JSValue e = JS_GetPropertyUint32(ctx, arr, i);
        bool same = JS_IsObject(e) && rec_matches(ctx, e, cb, capture);
        JS_FreeValue(ctx, e);
        if (same) { JS_FreeValue(ctx, arr); return JS_UNDEFINED; }
    }
    JS_SetPropertyUint32(ctx, arr, len, listener_record(ctx, cb, capture, once, passive != 0));
    JS_FreeValue(ctx, arr);
    /* §2.7 step 6: "If listener's signal is non-null, then add the following abort steps to it: remove an event
       listener with eventTarget and listener." An ABORT ALGORITHM, which is what §3.2 calls a piece of engine
       work that runs BEFORE the `abort` event and is invisible to the page — a page-visible `abort` listener
       would be one the page could see, remove, or have run out of order with its own. */
    if (abort_signal_is(ctx, signal)) {
        JSValue algo = listener_signal_algorithm(ctx, this_val, type, cb, capture);
        abort_signal_add_algorithm(ctx, signal, algo);   /* BORROWED: the signal takes its own reference */
        JS_FreeValue(ctx, algo);
    }
    return JS_UNDEFINED;
}

static JSValue remove_listener_with_type(JSContext *ctx, JSValueConst this_val, JSValueConst cb, const char *type,
                                         bool capture)
{
    JSValue map, arr, kept;
    uint32_t len, i, k = 0;

    map = listener_map(ctx, this_val, 0);
    if (!JS_IsObject(map)) { JS_FreeValue(ctx, map); return JS_UNDEFINED; }
    arr = JS_GetPropertyStr(ctx, map, type);
    if (!JS_IsArray(arr)) { JS_FreeValue(ctx, arr); JS_FreeValue(ctx, map); return JS_UNDEFINED; }
    len = arr_len(ctx, arr);
    kept = JS_NewArray(ctx);
    for (i = 0; i < len; i++) {
        JSValue e = JS_GetPropertyUint32(ctx, arr, i);
        bool same = JS_IsObject(e) && rec_matches(ctx, e, cb, capture);
        if (same) { listener_mark_removed(ctx, e); JS_FreeValue(ctx, e); }
        else JS_SetPropertyUint32(ctx, kept, k++, e);
    }
    JS_SetPropertyStr(ctx, map, type, kept);
    JS_FreeValue(ctx, arr);
    JS_FreeValue(ctx, map);
    return JS_UNDEFINED;
}

void event_target_add_listener(JSContext *ctx, JSValueConst target, const char *type, JSValueConst cb,
                               bool capture, bool once, int passive, JSValueConst signal)
{
    JS_FreeValue(ctx, add_listener_with_type(ctx, event_target_receiver(ctx, target), cb, type,
                                             capture, once, passive, signal));
}

void event_target_remove_listener(JSContext *ctx, JSValueConst target, const char *type, JSValueConst cb)
{
    JS_FreeValue(ctx, remove_listener_with_type(ctx, event_target_receiver(ctx, target), cb, type,
                                                /*capture*/ false));
}

/* §2.7's event listener list, asked as "is it non-empty for ANY type". The map is this file's own engine-built
   object, so walking it runs none of the page's code — and a handler ATTRIBUTE is in it because setting one
   registers a real listener, which is the whole reason the caller must not count for itself. */
bool event_target_has_any_listener(JSContext *ctx, JSValueConst target)
{
    JSValue map = listener_map(ctx, event_target_receiver(ctx, target), 0);
    JSPropertyEnum *tab = NULL;
    uint32_t n = 0, i;
    bool any = false;

    if (!JS_IsObject(map)) { JS_FreeValue(ctx, map); return false; }
    if (JS_GetOwnPropertyNames(ctx, &tab, &n, map, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) == 0) {
        for (i = 0; i < n && !any; i++) {
            JSValue arr = JS_GetProperty(ctx, map, tab[i].atom);
            if (JS_IsArray(arr) && arr_len(ctx, arr) > 0) any = true;
            JS_FreeValue(ctx, arr);
        }
        for (i = 0; i < n; i++) JS_FreeAtom(ctx, tab[i].atom);
        js_free(ctx, tab);
    }
    JS_FreeValue(ctx, map);
    return any;
}

/* add/removeEventListener's `type` is a Web IDL DOMString, so it is ToString on whatever the page passed and
   cannot be a JS_ToCString from C. They use the SHARED coerce-then-call machine rather than one of their own:
   what they have in common with getAttribute and createElement is exactly the thing that needs a machine, and a
   second copy is a second chance to get the resumption wrong.
   IT ALSO FIXES AN ORDERING MISTAKE I MADE. A bespoke version checked 2.7's "if callback is null, return"
   BEFORE the coercion, to avoid running a toString for a call that does nothing. That is backwards: Web IDL
   converts arguments in ORDER at call time, so `type` is converted first and the null-callback step is part of
   the algorithm that runs after. `addEventListener({toString(){ … }}, null)` DOES run that toString in a real
   browser, and now here. */
static JSValue idl_add_or_remove(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    JSValueConst opts = argc > 2 ? argv[2] : JS_UNDEFINED;
    JSValue signal = JS_UNDEFINED, passive_v = JS_UNDEFINED;
    const char *type;
    bool capture, once;
    int passive;
    JSValue r;

    /* WEB IDL CONVERTS EVERY ARGUMENT, IN ORDER, BEFORE THE ALGORITHM RUNS — so the shape of this body is
       "finish the conversions, THEN run §2.7", and the two halves are not interleavable. §2.8's `callback` is an
       `EventListener?`, which is a CALLBACK INTERFACE and not a function type: ANY object implements it, because
       its one operation is looked up BY NAME on the object each time it is invoked, so
       `el.addEventListener("x", {handleEvent(e){…}})` is an ordinary registration and used to be silently
       dropped here. A PRIMITIVE is the type's TypeError, and it is raised before the options are read because
       the callback is the earlier argument. */
    if (argc > 1 && !JS_IsObject(argv[1]) && !JS_IsNull(argv[1]) && !JS_IsUndefined(argv[1]))
        return JS_ThrowTypeError(ctx, "the event listener is not an object");
    /* The engine-built dictionary, whatever the page wrote: `{capture:true}`, a bare `true`, or nothing. The
       union's flattening happened in the declaration, so there is one shape to read here. */
    capture = idl_dict_bool(ctx, opts, "capture");
    once = idl_dict_bool(ctx, opts, "once");
    /* §2.7 "flatten more options" step 3.2: `passive` is null unless the member EXISTS, which is why this is a
       tristate and not a bool — a wheel listener registered with `{}` on the window is passive by default and
       one registered with `{passive:false}` is not. */
    passive_v = idl_dict_get(ctx, opts, "passive");
    passive = JS_IsUndefined(passive_v) ? -1 : (JS_ToBool(ctx, passive_v) ? 1 : 0);
    JS_FreeValue(ctx, passive_v);
    signal = idl_dict_get(ctx, opts, "signal");
    /* `AbortSignal signal` is an INTERFACE-typed member, so a value that is not one is a TypeError before the
       algorithm's step 1. The brand test is not the declaration's here because §3.2's brand is a private SLOT
       RECORD rather than a class opaque — IDL_INTERFACE tests the opaque, which an AbortSignal does not carry. */
    /* NULL IS NOT AN ABSENT MEMBER HERE. `AbortSignal signal` is NOT nullable, so `{signal: null}` is a
       TypeError and not "no signal" — the corpus asks for exactly that, twice, and it asks for it even when the
       CALLBACK is null, which is why the conversion has to happen before §2.7's "if callback is null, return".
       Only an ABSENT member (undefined) is the null the algorithm means. */
    if (!JS_IsUndefined(signal) && !abort_signal_is(ctx, signal)) {
        JS_FreeValue(ctx, signal);
        return JS_ThrowTypeError(ctx, "options.signal does not implement AbortSignal");
    }
    /* §2.7 "add an event listener" step 3 / "remove an event listener"'s equivalent: a NULL callback registers
       nothing. It is HERE, after every conversion, because that is where the spec puts it — which is what makes
       `addEventListener("x", null, {signal: null})` a TypeError about the signal rather than a silent no-op. */
    if (argc < 2 || JS_IsNull(argv[1]) || JS_IsUndefined(argv[1])) {
        JS_FreeValue(ctx, signal);
        return JS_UNDEFINED;
    }
    type = JS_ToCString(ctx, argv[0]);   /* a real string by now: this cannot reach the page */
    if (!type) {
        JS_FreeValue(ctx, signal);
        return JS_EXCEPTION;
    }
    r = (magic == 0) ? add_listener_with_type(ctx, event_target_receiver(ctx, this_val), argv[1], type,
                                              capture, once, passive, signal)
                     : remove_listener_with_type(ctx, event_target_receiver(ctx, this_val), argv[1], type, capture);
    JS_FreeCString(ctx, type);
    JS_FreeValue(ctx, signal);
    return r;
}

/* ---- EVENT HANDLER IDL ATTRIBUTES — HTML §8.1.7.2 --------------------------------------------------------
 *
 * `el.onclick = f` is not a listener registration a page could have written itself with addEventListener; it is
 * its own mechanism, and it was absent entirely. That absence is the single largest entry in this engine's IDL
 * gap report — about 150 of Window's 227 missing members are `on*` attributes, and the same list repeats on
 * Document and Element — and it is absent in the way that hurts most: `window.onload = init` is how a great
 * deal of real code starts, and it silently became an ordinary JS property that nothing ever read.
 *
 * ONE MECHANISM, NOT ONE PER NAME. An event handler attribute is entirely determined by its NAME: `onfoo` is
 * the handler for the event type `foo`. So the names are DATA — one X-macro list, from which the enum, the type
 * strings and the accessor table are all generated — and the behaviour is written once. Spelling the names out
 * rather than generating them from the IDL is the gap engine/idlgen.mjs exists to report; what must not happen
 * is an attribute that answers something its spec does not say.
 *
 * THE HANDLER IS NOT THE LISTENER, and that distinction is the whole design. §8.1.7 registers ONE listener the
 * first time a handler is set for a type, and later assignments change the HANDLER the listener reads — so the
 * listener keeps its position in the list. `el.onclick = a; el.addEventListener('click', b); el.onclick = c`
 * runs c then b, not b then c. Registering the handler function itself would append it and get that backwards.
 * So the listener list holds a MARKER for the handler slot, and the list snapshot resolves the marker to
 * whatever the handler is at dispatch time — a read of an engine-built map under a private Symbol, so it runs
 * none of the page's code and needs no request. Setting null removes the marker, which is §8.1.7's "deactivate".
 *
 * The handler map is an own property under a private Symbol, for the reason the listener map is: it makes the
 * handler per-flow for free, so `onclick` assigned in one arm of a fork is invisible to its sibling. */
#define EVENT_HANDLERS(X)                                                                                     \
    /* GlobalEventHandlers — HTML §8.1.7.2.1, on Window, Document and Element alike. */                       \
    X("onabort", EH_GLOBAL | EH_SIGNAL | EH_XHR) X("onauxclick", EH_GLOBAL) X("onbeforeinput", EH_GLOBAL)          \
    X("onbeforematch", EH_GLOBAL) X("onbeforetoggle", EH_GLOBAL) X("onblur", EH_GLOBAL) X("oncancel", EH_GLOBAL)      \
    X("oncanplay", EH_GLOBAL) X("oncanplaythrough", EH_GLOBAL)                          \
    /* `onchange` has TWO owners — GlobalEventHandlers and CSSOM VIEW §4.2's MediaQueryList — which is exactly
       what the mask is a BITMASK for. A second X() line for the same name would put the name in this list
       twice, and every consumer of the list (the IDL auditor, the content-attribute test) would then see a
       member that does not exist twice over. */                                                              \
    X("onchange", EH_GLOBAL | EH_MEDIA_QUERY_LIST) X("onclick", EH_GLOBAL)       \
    X("onclose", EH_GLOBAL) X("oncontextlost", EH_GLOBAL) X("oncontextmenu", EH_GLOBAL)                             \
    X("oncontextrestored", EH_GLOBAL) X("oncuechange", EH_GLOBAL) X("ondblclick", EH_GLOBAL) X("ondrag", EH_GLOBAL)   \
    X("ondragend", EH_GLOBAL) X("ondragenter", EH_GLOBAL) X("ondragleave", EH_GLOBAL) X("ondragover", EH_GLOBAL)      \
    X("ondragstart", EH_GLOBAL) X("ondrop", EH_GLOBAL) X("ondurationchange", EH_GLOBAL) X("onemptied", EH_GLOBAL)     \
    X("onended", EH_GLOBAL) X("onerror", EH_GLOBAL | EH_XHR) X("onfocus", EH_GLOBAL) X("onformdata", EH_GLOBAL)                \
    X("oninput", EH_GLOBAL) X("oninvalid", EH_GLOBAL) X("onkeydown", EH_GLOBAL) X("onkeypress", EH_GLOBAL)            \
    X("onkeyup", EH_GLOBAL) X("onload", EH_GLOBAL | EH_XHR) X("onloadeddata", EH_GLOBAL) X("onloadedmetadata", EH_GLOBAL)      \
    X("onloadstart", EH_GLOBAL | EH_XHR) X("onmousedown", EH_GLOBAL) X("onmouseenter", EH_GLOBAL) X("onmouseleave", EH_GLOBAL) \
    X("onmousemove", EH_GLOBAL) X("onmouseout", EH_GLOBAL) X("onmouseover", EH_GLOBAL) X("onmouseup", EH_GLOBAL)      \
    X("onpause", EH_GLOBAL) X("onplay", EH_GLOBAL) X("onplaying", EH_GLOBAL) X("onprogress", EH_GLOBAL | EH_XHR)               \
    X("onratechange", EH_GLOBAL) X("onreset", EH_GLOBAL) X("onresize", EH_GLOBAL) X("onscroll", EH_GLOBAL)            \
    X("onscrollend", EH_GLOBAL) X("onsecuritypolicyviolation", EH_GLOBAL) X("onseeked", EH_GLOBAL)                  \
    X("onseeking", EH_GLOBAL) X("onselect", EH_GLOBAL) X("onslotchange", EH_GLOBAL | EH_SHADOW_ROOT) X("onstalled", EH_GLOBAL)         \
    X("onsubmit", EH_GLOBAL) X("onsuspend", EH_GLOBAL) X("ontimeupdate", EH_GLOBAL) X("ontoggle", EH_GLOBAL)          \
    X("onvolumechange", EH_GLOBAL) X("onwaiting", EH_GLOBAL) X("onwheel", EH_GLOBAL)                                \
    /* DocumentAndElementEventHandlers — §8.1.7.2.3, on Document and Element (and Window, which mixes it in). */\
    X("oncopy", EH_GLOBAL) X("oncut", EH_GLOBAL) X("onpaste", EH_GLOBAL)                                            \
    /* WindowEventHandlers — §8.1.7.2.2, on Window (and, per the mixin, Document's body-delegated set). */     \
    X("onafterprint", EH_WINDOW) X("onbeforeprint", EH_WINDOW) X("onbeforeunload", EH_WINDOW)                       \
    X("onhashchange", EH_WINDOW) X("onlanguagechange", EH_WINDOW) X("onmessage", EH_WINDOW | EH_PORT)                          \
    X("onmessageerror", EH_WINDOW | EH_PORT) X("onoffline", EH_WINDOW) X("ononline", EH_WINDOW) X("onpagehide", EH_WINDOW)      \
    X("onpagereveal", EH_WINDOW) X("onpageshow", EH_WINDOW) X("onpageswap", EH_WINDOW) X("onpopstate", EH_WINDOW)     \
    X("onrejectionhandled", EH_WINDOW) X("onstorage", EH_WINDOW) X("onunhandledrejection", EH_WINDOW)               \
    X("onunload", EH_WINDOW)                                                                                    \
    /* Document's own — §3.1.1 and the Page Visibility API. */                                                 \
    X("onreadystatechange", EH_DOCUMENT | EH_XHR_READYSTATE) X("onvisibilitychange", EH_DOCUMENT) \
    /* XHR §3.3 — the two of its seven that belong to NO other mixin, so this list is where they arrive. */ \
    X("onloadend", EH_XHR) X("ontimeout", EH_XHR)

/* The NAMES are string literals, not stringified identifiers, so the IDL gap auditor — which scans a component
   for the property names it installs — can SEE them. Behind a `#n` it saw none of these and reported all ninety
   as absent, which is the audit lying by omission: the same failure as leaving an interface out of its map. */
static const char *const EH_NAME[] = {
#define X(n, m) n,
    EVENT_HANDLERS(X)
#undef X
};
#define EH_COUNT ((int)(sizeof(EH_NAME) / sizeof(EH_NAME[0])))

/* The EVENT TYPE each attribute handles: its own name past the `on`, which is what §8.1.7 says and why one
   list produces both. WRITTEN AS AN INDEX, not as `n + 2`: the two mean the identical thing and the second is
   the shape `-Wstring-plus-int` exists to catch, because on a string literal it is far more often someone
   expecting concatenation. Saying `&n[2]` says SUBSTRING, which is what §8.1.7 asks for. */
static const char *const EH_TYPE[] = {
#define X(n, m) &(n)[2],
    EVENT_HANDLERS(X)
#undef X
};

static const int EH_MASK[] = {
#define X(n, m) (m),
    EVENT_HANDLERS(X)
#undef X
};


/* The handler map (type -> handler) and the marker that stands for it in a listener list. The map is per
   TARGET; the marker is ONE object for the whole runtime, because it carries no information — its identity is
   the whole of what it means. */
static JSValue handler_map(JSContext *ctx, JSValueConst target, int create)
{
    JSAtom k;
    JSValue map;

    DCHECK(g_ready, "an event-handler map was asked for before the key existed");
    k = JS_ValueToAtom(ctx, g_handler_key);
    if (k == JS_ATOM_NULL)
        return JS_UNDEFINED;
    if (JS_GetOwnSlot(ctx, &map, target, k) <= 0)   /* an own SLOT, never a lookup — see listener_map */
        map = JS_UNDEFINED;
    if (!JS_IsObject(map) && create) {
        JS_FreeValue(ctx, map);
        map = idl_slots_new(ctx);
        if (!JS_IsException(map))
            JS_SetProperty(ctx, (JSValue)target, k, JS_DupValue(ctx, map));
    }
    JS_FreeAtom(ctx, k);
    return map;
}

/* The handler currently set for `type` on `target`, or JS_NULL. A map read, so no page code and no request —
   which is what lets the dispatch walk resolve the marker in place. */
static JSValue handler_current(JSContext *ctx, JSValueConst target, const char *type)
{
    JSValue map = handler_map(ctx, target, 0), h;

    if (!JS_IsObject(map)) { JS_FreeValue(ctx, map); return JS_NULL; }
    h = JS_GetPropertyStr(ctx, map, type);
    JS_FreeValue(ctx, map);
    return JS_IsFunction(ctx, h) ? h : (JS_FreeValue(ctx, h), JS_NULL);
}

static JSValue js_handler_get(JSContext *ctx, JSValueConst this_val, int magic)
{
    DCHECK(magic >= 0 && magic < EH_COUNT, "an event handler was declared with a magic the list does not name");
    return handler_current(ctx, this_val, EH_TYPE[magic]);
}

/* HTML §8.1.7.2's handler attributes are ordinarily pure state — assign a function, it is called when the event
   fires, and nothing else happens. The platform has ONE exception: §9.4.2 says setting `onmessage` on a
   MessagePort also STARTS the port, which is why a page that assigns onmessage never calls start() and a page
   that only uses addEventListener must. A general side-effect mechanism for a single member would be more
   machinery than the rule; instead the interested component registers here and decides for itself, by name and
   by its own brand test, which keeps this file from knowing what a MessagePort is. */
static void (*g_handler_set_hook)(JSContext *ctx, JSValueConst target, const char *name);

void event_target_set_handler_hook(void (*after_set)(JSContext *ctx, JSValueConst target, const char *name))
{
    g_handler_set_hook = after_set;
}

static JSValue js_handler_set(JSContext *ctx, JSValueConst this_val, JSValueConst val, int magic)
{
    const char *type;
    JSValue map;

    DCHECK(magic >= 0 && magic < EH_COUNT, "an event handler was declared with a magic the list does not name");
    type = EH_TYPE[magic];

    map = handler_map(ctx, this_val, 1);
    if (!JS_IsObject(map)) { JS_FreeValue(ctx, map); return JS_UNDEFINED; }
    /* §8.1.7.1: anything that is not callable sets the handler to null. A page assigning a string here is
       writing legacy markup-style code, which HTML compiles — this engine does not, and an uncompiled string
       is honestly not a handler rather than one that silently never fires. */
    if (JS_IsFunction(ctx, val)) {
        JS_SetPropertyStr(ctx, map, type, JS_DupValue(ctx, val));
        /* §8.1.7: the listener is registered ONCE, the first time a handler is set for this type. */
        add_listener_with_type(ctx, this_val, g_handler_marker, type, /*capture*/ false, /*once*/ false,
                               /*passive*/ -1, /*signal*/ JS_UNDEFINED);
    } else {
        JS_SetPropertyStr(ctx, map, type, JS_NULL);
        remove_listener_with_type(ctx, this_val, g_handler_marker, type, /*capture*/ false);   /* §8.1.7 "deactivate" */
    }
    /* AFTER the handler is registered, for the reason event_target.h gives: §9.4.2's start() delivers what is
       already queued, and running it first would fire those events at a target with no listener yet. */
    if (g_handler_set_hook)
        g_handler_set_hook(ctx, this_val, EH_NAME[magic]);
    JS_FreeValue(ctx, map);
    return JS_UNDEFINED;
}

/* HTML §8.1.7.2: an EVENT HANDLER CONTENT ATTRIBUTE is the content attribute of the same name as an event
   handler IDL attribute, and that name set is exactly the list above — so the question is answered here rather
   than by a second list somewhere else. Trusted Types §3.8 step 2 asks it of every setAttribute: an event
   handler content attribute demands a TrustedScript, so `el.setAttribute("onclick", s)` throws under
   `require-trusted-types-for 'script'` while `el.setAttribute("title", s)` does not, and a copy of this list
   that fell one name behind would answer that question differently from the property that shares the name. */
/* EVERY EVENT HANDLER CONTENT ATTRIBUTE NAME, ENUMERATED — §8.1.7.2 defines the set as the names of the event
   handler IDL attributes, so both this and the predicate below come off the one X-list rather than a second
   copy that would drift the first time a handler is added.
   IT IS AN ENUMERATION AND NOT A FILTER, which is the difference HTML §8.6.2's remove-unsafe needs: its step 4
   APPENDS every one of these to a configuration's removeAttributes list, and a caller that could only ask "is
   this one" can filter an allow-list it already has but can never build the deny-list the step describes. */
int event_target_handler_attribute_count(void) { return EH_COUNT; }

const char *event_target_handler_attribute_at(int i)
{
    DCHECK(i >= 0 && i < EH_COUNT, "an event handler content attribute was asked for by an out-of-range index");
    return EH_NAME[i];
}

bool event_target_is_handler_attribute(const char *name)
{
    int i;

    DCHECK(name != NULL, "the event handler content attribute test was asked about no name");
    /* ASCII case-insensitively: an attribute name reaching here has already been lowercased by DOM §4.9 step 2
       for an HTML element, but setAttributeNS performs no such lowercasing and `onClick` in an XML document is
       not an event handler content attribute — the compare is stated once here rather than at each caller. */
    for (i = 0; i < EH_COUNT; i++) {
        const char *n = EH_NAME[i];
        size_t k = 0;
        while (n[k] && name[k]) {
            char a = name[k];
            if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
            if (a != n[k]) break;
            k++;
        }
        if (!n[k] && !name[k]) return true;
    }
    return false;
}

void event_target_install_handlers(JSContext *ctx, JSValueConst target, int mask)
{
    int i;

    DCHECK(JS_IsObject(target), "event handlers were installed on something that is not an object");
    for (i = 0; i < EH_COUNT; i++) {
        JSAtom a;
        if (!(EH_MASK[i] & mask))
            continue;
        a = JS_NewAtom(ctx, EH_NAME[i]);
        CHECK(a != JS_ATOM_NULL, "an event handler name could not be interned");
        /* The getter/setter cprotos take their own signatures, which the magic-function constructor reaches
           through one pointer type — the same cast every JS_CGETSET_MAGIC_DEF performs at compile time. */
        JS_DefinePropertyGetSet(ctx, (JSValue)target, a,
                                JS_NewCFunctionMagic(ctx, (JSCFunctionMagic *)js_handler_get, EH_NAME[i], 0,
                                                     JS_CFUNC_getter_magic, i),
                                JS_NewCFunctionMagic(ctx, (JSCFunctionMagic *)js_handler_set, EH_NAME[i], 1,
                                                     JS_CFUNC_setter_magic, i),
                                JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE);
        JS_FreeAtom(ctx, a);
    }
}

/* §2.9 "invoke" step 8: the walk runs over a CLONE of the listener list. That matters more here than in a browser — the
   walk suspends across every listener, so one that adds or removes a listener has arbitrarily long to do it.
   ONE list walk, so the engine's own firing and the page's dispatchEvent can never disagree about which
   listeners a target has; what differs between them is only how each listener is DELIVERED. */
static JSValue listener_snapshot(JSContext *ctx, JSValueConst target, const char *type)
{
    JSValue arr = listener_list(ctx, target, type, 0), copy = JS_NewArray(ctx);
    uint32_t len, i, k = 0;

    if (!JS_IsArray(arr)) { JS_FreeValue(ctx, arr); return copy; }
    len = arr_len(ctx, arr);
    /* The RECORDS, copied as they stand. Resolving the handler marker and filtering by the capture flag both
       belong to the walk: §2.9's "inner invoke" is where the spec does them, and the walk visits one list
       TWICE (once per direction) with a different answer each time. */
    for (i = 0; i < len; i++)
        JS_SetPropertyUint32(ctx, copy, k++, JS_GetPropertyUint32(ctx, arr, i));
    JS_FreeValue(ctx, arr);
    return copy;
}

/* §2.9 "inner invoke" step 2: `once` REMOVES the listener from the live list before it is called, so a
   listener that re-enters the same dispatch does not see itself. It is removed from the LIVE list, never from
   the snapshot the walk is iterating — the snapshot is what makes a removal during dispatch not skip a
   sibling, which is the whole reason §2.9 takes one. */
static void listener_remove_record(JSContext *ctx, JSValueConst target, const char *type, JSValueConst rec)
{
    JSValue map = listener_map(ctx, target, 0), arr, kept;
    uint32_t len, i, k = 0;

    if (!JS_IsObject(map)) { JS_FreeValue(ctx, map); return; }
    arr = JS_GetPropertyStr(ctx, map, type);
    if (!JS_IsArray(arr)) { JS_FreeValue(ctx, arr); JS_FreeValue(ctx, map); return; }
    len = arr_len(ctx, arr);
    kept = JS_NewArray(ctx);
    for (i = 0; i < len; i++) {
        JSValue e = JS_GetPropertyUint32(ctx, arr, i);
        if (JS_VALUE_GET_PTR(e) == JS_VALUE_GET_PTR(rec)) { listener_mark_removed(ctx, e); JS_FreeValue(ctx, e); }
        else JS_SetPropertyUint32(ctx, kept, k++, e);
    }
    JS_SetPropertyStr(ctx, map, type, kept);
    JS_FreeValue(ctx, arr);
    JS_FreeValue(ctx, map);
}


/* §2.9 DISPATCH, as a machine — and the reason dispatchEvent could not exist before.
 *
 * The spec makes dispatch SYNCHRONOUS and makes its return value depend on what the listeners did: it answers
 * `!canceled`, so `if (!el.dispatchEvent(ev)) { … }` is how a page asks whether anything called preventDefault.
 * Neither of the two obvious implementations can answer that. Calling the listeners from C is the
 * drive-to-completion this engine aborts on — a listener body holds loops, awaits and concolic branches.
 * Enqueueing them as jobs answers before any of them has run, so the answer would always be "not cancelled".
 *
 * So each listener is a CALL REQUEST: the machine parks on it, the listener runs as ordinary preemptible page
 * code at whatever depth it likes, and the machine resumes at the listener it was on with `i` as its cursor.
 * That is the same shape every continuation-holding builtin in the engine has, which is why this needs no
 * machinery of its own beyond the state.
 *
 * THE LIST IS SNAPSHOT FIRST, which the spec requires and which matters here more than in a browser: a listener
 * that runs mid-walk can add or remove listeners, and this walk is suspended across every one of them. */
enum { DISPATCH_ARG = 0, CLICK_SYNTH = 1, DISPATCH_PAIR = 2 };

/* WHERE THIS MACHINE RESTS, AS §2.9 NUMBERS IT — read off the live standard rather than remembered, because the
   labels it carried before named steps that do not exist (there is no step 11.1; the activation behaviour is
   step 12.1, and the two listener loops are 6.13 and 6.14 inside the ONE `if` that step 6 is).
   It can suspend at three points: while WALKING the tree that makes the path (a page-sized walk, so it yields
   between parents), inside a listener, and inside the activation behaviour. */
#define DISPATCH_STAGES(X) \
    X(DISPATCH_INIT, "DOM §2.9 dispatch steps 1-6.8 (the dispatch flag, the target override, the relatedTarget " \
                     "retargeted against the target and the step 6 condition it decides, the target's own path " \
                     "item, whether the target is the activation target, whether it is an assigned slottable, " \
                     "and the first get the parent)") \
    X(DISPATCH_PATH, "DOM §2.9 dispatch steps 6.9-6.11 (walk get the parent, appending an event path ITEM per " \
                     "ancestor — retargeting the target, the relatedTarget and the touch targets at each " \
                     "shadow boundary and recording the closed-tree flags — one parent per yield, because the " \
                     "tree is the page's size)") \
    X(DISPATCH_CAPTURE, "DOM §2.9 dispatch step 6.13 (invoke each path item in REVERSE order with phase " \
                        "\"capturing\" — parked on the listener being called)") \
    X(DISPATCH_BUBBLE, "DOM §2.9 dispatch step 6.14 (invoke each path item in order with phase \"bubbling\" — " \
                       "parked on the listener being called)") \
    X(DISPATCH_ACTIVATION, "DOM §2.9 dispatch step 12.1 (the activation target's activation behaviour, run " \
                           "after the whole walk and only when nothing cancelled)")
enum { DISPATCH_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const DISPATCH_STEPS[] = { DISPATCH_STAGES(JS_STEP_STAGE_LABEL) NULL };

typedef struct JSDispatchState {
    JSStepHdr hdr;       /* FIRST — the driver writes the def and the operand bounds through it */
    uint8_t   cphase;    /* the call request's own phase, so a stage can hold a call across a suspension */
    uint32_t  i, n;      /* THE RESUME POINT: the listener being called, and how many there are */
    uint32_t  ti, tn;    /* THE OTHER: how far into the current PASS, and how long the whole path is */
    /* §2.9 step 6.4's isActivationEvent, decided once at step 6.4 and read again at 6.9.5.1 for every ancestor. */
    uint8_t   is_activation;
    /* THE ACTIVATION BEHAVIOUR'S OWN SUSPENSION. §4.6.3's is a navigation and a navigation fetches, so the
       behaviour is a step like everything else that can wait on the host: `aphase` is its resume point and
       `areq` the host request it is waiting on. They live here because the machine that can park is this one. */
    uint8_t   aphase;
    uint32_t  areq;
    /* THE LISTENER'S OWN `passive`, held across the call: §2.9 "inner invoke" RAISES the event's in-passive
       listener flag before the listener runs and lowers it after, so the machine has to remember which way to
       put it back when it resumes — the record it read it from is gone by then. */
    uint8_t   in_passive;
    /* THE CALLBACK'S OPERATION LOOKUP. A callback INTERFACE that is not callable has `handleEvent` read off it
       per invocation, and that read is the page's code — so the object survives the suspension on the state and
       `lphase` says a read is outstanding. Two markers rather than one because a listener can suspend TWICE: on
       its operation lookup and then on the call. */
    uint8_t   lphase;
    /* §2.9 "inner invoke" step 2.11's "IF THIS THROWS AN EXCEPTION, REPORT IT" — the walk does not unwind, it
       REPORTS and continues, and reporting fires an `error` event at the global, which runs the page's code.
       So the exception survives on the state and the report is a sub-request with its own work record.
       `reporting` is its resume marker, the third this walk needs: a listener can suspend on its operation
       lookup, on its own body, and on the report of what its body threw. */
    uint8_t   reporting;
    /* §2.9 step 6.7's SLOT-IN-CLOSED-TREE, and step 6.10's CLEARTARGETS. Both are one-bit walk state that has
       to survive a park between two ancestors, which is the whole reason they are on the state and not on the
       C stack of a loop that does not exist. */
    uint8_t   slot_in_closed_tree;
    uint8_t   clear_targets;
    ReportExceptionWork rep;
    JSValue   exc;       /* what the listener threw, held across the report (owned) */
    JSValue   lcb;       /* the listener's callback object, held across its operation lookup (owned) */
    JSValue   type;      /* the event's type, resolved once per target and needed by `once` (owned) */
    JSValue   path;      /* §2.9's propagation path — a list of event path ITEMS (owned) */
    JSValue   cur;       /* the target whose listeners are running, and the WALK's frontier while step 6.9
                            builds the path (owned) */
    /* §2.9's `target` LOCAL, which the walk MOVES: step 6.9.7.1 sets it to the parent every time the walk
       crosses out of a shadow tree, and every later item is appended with the shadow-adjusted target that
       follows. It is not `cur` — `cur` is where the walk IS, this is what the walk currently calls the target —
       and conflating them is how every item ends up with path[0] as its target. (owned) */
    JSValue   tgt;
    /* §2.9 step 6.6's SLOTTABLE: the node whose assigned slot the next get the parent will answer with, so that
       step 6.9.1 can recognise that the parent it was handed is that slot and raise slot-in-closed-tree when
       the slot's tree is closed. Null except across exactly that one hop. (owned) */
    JSValue   slottable;
    /* §2.9's ACTIVATION TARGET: the nearest entry of the path, TARGET FIRST, that has an activation behaviour.
       Picked while the path is built and run after the walk — see event_target.h. (owned) */
    JSValue   act;
    JSValue   arr;       /* that target's listener list SNAPSHOT (owned) */
    JSValue   ev;        /* the event (owned) */
    JSValue   result;    /* !canceled (owned) */
    JSValue   cb[3];     /* the call request buffer: [this, listener, event] */
} JSDispatchState;

/* WHAT THIS MACHINE OWNS. The call buffer is in here because a fork mid-listener must not hand two arms one
   invocation — that is exactly what the visit contract is for. */
static void js_dispatch_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSDispatchState *s = st;
    int k;
    v->val(ctx, &s->path);
    v->val(ctx, &s->type);
    v->val(ctx, &s->lcb);
    v->val(ctx, &s->exc);
    report_exception_work_visit(ctx, &s->rep, v);
    v->val(ctx, &s->cur);
    v->val(ctx, &s->tgt);
    v->val(ctx, &s->slottable);
    v->val(ctx, &s->act);
    v->val(ctx, &s->arr);
    v->val(ctx, &s->ev);
    v->val(ctx, &s->result);
    for (k = 0; k < 3; k++)
        v->val(ctx, &s->cb[k]);
}

static JSValue js_dispatch_fini(JSContext *ctx, void *st, bool take_result)
{
    JSDispatchState *s = st;
    JSValue r = take_result ? s->result : JS_UNDEFINED;
    int k;

    if (take_result) s->result = JS_UNDEFINED;
    JS_FreeValue(ctx, s->result);
    JS_FreeValue(ctx, s->path);
    JS_FreeValue(ctx, s->type);
    JS_FreeValue(ctx, s->lcb);
    JS_FreeValue(ctx, s->exc);
    report_exception_work_release(ctx, &s->rep);
    JS_FreeValue(ctx, s->cur);
    JS_FreeValue(ctx, s->tgt);
    JS_FreeValue(ctx, s->slottable);
    JS_FreeValue(ctx, s->act);
    JS_FreeValue(ctx, s->arr);
    JS_FreeValue(ctx, s->ev);
    s->result = s->path = s->type = s->lcb = s->exc = s->cur = s->act = s->arr = s->ev = JS_UNDEFINED;
    s->tgt = s->slottable = JS_UNDEFINED;
    for (k = 0; k < 3; k++) {
        JS_FreeValue(ctx, s->cb[k]);
        s->cb[k] = JS_UNDEFINED;
    }
    return r;
}

/* §2.9's GET THE PARENT, asked of whoever owns the tree. It answers null for an EventTarget with no tree at all
   — §2.7's own get the parent, which `new EventTarget()` and an AbortSignal both have — so this file needs no
   "is it in the tree" question and no window special case: a DOCUMENT's get the parent IS the window (unless the
   event is `load`), which is a fact about documents and belongs where documents are known.
   That special case used to live here as "append the realm's global whenever the ancestor hook answered an
   array", which put the window above a DETACHED node as well, and above a document dispatching `load`. */
static JSValue dispatch_get_parent(JSContext *ctx, JSValueConst target, JSValueConst ev)
{
    JSValue parent;

    if (!g_tree)
        return JS_NULL;   /* a host with no tree: §2.7's default, a path of one */
    parent = g_tree->get_parent(ctx, target, ev);
    DCHECK(JS_IsNull(parent) || JS_IsObject(parent),
           "§2.9's get the parent answered with something that is not an EventTarget and is not null — the "
           "walk appends whatever it is handed to the event path and then invokes listeners on it");
    return parent;
}

/* THE SHADOW TERMS, EACH WITH THE ANSWER A HOST WITH NO TREE HAS. A host that registered no tree has no
   document, so nothing in it is a node, a slot, a shadow root or an ancestor of anything — which is not a
   fallback but the same §2.7 default that gives such a host a path of one. Written once here so no step of the
   walk below carries a `g_tree &&` of its own. */
static bool dispatch_is_window(JSContext *ctx, JSValueConst t)
{
    return g_tree != NULL && g_tree->is_window(ctx, t);
}

static bool dispatch_is_slot(JSContext *ctx, JSValueConst t)
{
    return g_tree != NULL && g_tree->is_slot(ctx, t);
}

static bool dispatch_is_assigned_slottable(JSContext *ctx, JSValueConst t)
{
    return g_tree != NULL && g_tree->is_assigned_slottable(ctx, t);
}

/* §4.8's mode of `t` ITSELF — "invocationTarget is a shadow root whose mode is closed", which is "append to an
   event path" step 4's root-of-closed-tree. */
static bool dispatch_is_closed_shadow_root(JSContext *ctx, JSValueConst t)
{
    return g_tree != NULL && g_tree->shadow_root_mode(ctx, t) == EVENT_TREE_SHADOW_CLOSED;
}

/* The mode of `t`'s ROOT, which is a different question and is asked at two different steps: step 6.9.1.3's
   "parent's root is a shadow root whose mode is closed", and step 6.11's "is a node whose root is a shadow
   root" (`want_closed` false, which every mode but NOT_SHADOW_ROOT satisfies). */
static bool dispatch_root_is_shadow_root(JSContext *ctx, JSValueConst t, bool want_closed)
{
    JSValue root;
    int mode;

    if (!g_tree)
        return false;
    root = g_tree->root(ctx, t);
    mode = g_tree->shadow_root_mode(ctx, root);
    JS_FreeValue(ctx, root);
    return want_closed ? mode == EVENT_TREE_SHADOW_CLOSED : mode != EVENT_TREE_NOT_SHADOW_ROOT;
}

/* IS THIS EVENTTARGET THAT ONE. Platform objects have identity — a node's wrapper is the same object for as
   long as the node lives — so "target is relatedTarget" and "parent is relatedTarget" are pointer questions.
   Written once because BOTH of §2.9's uses compare against a POTENTIAL event target, and `null is null` is not
   the answer either of them wants: step 6's condition is about an event with a relatedTarget, and step 6.9.6
   ends the walk at the ancestor that IS one. */
static bool same_target(JSValueConst a, JSValueConst b)
{
    return JS_IsObject(a) && JS_IsObject(b) && JS_VALUE_GET_PTR(a) == JS_VALUE_GET_PTR(b);
}

/* DOM §4.8's RETARGETING ALGORITHM. See event_target.h for why it is a component function and not four inline
   copies. It is a LOOP because one climb is not enough: a node inside a shadow tree inside a shadow tree is
   retargeted to the outer host, and each iteration re-asks all three of step 1's disjuncts about the host it
   just climbed to. */
JSValue event_target_retarget(JSContext *ctx, JSValueConst a, JSValueConst b)
{
    JSValue cur, broot;
    bool b_is_node;

    DCHECK(JS_IsObject(a) || JS_IsNull(a),
           "§4.8's retargeting was given an A that is not a potential event target — the answer is handed to a "
           "listener as `event.relatedTarget`, and undefined is neither an EventTarget nor null");
    DCHECK(JS_IsObject(b) || JS_IsNull(b),
           "§4.8's retargeting was given a B that is not a potential event target — B is who A is about to be "
           "made visible to, and step 1's third disjunct asks whether B is a node");
    cur = JS_DupValue(ctx, a);
    if (!g_tree)
        return cur;   /* a host with no tree: nothing is a node, so step 1's first disjunct returns A at once */
    /* "B is a node", asked ONCE: B does not move, and the tree answers a null root for everything that is not
       a node — the same question the walk uses to tell a Window from a node without a second hook. */
    broot = g_tree->root(ctx, b);
    b_is_node = !JS_IsNull(broot);
    JS_FreeValue(ctx, broot);
    for (;;) {
        JSValue root, host;

        root = g_tree->root(ctx, cur);
        /* step 1, first disjunct: "A is not a node" — a Window, an AbortSignal, a plain `new EventTarget()`. */
        if (JS_IsNull(root)) {
            JS_FreeValue(ctx, root);
            return cur;
        }
        /* second: "A's root is not a shadow root" — A is in the document tree, so it is visible to everything
           in it and is reported as itself. */
        if (g_tree->shadow_root_mode(ctx, root) == EVENT_TREE_NOT_SHADOW_ROOT) {
            JS_FreeValue(ctx, root);
            return cur;
        }
        /* third: "B is a node and A's root is a shadow-including inclusive ancestor of B" — B is INSIDE the
           same shadow tree, so it may already see A and nothing is hidden from it. */
        if (b_is_node && g_tree->is_shadow_including_inclusive_ancestor(ctx, root, b)) {
            JS_FreeValue(ctx, root);
            return cur;
        }
        /* step 2: "set A to A's root's host", and go round again. */
        DCHECK(g_tree->shadow_host != NULL,
               "§4.8's retargeting has to climb from a shadow root to its HOST and the DOM registered no "
               "`shadow_host` in its EventTargetTree — BUILD IT: dom/node.c's NODE_EVENT_TREE must answer §4.8's "
               "host the way it already answers `root` and `shadow_root_mode`, by wrapping "
               "shadow_root_host(node_of(target)) and answering JS_NULL for anything that is not a shadow root. "
               "Until it does, an event whose relatedTarget or touch target is inside a shadow tree has no "
               "object to be retargeted TO, and this is the step that would have to invent one");
        host = g_tree->shadow_host(ctx, root);
        JS_FreeValue(ctx, root);
        DCHECK(JS_IsObject(host),
               "§4.8's retargeting climbed to a shadow root's host and the tree answered with none — §4.8's "
               "attach a shadow root gives every shadow root a host, so a null here is a shadow root whose host "
               "the tree lost, and the object it was hiding would be reported as null");
        JS_FreeValue(ctx, cur);
        cur = host;
    }
}

/* WEB IDL §3.2.16's `EventTarget?` — see event_target.h for why the conversion is stated here and once. */
JSValue event_target_nullable_of(JSContext *ctx, JSValueConst v, const char *what)
{
    JSValue p, target;
    bool ok = false;

    DCHECK(what != NULL && *what,
           "the `EventTarget?` conversion was asked to convert a value for a member it cannot name — the name "
           "is the whole of what the TypeError tells the page");
    if (JS_IsUndefined(v) || JS_IsNull(v))
        return JS_NULL;
    /* THE WALK NEVER TOUCHES A PROXY. JS_GetPrototype on one runs its getPrototypeOf trap — the page's code,
       from inside a C activation — and a Proxy is not a platform object implementing the interface anyway, so
       a link that is one ends the walk instead of being asked. */
    if (!JS_IsObject(v) || JS_IsProxy(v))
        return JS_ThrowTypeError(ctx, "%s must be an EventTarget or null", what);
    target = event_target_proto(ctx);
    p = JS_GetPrototype(ctx, v);
    while (JS_IsObject(p) && !JS_IsProxy(p)) {
        JSValue next;
        if (JS_VALUE_GET_PTR(p) == JS_VALUE_GET_PTR(target)) { ok = true; break; }
        next = JS_GetPrototype(ctx, p);
        JS_FreeValue(ctx, p);
        p = next;
    }
    JS_FreeValue(ctx, p);
    JS_FreeValue(ctx, target);
    if (!ok)
        return JS_ThrowTypeError(ctx, "%s must be an EventTarget or null", what);
    return JS_DupValue(ctx, v);
}

/* §2.9 steps 6.1-6.2 and 6.9.3-6.9.4: a NEW LIST holding each of the event's touch targets RETARGETED against
   `against`. JS_NULL when the event's touch target list is empty, which is every event but a TouchEvent and is
   the same list one allocation cheaper — the item's field carries that spelling too. OWNED. */
static JSValue dispatch_retarget_touch_targets(JSContext *ctx, JSValueConst ev, JSValueConst against)
{
    JSValue list = event_touch_target_list(ctx, ev), out;
    uint32_t i, n;

    if (!JS_IsArray(list)) {
        JS_FreeValue(ctx, list);
        return JS_NULL;
    }
    n = arr_len(ctx, list);
    out = JS_NewArray(ctx);
    CHECK(!JS_IsException(out), "§2.9's retargeted touch target list could not be allocated");
    for (i = 0; i < n; i++) {
        JSValue t = JS_GetPropertyUint32(ctx, list, i);

        JS_SetPropertyUint32(ctx, out, i, event_target_retarget(ctx, t, against));
        JS_FreeValue(ctx, t);
    }
    JS_FreeValue(ctx, list);
    return out;
}

/* Step 6.9.5's second disjunct: "target's root is a shadow-including inclusive ancestor of parent". FALSE when
   `target` is not a node, which is the answer that matters — it is what stops the walk treating the window as a
   boundary crossing on the way past a detached target. */
static bool dispatch_target_root_contains(JSContext *ctx, JSValueConst target, JSValueConst parent)
{
    JSValue root;
    bool contains;

    if (!g_tree)
        return false;
    root = g_tree->root(ctx, target);
    contains = g_tree->is_shadow_including_inclusive_ancestor(ctx, root, parent);
    JS_FreeValue(ctx, root);
    return contains;
}

static int js_dispatch_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    JSDispatchState *s = st;
    /* BOTH ARE JUMPED PAST. The re-entry gotos land after these are assigned on the straight path, so they are
       initialised at their declaration rather than left for a `goto` to read. */
    JSValue fn = JS_UNDEFINED, ignored = JS_UNDEFINED;
    int r;

    if (s->hdr.stage == DISPATCH_INIT) {
        JSValueConst target;

        JS_FreeValue(ctx, cb_result);
        cb_result = JS_UNDEFINED;
        s->path = s->type = s->lcb = s->exc = s->cur = s->act = s->arr = s->ev = s->result = JS_UNDEFINED;
        s->tgt = s->slottable = JS_UNDEFINED;
        s->cb[0] = s->cb[1] = s->cb[2] = JS_UNDEFINED;
        report_exception_work_start(&s->rep);
        /* THREE ENTRIES, ONE MACHINE, and `arg` is decided at REGISTRATION so no call site chooses:
             DISPATCH_ARG  — dispatchEvent: the receiver is the target, the page supplied the event.
             CLICK_SYNTH   — §3.2.2 click(): the receiver is the target and the event is BUILT, because click
                             is "fire a synthetic pointer event named click", which IS this dispatch.
             DISPATCH_PAIR — the ENGINE firing its own event, where there is no receiver to be the target
                             because the caller is C: both come in as arguments.
           The third is what let the second DELIVERY go. The engine used to enqueue each listener as its own
           job — a walk with no continuation, so it could not see stopImmediatePropagation, could not answer
           whether anything cancelled, and was a second implementation of §2.9 beside this one. */
        target = (s->hdr.arg == DISPATCH_PAIR) ? step_arg(&s->hdr, 0)
                                              : event_target_receiver(ctx, s->hdr.this_val);
        s->ev = (s->hdr.arg == CLICK_SYNTH)
                    ? event_new_untrusted(ctx, "click", /*bubbles*/ true, /*cancelable*/ true)
                    : JS_DupValue(ctx, step_arg(&s->hdr, s->hdr.arg == DISPATCH_PAIR ? 1 : 0));
        /* §2.9 step 1: the argument must BE an Event. The slot record is the brand — a page cannot forge the
           symbol it hangs off, so an object shaped like an event is still not one. */
        if (!event_is(ctx, s->ev)) {
            JS_ThrowTypeError(ctx, "dispatchEvent requires an Event");
            return JS_STEP_ABRUPT;
        }
        /* §2.9 step 2: an event already being dispatched cannot be dispatched again. */
        if (event_dispatch_flag(ctx, s->ev)) {
            JS_ThrowDOMException(ctx, "InvalidStateError", "the event is already being dispatched");
            return JS_STEP_ABRUPT;
        }
        /* §2.7 dispatchEvent's OTHER early throw, and it is the same step: an event whose INITIALIZED FLAG is
           unset cannot be dispatched. Only §4.5's createEvent makes one, and the two-call shape it exists for
           — `createEvent` then `initEvent` — is only meaningful because dispatching between them throws. */
        if (!event_initialized(ctx, s->ev)) {
            JS_ThrowDOMException(ctx, "InvalidStateError",
                                 "the event was created by createEvent and never initialised");
            return JS_STEP_ABRUPT;
        }
        event_set_dispatch_flag(ctx, s->ev, true);
        /* §2.9 step 3: an event the PAGE dispatches is untrusted, whatever it was when constructed. One the
           ENGINE fires keeps the flag it was built with, which is the whole difference between them. */
        if (s->hdr.arg != DISPATCH_PAIR)
            event_set_trusted(ctx, s->ev, false);
        s->type = event_type(ctx, s->ev);
        s->clear_targets = 0;         /* step 5 */
        {
            /* §2.9 step 4: the event's relatedTarget RETARGETED against the target. Everything after it is
               about THIS value and not about the event's, which is the difference step 6 turns on. */
            JSValue er = event_related_target(ctx, s->ev);
            JSValue related = event_target_retarget(ctx, er, target);
            /* §2.9 step 6: "If target is not relatedTarget OR target is event's relatedTarget". When it fails,
               the whole of steps 6.1-6.14 is skipped — no path, no listeners, no activation target — and the
               dispatch runs steps 7-13 over an event that never propagated. That is what stops a `mouseover`
               whose relatedTarget retargets to the target itself from firing at it: the pointer never left the
               element as far as this element can tell. The second disjunct is the escape hatch for an event
               deliberately dispatched AT its own relatedTarget, which did not move and must still fire. */
            bool suppressed = same_target(target, related) && !same_target(target, er);
            JSValue touch = suppressed ? JS_NULL : dispatch_retarget_touch_targets(ctx, s->ev, target);

            JS_FreeValue(ctx, er);
            if (suppressed) {
                /* nothing to walk and nothing to invoke: both passes below find an empty path and fall through
                   to step 7. The event's own path stays the empty list, so composedPath answers with it. */
                s->tn = s->ti = s->n = s->i = 0;
                s->hdr.stage = DISPATCH_CAPTURE;
            } else {
                /* §2.9 step 6.3: APPEND TO AN EVENT PATH with event, target, TARGETOVERRIDE, relatedTarget,
                   touchTargets and false. The path is the EVENT's — composedPath reads it — so it is published
                   on the event as it grows rather than kept privately here.
                   STEP 2: targetOverride is the target, UNLESS the dispatch was given one. HTML gives one for
                   `pagehide`, `pageshow`, `unload` and `beforeunload` — fired AT the Window with the DOCUMENT
                   as their target — through what the spec spells as the legacy target override flag. Without
                   it those events report the Window, which is what a page's `e.target` reads. */
                JSValueConst given = step_arg(&s->hdr, 2);
                JSValueConst override = JS_IsUndefined(given) ? target : given;

                s->path = event_path_new(ctx);
                event_path_append(ctx, s->path, target, override, related, touch,
                                  dispatch_is_closed_shadow_root(ctx, target), /*slotInClosedTree*/ false);
                /* THE SIZE IS THE PATH'S, never a counter kept beside it: the two are read together at every
                   step of the two passes, and a counter that drifts by one walks off the end or drops the
                   root. */
                s->tn = event_path_length(ctx, s->path);
                event_set_path(ctx, s->ev, s->path);
                /* §2.9 step 6.4's isActivationEvent. The spec says "event is a MouseEvent object and event's
                   type is `click`"; this engine has no MouseEvent interface yet, so the type is the whole of
                   what it can ask — which is why `Event-dispatch-click` (a `new MouseEvent("click")`) is a
                   MouseEvent gap and not a dispatch one. */
                if (JS_IsString(s->type)) {
                    const char *t = JS_ToCString(ctx, s->type);
                    s->is_activation = t != NULL && !strcmp(t, "click");
                    if (t) JS_FreeCString(ctx, t);
                }
                /* §2.9 step 6.5: the TARGET is the activation target if it has one — no `bubbles` condition
                   here, which is the difference from step 6.9.5.1's test on an ancestor. */
                if (s->is_activation && g_has_activation && g_has_activation(ctx, target))
                    s->act = JS_DupValue(ctx, target);
                /* step 6.6: "Let slottable be target, if target is a slottable and is ASSIGNED, and null
                   otherwise." An assigned slottable's get the parent answers with its SLOT, and step 6.9.1 is
                   the only place that can tell that hop apart from an ordinary parent — so the fact is carried
                   across the ask. */
                if (dispatch_is_assigned_slottable(ctx, target))
                    s->slottable = JS_DupValue(ctx, target);
                s->slot_in_closed_tree = 0;   /* step 6.7 */
                /* step 6.8: the first get the parent. `cur` carries the walk's frontier from here to the end of
                   6.9, and `tgt` carries the walk's own `target`, which 6.9.7.1 moves at every shadow
                   boundary. */
                s->cur = JS_DupValue(ctx, target);
                s->tgt = JS_DupValue(ctx, target);
                s->hdr.stage = DISPATCH_PATH;
            }
            JS_FreeValue(ctx, related);
            JS_FreeValue(ctx, touch);
        }
    }

    if (s->hdr.stage == DISPATCH_PATH) {
        /* §2.9 step 6.9: "While parent is non-null" — append it and ask it for ITS parent. A tree is the PAGE's
           size, so this yields between parents rather than walking to the root inside one opcode; the frontier
           is on the state, so a park in the middle resumes at the ancestor it had reached. */
        JSValue parent;

        JS_FreeValue(ctx, cb_result);
        cb_result = JS_UNDEFINED;
        parent = dispatch_get_parent(ctx, s->cur, s->ev);
        JS_FreeValue(ctx, s->cur);
        s->cur = parent;
        if (JS_IsObject(parent)) {
            /* step 6.9.1: the parent the walk was just handed is the SLOT the previous item is assigned to —
               §4.4's get the parent says so, which is why this is an assert and not a test. Crossing INTO a
               closed shadow tree through a slot is what slot-in-closed-tree records, and it records it for
               THIS item, before the append below and until step 6.9.9 clears it. */
            if (JS_IsObject(s->slottable)) {
                DCHECK(dispatch_is_slot(ctx, parent),
                       "§2.9 step 6.9.1: the previous path entry is an ASSIGNED slottable, so §4.4's get the "
                       "parent must have answered with its assigned slot — a parent that is not a slot means "
                       "the two algorithms disagree about what `assigned` means");
                JS_FreeValue(ctx, s->slottable);
                s->slottable = JS_UNDEFINED;
                if (dispatch_root_is_shadow_root(ctx, parent, /*want_closed*/ true))
                    s->slot_in_closed_tree = 1;
            }
            /* step 6.9.2: and this parent may itself be slotted, one component inside another. */
            if (dispatch_is_assigned_slottable(ctx, parent))
                s->slottable = JS_DupValue(ctx, parent);
            /* steps 6.9.3-6.9.4: the event's relatedTarget and each of its touch targets, RETARGETED against
               THIS parent. They are per-ancestor values and not the dispatch's — the same relatedTarget is one
               object to an ancestor inside its shadow tree and the tree's host to one outside — which is why
               they are computed here, per iteration, and go into the item rather than onto the state. */
            {
                JSValue er = event_related_target(ctx, s->ev);
                JSValue related = event_target_retarget(ctx, er, parent);
                JSValue touch = dispatch_retarget_touch_targets(ctx, s->ev, parent);
                bool ended = false;

                JS_FreeValue(ctx, er);
                if (dispatch_is_window(ctx, parent) || dispatch_target_root_contains(ctx, s->tgt, parent)) {
                    /* step 6.9.5: still inside the tree the walk currently calls the target's, so the item gets
                       NO shadow-adjusted target and `invoke` will keep answering with the one further in.
                       6.9.5.1: an ANCESTOR becomes the activation target only for an event that BUBBLES, and
                       only while none has been picked — the nearest one, target first, wins. */
                    if (s->is_activation && !JS_IsObject(s->act) && event_bubbles(ctx, s->ev) &&
                        g_has_activation && g_has_activation(ctx, parent))
                        s->act = JS_DupValue(ctx, parent);
                    event_path_append(ctx, s->path, parent, JS_NULL, related, touch,
                                      dispatch_is_closed_shadow_root(ctx, parent), s->slot_in_closed_tree);
                } else if (same_target(parent, related)) {
                    /* step 6.9.6: the walk has reached the retargeted relatedTarget itself. "Set parent to
                       null" ENDS the walk without appending — the event never propagates past the object it is
                       reported as coming from, which is what makes `mouseout` stop at the common ancestor. */
                    ended = true;
                } else {
                    /* step 6.9.7: the walk has left the tree it was in — this parent is the shadow HOST — so
                       the event RETARGETS here: everything from this item outward reports the host as `target`,
                       which is the whole of what a shadow tree hides.
                       6.9.7.2 has NO `bubbles` condition, unlike 6.9.5.1 — the host of a shadow tree the event
                       came out of is an activation target for a non-bubbling event too. */
                    JS_FreeValue(ctx, s->tgt);
                    s->tgt = JS_DupValue(ctx, parent);
                    if (s->is_activation && !JS_IsObject(s->act) && g_has_activation &&
                        g_has_activation(ctx, parent))
                        s->act = JS_DupValue(ctx, parent);
                    event_path_append(ctx, s->path, parent, parent, related, touch,
                                      dispatch_is_closed_shadow_root(ctx, parent), s->slot_in_closed_tree);
                }
                JS_FreeValue(ctx, related);
                JS_FreeValue(ctx, touch);
                if (!ended) {
                    s->tn = event_path_length(ctx, s->path);
                    s->slot_in_closed_tree = 0;   /* step 6.9.9, and it is per ITERATION, not per tree */
                    return JS_STEP_YIELD;
                }
                /* step 6.9.6 set parent to null, so 6.9.8 does not ask again and the while ends: fall out of
                   the walk with the path exactly as it stands. */
            }
        }
        JS_FreeValue(ctx, s->cur);
        s->cur = JS_UNDEFINED;
        JS_FreeValue(ctx, s->slottable);
        s->slottable = JS_UNDEFINED;
        /* steps 6.10-6.11: clearTargetsItem is the LAST item with a non-null shadow-adjusted target — the
           outermost thing the event still calls its target — and if ANY of that item's three target-bearing
           fields is a node inside a shadow tree, all three are CLEARED once the walk is over (step 11), so a
           page holding the event afterwards cannot read a node out of a tree it was never given. It is a
           DISJUNCTION over shadow-adjusted target, relatedTarget and every entry of the touch target list:
           an event whose TARGET is in the document tree can still carry a relatedTarget that is not. */
        {
            uint32_t k = s->tn;

            while (k-- > 0) {
                JSValue item = event_path_item(ctx, s->path, k);
                JSValue sat = event_path_shadow_adjusted_target(ctx, item);
                bool found = JS_IsObject(sat);

                if (found) {
                    JSValue related = event_path_related_target(ctx, item);
                    JSValue touch = event_path_touch_targets(ctx, item);

                    s->clear_targets = dispatch_root_is_shadow_root(ctx, sat, /*want_closed*/ false) ||
                                       dispatch_root_is_shadow_root(ctx, related, /*want_closed*/ false);
                    if (!s->clear_targets && JS_IsArray(touch)) {
                        uint32_t j, m = arr_len(ctx, touch);

                        for (j = 0; j < m && !s->clear_targets; j++) {
                            JSValue t = JS_GetPropertyUint32(ctx, touch, j);

                            s->clear_targets = dispatch_root_is_shadow_root(ctx, t, /*want_closed*/ false);
                            JS_FreeValue(ctx, t);
                        }
                    }
                    JS_FreeValue(ctx, related);
                    JS_FreeValue(ctx, touch);
                }
                JS_FreeValue(ctx, sat);
                JS_FreeValue(ctx, item);
                if (found)
                    break;
            }
        }
        s->ti = 0;
        s->n = s->i = 0;
        s->hdr.stage = DISPATCH_CAPTURE;
    }

    if (s->hdr.stage == DISPATCH_ACTIVATION) {
        /* re-entered inside the activation behaviour — see below. It waits on the HOST, not on a call, so
           whatever the resume carries is not this machine's and is released here rather than leaked. */
        JS_FreeValue(ctx, cb_result);
        goto activation;
    }
    /* A re-entry with no call in flight carries nothing this walk wants; the one that does is consumed by the
       request it belongs to, at resume_listener. */
    if (s->cphase == 0 && s->lphase == 0 && s->reporting == 0) { JS_FreeValue(ctx, cb_result); cb_result = JS_UNDEFINED; }
    /* §2.9 steps 6.13 and 6.14: TWO passes over the path, not three legs over parts of it. The old shape ran the
       target as a leg of its own with BOTH kinds of listener in registration order, which is a different answer
       from the spec's and one the corpus asks for directly: at the target a CAPTURING listener runs in the
       capturing pass and a bubbling one in the bubbling pass, so registering bubble-then-capture still fires
       capture first. It also made "does this event bubble" a question about the LEG rather than about the ITEM,
       and for the target item the answer is that it is invoked either way. */
    for (;;) {
        while (s->i < s->n) {
            JSValue rec;

            /* §2.9: stopImmediatePropagation ends the walk between listeners, which is the only place it can
               be observed — the flag was set by a listener that has already returned. */
            if (s->reporting)
                goto report_throw;   /* re-entered inside the REPORT of what the last listener threw */
            if (s->cphase == 0 && s->lphase == 0 && event_stop_immediate(ctx, s->ev))
                break;
            if (s->cphase != 0) {
                /* RE-ENTERED INSIDE THE CALL BELOW. The record is NOT re-read: `once` has already removed it
                   from the live list and set its removed field, so a re-read would skip the very listener whose
                   answer is arriving and lose the call's result. The callee is held by the request buffer. */
                fn = JS_UNDEFINED;
                goto resume_listener;
            }
            if (s->lphase != 0)
                goto resolve_operation;   /* re-entered inside the `handleEvent` READ — same reason */
            rec = JS_GetPropertyUint32(ctx, s->arr, s->i);
            if (!JS_IsObject(rec)) {
                JS_FreeValue(ctx, rec);
                s->i++;
                continue;
            }
            /* §2.9 "inner invoke" step 2: a listener whose REMOVED field is set is skipped. The walk iterates a
               SNAPSHOT so that an added listener does not run, and the spec's own note says removal must still
               have an effect — which is exactly what the field is for. Without it, `removeEventListener` called
               from inside a dispatch removed the listener from the live list and the snapshot ran it anyway. */
            if (rec_flag(ctx, rec, "removed")) {
                JS_FreeValue(ctx, rec);
                s->i++;
                continue;
            }
            /* §2.9 "inner invoke" steps 2.3-2.4: the CAPTURING pass runs only capturing listeners and the
               BUBBLING pass only the others, at every item of the path INCLUDING the target. */
            if (rec_flag(ctx, rec, "capture") != (s->hdr.stage == DISPATCH_CAPTURE)) {
                JS_FreeValue(ctx, rec);
                s->i++;
                continue;
            }
            fn = rec_cb(ctx, rec);
            /* §8.1.7: the HANDLER SLOT resolves HERE, at dispatch time, to whatever `ontype` currently is —
               that is what keeps the slot's POSITION in the list while its handler changes underneath it. An
               UNSET handler is not a listener at all, which is why this one skip stays a skip: there is nothing
               to look an operation up on. */
            if (JS_VALUE_GET_PTR(fn) == JS_VALUE_GET_PTR(g_handler_marker)) {
                const char *t = JS_IsString(s->type) ? JS_ToCString(ctx, s->type) : NULL;
                JS_FreeValue(ctx, fn);
                fn = t ? handler_current(ctx, s->cur, t) : JS_UNDEFINED;
                if (t) JS_FreeCString(ctx, t);
                if (!JS_IsFunction(ctx, fn)) {
                    JS_FreeValue(ctx, fn);
                    JS_FreeValue(ctx, rec);
                    s->i++;
                    continue;
                }
            }
            /* §2.9 "inner invoke" step 2.5: a `once` listener is removed BEFORE it is called, so a listener that
               re-enters this dispatch cannot see itself. Removing it after would let a re-entrant fire run it
               a second time, which is the exact bug `once` exists to prevent. */
            if (rec_flag(ctx, rec, "once")) {
                const char *t = JS_IsString(s->type) ? JS_ToCString(ctx, s->type) : NULL;
                if (t) { listener_remove_record(ctx, s->cur, t, rec); JS_FreeCString(ctx, t); }
            }
            /* §2.9 "inner invoke" step 2.9: a PASSIVE listener raises the event's in-passive listener flag for
               the duration of the call, which is what makes its preventDefault() do nothing. */
            s->in_passive = rec_flag(ctx, rec, "passive");
            if (s->in_passive)
                event_set_in_passive(ctx, s->ev, true);
            JS_FreeValue(ctx, rec);
            JS_FreeValue(ctx, s->lcb);
            s->lcb = fn;   /* held across the operation lookup below, which can run the page's code */
resolve_operation:
            /* §2.9 "inner invoke" step 2.11 is "CALL A USER OBJECT'S OPERATION with listener's callback and
               `handleEvent`", and Web IDL §3.12 step 10 is what that means for a callback INTERFACE: a callable
               callback is itself the operation and keeps the given `this`; a NON-callable one has `handleEvent`
               READ OFF IT — per invocation, so a page that swaps the method between two dispatches gets both —
               and is then the `this` of that call.
               The read is the page's code (an accessor, a Proxy trap), so it is a REQUEST and not a
               JS_GetPropertyStr; that is also why `event-global-set-before-handleEvent-lookup` can observe where
               in the algorithm it happens. */
            fn = JS_DupValue(ctx, s->lcb);
            if (!JS_IsFunction(ctx, fn)) {
                JSAtom op;
                JSValue m = JS_UNDEFINED;

                JS_FreeValue(ctx, fn);
                op = JS_NewAtom(ctx, "handleEvent");
                r = step_getprop_run(ctx, &s->hdr, s->lcb, op, cb_result, &m, out_cb, out_argc);
                JS_FreeAtom(ctx, op);
                cb_result = JS_UNDEFINED;
                if (r > 0) { s->lphase = 1; return r; }   /* parked ON THE READ; the resume comes back here */
                s->lphase = 0;
                if (r < 0) {
                    /* Web IDL §3.12 step 10.2: an ABRUPT Get is RETURNED as it stands. The read reports it here
                       because this machine's definition declares catches_abrupt, and §2.9 "inner invoke" step
                       2.11 says what to do with it — REPORT it and carry on down the listener list, never
                       unwind the dispatch. `EventListener-handleEvent`'s "rethrows errors when getting
                       handleEvent" is exactly this listener. */
                    goto listener_threw;
                }
                if (!JS_IsFunction(ctx, m)) {
                    /* Web IDL §3.12 step 10.4: a non-callable operation is a TypeError, reported the same way. */
                    JS_ThrowTypeError(ctx, "the event listener's `handleEvent` is not callable");
                    JS_FreeValue(ctx, m);
                    goto listener_threw;
                }
                fn = m;
                /* Web IDL §3.12 step 10.5: the receiver becomes the callback OBJECT, overriding currentTarget. */
                r = step_call_run(ctx, &s->cphase, STEP_CB(s->cb), fn, s->lcb, 1, (JSValueConst *)&s->ev,
                                  cb_result, &ignored, out_cb, out_argc);
                JS_FreeValue(ctx, fn);
                cb_result = JS_UNDEFINED;
                if (r > 0) return r;
                goto listener_returned;
            }
            /* §2.9 "inner invoke" step 2.11: the listener is called with `this` = currentTarget and the event as
               its one argument. A CALL REQUEST, so the listener is ordinary preemptible page code and this
               machine parks — which is the whole reason the engine's own firing can share this walk. */
resume_listener:
            r = step_call_run(ctx, &s->cphase, STEP_CB(s->cb), fn, s->cur, 1, (JSValueConst *)&s->ev,
                              cb_result, &ignored, out_cb, out_argc);
            JS_FreeValue(ctx, fn);   /* the request DUP'd it into the buffer, which is what holds it parked */
            cb_result = JS_UNDEFINED;
            if (r > 0) return r;         /* parked ON THIS LISTENER; the resume comes back to it */
listener_returned:
            if (s->in_passive) {
                event_set_in_passive(ctx, s->ev, false);
                s->in_passive = 0;
            }
            JS_FreeValue(ctx, s->lcb);
            s->lcb = JS_UNDEFINED;
            /* §2.9 "inner invoke" step 2.11: "If this throws an exception exception: report exception". THE
               WALK DOES NOT UNWIND — the next listener runs, the remaining path items run, and dispatchEvent
               still answers !canceled. This machine therefore DECLARES catches_abrupt, so a throwing listener
               arrives as a value here instead of tearing the dispatch down; without it, one page's throw
               skipped every listener after it and the exception was swallowed with nothing to say so. */
            if (JS_IsException(ignored)) {
                ignored = JS_UNDEFINED;
listener_threw:
                /* Reached from the operation lookup as well, which has not run the cleanup above — both are
                   idempotent, which is why one label serves both arrivals. */
                if (s->in_passive) { event_set_in_passive(ctx, s->ev, false); s->in_passive = 0; }
                JS_FreeValue(ctx, s->lcb);
                s->lcb = JS_UNDEFINED;
                s->exc = JS_GetException(ctx);
                s->reporting = 1;
            }
            JS_FreeValue(ctx, ignored);  /* §2.9: a listener's return value is discarded */
report_throw:
            if (s->reporting) {
                r = report_exception_run(ctx, &s->rep, s->exc, cb_result, out_cb, out_argc);
                cb_result = JS_UNDEFINED;
                if (r > 0) return r;    /* parked inside the `error` event's own dispatch */
                s->reporting = 0;
                JS_FreeValue(ctx, s->exc);
                s->exc = JS_UNDEFINED;
            }
            s->i++;
        }
        /* ON TO THE NEXT PATH ITEM, and then to the next pass. `ti` names the item ABOUT to be invoked and is
           advanced once its listeners are set up, so the FIRST item of each pass is ti == 0 and no entry point
           has to say so twice. */
        for (;;) {
            uint32_t idx;
            const char *type;
            bool at_target;
            JSValue item, sat;

            if (s->ti >= s->tn) {
                if (s->hdr.stage == DISPATCH_BUBBLE) goto walked;
                s->hdr.stage = DISPATCH_BUBBLE;
                s->ti = 0;
                continue;
            }
            /* the capturing pass walks the path in REVERSE (root first); the bubbling pass walks it forwards. */
            idx = (s->hdr.stage == DISPATCH_CAPTURE) ? (s->tn - 1 - s->ti) : s->ti;
            item = event_path_item(ctx, s->path, idx);
            sat = event_path_shadow_adjusted_target(ctx, item);
            /* §2.9 steps 6.13.1 / 6.14.1: AT_TARGET is not "index zero", it is "this item HAS a shadow-adjusted
               target" — which is true of every item the event retargeted at, so an event dispatched inside a
               shadow tree is AT_TARGET twice, once for the node and once for its host. */
            at_target = JS_IsObject(sat);
            /* §2.9 step 6.14.2.1: in the BUBBLING pass an item that is not the target is skipped entirely for an
               event that does not bubble. The TARGET is invoked either way — which is why a non-bubbling event
               still reaches the target's non-capturing listeners. */
            if (!at_target && s->hdr.stage == DISPATCH_BUBBLE && !event_bubbles(ctx, s->ev)) {
                JS_FreeValue(ctx, sat);
                JS_FreeValue(ctx, item);
                s->ti++;
                continue;
            }
            /* §2.9 steps 6.13.1-6.13.2: the phase is AT_TARGET for an item that is a target, whichever pass is
               running, and the pass's own phase otherwise. */
            event_set_phase(ctx, s->ev, at_target ? 2 : (s->hdr.stage == DISPATCH_CAPTURE ? 1 : 3));
            /* "invoke" steps 1-3: the event's TARGET is the nearest shadow-adjusted target AT OR BEFORE this
               item — so every entry inside a shadow tree reports the node the event was dispatched at, and
               every entry from the host outward reports the host. It is a walk BACKWARD along the path and not
               a value set once for the dispatch, which is the whole of what retargeting is. */
            {
                uint32_t k = idx;

                while (!JS_IsObject(sat) && k > 0) {
                    JSValue back;

                    JS_FreeValue(ctx, sat);
                    back = event_path_item(ctx, s->path, --k);
                    sat = event_path_shadow_adjusted_target(ctx, back);
                    JS_FreeValue(ctx, back);
                }
                DCHECK(JS_IsObject(sat),
                       "§2.9 invoke step 2 ran off the front of the event path — item 0 is appended at step 6.3 "
                       "with the target as its shadow-adjusted target, so the backward walk always stops");
            }
            event_set_target(ctx, s->ev, sat);
            JS_FreeValue(ctx, sat);
            /* "invoke" steps 4-5: the event's relatedTarget and touch target list are THIS ITEM's — the forms
               §2.9 retargeted against this item's invocation target while it built the path. They are set per
               item and not once for the walk, for the same reason `target` is: a listener outside a shadow tree
               must read the host where one inside reads the node. */
            {
                JSValue related = event_path_related_target(ctx, item);
                JSValue touch = event_path_touch_targets(ctx, item);

                event_set_related_target(ctx, s->ev, related);
                event_set_touch_target_list(ctx, s->ev, touch);
                JS_FreeValue(ctx, related);
                JS_FreeValue(ctx, touch);
            }
            /* "invoke" step 6: a walk that has been stopped still RUNS, item by item, and returns before it
               invokes anything. That is not the same as ending the walk here, which is what this did: the
               event's `target` is written by steps 1-3 above BEFORE the return, so the outer entries of a
               retargeted path go on adjusting it after a listener has called stopPropagation. */
            if (event_stop_propagation(ctx, s->ev)) {
                JS_FreeValue(ctx, item);
                s->ti++;
                continue;
            }
            JS_FreeValue(ctx, s->cur);
            JS_FreeValue(ctx, s->arr);
            s->cur = event_path_invocation_target(ctx, item);   /* "invoke" step 7 */
            JS_FreeValue(ctx, item);
            event_set_current(ctx, s->ev, s->cur);
            type = JS_IsString(s->type) ? JS_ToCString(ctx, s->type) : NULL;
            s->arr = type ? listener_snapshot(ctx, s->cur, type) : JS_NewArray(ctx);
            if (type) JS_FreeCString(ctx, type);
            s->n = arr_len(ctx, s->arr);
            s->i = 0;
            s->ti++;
            break;
        }
    }

walked:
    JS_FreeValue(ctx, cb_result);
    /* §2.9 steps 7-10: eventPhase NONE, currentTarget null, the path empty, and the dispatch and both stop
       flags UNSET — one operation, because the spec states them together and because leaving the stop flags set
       made the SAME event unusable for a second dispatch. */
    event_end_dispatch(ctx, s->ev);
    /* §2.9 step 11: if clearTargets, the event's target, relatedTarget and touch target list are CLEARED —
       ONE operation, because the standard states them as one step and because two of the three shipped without
       the first. See event.h. */
    if (s->clear_targets)
        event_clear_targets(ctx, s->ev);

activation:
    /* §2.9's step 12, and it is last for a reason: the activation behaviour runs AFTER the whole walk and
       ONLY if nothing cancelled — which is the entire meaning of `preventDefault()` on a click. It runs with
       the event already cleaned up, so a behaviour that reads `currentTarget` sees null, as it must. */
    if (JS_IsObject(s->act) && !event_canceled(ctx, s->ev)) {
        int ar;
        DCHECK(g_run_activation != NULL, "an activation target was picked with nothing to perform");
        /* STAGE 2 IS THE RESUME POINT. The behaviour may wait on the host, and when it does the whole dispatch
           parks here — after the walk, with the event already cleaned up — and re-enters at exactly this line
           rather than replaying three legs of listeners. */
        s->hdr.stage = DISPATCH_ACTIVATION;
        ar = g_run_activation(ctx, s->act, s->ev, &s->aphase, &s->areq);
        if (ar != JS_STEP_DONE) return ar;
    }
    s->result = JS_NewBool(ctx, !event_canceled(ctx, s->ev));
    return JS_STEP_DONE;
}

static const JSTrampStepDef js_dispatch_def = {
    sizeof(JSDispatchState), js_dispatch_step, js_dispatch_fini, DISPATCH_ARG, .catches_abrupt = 1, .visit = js_dispatch_visit,
    .algorithm = "DOM §2.9 dispatch", .steps = DISPATCH_STEPS
};
/* §3.2.2 click(). The SAME machine — a click is a dispatch, and giving it its own would be two implementations
   of §2.9 that could disagree about listener order, the handler slot or the canceled flag. */
static const JSTrampStepDef js_click_def = {
    sizeof(JSDispatchState), js_dispatch_step, js_dispatch_fini, CLICK_SYNTH, .catches_abrupt = 1, .visit = js_dispatch_visit,
    .algorithm = "DOM §2.9 dispatch", .steps = DISPATCH_STEPS
};
static int g_click_stepid = -1;

void event_target_install_click(JSContext *ctx, JSValueConst target)
{
    DCHECK(JS_IsObject(target), "click was installed on something that is not an object");
    if (g_click_stepid < 0)
        g_click_stepid = JS_RegisterStepDef(JS_GetRuntime(ctx), &js_click_def);
    idl_install_step_method(ctx, target, "click", 0, g_click_stepid);
}

/* THE INTERNAL DOOR, MINTED IN THE FIRING REALM. A C function's `ctx` is its DEFINING realm, and this machine
   reads §7.6's window off it — so the dispatcher a child document fires through has to be the child's. It costs
   one function object per fire, which a dispatch that already snapshots a listener list per target does not
   notice, and it removes the runtime-lifetime object that would otherwise have to be freed and per-realm at the
   same time. OWNED by the caller. */
static JSValue dispatch_fn_new(JSContext *ctx)
{
    JSValue fn;

    DCHECK(g_dispatch_pair_stepid >= 0,
           "the engine fired an event before event_target_init declared the dispatcher — there is one dispatch, "
           "and this is the only way a C caller reaches it");
    fn = JS_NewCFunction2(ctx, NULL, "dispatch", 2, JS_CFUNC_step, g_dispatch_pair_stepid);
    CHECK(!JS_IsException(fn), "the internal event dispatcher could not be allocated");
    return fn;
}

/* THE ENGINE FIRING ITS OWN EVENT — `load`, `DOMContentLoaded`, `abort`. It builds the event and hands it to
   the SAME §2.9 machine, reached as a queued task because its callers are plain C the scheduler drives and
   cannot park. That is the whole fix: this used to walk the listener list ITSELF and enqueue each listener as
   its own job, which was a second implementation of §2.9 beside the machine — one that could not see
   stopImmediatePropagation (each listener was a separate job with no walk between them), could not bubble
   properly (the caller passed the window in by hand as `bubble_to`), and could not answer whether anything
   cancelled. There is one dispatch now; what differs between the two reach-paths is only whether the caller
   can park, which is a property of the CALLER and not of the algorithm.
   The event stays TRUSTED, which is what distinguishes one the engine fired from one the page dispatched. */
void event_target_fire(JSContext *ctx, JSValueConst target, JSValue ev, JSValueConst target_override)
{
    JSValueConst argv[3];
    JSValue fn;

    if (JS_IsException(ev)) { JS_FreeValue(ctx, ev); return; }
    argv[0] = target;
    argv[1] = ev;
    /* §2.9 STEP 2's targetOverride, AS AN ARGUMENT. It is a parameter OF THE DISPATCH and not state on the
       event — the same event fired twice, once with an override and once without, is two dispatches with two
       different `target`s — so it travels with the invocation.
       IT IS THE TARGET ITSELF rather than HTML's boolean, because the flag's whole content is "use the target's
       associated Document", and the caller that passes it is the one holding that Document. Asked as a boolean,
       this component would have to reach into document.c to resolve a Window it may not even own the realm of;
       asked as the value, it is the spec's own parameter and there is nothing to resolve.
       There was no way to pass it at all before, so `pagehide`, `pageshow`, `unload` and `beforeunload` — the
       only fires HTML gives it to — would have reported the Window where the spec says the Document. */
    argv[2] = target_override;
    fn = dispatch_fn_new(ctx);
    /* A JOB, so the dispatch runs as a call-root flow: preemptible, forkable and parkable like any other
       program, which is what every listener body needs and what a C activation cannot host. */
    JS_EnqueueCallJob(ctx, fn, 3, argv);
    JS_FreeValue(ctx, fn);
    JS_FreeValue(ctx, ev);
}

/* THE SAME FIRE, SYNCHRONOUSLY — for a caller that CAN park. §2.9 dispatch is synchronous, and some of the
   engine's own fires are specified that way: §3.2's `abort` happens inside abort(), so a page that calls
   ac.abort() and then reads a flag its listener set must see it already set. A queued fire answers the
   question after the caller has returned.
   It is the SAME machine through the same internal door — only the reach differs, which is the whole point of
   there being one dispatch: a caller that can park calls it as a REQUEST, one that cannot enqueues it as a job.
   `phase` and `cb` are the caller machine's own; `cb` needs FOUR slots ([this, func, target, event]), and it
   is FORWARDED, so its capacity is forwarded with it and the caller passes both through STEP_CB — a buffer
   that has decayed to a pointer can no longer say how big it is.
     0 = done (*pnot_canceled set when asked), 3 = the caller must return that step code. */
int event_target_fire_run(JSContext *ctx, uint8_t *phase, JSValue *cb, int cb_cap, JSValueConst target,
                          JSValueConst ev, JSValueConst target_override, JSValue in,
                          bool *pnot_canceled, JSValue **out_cb, int *out_argc)
{
    JSValueConst argv[3];
    JSValue out = JS_UNDEFINED;
    int r;

    /* ASKED ON BOTH LEGS, because the resume leg forwards the same capacity and a caller that got the first one
       right by accident must not get the second one wrong in silence. */
    DCHECK(cb_cap >= EVENT_FIRE_CB_SLOTS,
           "a fire request was handed a buffer narrower than §2.9's three-argument dispatch — declare it "
           "EventFireCb rather than counting the slots");

    if (*phase == 0) {
        JSValue fn = dispatch_fn_new(ctx);
        DCHECK(JS_IsObject(ev), "a synchronous fire was handed no event — §2.9 dispatches one that exists");
        argv[0] = target;
        argv[1] = ev;
        argv[2] = target_override;   /* §2.9 step 2's targetOverride — see event_target_fire */
        /* step_call_run DUPS the callee into the request buffer, which is what holds it across the suspension —
           so this realm's dispatcher is released here and the parked call still owns one. */
        r = step_call_run(ctx, phase, cb, cb_cap, fn, JS_UNDEFINED, 3, argv, in, &out, out_cb, out_argc);
        JS_FreeValue(ctx, fn);
        DCHECK(r == JS_STEP_CALL, "the dispatch request answered without parking");
        return r;
    }
    r = step_call_run(ctx, phase, cb, cb_cap, JS_UNDEFINED, JS_UNDEFINED, 3, NULL, in, &out, out_cb, out_argc);
    DCHECK(r == 0, "a synchronous fire resumed into something other than its answer");
    if (pnot_canceled) *pnot_canceled = JS_ToBool(ctx, out);
    JS_FreeValue(ctx, out);
    return 0;
}

static const JSTrampStepDef js_dispatch_pair_def = {
    sizeof(JSDispatchState), js_dispatch_step, js_dispatch_fini, DISPATCH_PAIR, .catches_abrupt = 1, .visit = js_dispatch_visit,
    .algorithm = "DOM §2.9 dispatch", .steps = DISPATCH_STEPS
};

/* §2.7's INTERFACE PROTOTYPE OBJECT, FOR ONE REALM — built at the end of the file because the dispatch machine
   it installs is declared just above. It is the FIRST entry in core/realm.h's list, so every realm — the
   agent's own included — has it before any interface that inherits EventTarget builds its prototype. */
static void event_target_install(JSContext *ctx)
{
    JSValue proto, prev;

    DCHECK(g_ready, "a realm asked for EventTarget.prototype before event_target_init declared the interface");
    prev = JS_GetClassProto(ctx, g_et_class);
    DCHECK(JS_IsNull(prev),
           "event_target_install ran twice in one realm — §3.7 gives a realm ONE EventTarget.prototype, and a "
           "second would leave the objects already chained to the first answering out of a discarded one");
    JS_FreeValue(ctx, prev);
    /* THE DISPATCH DECLARATIONS are the RUNTIME's — a step def is registered against the runtime and there is
       one §2.9 machine — so they are declared once and every realm's members carry the same ids. */
    if (g_dispatch_stepid < 0) {
        g_dispatch_stepid = JS_RegisterStepDef(JS_GetRuntime(ctx), &js_dispatch_def);
        g_dispatch_pair_stepid = JS_RegisterStepDef(JS_GetRuntime(ctx), &js_dispatch_pair_def);
    }

    proto = JS_NewObject(ctx);
    CHECK(!JS_IsException(proto), "EventTarget.prototype could not be allocated");
    idl_interface_tag(ctx, proto, "EventTarget");
    idl_install_method(ctx, proto, "addEventListener", 2, g_add_stepid);
    idl_install_method(ctx, proto, "removeEventListener", 2, g_remove_stepid);
    idl_install_step_method(ctx, proto, "dispatchEvent", 1, g_dispatch_stepid);
    JS_SetClassProto(ctx, g_et_class, proto);   /* the realm owns it from here */
}
