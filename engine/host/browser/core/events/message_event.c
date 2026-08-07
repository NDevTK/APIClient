/* THE MessageEvent INTERFACE — HTML §9.4.1.
 *
 * WHY IT IS FIRST. Everything in HTML 9.4 dispatches one of these: a MessagePort delivers its message as a
 * MessageEvent, `window.postMessage` queues a task that fires one, a BroadcastChannel fans one out, a
 * WebSocket and an EventSource each deliver theirs the same way. So it is the piece the rest are written in
 * terms of, and it is the piece that has to exist before any of them can be built.
 *
 * IT IS A REAL SUBCLASS, not an Event with five extra properties hung on it. `interface MessageEvent : Event`
 * is a prototype chain a page walks — `ev instanceof Event` is true, `MessageEvent.prototype.__proto__ ===
 * Event.prototype`, and `initEvent` works on one. That is why event.h grew event_new_derived rather than this
 * file minting a plain object: the constructor runs Event's constructor steps with THIS interface's prototype,
 * then its own.
 *
 * THE SLOTS ARE OWN PROPERTIES UNDER A PRIVATE SYMBOL, for the reason event.c gives at length: a slot written
 * as a property write is captured by the COW delta, so an event's state time-travels for free, and the symbol
 * is a brand a page cannot forge.
 *
 * TWO MEMBERS NAME TYPES THAT DO NOT EXIST YET, and they are HONEST about it rather than untyped. `source` is
 * a `MessageEventSource?` — a WindowProxy, a MessagePort or a ServiceWorker — and `ports` is a
 * `sequence<MessagePort>`. None of those three interfaces is built, so the only value either member can take
 * is its empty one, and anything else is the TypeError Web IDL's own conversion would raise. That is not a
 * restriction this file invents: it is what the conversion DOES when nothing satisfies the type. When §9.4.2's
 * MessagePort lands, the brand test in message_source_ok/ports_from_sequence is where it plugs in, and the
 * TypeError stops being reachable for a real port. */
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "core/idl_slots.h"
#include "core/idl_args.h"
#include "core/events/event.h"
#include "core/events/message_event.h"

static JSValue g_key;        /* the private Symbol §9.4.1's own slots hang off */
static JSValue g_proto = JS_UNDEFINED;
static int     g_ready;
static int     g_ctor_stepid = -1;

/* §9.4.1's own slots — the five attributes Event does not have. One record, read as an OWN slot for the reason
   event.c states: a property LOOKUP walks the prototype chain into the solver's absent-state seam. */
static JSValue me_slots(JSContext *ctx, JSValueConst ev)
{
    JSAtom k;
    JSValue slots;

    DCHECK(g_ready, "a MessageEvent's slots were asked for before message_event_init ran");
    if (!JS_IsObject(ev))
        return JS_UNDEFINED;
    k = JS_ValueToAtom(ctx, g_key);
    if (k == JS_ATOM_NULL)
        return JS_UNDEFINED;
    if (JS_GetOwnSlot(ctx, &slots, ev, k) <= 0)
        slots = JS_UNDEFINED;
    JS_FreeAtom(ctx, k);
    return slots;
}

bool message_event_is(JSContext *ctx, JSValueConst v)
{
    JSValue slots = me_slots(ctx, v);
    bool ok = JS_IsObject(slots);
    JS_FreeValue(ctx, slots);
    return ok;
}

/* ---- the two members whose types are not built yet ---------------------------------------------------------- */

/* `MessageEventSource?` = (WindowProxy or MessagePort or ServiceWorker). Null is the only inhabitant this
   engine can produce, so anything else is the union's TypeError — the same answer Web IDL gives for a value
   that matches no arm. Returns 0 with the throw live. */
static int message_source_ok(JSContext *ctx, JSValueConst v)
{
    if (JS_IsNull(v) || JS_IsUndefined(v))
        return 1;
    JS_ThrowTypeError(ctx, "a MessageEvent's `source` must be a WindowProxy, a MessagePort or a ServiceWorker "
                           "— this engine has none of the three, so only null is a value it can take");
    return 0;
}

