/* THE FormDataEvent INTERFACE — HTML §4.10.22.1.
 *
 * WHY IT EXISTS HERE. §4.10.22.4 step 7 fires one at the form once the entry list is built, and its `formData`
 * attribute is the ONLY handle a page has on that list: "operations on the FormData object will affect form
 * data to be submitted". So this is not decoration around the entry list — it is the entry list's one
 * scriptable seam, and a submission whose `formdata` handler appended a CSRF token submits that token.
 *
 * `interface FormDataEvent : Event`, a REAL subclass like ProgressEvent: its prototype chains to the realm's
 * `Event.prototype`, so `e instanceof Event` holds and `initEvent` works on one. The base half is
 * event_new_derived's; the one own attribute hangs off a private Symbol, which makes it a slot a page cannot
 * forge and — because a slot written as a property write is captured by the COW delta — state that
 * time-travels with the flow that fired the event. */
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "core/idl_slots.h"
#include "core/idl_args.h"
#include "core/realm.h"
#include "core/events/event.h"
#include "core/html/form_data.h"
#include "core/html/form_data_event.h"

static JSValue    g_key;         /* the private Symbol this interface's own slot hangs off */
static JSClassID  g_fde_class;   /* the class exists for its per-REALM prototype slot; nothing wears it */
static int        g_ready;
static int        g_ctor_stepid = -1;

static void form_data_event_install_proto(JSContext *ctx);

static JSValue fde_proto(JSContext *ctx)
{
    JSValue proto = JS_GetClassProto(ctx, g_fde_class);

    DCHECK(!JS_IsNull(proto),
           "FormDataEvent.prototype was asked for in a realm that never ran its per-realm install");
    return proto;   /* OWNED */
}

/* §4.10.22.1: "The formData attribute must return the value it was initialized to." */
static JSValue js_fde_get_form_data(JSContext *ctx, JSValueConst this_val, int magic)
{
    JSAtom k;
    JSValue slots, v;

    (void)magic;
    DCHECK(g_ready, "a FormDataEvent attribute was read before form_data_event_init ran");
    if (!JS_IsObject(this_val))
        return JS_ThrowTypeError(ctx, "a FormDataEvent attribute was read on something that is not one");
    k = JS_ValueToAtom(ctx, g_key);
    if (k == JS_ATOM_NULL) return JS_EXCEPTION;
    if (JS_GetOwnSlot(ctx, &slots, this_val, k) <= 0) slots = JS_UNDEFINED;
    JS_FreeAtom(ctx, k);
    if (!JS_IsObject(slots)) {
        JS_FreeValue(ctx, slots);
        return JS_ThrowTypeError(ctx, "a FormDataEvent attribute was read on something that is not one");
    }
    v = JS_GetPropertyStr(ctx, slots, "formData");
    JS_FreeValue(ctx, slots);
    return v;
}

/* The one own slot, placed on an event whose Event half is already built. Returns -1 with the throw live. */
static int fde_init_slot(JSContext *ctx, JSValueConst ev, JSValueConst form_data)
{
    JSValue slots = idl_slots_new(ctx);
    JSAtom k = JS_ValueToAtom(ctx, g_key);

    if (JS_IsException(slots) || k == JS_ATOM_NULL) {
        JS_FreeValue(ctx, slots);
        if (k != JS_ATOM_NULL) JS_FreeAtom(ctx, k);
        return -1;
    }
    JS_SetPropertyStr(ctx, slots, "formData", JS_DupValue(ctx, form_data));
    JS_SetProperty(ctx, (JSValue)ev, k, slots);
    JS_FreeAtom(ctx, k);
    return 0;
}

JSValue form_data_event_new(JSContext *ctx, JSValueConst form_data)
{
    JSValue tv = JS_NewString(ctx, "formdata");
    /* §4.10.22.4 step 7 names two initialisers and no others: `formData`, and `bubbles` true. It is not
       cancelable — there is nothing for a handler to cancel, the list is already built — and it IS trusted:
       the user agent fired it. */
    JSValue ev = event_new_derived(ctx, fde_proto(ctx), tv, /*bubbles*/ true, /*cancelable*/ false,
                                   /*composed*/ false, /*trusted*/ true);

    JS_FreeValue(ctx, tv);
    if (JS_IsException(ev))
        return ev;
    DCHECK(form_data_is(form_data),
           "§4.10.22.4 step 6's `form data` was not a FormData — the event's attribute is typed `FormData` and "
           "nothing else can satisfy it");
    if (fde_init_slot(ctx, ev, form_data) < 0) {
        JS_FreeValue(ctx, ev);
        return JS_EXCEPTION;
    }
    return ev;
}

/* ---- the constructor --------------------------------------------------------------------------------------
 *
 * `constructor(DOMString type, FormDataEventInit eventInitDict)`. The dictionary argument is REQUIRED, which is
 * unusual and is the IDL's own doing: `FormDataEventInit` has a `required FormData formData` member, so there
 * is no dictionary a caller could omit. FormDataEventInit INHERITS EventInit, and Web IDL converts a
 * dictionary's members with the INHERITED ones first and each level lexicographically among itself — which is
 * the order this list is in, and the order a page pins by throwing from one getter. */
