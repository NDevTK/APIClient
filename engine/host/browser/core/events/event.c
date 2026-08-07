/* THE EVENT INTERFACE — DOM §2.2.
 *
 * WHAT WAS HERE BEFORE. event_target.c built the listener's argument with JS_NewObject and hung eight data
 * properties on it. That is not an Event, and three separate things followed from it. There was no `Event`
 * global, so `new Event('x')`, `new CustomEvent(...)` and every `instanceof Event` a page writes found nothing
 * — and because `Event` is on the platform-names list, reading it THREW rather than being mistaken for app
 * state, so a page that feature-tested that way stopped there. `dispatchEvent` could not exist at all, because
 * it takes an Event and there was no Event to take. And the flags were PUBLIC data properties: a page could
 * assign `ev.defaultPrevented = true` and the engine would believe it, while the spec makes it a getter over an
 * internal slot that only preventDefault() sets.
 *
 * THE SLOTS ARE OWN PROPERTIES UNDER A PRIVATE SYMBOL, the same shape abort.c uses for [[Signal]]. That is not
 * a shortcut around a C struct — it is what makes the event's state TIME-TRAVEL for free: a flag set by one
 * forked arm's listener is a property write like any other, so the COW delta captures it and the sibling arm's
 * dispatch of the same event object never sees it. A C struct behind an opaque would need its own delta kind.
 * It is also the brand: a page cannot forge the symbol, so "does it carry the slot record" IS `instanceof
 * Event` for the algorithms, and it stays true across subclassing the way a class-id check would not.
 *
 * WHAT IS ABSENT AND WHY. CustomEvent, the typed events (MouseEvent, KeyboardEvent…) and composed/shadow
 * retargeting are their own interfaces with their own state; they are honestly missing rather than approximated
 * by an Event with extra properties, and the IDL audit names them. */
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "core/idl_slots.h"
#include "quickjs-step.h"
#include "core/idl_args.h"
#include "core/events/event.h"
#include "core/timing/timer.h"

/* The private key the event's internal slots hang off — a Symbol, so a page enumerating the object cannot see
   it and cannot collide with it, for the reason the platform uses internal slots. */
static JSValue g_key;
static int     g_ready;
static JSValue g_proto;
static int     g_ctor_stepid = -1;

/* §2.2's initialised-event slots. One record, so a brand check is one read. */
static JSValue event_slots(JSContext *ctx, JSValueConst ev)
{
    JSAtom k;
    JSValue slots;

    DCHECK(g_ready, "an Event's slots were asked for before event_init ran");
    if (!JS_IsObject(ev))
        return JS_UNDEFINED;
    k = JS_ValueToAtom(ctx, g_key);
    if (k == JS_ATOM_NULL)
        return JS_UNDEFINED;
    /* AN OWN SLOT, never a property LOOKUP: a lookup on a page object walks its prototype chain and reaches the
       solver's absent-state seam, which mints a concolic for a name nobody defined. An internal slot is by
       definition an own slot, so it is read as one. */
    if (JS_GetOwnSlot(ctx, &slots, ev, k) <= 0)
        slots = JS_UNDEFINED;
    JS_FreeAtom(ctx, k);
    return slots;
}

static bool slot_flag(JSContext *ctx, JSValueConst slots, const char *name)
{
    JSValue v = JS_GetPropertyStr(ctx, slots, name);
    bool b = JS_ToBool(ctx, v);
    JS_FreeValue(ctx, v);
    return b;
}

bool event_is(JSContext *ctx, JSValueConst v)
{
    JSValue slots = event_slots(ctx, v);
    bool ok = JS_IsObject(slots);
    JS_FreeValue(ctx, slots);
    return ok;
}

JSValue event_type(JSContext *ctx, JSValueConst ev)
{
    JSValue slots = event_slots(ctx, ev), t;
    if (!JS_IsObject(slots)) { JS_FreeValue(ctx, slots); return JS_UNDEFINED; }
    t = JS_GetPropertyStr(ctx, slots, "type");
    JS_FreeValue(ctx, slots);
    return t;
}

