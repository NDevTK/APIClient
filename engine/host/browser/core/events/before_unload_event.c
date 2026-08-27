/* THE BeforeUnloadEvent INTERFACE — HTML §7.2.7.7.
 *
 *     [Exposed=Window]
 *     interface BeforeUnloadEvent : Event {
 *       attribute DOMString returnValue;
 *     };
 *
 * WHY IT EXISTS AND WHAT WAS BLOCKED ON IT. `onbeforeunload` is already installed as an event handler IDL
 * attribute, so a bundle can register for it and NOTHING could ever fire at it: §7.5.9's "fire beforeunload"
 * fires the event "using BeforeUnloadEvent", which did not exist. The handler behind that name is where an app
 * with unsaved state posts its last beacon and asks the user to stay — a whole branch that never ran.
 *
 * IT HAS NO CONSTRUCTOR, and that is the IDL rather than an omission: "There are no BeforeUnloadEvent-specific
 * initialization methods." So the interface object is idl_args' non-constructible one — `new
 * BeforeUnloadEvent()` is a TypeError, and DOM §4.5's `createEvent('BeforeUnloadEvent')` row (the first in its
 * table) is how a page makes one. A NULL C function pointer is not "no constructor", it is a crash where the
 * spec says TypeError.
 *
 * `returnValue` IS A LEGACY MEMBER WITH REAL DEFINED BEHAVIOUR, not a compatibility no-op: §7.2.7.7 says the
 * interface "allows checking if unloading is canceled to be controlled not only by canceling the event, but by
 * setting the returnValue attribute to a value besides the empty string", and §7.5.9's condition reads
 * "eventFiringResult is false, OR the returnValue attribute of event is not the empty string". So a handler
 * whose entire body is `e.returnValue = 'unsaved'` — which is how the overwhelming majority of shipped code
 * spells it — cancels the unload without ever touching preventDefault. Storing that string and never reading
 * it would be the stub §NO STUBS names: the attribute would answer correctly and the DECISION would be wrong.
 * The attribute itself is exactly as stated: set to the empty string when the event is created, returns the
 * last value it was set to, and setting sets it. It is a DOMString "only for historical reasons" — any
 * non-empty value means the same thing, and the actual text is ignored when a real UA shows its prompt.
 *
 * IT IS A REAL SUBCLASS: its prototype chains to the realm's `Event.prototype`, so `e instanceof Event` holds.
 * That also means its own `returnValue` SHADOWS DOM §2.2's legacy `Event.returnValue` (the inverse spelling of
 * defaultPrevented) — which is the spec's own arrangement, and the reason the two must not be confused: on
 * Event, `returnValue = false` sets the canceled flag; on this interface, the attribute is a string with no
 * effect on that flag at all, and the cancel decision reads BOTH.
 *
 * THE SLOT IS AN OWN PROPERTY UNDER A PRIVATE SYMBOL, for the reason event.c gives: a slot written as a
 * property write is captured by the COW delta, so a `returnValue` one flow's handler set is not seen by the
 * flow exploring the other arm — and the symbol is a brand a page cannot forge. */
#include <stddef.h>

#include "check.h"
#include "quickjs.h"
#include "core/events/before_unload_event.h"
#include "core/events/event.h"
#include "core/idl_args.h"
#include "core/idl_slots.h"
#include "core/realm.h"

static JSValue   g_key;         /* the private Symbol this interface's own slot hangs off */
static JSClassID g_bue_class;   /* the class exists for its per-REALM prototype slot; nothing wears it */
static int       g_ready;
static int       g_id_return_value_set = -1;