/* `sequence<MessagePort>` → the FrozenArray the attribute answers. §9.4.1's `ports` is frozen because a page
   must not be able to add a port to an event it received, and Web IDL's FrozenArray is how the standard says
   so — an ordinary array here would be a different type that happens to look the same until a page writes to
   it. An empty sequence is the only one that converts today; see the file comment. Returns JS_EXCEPTION. */
static JSValue ports_from_sequence(JSContext *ctx, JSValueConst v)
{
    JSValue arr;
    int64_t n = 0;

    if (JS_IsUndefined(v) || JS_IsNull(v)) {
        n = 0;
    } else {
        /* A sequence is read through its LENGTH and indices; the read itself is the page's code only if the
           value is an exotic object, and the declaration has already converted the argument for the
           constructor. `ports` reaching here as a non-object at all is the TypeError the sequence type gives. */
        JSValue len;
        if (!JS_IsObject(v))
            return JS_ThrowTypeError(ctx, "a MessageEvent's `ports` must be a sequence");
        len = JS_GetPropertyStr(ctx, v, "length");
        if (JS_IsException(len))
            return len;
        if (JS_ToInt64(ctx, &n, len) < 0) { JS_FreeValue(ctx, len); return JS_EXCEPTION; }
        JS_FreeValue(ctx, len);
    }
    if (n != 0)
        return JS_ThrowTypeError(ctx, "a MessageEvent's `ports` must contain MessagePorts — this engine has no "
                                      "MessagePort, so only an empty sequence is a value it can take");
    arr = JS_NewArray(ctx);
    if (JS_IsException(arr))
        return arr;
    /* FROZEN, per the FrozenArray type — and an ARRAY is not frozen by preventing extensions, which is the
       trap this hit. `Object.isFrozen([])` was FALSE afterwards, because an array always carries an own
       `length` and `length` is writable; SetIntegrityLevel("frozen") makes every own property non-writable and
       `length` is one. So both steps run. (When §9.4.2's MessagePort makes a non-empty sequence reachable, each
       index needs the same treatment, and this is the loop that grows.) */
    {
        JSAtom len_atom = JS_NewAtom(ctx, "length");
        int r;
        if (len_atom == JS_ATOM_NULL) { JS_FreeValue(ctx, arr); return JS_EXCEPTION; }
        r = JS_DefineProperty(ctx, arr, len_atom, JS_UNDEFINED, JS_UNDEFINED, JS_UNDEFINED,
                              JS_PROP_HAS_WRITABLE);   /* writable BIT clear = non-writable */
        JS_FreeAtom(ctx, len_atom);
        if (r < 0 || JS_PreventExtensions(ctx, arr) < 0) { JS_FreeValue(ctx, arr); return JS_EXCEPTION; }
    }
    return arr;
}

/* ---- initialise ------------------------------------------------------------------------------------------- */

/* §9.4.1's own initialisation, over an object Event's steps have already built. `ports` is already the frozen
   array; every other value is stored as it arrived. */
static int me_init_slots(JSContext *ctx, JSValueConst ev, JSValueConst data, JSValueConst origin,
                         JSValueConst last_id, JSValueConst source, JSValue ports)
{
    JSValue slots = idl_slots_new(ctx);
    JSAtom k = JS_ValueToAtom(ctx, g_key);

    if (JS_IsException(slots) || k == JS_ATOM_NULL) {
        JS_FreeValue(ctx, slots);
        JS_FreeValue(ctx, ports);
        return -1;
    }
    JS_SetPropertyStr(ctx, slots, "data", JS_DupValue(ctx, data));
    JS_SetPropertyStr(ctx, slots, "origin", JS_DupValue(ctx, origin));
    JS_SetPropertyStr(ctx, slots, "lastEventId", JS_DupValue(ctx, last_id));
    JS_SetPropertyStr(ctx, slots, "source", JS_IsUndefined(source) ? JS_NULL : JS_DupValue(ctx, source));
    JS_SetPropertyStr(ctx, slots, "ports", ports);
    JS_SetProperty(ctx, (JSValue)ev, k, slots);
    JS_FreeAtom(ctx, k);
    return 0;
}

