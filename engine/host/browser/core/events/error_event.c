/* THE ErrorEvent INTERFACE — HTML "Interface ErrorEvent".
 *
 * WHY IT EXISTS AND WHAT WAS BLOCKED ON IT. Three algorithms in this engine invoke the page's code with the
 * instruction "report the exception" and then CONTINUE: DOM §2.9's inner invoke step 2.11 (a listener that
 * throws must not stop the walk), HTML §8.12 Animation frames's animation-frame callback, and RESIZE OBSERVER §3.4.6's loop
 * error. None of them could, because "report an exception" fires an `error` event at the global carrying an
 * ErrorEvent and there was no ErrorEvent — so rendering.c carried a DFAIL naming this file, and event_target.c
 * let a throwing listener tear the whole dispatch down. One interface, three callers.
 *
 * IT IS A REAL SUBCLASS, like MessageEvent: `ErrorEvent.prototype.__proto__ === Event.prototype`, so
 * `e instanceof Event` is true and `initEvent` works on one. The base half is event_new_derived's.
 *
 * THE SLOTS ARE OWN PROPERTIES UNDER A PRIVATE SYMBOL, for the reason event.c gives: a slot written as a
 * property write is captured by the COW delta, so the event's state time-travels for free, and the symbol is a
 * brand a page cannot forge. */
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "core/idl_slots.h"
#include "core/idl_args.h"
#include "core/realm.h"
#include "core/events/event.h"
#include "core/events/error_event.h"

static JSValue g_key;        /* the private Symbol this interface's own slots hang off */
/* PER REALM, for the reason event.c states: a C member runs in the realm that DEFINED it, so one shared
   prototype answers every document out of whichever realm built it first. */
static JSClassID g_ee_class;
static int g_ready;
static int g_ctor_stepid = -1;

enum { EE_MESSAGE = 0, EE_FILENAME, EE_LINENO, EE_COLNO, EE_ERROR };
static const char *const EE_SLOT[] = { "message", "filename", "lineno", "colno", "error" };