static bool event_read_flag(JSContext *ctx, JSValueConst ev, const char *name)
{
    JSValue slots = event_slots(ctx, ev);
    bool b = JS_IsObject(slots) && slot_flag(ctx, slots, name);
    JS_FreeValue(ctx, slots);
    return b;
}

static void event_write_flag(JSContext *ctx, JSValueConst ev, const char *name, bool on)
{
    JSValue slots = event_slots(ctx, ev);
    if (JS_IsObject(slots))
        JS_SetPropertyStr(ctx, slots, name, JS_NewBool(ctx, on));
    JS_FreeValue(ctx, slots);
}

bool event_canceled(JSContext *ctx, JSValueConst ev)       { return event_read_flag(ctx, ev, "canceled"); }
/* §2.9 reads these while it walks: whether the event travels up the path at all, and whether a listener
   stopped it between targets. */
bool event_bubbles(JSContext *ctx, JSValueConst ev)        { return event_read_flag(ctx, ev, "bubbles"); }
bool event_stop_propagation(JSContext *ctx, JSValueConst ev) { return event_read_flag(ctx, ev, "stopPropagation"); }

/* §2.2 eventPhase, which the walk moves: AT_TARGET for the target itself, BUBBLING_PHASE for its ancestors. */
void event_set_phase(JSContext *ctx, JSValueConst ev, int phase)
{
    JSValue slots = event_slots(ctx, ev);
    if (!JS_IsObject(slots)) { JS_FreeValue(ctx, slots); return; }
    JS_SetPropertyStr(ctx, slots, "eventPhase", JS_NewInt32(ctx, phase));
    JS_FreeValue(ctx, slots);
}
bool event_stop_immediate(JSContext *ctx, JSValueConst ev) { return event_read_flag(ctx, ev, "stopImmediate"); }
bool event_dispatch_flag(JSContext *ctx, JSValueConst ev)  { return event_read_flag(ctx, ev, "dispatch"); }
void event_set_dispatch_flag(JSContext *ctx, JSValueConst ev, bool on)
{
    event_write_flag(ctx, ev, "dispatch", on);
}

/* §2.9 step 3: an event the PAGE dispatches is not trusted, whatever it was when it was constructed. */
void event_set_trusted(JSContext *ctx, JSValueConst ev, bool trusted)
{
    event_write_flag(ctx, ev, "isTrusted", trusted);
}

/* §2.9 "clean up": the walk is over, so there is no current target and no phase. `target` STAYS — a page reads
   it after dispatchEvent returns, which is the difference between the two. */
void event_clear_current(JSContext *ctx, JSValueConst ev)
{
    JSValue slots = event_slots(ctx, ev);
    if (!JS_IsObject(slots)) { JS_FreeValue(ctx, slots); return; }
    JS_SetPropertyStr(ctx, slots, "currentTarget", JS_NULL);
    JS_SetPropertyStr(ctx, slots, "eventPhase", JS_NewInt32(ctx, 0));
    JS_FreeValue(ctx, slots);
}

void event_set_targets(JSContext *ctx, JSValueConst ev, JSValueConst target, JSValueConst current)
{
    JSValue slots = event_slots(ctx, ev);
    if (!JS_IsObject(slots)) { JS_FreeValue(ctx, slots); return; }
    JS_SetPropertyStr(ctx, slots, "target", JS_DupValue(ctx, target));
    JS_SetPropertyStr(ctx, slots, "currentTarget", JS_DupValue(ctx, current));
    JS_FreeValue(ctx, slots);   /* the PHASE is the walk's to set — it knows which step of the path this is */
}

/* §2.2 "initialize": the slot record every Event carries. `isTrusted` is the one thing that distinguishes an
   event the ENGINE fired from one the page constructed, and it is the reason this is a parameter rather than a
   constant — a page checks it. */