JSValue message_event_new(JSContext *ctx, const char *type, JSValueConst data, const char *origin,
                          JSValueConst source)
{
    JSValue t, ev, ports, empty;

    DCHECK(g_ready, "a MessageEvent was minted before message_event_init ran");
    t = JS_NewString(ctx, type);
    if (JS_IsException(t)) return t;
    /* §9.4.1's events do not bubble and are not cancelable — the standard fires them with neither flag, and a
       page's `stopPropagation` on one has nothing to stop. isTrusted is TRUE: the engine fired it. */
    ev = event_new_derived(ctx, g_proto, t, false, false, false, /*trusted*/ true);
    JS_FreeValue(ctx, t);
    if (JS_IsException(ev)) return ev;
    ports = ports_from_sequence(ctx, JS_UNDEFINED);
    if (JS_IsException(ports)) { JS_FreeValue(ctx, ev); return JS_EXCEPTION; }
    empty = JS_NewString(ctx, "");
    {
        JSValue o = JS_NewString(ctx, origin ? origin : "");
        int r = me_init_slots(ctx, ev, data, o, empty, source, ports);
        JS_FreeValue(ctx, o);
        JS_FreeValue(ctx, empty);
        if (r < 0) { JS_FreeValue(ctx, ev); return JS_EXCEPTION; }
    }
    return ev;
}

/* ---- the attributes ---------------------------------------------------------------------------------------- */

enum { ME_DATA = 0, ME_ORIGIN, ME_LAST_ID, ME_SOURCE, ME_PORTS, ME_N };
static const char *const ME_NAMES[ME_N] = { "data", "origin", "lastEventId", "source", "ports" };

static JSValue js_me_get(JSContext *ctx, JSValueConst this_val, int magic)
{
    JSValue slots = me_slots(ctx, this_val), v;

    if (!JS_IsObject(slots)) {
        JS_FreeValue(ctx, slots);
        return JS_ThrowTypeError(ctx, "not a MessageEvent");
    }
    DCHECK(magic >= 0 && magic < ME_N, "a MessageEvent attribute was declared with a magic this component "
                                       "does not answer");
    v = JS_GetPropertyStr(ctx, slots, ME_NAMES[magic]);
    JS_FreeValue(ctx, slots);
    return v;
}

/* §9.4.1's `initMessageEvent`, the legacy initialiser every Event subclass with a history has. It is not a
   second constructor: it RE-INITIALISES an event that already exists, base half and own half, which is why it
   reaches initEvent's own steps through the base slots rather than rebuilding the object. */
static JSValue js_me_init(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    JSValue slots, ports;

    (void)magic;
    if (!message_event_is(ctx, this_val))
        return JS_ThrowTypeError(ctx, "not a MessageEvent");
    if (argc > 6 && !message_source_ok(ctx, argv[6]))
        return JS_EXCEPTION;
    ports = ports_from_sequence(ctx, argc > 7 ? argv[7] : JS_UNDEFINED);
    if (JS_IsException(ports))
        return JS_EXCEPTION;
    /* The BASE half first, exactly as the constructor orders it. */
    if (!event_reinit(ctx, this_val, argc > 0 ? argv[0] : JS_UNDEFINED,
                      argc > 1 && JS_ToBool(ctx, argv[1]), argc > 2 && JS_ToBool(ctx, argv[2]))) {
        JS_FreeValue(ctx, ports);
        return JS_UNDEFINED;   /* §2.2: an event being dispatched right now is not re-initialised */
    }
    slots = me_slots(ctx, this_val);
    JS_SetPropertyStr(ctx, slots, "data", argc > 3 ? JS_DupValue(ctx, argv[3]) : JS_NULL);
    JS_SetPropertyStr(ctx, slots, "origin", argc > 4 ? JS_DupValue(ctx, argv[4]) : JS_NewString(ctx, ""));
    JS_SetPropertyStr(ctx, slots, "lastEventId", argc > 5 ? JS_DupValue(ctx, argv[5]) : JS_NewString(ctx, ""));
    JS_SetPropertyStr(ctx, slots, "source",
                      argc > 6 && !JS_IsUndefined(argv[6]) ? JS_DupValue(ctx, argv[6]) : JS_NULL);
    JS_SetPropertyStr(ctx, slots, "ports", ports);
    JS_FreeValue(ctx, slots);
    return JS_UNDEFINED;
}