static JSValue ee_slots(JSContext *ctx, JSValueConst ev)
{
    JSAtom k;
    JSValue slots;

    DCHECK(g_ready, "an ErrorEvent's slots were asked for before error_event_init ran");
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

static JSValue js_ee_get(JSContext *ctx, JSValueConst this_val, int magic)
{
    JSValue slots = ee_slots(ctx, this_val), v;

    DCHECK(magic >= 0 && magic < (int)(sizeof(EE_SLOT) / sizeof(EE_SLOT[0])),
           "an ErrorEvent attribute was declared with a magic the slot table does not name");
    if (!JS_IsObject(slots)) {
        JS_FreeValue(ctx, slots);
        return JS_ThrowTypeError(ctx, "an ErrorEvent attribute was read on something that is not one");
    }
    v = JS_GetPropertyStr(ctx, slots, EE_SLOT[magic]);
    JS_FreeValue(ctx, slots);
    return v;
}

/* The five own slots, placed on an event whose Event half is already built. Returns -1 with the throw live. */
static int ee_init_slots(JSContext *ctx, JSValueConst ev, JSValueConst message, JSValueConst filename,
                         uint32_t lineno, uint32_t colno, JSValueConst error)
{
    JSValue slots = idl_slots_new(ctx);
    JSAtom k = JS_ValueToAtom(ctx, g_key);

    if (JS_IsException(slots) || k == JS_ATOM_NULL) {
        JS_FreeValue(ctx, slots);
        if (k != JS_ATOM_NULL) JS_FreeAtom(ctx, k);
        return -1;
    }
    JS_SetPropertyStr(ctx, slots, "message", JS_DupValue(ctx, message));
    JS_SetPropertyStr(ctx, slots, "filename", JS_DupValue(ctx, filename));
    JS_SetPropertyStr(ctx, slots, "lineno", JS_NewUint32(ctx, lineno));
    JS_SetPropertyStr(ctx, slots, "colno", JS_NewUint32(ctx, colno));
    JS_SetPropertyStr(ctx, slots, "error", JS_DupValue(ctx, error));
    JS_SetProperty(ctx, (JSValue)ev, k, slots);
    JS_FreeAtom(ctx, k);
    return 0;
}

JSValue error_event_new(JSContext *ctx, const char *type_name, bool cancelable, JSValueConst message,
                        JSValueConst filename, uint32_t lineno, uint32_t colno, JSValueConst error)
{
    JSValue type;
    JSValue ev;

    DCHECK(type_name != NULL && *type_name,
           "an ErrorEvent was minted with no event type — the two algorithms that fire this interface name "
           "theirs (§8.1.4.6's `error`, §7.2.6.8's `navigateerror`), because the type belongs to the fire");
    type = JS_NewString(ctx, type_name);
    if (JS_IsException(type))
        return type;
    /* DOM's fire-an-event, with whatever the CALLING algorithm's flags are — see error_event.h. It does not
       bubble, it is never composed, and it IS trusted, because the user agent fired it. */
    ev = event_new_derived(ctx, error_event_proto(ctx), type, /*bubbles*/ false, cancelable,
                           /*composed*/ false, /*trusted*/ true);
    JS_FreeValue(ctx, type);
    if (JS_IsException(ev))
        return ev;
    if (ee_init_slots(ctx, ev, message, filename, lineno, colno, error) < 0) {
        JS_FreeValue(ctx, ev);
        return JS_EXCEPTION;
    }
    return ev;
}

bool error_event_is(JSContext *ctx, JSValueConst ev)
{
    JSValue slots = ee_slots(ctx, ev);
    bool is = JS_IsObject(slots);

    JS_FreeValue(ctx, slots);
    return is;
}

void error_event_handler_arguments(JSContext *ctx, JSValueConst ev, JSValue *out)
{
    JSValue slots = ee_slots(ctx, ev);
    int i;

    DCHECK(out != NULL, "§8.1.8.1 step 5's five-argument invocation was asked for with nowhere to put them");
    DCHECK(JS_IsObject(slots),
           "§8.1.8.1 step 5's five-argument invocation was asked of an event that is not an ErrorEvent — step "
           "4's `special error event handling` is the only thing that reaches this, and its first conjunct is "
           "that the event IS one, so the two have disagreed");
    for (i = 0; i < (int)(sizeof(EE_SLOT) / sizeof(EE_SLOT[0])); i++)
        out[i] = JS_GetPropertyStr(ctx, slots, EE_SLOT[i]);
    /* The five slots are placed together by ee_init_slots and by the constructor, and nothing else writes the
       record — so a HOLE here is a fifth writer nobody declared, and the handler would silently be called with
       `undefined` where the standard names an attribute value. */
    DCHECK(JS_IsString(out[EE_MESSAGE]) && JS_IsString(out[EE_FILENAME]) &&
               JS_IsNumber(out[EE_LINENO]) && JS_IsNumber(out[EE_COLNO]),
           "an ErrorEvent's slot record is missing one of the four attributes §8.1.8.1 step 5 names before "
           "`error` — the record is written in one place and read in another, and they have drifted");
    JS_FreeValue(ctx, slots);
}

/* ---- the constructor -------------------------------------------------------------------------------------- */

/* `constructor(DOMString type, optional ErrorEventInit eventInitDict = {})`. ErrorEventInit INHERITS EventInit,
   and Web IDL converts a dictionary's members with the INHERITED ones first and each level lexicographically
   among itself — which is the order this list is in, and the order a page pins by throwing from one getter. */
static const IdlArgType EE_CTOR_ARGS[2] = { IDL_DOMSTRING, IDL_DICT };
/* THE LEVEL IS THE INHERITANCE DEPTH, and it is what makes this list the spec's read order rather than one
   sorted list: Web IDL §3.2.17 reads the INHERITED dictionary's members first and each dictionary's own members
   lexicographically among THEMSELVES. `colno` sorts before `composed`, so a single sorted list would read
   ErrorEventInit's own member before EventInit's — an order a page pins by throwing from one getter. */
static const IdlDictMember EE_INIT[] = {
    { "bubbles", IDL_BOOLEAN }, { "cancelable", IDL_BOOLEAN }, { "composed", IDL_BOOLEAN },
    { "colno", IDL_UNSIGNED_LONG, false, NULL, 1 }, { "error", IDL_ANY, false, NULL, 1 },
    { "filename", IDL_USVSTRING, false, NULL, 1 }, { "lineno", IDL_UNSIGNED_LONG, false, NULL, 1 },
    { "message", IDL_DOMSTRING, false, NULL, 1 },
};

static uint32_t ee_dict_u32(JSContext *ctx, JSValueConst init, const char *name)
{
    JSValue v = idl_dict_get(ctx, init, name);
    uint32_t n = 0;

    /* The declaration has already converted the member to a NUMBER, so this runs none of the page's code; an
       absent member is the IDL's `= 0` default. */
    if (!JS_IsUndefined(v))
        JS_ToUint32(ctx, &n, v);
    JS_FreeValue(ctx, v);
    return n;
}

static JSValue js_ee_ctor(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    JSValueConst init = argc > 1 ? argv[1] : JS_UNDEFINED;
    JSValue ev, message, filename, error;

    (void)magic;
    if (JS_IsUndefined(this_val))
        return JS_ThrowTypeError(ctx, "constructor ErrorEvent requires 'new'");
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "ErrorEvent constructor requires a type");
    message  = idl_dict_get(ctx, init, "message");
    filename = idl_dict_get(ctx, init, "filename");
    error    = idl_dict_get(ctx, init, "error");
    if (JS_IsUndefined(message))  { JS_FreeValue(ctx, message);  message  = JS_NewString(ctx, ""); }
    if (JS_IsUndefined(filename)) { JS_FreeValue(ctx, filename); filename = JS_NewString(ctx, ""); }
    /* §2.2's constructor steps with THIS interface's prototype — an event the PAGE constructs is untrusted. */
    ev = event_new_derived(ctx, error_event_proto(ctx), argv[0],
                           idl_dict_bool(ctx, init, "bubbles"),
                           idl_dict_bool(ctx, init, "cancelable"),
                           idl_dict_bool(ctx, init, "composed"), /*trusted*/ false);
    if (!JS_IsException(ev) &&
        ee_init_slots(ctx, ev, message, filename,
                      ee_dict_u32(ctx, init, "lineno"), ee_dict_u32(ctx, init, "colno"), error) < 0) {
        JS_FreeValue(ctx, ev);
        ev = JS_EXCEPTION;
    }
    JS_FreeValue(ctx, message);
    JS_FreeValue(ctx, filename);
    JS_FreeValue(ctx, error);
    return ev;
}