static JSValue event_make(JSContext *ctx, JSValueConst type, bool bubbles, bool cancelable,
                          bool composed, bool trusted)
{
    JSValue ev, slots;
    JSAtom k;

    DCHECK(g_ready, "an Event was minted before event_init ran");
    ev = JS_NewObjectProto(ctx, g_proto);
    if (JS_IsException(ev))
        return ev;
    slots = idl_slots_new(ctx);
    k = JS_ValueToAtom(ctx, g_key);
    CHECK(!JS_IsException(slots) && k != JS_ATOM_NULL, "the Event slot record allocation failed");
    JS_SetPropertyStr(ctx, slots, "type", JS_DupValue(ctx, type));
    JS_SetPropertyStr(ctx, slots, "target", JS_NULL);
    JS_SetPropertyStr(ctx, slots, "currentTarget", JS_NULL);
    JS_SetPropertyStr(ctx, slots, "eventPhase", JS_NewInt32(ctx, 0));   /* NONE until it is dispatched */
    JS_SetPropertyStr(ctx, slots, "bubbles", JS_NewBool(ctx, bubbles));
    JS_SetPropertyStr(ctx, slots, "cancelable", JS_NewBool(ctx, cancelable));
    JS_SetPropertyStr(ctx, slots, "composed", JS_NewBool(ctx, composed));
    JS_SetPropertyStr(ctx, slots, "isTrusted", JS_NewBool(ctx, trusted));
    /* §2.2 timeStamp — the moment the event was created, on the VIRTUAL clock the timer task source orders by.
       There is no wall clock in a headless run and a second time source would disagree with the queue that runs
       the listeners, so there is one clock and this reads it. */
    JS_SetPropertyStr(ctx, slots, "timeStamp", JS_NewFloat64(ctx, timer_now()));
    JS_SetPropertyStr(ctx, slots, "canceled", JS_FALSE);
    JS_SetPropertyStr(ctx, slots, "stopPropagation", JS_FALSE);
    JS_SetPropertyStr(ctx, slots, "stopImmediate", JS_FALSE);
    JS_SetPropertyStr(ctx, slots, "dispatch", JS_FALSE);
    JS_SetProperty(ctx, ev, k, slots);
    JS_FreeAtom(ctx, k);
    return ev;
}

JSValue event_new(JSContext *ctx, const char *type, bool bubbles, bool cancelable)
{
    JSValue t = JS_NewString(ctx, type);
    JSValue ev = event_make(ctx, t, bubbles, cancelable, false, /*trusted*/ true);
    JS_FreeValue(ctx, t);
    return ev;
}

/* The same event with isTrusted FALSE — §3.2.2's synthetic click, which the spec says is untrusted because the
   page and not the user caused it, and which a page checks before acting on one. */
JSValue event_new_untrusted(JSContext *ctx, const char *type, bool bubbles, bool cancelable)
{
    JSValue t = JS_NewString(ctx, type);
    JSValue ev = event_make(ctx, t, bubbles, cancelable, false, /*trusted*/ false);
    JS_FreeValue(ctx, t);
    return ev;
}

/* §2.2 the read-only attributes, every one over a SLOT rather than over a property the page can assign.
   magic indexes SLOT_NAME. */
static const char *const SLOT_NAME[] = {
    "type", "target", "currentTarget", "eventPhase", "bubbles", "cancelable", "composed", "isTrusted",
    "timeStamp",
};

static JSValue js_event_get(JSContext *ctx, JSValueConst this_val, int magic)
{
    JSValue slots = event_slots(ctx, this_val), v;

    DCHECK(magic >= 0 && magic < (int)(sizeof(SLOT_NAME) / sizeof(SLOT_NAME[0])),
           "an Event attribute was declared with a magic the slot table does not name");
    if (!JS_IsObject(slots)) {
        JS_FreeValue(ctx, slots);
        return JS_ThrowTypeError(ctx, "an Event attribute was read on something that is not an Event");
    }
    v = JS_GetPropertyStr(ctx, slots, SLOT_NAME[magic]);
    JS_FreeValue(ctx, slots);
    return v;
}

/* §2.2 defaultPrevented is the CANCELED flag, and srcElement is target under its legacy name. */
static JSValue js_event_derived(JSContext *ctx, JSValueConst this_val, int magic)
{
    if (magic == 0) return JS_NewBool(ctx, event_canceled(ctx, this_val));
    DCHECK(magic == 1, "an Event derived attribute was declared with a magic this file does not name");
    return js_event_get(ctx, this_val, 1);   /* srcElement */
}

