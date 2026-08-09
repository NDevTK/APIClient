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
#include "core/events/event.h"

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
/* §2.9's PROPAGATION PATH IS THE TREE'S QUESTION, not this component's. A target's ancestors are the DOM's,
   and an EventTarget that is not a Node — an AbortSignal, which Streams §5.4 gives every writable controller —
   has none. Naming node.c here made EVERY host that installs events link the whole DOM and lexbor with it,
   which is why the streams gate could not build with a real AbortSignal in it. The tree registers the walk; a
   host that registers none has a flat path, which is exactly right for one with no document. */
static JSValue (*g_ancestors)(JSContext *ctx, JSValueConst target);
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
            { "capture", IDL_BOOLEAN }, { "once", IDL_BOOLEAN }, { "passive", IDL_BOOLEAN },
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

void event_target_set_tree(JSValue (*ancestors)(JSContext *ctx, JSValueConst target))
{
    g_ancestors = ancestors;
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
static JSValue listener_record(JSContext *ctx, JSValueConst cb, bool capture, bool once)
{
    JSValue rec = JS_NewObjectProto(ctx, JS_NULL);
    CHECK(!JS_IsException(rec), "event listeners: OOM recording a registration — a dropped listener is page "
                                "code that never runs");
    JS_SetPropertyStr(ctx, rec, "cb", JS_DupValue(ctx, cb));
    JS_SetPropertyStr(ctx, rec, "capture", JS_NewBool(ctx, capture));
    JS_SetPropertyStr(ctx, rec, "once", JS_NewBool(ctx, once));
    return rec;
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

/* The listener-list work, once `type` is a real string. Split from the coercion so the part that CAN reach the
   page's code is a request and the part that cannot is ordinary C. */
static JSValue add_listener_with_type(JSContext *ctx, JSValueConst this_val, JSValueConst cb, const char *type,
                                      bool capture, bool once)
{
    JSValue arr = listener_list(ctx, this_val, type, 1);
    uint32_t len, i;

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
    JS_SetPropertyUint32(ctx, arr, len, listener_record(ctx, cb, capture, once));
    JS_FreeValue(ctx, arr);
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
        if (same) JS_FreeValue(ctx, e);
        else JS_SetPropertyUint32(ctx, kept, k++, e);
    }
    JS_SetPropertyStr(ctx, map, type, kept);
    JS_FreeValue(ctx, arr);
    JS_FreeValue(ctx, map);
    return JS_UNDEFINED;
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
    const char *type;
    bool capture, once;
    JSValue r;

    if (argc < 2 || (magic == 0 && !JS_IsFunction(ctx, argv[1])))
        return JS_UNDEFINED;   /* 2.7: a non-callable listener is ignored, not an error */
    /* The engine-built dictionary, whatever the page wrote: `{capture:true}`, a bare `true`, or nothing. The
       union's flattening happened in the declaration, so there is one shape to read here. */
    capture = idl_dict_bool(ctx, opts, "capture");
    once = idl_dict_bool(ctx, opts, "once");
    type = JS_ToCString(ctx, argv[0]);   /* a real string by now: this cannot reach the page */
    if (!type)
        return JS_EXCEPTION;
    r = (magic == 0) ? add_listener_with_type(ctx, event_target_receiver(ctx, this_val), argv[1], type, capture, once)
                     : remove_listener_with_type(ctx, event_target_receiver(ctx, this_val), argv[1], type, capture);
    JS_FreeCString(ctx, type);
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
    X("onabort", EH_GLOBAL | EH_SIGNAL) X("onauxclick", EH_GLOBAL) X("onbeforeinput", EH_GLOBAL)                    \
    X("onbeforematch", EH_GLOBAL) X("onbeforetoggle", EH_GLOBAL) X("onblur", EH_GLOBAL) X("oncancel", EH_GLOBAL)      \
    X("oncanplay", EH_GLOBAL) X("oncanplaythrough", EH_GLOBAL) X("onchange", EH_GLOBAL) X("onclick", EH_GLOBAL)       \
    X("onclose", EH_GLOBAL) X("oncontextlost", EH_GLOBAL) X("oncontextmenu", EH_GLOBAL)                             \
    X("oncontextrestored", EH_GLOBAL) X("oncuechange", EH_GLOBAL) X("ondblclick", EH_GLOBAL) X("ondrag", EH_GLOBAL)   \
    X("ondragend", EH_GLOBAL) X("ondragenter", EH_GLOBAL) X("ondragleave", EH_GLOBAL) X("ondragover", EH_GLOBAL)      \
    X("ondragstart", EH_GLOBAL) X("ondrop", EH_GLOBAL) X("ondurationchange", EH_GLOBAL) X("onemptied", EH_GLOBAL)     \
    X("onended", EH_GLOBAL) X("onerror", EH_GLOBAL) X("onfocus", EH_GLOBAL) X("onformdata", EH_GLOBAL)                \
    X("oninput", EH_GLOBAL) X("oninvalid", EH_GLOBAL) X("onkeydown", EH_GLOBAL) X("onkeypress", EH_GLOBAL)            \
    X("onkeyup", EH_GLOBAL) X("onload", EH_GLOBAL) X("onloadeddata", EH_GLOBAL) X("onloadedmetadata", EH_GLOBAL)      \
    X("onloadstart", EH_GLOBAL) X("onmousedown", EH_GLOBAL) X("onmouseenter", EH_GLOBAL) X("onmouseleave", EH_GLOBAL) \
    X("onmousemove", EH_GLOBAL) X("onmouseout", EH_GLOBAL) X("onmouseover", EH_GLOBAL) X("onmouseup", EH_GLOBAL)      \
    X("onpause", EH_GLOBAL) X("onplay", EH_GLOBAL) X("onplaying", EH_GLOBAL) X("onprogress", EH_GLOBAL)               \
    X("onratechange", EH_GLOBAL) X("onreset", EH_GLOBAL) X("onresize", EH_GLOBAL) X("onscroll", EH_GLOBAL)            \
    X("onscrollend", EH_GLOBAL) X("onsecuritypolicyviolation", EH_GLOBAL) X("onseeked", EH_GLOBAL)                  \
    X("onseeking", EH_GLOBAL) X("onselect", EH_GLOBAL) X("onslotchange", EH_GLOBAL) X("onstalled", EH_GLOBAL)         \
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
    X("onreadystatechange", EH_DOCUMENT) X("onvisibilitychange", EH_DOCUMENT)

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
        add_listener_with_type(ctx, this_val, g_handler_marker, type, /*capture*/ false, /*once*/ false);
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

/* §2.9 step 5: dispatch runs over a COPY of the listener list. That matters more here than in a browser — the
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
        if (JS_VALUE_GET_PTR(e) == JS_VALUE_GET_PTR(rec)) JS_FreeValue(ctx, e);
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

typedef struct JSDispatchState {
    JSStepHdr hdr;       /* FIRST — the driver writes the def and the operand bounds through it */
    uint8_t   stage;
    uint8_t   cphase;    /* the call request's own phase, so a stage can hold a call across a suspension */
    uint32_t  i, n;      /* THE RESUME POINT: the listener being called, and how many there are */
    uint32_t  ti, tn;    /* THE OTHER: how far into the current LEG, and how long the whole path is */
    uint8_t   leg;       /* §2.9's three legs — 0 capturing (root->target), 1 AT_TARGET, 2 bubbling */
    /* THE ACTIVATION BEHAVIOUR'S OWN SUSPENSION. §4.6.3's is a navigation and a navigation fetches, so the
       behaviour is a step like everything else that can wait on the host: `aphase` is its resume point and
       `areq` the host request it is waiting on. They live here because the machine that can park is this one. */
    uint8_t   aphase;
    uint32_t  areq;
    JSValue   type;      /* the event's type, resolved once per target and needed by `once` (owned) */
    JSValue   path;      /* §2.9's propagation path — the target and its ancestors (owned) */
    JSValue   cur;       /* the target whose listeners are running (owned) */
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
    v->val(ctx, &s->cur);
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
    JS_FreeValue(ctx, s->cur);
    JS_FreeValue(ctx, s->act);
    JS_FreeValue(ctx, s->arr);
    JS_FreeValue(ctx, s->ev);
    s->result = s->path = s->type = s->cur = s->act = s->arr = s->ev = JS_UNDEFINED;
    for (k = 0; k < 3; k++) {
        JS_FreeValue(ctx, s->cb[k]);
        s->cb[k] = JS_UNDEFINED;
    }
    return r;
}

/* §2.9 THE PROPAGATION PATH: the target, then its ancestors, then the window for a document. It is stored
   target-first and walked in BOTH directions — computing it is what let `bubble_to` go: event_target_fire used
   to pass window in by hand as a special case, which is the document's ancestor stated twice, once in the spec
   and once in an argument. */
static JSValue dispatch_path(JSContext *ctx, JSValueConst target)
{
    JSValue path = JS_NewArray(ctx);
    uint32_t k = 0;

    JS_SetPropertyUint32(ctx, path, k++, JS_DupValue(ctx, target));
    if (g_ancestors) {
        /* THE HOOK ANSWERS "IS THIS TARGET IN THE TREE", and the ancestor list is how it says yes. An
           EventTarget that is in no tree — an AbortSignal, which Streams §5.4 gives every writable controller —
           gets UNDEFINED and a path of one. A DOCUMENT gets an EMPTY array, which is a different answer: it IS
           in the tree, it simply has nothing above it, and §7.6 still puts the window above THAT. Testing the
           list's length instead of its presence collapsed the two and dropped the window off the document's
           path, so `window.addEventListener('DOMContentLoaded', …)` — the way most bundles start — never ran. */
        JSValue up = g_ancestors(ctx, target);
        if (JS_IsArray(up)) {
            uint32_t i, n = 0;
            JSValue len_v = JS_GetPropertyStr(ctx, up, "length");
            JS_ToUint32(ctx, &n, len_v);
            JS_FreeValue(ctx, len_v);
            for (i = 0; i < n; i++)
                JS_SetPropertyUint32(ctx, path, k++, JS_GetPropertyUint32(ctx, up, i));
            /* §7.6 puts the WINDOW above the document, and it is THIS realm's — a document and its window
               are one browsing context, so asking the realm is what makes a popup's own dispatch reach its own
               window rather than whichever realm installed last. */
            JS_SetPropertyUint32(ctx, path, k++, JS_DupValue(ctx, event_target_global(ctx)));
        }
        JS_FreeValue(ctx, up);
    }
    return path;
}

static int js_dispatch_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    JSDispatchState *s = st;
    JSValue fn, ignored;
    int r;

    if (s->stage == 0) {
        JSValueConst target;

        JS_FreeValue(ctx, cb_result);
        cb_result = JS_UNDEFINED;
        s->path = s->type = s->cur = s->act = s->arr = s->ev = s->result = JS_UNDEFINED;
        s->cb[0] = s->cb[1] = s->cb[2] = JS_UNDEFINED;
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
        event_set_dispatch_flag(ctx, s->ev, true);
        /* §2.9 step 3: an event the PAGE dispatches is untrusted, whatever it was when constructed. One the
           ENGINE fires keeps the flag it was built with, which is the whole difference between them. */
        if (s->hdr.arg != DISPATCH_PAIR)
            event_set_trusted(ctx, s->ev, false);
        s->type = event_type(ctx, s->ev);
        s->path = dispatch_path(ctx, target);
        /* §2.9's ACTIVATION TARGET, picked while the path is here and BEFORE any listener runs — the nearest
           entry, target first, that has one. It is the target's own ancestors that make a click on a `<span>`
           inside an `<a>` follow the link, which is why it is the PATH that is searched and not the target.
           Only a `click` has one: §2.9 sets an activation target for that type alone, which is why a synthetic
           `dispatchEvent(new Event('foo'))` at an anchor navigates nowhere. */
        if (g_has_activation && JS_IsString(s->type)) {
            const char *t = JS_ToCString(ctx, s->type);
            bool is_click = t != NULL && !strcmp(t, "click");
            if (t) JS_FreeCString(ctx, t);
            if (is_click) {
                uint32_t k, kn = 0;
                JSValue len_v = JS_GetPropertyStr(ctx, s->path, "length");
                JS_ToUint32(ctx, &kn, len_v);
                JS_FreeValue(ctx, len_v);
                for (k = 0; k < kn; k++) {
                    JSValue e = JS_GetPropertyUint32(ctx, s->path, k);
                    if (JS_IsObject(e) && g_has_activation(ctx, e)) { s->act = e; break; }
                    JS_FreeValue(ctx, e);
                }
            }
        }
        s->tn = 0;
        JS_ToUint32(ctx, &s->tn, JS_GetPropertyStr(ctx, s->path, "length"));
        s->ti = 0;
        s->leg = 0;
        s->n = s->i = 0;
        s->stage = 1;
    }

    if (s->stage == 2) goto activation;   /* re-entered inside the activation behaviour — see below */
    for (;;) {
        while (s->i < s->n) {
            JSValue rec;
            bool want_capture;

            /* §2.9: stopImmediatePropagation ends the walk between listeners, which is the only place it can
               be observed — the flag was set by a listener that has already returned. */
            if (s->cphase == 0 && event_stop_immediate(ctx, s->ev))
                break;
            rec = JS_GetPropertyUint32(ctx, s->arr, s->i);
            if (!JS_IsObject(rec)) {
                JS_FreeValue(ctx, rec);
                JS_FreeValue(ctx, cb_result);
                cb_result = JS_UNDEFINED;
                s->i++;
                continue;
            }
            /* §2.9 "invoke": the CAPTURING leg runs only capturing listeners and the BUBBLING leg only the
               others; AT_TARGET runs both, which is why the target is its own leg rather than an end of one. */
            want_capture = (s->leg == 0);
            if (s->leg != 1 && rec_flag(ctx, rec, "capture") != want_capture) {
                JS_FreeValue(ctx, rec);
                JS_FreeValue(ctx, cb_result);
                cb_result = JS_UNDEFINED;
                s->i++;
                continue;
            }
            fn = rec_cb(ctx, rec);
            /* §8.1.7: the HANDLER SLOT resolves HERE, at dispatch time, to whatever `ontype` currently is —
               that is what keeps the slot's POSITION in the list while its handler changes underneath it. */
            if (JS_VALUE_GET_PTR(fn) == JS_VALUE_GET_PTR(g_handler_marker)) {
                const char *t = JS_IsString(s->type) ? JS_ToCString(ctx, s->type) : NULL;
                JS_FreeValue(ctx, fn);
                fn = t ? handler_current(ctx, s->cur, t) : JS_UNDEFINED;
                if (t) JS_FreeCString(ctx, t);
            }
            if (!JS_IsFunction(ctx, fn)) {
                JS_FreeValue(ctx, fn);
                JS_FreeValue(ctx, rec);
                JS_FreeValue(ctx, cb_result);
                cb_result = JS_UNDEFINED;
                s->i++;
                continue;
            }
            /* §2.9 "inner invoke" step 2: a `once` listener is removed BEFORE it is called, so a listener that
               re-enters this dispatch cannot see itself. Removing it after would let a re-entrant fire run it
               a second time, which is the exact bug `once` exists to prevent. */
            if (s->cphase == 0 && rec_flag(ctx, rec, "once")) {
                const char *t = JS_IsString(s->type) ? JS_ToCString(ctx, s->type) : NULL;
                if (t) { listener_remove_record(ctx, s->cur, t, rec); JS_FreeCString(ctx, t); }
            }
            JS_FreeValue(ctx, rec);
            /* §2.9 "inner invoke": the listener is called with `this` = currentTarget and the event as its one
               argument. A CALL REQUEST, so the listener is ordinary preemptible page code and this machine
               parks — which is the whole reason the engine's own firing can share this walk. */
            r = step_call_run(ctx, &s->cphase, STEP_CB(s->cb), fn, s->cur, 1, (JSValueConst *)&s->ev,
                              cb_result, &ignored, out_cb, out_argc);
            JS_FreeValue(ctx, fn);
            cb_result = JS_UNDEFINED;
            if (r > 0) return r;         /* parked ON THIS LISTENER; the resume comes back to it */
            JS_FreeValue(ctx, ignored);  /* §2.9: a listener's return value is discarded */
            s->i++;
        }
        /* ON TO THE NEXT TARGET. §2.9 walks the path THREE times: down it for the capturing listeners, once at
           the target for all of them, and back up it for the bubbling ones. `ti` counts within the current leg,
           so advancing is "next in this leg, or the first of the next one" and nothing else.
           stopPropagation ends the walk entirely — it is checked here because the flag is set by a listener
           that has already returned, exactly like stopImmediatePropagation above. */
        if (s->ti > 0 && event_stop_propagation(ctx, s->ev)) break;
        for (;;) {
            uint32_t legn = (s->leg == 1) ? 1 : (s->tn > 0 ? s->tn - 1 : 0);
            if (s->ti < legn) break;
            if (s->leg == 2) { legn = 0; break; }
            s->leg++;
            s->ti = 0;
            /* §2.9: the bubbling leg happens only for an event that bubbles. AT_TARGET always does. */
            if (s->leg == 2 && !event_bubbles(ctx, s->ev)) { s->leg = 3; }
            if (s->leg >= 3) break;
        }
        if (s->leg >= 3) break;
        if (s->leg == 2 && !event_bubbles(ctx, s->ev)) break;
        {
            /* leg 0 walks the path BACKWARDS (root first); leg 1 is the target; leg 2 walks it forwards. */
            uint32_t idx = (s->leg == 0) ? (s->tn - 1 - s->ti) : (s->leg == 1 ? 0 : s->ti + 1);
            uint32_t legn = (s->leg == 1) ? 1 : (s->tn > 0 ? s->tn - 1 : 0);
            JSValue first;
            const char *type;

            if (s->ti >= legn) break;
            JS_FreeValue(ctx, s->cur);
            JS_FreeValue(ctx, s->arr);
            s->cur = JS_GetPropertyUint32(ctx, s->path, idx);
            first = JS_GetPropertyUint32(ctx, s->path, 0);
            type = JS_IsString(s->type) ? JS_ToCString(ctx, s->type) : NULL;
            /* §2.9: `target` is where the event was dispatched and does not move; `currentTarget` is whose
               listeners are running now, and the phase is the leg. */
            event_set_targets(ctx, s->ev, first, s->cur);
            event_set_phase(ctx, s->ev, s->leg == 0 ? 1 : (s->leg == 1 ? 2 : 3));
            s->arr = type ? listener_snapshot(ctx, s->cur, type) : JS_NewArray(ctx);
            if (type) JS_FreeCString(ctx, type);
            JS_FreeValue(ctx, first);
        }
        s->n = arr_len(ctx, s->arr);
        s->i = 0;
        s->ti++;
    }
    JS_FreeValue(ctx, cb_result);

activation:
    /* §2.9 "clean up": the dispatch flag clears, the phase goes back to NONE and currentTarget to null. */
    event_set_dispatch_flag(ctx, s->ev, false);
    event_clear_current(ctx, s->ev);
    /* §2.9's LAST STEP, and it is last for a reason: the activation behaviour runs AFTER the whole walk and
       ONLY if nothing cancelled — which is the entire meaning of `preventDefault()` on a click. It runs with
       the event already cleaned up, so a behaviour that reads `currentTarget` sees null, as it must. */
    if (JS_IsObject(s->act) && !event_canceled(ctx, s->ev)) {
        int ar;
        DCHECK(g_run_activation != NULL, "an activation target was picked with nothing to perform");
        /* STAGE 2 IS THE RESUME POINT. The behaviour may wait on the host, and when it does the whole dispatch
           parks here — after the walk, with the event already cleaned up — and re-enters at exactly this line
           rather than replaying three legs of listeners. */
        s->stage = 2;
        ar = g_run_activation(ctx, s->act, s->ev, &s->aphase, &s->areq);
        if (ar != JS_STEP_DONE) return ar;
    }
    s->result = JS_NewBool(ctx, !event_canceled(ctx, s->ev));
    return JS_STEP_DONE;
}