static const IdlArgType FDE_CTOR_ARGS[2] = { IDL_DOMSTRING, IDL_DICT };
static const IdlDictMember FDE_INIT[] = {
    { "bubbles", IDL_BOOLEAN }, { "cancelable", IDL_BOOLEAN }, { "composed", IDL_BOOLEAN },
    { "formData", IDL_INTERFACE, true, NULL, 1 },
};

static JSValue js_fde_ctor(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    JSValueConst init = argc > 1 ? argv[1] : JS_UNDEFINED;
    JSValue fd, ev;

    (void)magic;
    if (JS_IsUndefined(this_val))
        return JS_ThrowTypeError(ctx, "constructor FormDataEvent requires 'new'");
    if (argc < 2)
        return JS_ThrowTypeError(ctx, "FormDataEvent constructor requires a type and an init dictionary");
    /* §2.2's constructor steps with THIS interface's prototype — an event the PAGE constructs is untrusted. */
    ev = event_new_derived(ctx, fde_proto(ctx), argv[0],
                           idl_dict_bool(ctx, init, "bubbles"),
                           idl_dict_bool(ctx, init, "cancelable"),
                           idl_dict_bool(ctx, init, "composed"), /*trusted*/ false);
    if (JS_IsException(ev))
        return ev;
    /* The member is declared `required` and interface-typed, so the declaration has already refused an absent
       one and anything that is not a FormData; this reads what it left. */
    fd = idl_dict_get(ctx, init, "formData");
    if (fde_init_slot(ctx, ev, fd) < 0) {
        JS_FreeValue(ctx, fd);
        JS_FreeValue(ctx, ev);
        return JS_EXCEPTION;
    }
    JS_FreeValue(ctx, fd);
    return ev;
}

/* ---- install ----------------------------------------------------------------------------------------------- */

static const JSCFunctionListEntry js_fde_proto[] = {
    JS_CGETSET_MAGIC_DEF("formData", js_fde_get_form_data, NULL, 0),
};

void form_data_event_init(JSContext *ctx)
{
    JSClassDef d = { "FormDataEvent" };

    DCHECK(!g_ready, "form_data_event_init ran twice — the interface is declared once per AGENT");
    g_key = JS_NewSymbol(ctx, "formDataEventSlots", false);
    CHECK(!JS_IsException(g_key), "the FormDataEvent slot key allocation failed");
    JS_NewClassID(JS_GetRuntime(ctx), &g_fde_class);
    JS_NewClass(JS_GetRuntime(ctx), g_fde_class, &d);
    g_ctor_stepid = idl_method_id_dict(ctx, FDE_CTOR_ARGS, 2, FDE_INIT,
                                       (int)(sizeof(FDE_INIT) / sizeof(FDE_INIT[0])), js_fde_ctor, 0);
    idl_iface_brand(form_data_class_id());   /* the `required FormData formData` member's brand */
    g_ready = 1;
    realm_declare_intrinsic(form_data_event_install_proto);
}

static void form_data_event_install_proto(JSContext *ctx)
{
    JSValue proto, prev, base;

    DCHECK(g_ready, "a realm asked for FormDataEvent.prototype before form_data_event_init declared it");
    prev = JS_GetClassProto(ctx, g_fde_class);
    DCHECK(JS_IsNull(prev), "form_data_event_install_proto ran twice in one realm");
    JS_FreeValue(ctx, prev);
    base = event_proto(ctx);
    proto = JS_NewObjectProto(ctx, base);
    JS_FreeValue(ctx, base);
    CHECK(!JS_IsException(proto), "FormDataEvent.prototype could not be allocated");
    idl_interface_tag(ctx, proto, "FormDataEvent");
    JS_SetPropertyFunctionList(ctx, proto, js_fde_proto, (int)(sizeof(js_fde_proto) / sizeof(js_fde_proto[0])));
    JS_SetClassProto(ctx, g_fde_class, proto);
}

void form_data_event_install(JSContext *ctx, JSValueConst global)
{
    JSValue ctor, proto;

    DCHECK(g_ready, "FormDataEvent was installed before form_data_event_init declared it");
    ctor = idl_step_constructor(ctx, "FormDataEvent", 2, g_ctor_stepid);
    CHECK(!JS_IsException(ctor), "the FormDataEvent interface object could not be allocated");
    proto = fde_proto(ctx);
    JS_SetConstructor(ctx, ctor, proto);
    JS_FreeValue(ctx, proto);
    JS_SetPropertyStr(ctx, (JSValue)global, "FormDataEvent", ctor);
}

void form_data_event_free(JSContext *ctx)
{
    if (!g_ready) return;
    JS_FreeValue(ctx, g_key);   /* the prototypes are the REALMS' — each is released with its context */
    g_key = JS_UNDEFINED;
    g_ready = 0;
    g_ctor_stepid = -1;
}