/* §2.2 cancelBubble — a getter AND a setter, and the setter only ever sets. `ev.cancelBubble = false` does
   nothing at all, which is the legacy behaviour the spec writes out and which a page relies on. */
static JSValue js_event_get_cancel_bubble(JSContext *ctx, JSValueConst this_val, int magic)
{
    (void)magic;
    return JS_NewBool(ctx, event_read_flag(ctx, this_val, "stopPropagation"));
}

static JSValue js_event_set_cancel_bubble(JSContext *ctx, JSValueConst this_val, JSValueConst val, int magic)
{
    (void)magic;
    if (JS_ToBool(ctx, val))
        event_write_flag(ctx, this_val, "stopPropagation", true);
    return JS_UNDEFINED;
}

/* §2.2 returnValue — the inverse legacy spelling of defaultPrevented: reading it is "not canceled", and
   assigning FALSE cancels. Assigning true does nothing, which is again what the spec writes out. */
static JSValue js_event_get_return_value(JSContext *ctx, JSValueConst this_val, int magic)
{
    (void)magic;
    return JS_NewBool(ctx, !event_canceled(ctx, this_val));
}

static JSValue js_event_set_return_value(JSContext *ctx, JSValueConst this_val, JSValueConst val, int magic)
{
    (void)magic;
    if (!JS_ToBool(ctx, val) && event_read_flag(ctx, this_val, "cancelable"))
        event_write_flag(ctx, this_val, "canceled", true);
    return JS_UNDEFINED;
}

/* §2.2 the three flag methods. Each writes its own flag, which is all the spec says they do — they were ONE
   shared no-op, and a no-op preventDefault is not a small inaccuracy: whether the default action was cancelled
   is the one thing dispatchEvent reports, so a page branching on it was reading a constant.
   magic: 0 = preventDefault, 1 = stopPropagation, 2 = stopImmediatePropagation. */
static JSValue js_event_flag(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    (void)argc; (void)argv;
    if (!event_is(ctx, this_val))
        return JS_ThrowTypeError(ctx, "an Event method was called on something that is not an Event");
    if (magic == 0) {
        /* §2.2: preventDefault does nothing unless the event is cancelable. */
        if (event_read_flag(ctx, this_val, "cancelable"))
            event_write_flag(ctx, this_val, "canceled", true);
        return JS_UNDEFINED;
    }
    event_write_flag(ctx, this_val, "stopPropagation", true);
    if (magic == 2)
        event_write_flag(ctx, this_val, "stopImmediate", true);
    return JS_UNDEFINED;
}

/* §2.2 composedPath — the path the event travelled. This engine dispatches AT THE TARGET only (there is no
   capture or bubble walk yet), so the path is the current target alone when one is set and empty otherwise,
   which is exactly what the spec's algorithm produces for that path. */
static JSValue js_event_composed_path(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    JSValue arr = JS_NewArray(ctx), cur;

    (void)argc; (void)argv;
    if (!event_is(ctx, this_val)) {
        JS_FreeValue(ctx, arr);
        return JS_ThrowTypeError(ctx, "composedPath called on something that is not an Event");
    }
    cur = js_event_get(ctx, this_val, 2);
    if (JS_IsObject(cur)) JS_SetPropertyUint32(ctx, arr, 0, cur);
    else                  JS_FreeValue(ctx, cur);
    return arr;
}

/* §2.2 initEvent(type, bubbles, cancelable) — the legacy initializer, and it is NOT a no-op: it re-initialises
   an event that is not currently being dispatched. The booleans are ToBoolean, which is total and runs none of
   the page's code, so only `type` is a coercion and the shared machine performs it. */