static const JSTrampStepDef js_dispatch_def = {
    sizeof(JSDispatchState), js_dispatch_step, js_dispatch_fini, DISPATCH_ARG, .visit = js_dispatch_visit
};
/* §3.2.2 click(). The SAME machine — a click is a dispatch, and giving it its own would be two implementations
   of §2.9 that could disagree about listener order, the handler slot or the canceled flag. */
static const JSTrampStepDef js_click_def = {
    sizeof(JSDispatchState), js_dispatch_step, js_dispatch_fini, CLICK_SYNTH, .visit = js_dispatch_visit
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
void event_target_fire(JSContext *ctx, JSValueConst target, JSValue ev)
{
    JSValueConst argv[2];
    JSValue fn;

    if (JS_IsException(ev)) { JS_FreeValue(ctx, ev); return; }
    argv[0] = target;
    argv[1] = ev;
    fn = dispatch_fn_new(ctx);
    /* A JOB, so the dispatch runs as a call-root flow: preemptible, forkable and parkable like any other
       program, which is what every listener body needs and what a C activation cannot host. */
    JS_EnqueueCallJob(ctx, fn, 2, argv);
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
                          JSValueConst ev, JSValue in,
                          bool *pnot_canceled, JSValue **out_cb, int *out_argc)
{
    JSValueConst argv[2];
    JSValue out = JS_UNDEFINED;
    int r;

    if (*phase == 0) {
        JSValue fn = dispatch_fn_new(ctx);
        DCHECK(JS_IsObject(ev), "a synchronous fire was handed no event — §2.9 dispatches one that exists");
        argv[0] = target;
        argv[1] = ev;
        /* step_call_run DUPS the callee into the request buffer, which is what holds it across the suspension —
           so this realm's dispatcher is released here and the parked call still owns one. */
        r = step_call_run(ctx, phase, cb, cb_cap, fn, JS_UNDEFINED, 2, argv, in, &out, out_cb, out_argc);
        JS_FreeValue(ctx, fn);
        DCHECK(r == JS_STEP_CALL, "the dispatch request answered without parking");
        return r;
    }
    r = step_call_run(ctx, phase, cb, cb_cap, JS_UNDEFINED, JS_UNDEFINED, 2, NULL, in, &out, out_cb, out_argc);
    DCHECK(r == 0, "a synchronous fire resumed into something other than its answer");
    if (pnot_canceled) *pnot_canceled = JS_ToBool(ctx, out);
    JS_FreeValue(ctx, out);
    return 0;
}

static const JSTrampStepDef js_dispatch_pair_def = {
    sizeof(JSDispatchState), js_dispatch_step, js_dispatch_fini, DISPATCH_PAIR, .visit = js_dispatch_visit
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