/* ---- the constructor -------------------------------------------------------------------------------------- */

/* `constructor(DOMString type, optional MessageEventInit eventInitDict = {})`. MessageEventInit INHERITS
   EventInit, and Web IDL converts a dictionary's members in lexicographic order with the INHERITED ones first
   — which is the order this list is in, and the order a page pins by throwing from one getter and counting
   which others ran. */
static const IdlArgType ME_CTOR_ARGS[2] = { IDL_DOMSTRING, IDL_DICT };
static const IdlDictMember ME_INIT[] = {
    { "bubbles", IDL_BOOLEAN }, { "cancelable", IDL_BOOLEAN }, { "composed", IDL_BOOLEAN },
    { "data", IDL_ANY }, { "lastEventId", IDL_DOMSTRING }, { "origin", IDL_USVSTRING },
    { "ports", IDL_ANY }, { "source", IDL_ANY },
};

static JSValue js_me_ctor(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    JSValueConst init = argc > 1 ? argv[1] : JS_UNDEFINED;
    JSValue ev, ports, data, origin, last_id, source;

    (void)magic;
    if (JS_IsUndefined(this_val))
        return JS_ThrowTypeError(ctx, "constructor MessageEvent requires 'new'");
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "MessageEvent constructor requires a type");

    /* THE DICTIONARY MAY NOT BE THERE AT ALL. `new MessageEvent('m')` passes no second argument, and §3.2.19
       says a dictionary with no required members converts from undefined to one where every member is ABSENT —
       so the reads are guarded rather than performed on undefined, which throws. Reading them anyway is how
       the plain one-argument construction — the shape every real dispatch uses — became a TypeError.
       When the dictionary IS there, the declaration has already converted every member, so these five reads
       are of a plain object the engine built: none of the page's code runs here, which is why the constructor
       is not a step machine. */
    if (JS_IsObject(init)) {
        data    = JS_GetPropertyStr(ctx, init, "data");
        origin  = JS_GetPropertyStr(ctx, init, "origin");
        last_id = JS_GetPropertyStr(ctx, init, "lastEventId");
        source  = JS_GetPropertyStr(ctx, init, "source");
        ports   = JS_GetPropertyStr(ctx, init, "ports");
    } else {
        data = origin = last_id = source = ports = JS_UNDEFINED;
    }
    if (!message_source_ok(ctx, source)) {
        ev = JS_EXCEPTION;
    } else {
        JSValue frozen = ports_from_sequence(ctx, ports);
        if (JS_IsException(frozen)) {
            ev = JS_EXCEPTION;
        } else {
            /* §2.2's constructor steps, with THIS interface's prototype — the base half of the subclass. An
               event the PAGE constructs is untrusted, which is the whole point of the flag. */
            ev = event_new_derived(ctx, g_proto, argv[0],
                                   idl_dict_bool(ctx, init, "bubbles"),
                                   idl_dict_bool(ctx, init, "cancelable"),
                                   idl_dict_bool(ctx, init, "composed"), /*trusted*/ false);
            if (JS_IsException(ev)) {
                JS_FreeValue(ctx, frozen);
            } else {
                JSValue d = JS_IsUndefined(data) ? JS_NULL : data;          /* the dictionary's defaults */
                JSValue o = JS_IsUndefined(origin) ? JS_NewString(ctx, "") : JS_DupValue(ctx, origin);
                JSValue l = JS_IsUndefined(last_id) ? JS_NewString(ctx, "") : JS_DupValue(ctx, last_id);
                if (me_init_slots(ctx, ev, d, o, l, source, frozen) < 0) {
                    JS_FreeValue(ctx, ev);
                    ev = JS_EXCEPTION;
                }
                JS_FreeValue(ctx, o);
                JS_FreeValue(ctx, l);
            }
        }
    }
    JS_FreeValue(ctx, data);
    JS_FreeValue(ctx, origin);
    JS_FreeValue(ctx, last_id);
    JS_FreeValue(ctx, source);
    JS_FreeValue(ctx, ports);
    return ev;
}