/* This event's own slot record, or JS_UNDEFINED for anything that is not one of these. */
static JSValue bue_slots(JSContext *ctx, JSValueConst ev)
{
    JSAtom k;
    JSValue slots;

    DCHECK(g_ready, "a BeforeUnloadEvent's slots were asked for before before_unload_event_init ran");
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

/* §7.2.7.7: "On getting, it must return the last value it was set to." */
static JSValue js_bue_get_return_value(JSContext *ctx, JSValueConst this_val, int magic)
{
    JSValue slots = bue_slots(ctx, this_val), v;

    (void)magic;
    if (!JS_IsObject(slots)) {
        JS_FreeValue(ctx, slots);
        return JS_ThrowTypeError(ctx, "a BeforeUnloadEvent attribute was read on something that is not one");
    }
    v = JS_GetPropertyStr(ctx, slots, "returnValue");
    JS_FreeValue(ctx, slots);
    return v;
}

/* §7.2.7.7: "On setting, the attribute must be set to the new value." The value arrives already converted to a
   DOMString by the declaration, so the page's toString has already run and nothing here runs it a second
   time. There is no [LegacyNullToEmptyString] on this attribute — `e.returnValue = null` is the string
   "null", which is not the empty string and therefore ASKS TO CANCEL, exactly as in a real browser. */
static JSValue js_bue_set_return_value(JSContext *ctx, JSValueConst this_val, JSValueConst val, int magic)
{
    JSValue slots = bue_slots(ctx, this_val);

    (void)magic;
    if (!JS_IsObject(slots)) {
        JS_FreeValue(ctx, slots);
        return JS_ThrowTypeError(ctx, "a BeforeUnloadEvent attribute was set on something that is not one");
    }
    DCHECK(JS_IsString(val), "the BeforeUnloadEvent returnValue setter was handed something that is not a "
                             "string — the DOMString conversion is the DECLARATION's and a body that coerces "
                             "one runs the page's toString from C");
    JS_SetPropertyStr(ctx, slots, "returnValue", JS_DupValue(ctx, val));
    JS_FreeValue(ctx, slots);
    return JS_UNDEFINED;
}

/* The one own slot, placed on an event whose Event half is already built. Returns -1 with the throw live. */
static int bue_init_slot(JSContext *ctx, JSValueConst ev)
{
    JSValue slots = idl_slots_new(ctx);
    JSAtom k = JS_ValueToAtom(ctx, g_key);
    JSValue empty;

    if (JS_IsException(slots) || k == JS_ATOM_NULL) {
        JS_FreeValue(ctx, slots);
        if (k != JS_ATOM_NULL) JS_FreeAtom(ctx, k);
        return -1;
    }
    /* "When the event is created, the attribute must be set to the empty string." */
    empty = JS_NewString(ctx, "");
    if (JS_IsException(empty)) {
        JS_FreeValue(ctx, slots);
        JS_FreeAtom(ctx, k);
        return -1;
    }
    JS_SetPropertyStr(ctx, slots, "returnValue", empty);
    JS_SetProperty(ctx, (JSValue)ev, k, slots);
    JS_FreeAtom(ctx, k);
    return 0;
}

JSValue before_unload_event_new(JSContext *ctx)
{
    JSValue tv, ev, proto;

    DCHECK(g_ready, "a beforeunload event was fired before before_unload_event_init declared the interface — "
                    "HTML §7.5.9 fires it USING BeforeUnloadEvent, so the interface has to exist before the "
                    "unload machine can");
    proto = JS_GetClassProto(ctx, g_bue_class);
    DCHECK(!JS_IsNull(proto),
           "BeforeUnloadEvent.prototype was asked for in a realm that never ran its per-realm install");
    tv = JS_NewString(ctx, "beforeunload");
    if (JS_IsException(tv)) {
        JS_FreeValue(ctx, proto);
        return tv;
    }
    /* §7.5.9 step 4 names ONE initialiser: `cancelable` true. It does not bubble, it is not composed, and it
       IS trusted — the user agent fired it. */
    ev = event_new_derived(ctx, proto, tv, /*bubbles*/ false, /*cancelable*/ true,
                           /*composed*/ false, /*trusted*/ true);
    JS_FreeValue(ctx, tv);
    if (JS_IsException(ev))
        return ev;
    if (bue_init_slot(ctx, ev) < 0) {
        JS_FreeValue(ctx, ev);
        return JS_EXCEPTION;
    }
    return ev;
}

bool before_unload_event_is(JSContext *ctx, JSValueConst ev)
{
    JSValue slots = bue_slots(ctx, ev);
    bool is = JS_IsObject(slots);

    JS_FreeValue(ctx, slots);
    return is;
}

/* "the returnValue attribute of event is not the empty string", asked the way both of its callers need it.
   BYTE LENGTH, not truthiness — `e.returnValue = "0"` is not the empty string, and neither is a lone U+0000. */
bool before_unload_event_return_value_is_empty(JSContext *ctx, JSValueConst ev)
{
    JSValue slots = bue_slots(ctx, ev), v;
    const char *s;
    size_t len = 0;
    bool empty;

    DCHECK(g_ready, "a BeforeUnloadEvent's returnValue was read before before_unload_event_init ran");
    DCHECK(JS_IsObject(slots),
           "a BeforeUnloadEvent's returnValue was read off something that is not one — every caller reaches "
           "this behind the brand test above, so the two have disagreed");
    v = JS_GetPropertyStr(ctx, slots, "returnValue");
    JS_FreeValue(ctx, slots);
    DCHECK(JS_IsString(v), "a BeforeUnloadEvent's returnValue slot held something that is not a string — the "
                           "attribute is a DOMString, its setter takes a converted one, and creation puts the "
                           "empty string there, so there is no third writer");
    s = JS_ToCStringLen(ctx, &len, v);
    CHECK(s != NULL, "the BeforeUnloadEvent's returnValue could not be read");
    empty = len == 0;
    JS_FreeCString(ctx, s);
    JS_FreeValue(ctx, v);
    return empty;
}

void before_unload_event_set_return_value(JSContext *ctx, JSValueConst ev, JSValueConst v)
{
    JSValue slots = bue_slots(ctx, ev);

    DCHECK(g_ready, "a BeforeUnloadEvent's returnValue was written before before_unload_event_init ran");
    DCHECK(JS_IsObject(slots),
           "a BeforeUnloadEvent's returnValue was written on something that is not one");
    DCHECK(JS_IsString(v), "a BeforeUnloadEvent's returnValue was written with something that is not a string "
                           "— the DOMString conversion belongs to whoever produced the value, and a body that "
                           "coerced one here would run the page's toString from C");
    JS_SetPropertyStr(ctx, slots, "returnValue", JS_DupValue(ctx, v));
    JS_FreeValue(ctx, slots);
}

bool before_unload_event_asks_to_cancel(JSContext *ctx, JSValueConst ev)
{
    DCHECK(g_ready, "the unload cancel decision was read before before_unload_event_init ran");
    DCHECK(before_unload_event_is(ctx, ev),
           "checking if unloading is canceled read its answer off something that is not the BeforeUnloadEvent "
           "§7.5.9 fired — the two disjuncts are that event's canceled flag and that event's returnValue");
    /* The FIRST disjunct: "eventFiringResult is false", which DOM §2.10 makes exactly "the canceled flag is
       set". Asked first because it needs no allocation, not because it is the more likely one — shipped code
       spells this with returnValue far more often than with preventDefault(). */
    if (event_canceled(ctx, ev))
        return true;
    /* The SECOND: "the returnValue attribute of event is not the empty string". */
    return !before_unload_event_return_value_is_empty(ctx, ev);
}

/* ---- install ------------------------------------------------------------------------------------------------ */

void before_unload_event_init(JSContext *ctx)
{
    JSClassDef d = { "BeforeUnloadEvent" };

    DCHECK(!g_ready, "before_unload_event_init ran twice — the interface is declared once per AGENT");
    g_key = JS_NewSymbol(ctx, "beforeUnloadEventSlots", false);
    CHECK(!JS_IsException(g_key), "the BeforeUnloadEvent slot key allocation failed");
    JS_NewClassID(JS_GetRuntime(ctx), &g_bue_class);
    JS_NewClass(JS_GetRuntime(ctx), g_bue_class, &d);
    /* Declared HERE, at agent init: a fresh setter id minted from a per-realm install is a member being minted
       per realm, which is what idl_declared_before_seal exists to catch. */
    g_id_return_value_set = idl_setter_id(ctx, IDL_DOMSTRING, /*null_to_empty*/ false,
                                          js_bue_set_return_value, 0);
    g_ready = 1;
    realm_declare_intrinsic(before_unload_event_install_protos);
}

void before_unload_event_install_protos(JSContext *ctx)
{
    JSValue proto, prev, base, ctor, global;

    DCHECK(g_ready, "a realm asked for BeforeUnloadEvent before before_unload_event_init declared it");
    prev = JS_GetClassProto(ctx, g_bue_class);
    DCHECK(JS_IsNull(prev), "before_unload_event_install_protos ran twice in one realm");
    JS_FreeValue(ctx, prev);
    base = event_proto(ctx);
    proto = JS_NewObjectProto(ctx, base);
    JS_FreeValue(ctx, base);
    CHECK(!JS_IsException(proto), "BeforeUnloadEvent.prototype could not be allocated");
    idl_interface_tag(ctx, proto, "BeforeUnloadEvent");
    idl_install_accessor(ctx, proto, "returnValue", js_bue_get_return_value, 0, g_id_return_value_set);
    JS_SetClassProto(ctx, g_bue_class, JS_DupValue(ctx, proto));

    /* §3.7.1's interface object for an interface that declares NO constructor — call and construct both throw
       a TypeError, and its `prototype` is the prototype this same install just built. */
    ctor = idl_interface_object(ctx, "BeforeUnloadEvent", proto);
    JS_FreeValue(ctx, proto);
    global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, "BeforeUnloadEvent", ctor);
    JS_FreeValue(ctx, global);
}

void before_unload_event_free(JSContext *ctx)
{
    if (!g_ready) return;
    JS_FreeValue(ctx, g_key);   /* the prototypes are the REALMS' — each is released with its context */
    g_key = JS_UNDEFINED;
    g_ready = 0;
    g_id_return_value_set = -1;
}