static JSValue js_event_init_event(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    JSValue slots;

    (void)magic;
    slots = event_slots(ctx, this_val);
    if (!JS_IsObject(slots)) {
        JS_FreeValue(ctx, slots);
        return JS_ThrowTypeError(ctx, "initEvent called on something that is not an Event");
    }
    /* §2.2 step 1: "If this's dispatch flag is set, then return." */
    if (slot_flag(ctx, slots, "dispatch")) { JS_FreeValue(ctx, slots); return JS_UNDEFINED; }
    JS_SetPropertyStr(ctx, slots, "type", argc > 0 ? JS_DupValue(ctx, argv[0]) : JS_NewString(ctx, "undefined"));
    JS_SetPropertyStr(ctx, slots, "bubbles", JS_NewBool(ctx, argc > 1 && JS_ToBool(ctx, argv[1])));
    JS_SetPropertyStr(ctx, slots, "cancelable", JS_NewBool(ctx, argc > 2 && JS_ToBool(ctx, argv[2])));
    /* the spec's own re-init: the flags and the target go back to their initial state. */
    JS_SetPropertyStr(ctx, slots, "canceled", JS_FALSE);
    JS_SetPropertyStr(ctx, slots, "stopPropagation", JS_FALSE);
    JS_SetPropertyStr(ctx, slots, "stopImmediate", JS_FALSE);
    JS_SetPropertyStr(ctx, slots, "target", JS_NULL);
    JS_FreeValue(ctx, slots);
    return JS_UNDEFINED;
}

/* THE CONSTRUCTOR — `new Event(type, optional EventInit eventInit = {})`. A step machine because BOTH of its
   arguments are the page's code: `type` is a DOMString (ToString), and EventInit is three property reads that
   an accessor or a Proxy trap turns into a call. It declares itself through the shared IDL machine rather than
   hand-rolling either, which is why there is no coercion code here at all — by the time this body runs, `type`
   is a real string and the dictionary is a plain object the engine built. */
static const IdlArgType EVENT_CTOR_ARGS[2] = { IDL_DOMSTRING, IDL_DICT };
static const IdlDictMember EVENT_INIT[] = {   /* EventInit, in the order the IDL declares it */
    { "bubbles", IDL_BOOLEAN }, { "cancelable", IDL_BOOLEAN }, { "composed", IDL_BOOLEAN },
};

static JSValue js_event_ctor(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    JSValueConst init = argc > 1 ? argv[1] : JS_UNDEFINED;

    (void)magic;
    /* JS_CFUNC_step_ctor delivers NEW_TARGET in the receiver slot and undefined for a plain call, which is how
       `Event('x')` is told apart from `new Event('x')` — the IDL declares a constructor, so the former throws. */
    if (JS_IsUndefined(this_val))
        return JS_ThrowTypeError(ctx, "constructor Event requires 'new'");
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "Event constructor requires a type");
    /* §2.2: an event the PAGE constructs is not trusted, which is the whole point of the flag. */
    return event_make(ctx, argv[0],
                      idl_dict_bool(ctx, init, "bubbles"),
                      idl_dict_bool(ctx, init, "cancelable"),
                      idl_dict_bool(ctx, init, "composed"),
                      /*trusted*/ false);
}

static const JSCFunctionListEntry js_event_proto[] = {
    JS_CGETSET_MAGIC_DEF("type", js_event_get, NULL, 0),
    JS_CGETSET_MAGIC_DEF("target", js_event_get, NULL, 1),
    JS_CGETSET_MAGIC_DEF("currentTarget", js_event_get, NULL, 2),
    JS_CGETSET_MAGIC_DEF("eventPhase", js_event_get, NULL, 3),
    JS_CGETSET_MAGIC_DEF("bubbles", js_event_get, NULL, 4),
    JS_CGETSET_MAGIC_DEF("cancelable", js_event_get, NULL, 5),
    JS_CGETSET_MAGIC_DEF("composed", js_event_get, NULL, 6),
    JS_CGETSET_MAGIC_DEF("isTrusted", js_event_get, NULL, 7),
    JS_CGETSET_MAGIC_DEF("timeStamp", js_event_get, NULL, 8),
    JS_CGETSET_MAGIC_DEF("defaultPrevented", js_event_derived, NULL, 0),
    JS_CGETSET_MAGIC_DEF("srcElement", js_event_derived, NULL, 1),
    JS_CFUNC_MAGIC_DEF("preventDefault", 0, js_event_flag, 0),
    JS_CFUNC_MAGIC_DEF("stopPropagation", 0, js_event_flag, 1),
    JS_CFUNC_MAGIC_DEF("stopImmediatePropagation", 0, js_event_flag, 2),
    JS_CFUNC_DEF("composedPath", 0, js_event_composed_path),
};