/* ---- install ------------------------------------------------------------------------------------------------ */

static const JSCFunctionListEntry js_me_proto[] = {
    JS_CGETSET_MAGIC_DEF("data", js_me_get, NULL, ME_DATA),
    JS_CGETSET_MAGIC_DEF("origin", js_me_get, NULL, ME_ORIGIN),
    JS_CGETSET_MAGIC_DEF("lastEventId", js_me_get, NULL, ME_LAST_ID),
    JS_CGETSET_MAGIC_DEF("source", js_me_get, NULL, ME_SOURCE),
    JS_CGETSET_MAGIC_DEF("ports", js_me_get, NULL, ME_PORTS),
};

void message_event_init(JSContext *ctx)
{
    DCHECK(!g_ready, "message_event_init ran twice — one instance is one document");
    g_key = JS_NewSymbol(ctx, "messageEventSlots", false);
    CHECK(!JS_IsException(g_key), "the MessageEvent slot key allocation failed");
    /* THE PROTOTYPE CHAIN IS THE SUBCLASSING. `MessageEvent.prototype.__proto__ === Event.prototype` is what a
       page walks and what `instanceof Event` reads; a flat prototype would answer false for both. */
    g_proto = JS_NewObjectProto(ctx, event_proto());
    CHECK(!JS_IsException(g_proto), "MessageEvent.prototype could not be allocated");
    idl_interface_tag(ctx, g_proto, "MessageEvent");
    g_ready = 1;
    JS_SetPropertyFunctionList(ctx, g_proto, js_me_proto,
                               (int)(sizeof(js_me_proto) / sizeof(js_me_proto[0])));
    {
        /* `initMessageEvent(type, bubbles, cancelable, data, origin, lastEventId, source, ports)` — eight
           arguments, seven of them optional, each converted by the declaration. */
        static const IdlArgType INIT_ARGS[8] = {
            IDL_DOMSTRING, IDL_BOOLEAN, IDL_BOOLEAN, IDL_ANY,
            IDL_USVSTRING, IDL_DOMSTRING, IDL_ANY, IDL_ANY,
        };
        idl_install_method(ctx, g_proto, "initMessageEvent", 1,
                           idl_method_id(ctx, INIT_ARGS, 8, js_me_init, 0));
        idl_optional_from(1);
    }
    g_ctor_stepid = idl_method_id_dict(ctx, ME_CTOR_ARGS, 2, ME_INIT,
                                       (int)(sizeof(ME_INIT) / sizeof(ME_INIT[0])), js_me_ctor, 0);
    idl_optional_from(1);   /* §9.4.1: `constructor(DOMString type, optional MessageEventInit init = {})` */
}

void message_event_install(JSContext *ctx, JSValueConst global)
{
    JSValue ctor;

    DCHECK(g_ready, "MessageEvent was installed before message_event_init built its prototype");
    ctor = idl_step_constructor(ctx, "MessageEvent", 1, g_ctor_stepid);
    CHECK(!JS_IsException(ctor), "the MessageEvent interface object could not be allocated");
    JS_SetConstructor(ctx, ctor, g_proto);
    JS_SetPropertyStr(ctx, (JSValue)global, "MessageEvent", ctor);
}

JSValue message_event_proto(void)
{
    DCHECK(g_ready, "MessageEvent.prototype was asked for before message_event_init built it");
    return g_proto;
}

void message_event_free(JSContext *ctx)
{
    if (!g_ready) return;
    JS_FreeValue(ctx, g_key);
    JS_FreeValue(ctx, g_proto);
    g_key = JS_UNDEFINED;
    g_proto = JS_UNDEFINED;
    g_ready = 0;
    g_ctor_stepid = -1;
}