/* ---- install ------------------------------------------------------------------------------------------------ */

static const JSCFunctionListEntry js_ee_proto[] = {
    JS_CGETSET_MAGIC_DEF("message", js_ee_get, NULL, EE_MESSAGE),
    JS_CGETSET_MAGIC_DEF("filename", js_ee_get, NULL, EE_FILENAME),
    JS_CGETSET_MAGIC_DEF("lineno", js_ee_get, NULL, EE_LINENO),
    JS_CGETSET_MAGIC_DEF("colno", js_ee_get, NULL, EE_COLNO),
    JS_CGETSET_MAGIC_DEF("error", js_ee_get, NULL, EE_ERROR),
};

void error_event_init(JSContext *ctx)
{
    JSClassDef d = { "ErrorEvent" };

    DCHECK(!g_ready, "error_event_init ran twice — the interface is declared once per AGENT");
    g_key = JS_NewSymbol(ctx, "errorEventSlots", false);
    CHECK(!JS_IsException(g_key), "the ErrorEvent slot key allocation failed");
    JS_NewClassID(JS_GetRuntime(ctx), &g_ee_class);
    JS_NewClass(JS_GetRuntime(ctx), g_ee_class, &d);
    g_ctor_stepid = idl_method_id_dict(ctx, EE_CTOR_ARGS, 2, EE_INIT,
                                       (int)(sizeof(EE_INIT) / sizeof(EE_INIT[0])), js_ee_ctor, 0);
    idl_optional_from(1);
    g_ready = 1;
    realm_declare_intrinsic(error_event_install_proto);
}

void error_event_install_proto(JSContext *ctx)
{
    JSValue proto, prev, base;

    DCHECK(g_ready, "a realm asked for ErrorEvent.prototype before error_event_init declared it");
    prev = JS_GetClassProto(ctx, g_ee_class);
    DCHECK(JS_IsNull(prev), "error_event_install_proto ran twice in one realm");
    JS_FreeValue(ctx, prev);
    base = event_proto(ctx);
    proto = JS_NewObjectProto(ctx, base);
    JS_FreeValue(ctx, base);
    CHECK(!JS_IsException(proto), "ErrorEvent.prototype could not be allocated");
    idl_interface_tag(ctx, proto, "ErrorEvent");
    JS_SetPropertyFunctionList(ctx, proto, js_ee_proto,
                               (int)(sizeof(js_ee_proto) / sizeof(js_ee_proto[0])));
    JS_SetClassProto(ctx, g_ee_class, proto);
}

void error_event_install(JSContext *ctx, JSValueConst global)
{
    JSValue ctor;

    DCHECK(g_ready, "ErrorEvent was installed before error_event_init declared it");
    ctor = idl_step_constructor(ctx, "ErrorEvent", 1, g_ctor_stepid);
    CHECK(!JS_IsException(ctor), "the ErrorEvent interface object could not be allocated");
    {
        JSValue proto = error_event_proto(ctx);
        JS_SetConstructor(ctx, ctor, proto);
        JS_FreeValue(ctx, proto);
    }
    JS_SetPropertyStr(ctx, (JSValue)global, "ErrorEvent", ctor);
}

JSValue error_event_proto(JSContext *ctx)
{
    JSValue proto = JS_GetClassProto(ctx, g_ee_class);
    DCHECK(!JS_IsNull(proto),
           "ErrorEvent.prototype was asked for in a realm that never ran error_event_install_proto");
    return proto;   /* OWNED */
}

void error_event_free(JSContext *ctx)
{
    if (!g_ready) return;
    JS_FreeValue(ctx, g_key);   /* the prototypes are the REALMS' — each is released with its context */
    g_key = JS_UNDEFINED;
    g_ready = 0;
    g_ctor_stepid = -1;
}