/* §2.2 the phase constants. Web IDL puts a `const` on the prototype AND the interface object, so one table
   installs both — reached by NAME rather than by slicing the tail off the member list, which is a silent break
   the day a member is appended. */
static const JSCFunctionListEntry js_event_consts[] = {
    JS_PROP_INT32_DEF("NONE", 0, 0),
    JS_PROP_INT32_DEF("CAPTURING_PHASE", 1, 0),
    JS_PROP_INT32_DEF("AT_TARGET", 2, 0),
    JS_PROP_INT32_DEF("BUBBLING_PHASE", 3, 0),
};

void event_init(JSContext *ctx)
{
    DCHECK(!g_ready, "event_init ran twice — one instance is one document");
    g_key = JS_NewSymbol(ctx, "eventSlots", false);
    CHECK(!JS_IsException(g_key), "the Event slot key allocation failed");
    g_proto = JS_NewObject(ctx);
    CHECK(!JS_IsException(g_proto), "Event.prototype could not be allocated");
    idl_interface_tag(ctx, g_proto, "Event");
    g_ready = 1;
    JS_SetPropertyFunctionList(ctx, g_proto, js_event_proto,
                               (int)(sizeof(js_event_proto) / sizeof(js_event_proto[0])));
    JS_SetPropertyFunctionList(ctx, g_proto, js_event_consts,
                               (int)(sizeof(js_event_consts) / sizeof(js_event_consts[0])));
    /* cancelBubble and returnValue are the two LEGACY attributes with setters, and each setter only ever sets
       — which is the behaviour that makes them legacy and the reason they are not aliases. */
    idl_install_accessor(ctx, g_proto, "cancelBubble", js_event_get_cancel_bubble, 0,
                         idl_setter_id(ctx, IDL_ANY, false, js_event_set_cancel_bubble, 0));
    idl_install_accessor(ctx, g_proto, "returnValue", js_event_get_return_value, 0,
                         idl_setter_id(ctx, IDL_ANY, false, js_event_set_return_value, 0));
    {
        static const IdlArgType INIT_ARGS[3] = { IDL_DOMSTRING, IDL_ANY, IDL_ANY };
        idl_install_method(ctx, g_proto, "initEvent", 3,
                           idl_method_id(ctx, INIT_ARGS, 3, js_event_init_event, 0));
        idl_optional_from(1);   /* §2.2: `initEvent(type, optional bubbles, optional cancelable)` */
    }
    g_ctor_stepid = idl_method_id_dict(ctx, EVENT_CTOR_ARGS, 2, EVENT_INIT,
                                      (int)(sizeof(EVENT_INIT) / sizeof(EVENT_INIT[0])),
                                      js_event_ctor, 0);
    idl_optional_from(1);   /* §2.2: `constructor(DOMString type, optional EventInit eventInitDict = {})` */
}

JSValue event_proto(void)
{
    DCHECK(g_ready, "Event.prototype was asked for before event_init built it");
    return g_proto;
}

void event_install(JSContext *ctx, JSValueConst global)
{
    JSValue ctor;

    DCHECK(g_ready, "Event was installed before event_init built its prototype");
    /* A step machine that is also a CONSTRUCTOR: `new Event(type, init)` converts two page-reachable arguments
       before the body runs, and JS_CFUNC_step_ctor is what makes the declaration usable with `new`. */
    ctor = idl_step_constructor(ctx, "Event", 2, g_ctor_stepid);
    CHECK(!JS_IsException(ctor), "the Event interface object could not be allocated");
    JS_SetConstructor(ctx, ctor, g_proto);
    JS_SetPropertyFunctionList(ctx, ctor, js_event_consts,
                               (int)(sizeof(js_event_consts) / sizeof(js_event_consts[0])));
    JS_SetPropertyStr(ctx, (JSValue)global, "Event", ctor);
}

void event_free(JSContext *ctx)
{
    if (!g_ready)
        return;
    JS_FreeValue(ctx, g_key);
    JS_FreeValue(ctx, g_proto);
    g_key = g_proto = JS_UNDEFINED;
    g_ready = 0;
    g_ctor_stepid = -1;
}
