/* THE InputDeviceCapabilities INTERFACE — Input Device Capabilities, §"The InputDeviceCapabilities interface".
 *
 *     [Exposed=Window]
 *     interface InputDeviceCapabilities {
 *       constructor(optional InputDeviceCapabilitiesInit deviceInitDict = {});
 *       readonly attribute boolean firesTouchEvents;
 *       readonly attribute boolean pointerMovementScrolls;
 *     };
 *     dictionary InputDeviceCapabilitiesInit {
 *       boolean firesTouchEvents = false;
 *       boolean pointerMovementScrolls = false;
 *     };
 *
 * WHY IT EXISTS AND WHAT WAS BLOCKED ON IT. It is the TYPE of the one member §"Extensions to the UIEvent
 * interface and UIEventInit dictionary" adds to UIEvent, so `sourceCapabilities` could not be installed
 * anywhere until this interface existed: a nullable interface-typed member whose interface does not exist has
 * exactly one value a page can tell apart, and answering `null` for every event would have made "no input
 * device was responsible" — which the spec states as a POSITIVE fact about a resize event — indistinguishable
 * from an engine that can express nothing else. UIEvent, FocusEvent, MouseEvent and KeyboardEvent all reported
 * the member absent, and they reported it because of this file and not because of four gaps.
 *
 * IT IS NOT HEADLESS-SHRUGGING TO ANSWER FROM THE DICTIONARY, BECAUSE THE DICTIONARY IS WHAT THE SPEC READS.
 * Every attribute here is `readonly` with no algorithm behind it and no setter: §"The InputDeviceCapabilities
 * interface" declares a CONSTRUCTOR taking `InputDeviceCapabilitiesInit` and two booleans, and a browser with a
 * touchscreen answers a page-constructed `new InputDeviceCapabilities({firesTouchEvents: true})` exactly as
 * this does. What a device would decide is which instance a TRUSTED event carries, and that is the event's
 * question rather than this interface's — an event this engine creates has no input device behind it, which is
 * the spec's own `null` and not a value this file has to invent.
 *
 * THE STATE IS AN INTERNAL SLOT RECORD under a private Symbol, for core/idl_slots.h's reason: a property write
 * is captured by the per-flow COW delta, so an instance a flow mints time-travels with that flow, and the
 * Symbol is a brand a page cannot forge. The CLASS is the other brand and the two are not redundant — the
 * class is what `idl_iface_brand` names, so `new UIEvent('x', {sourceCapabilities: {}})` is a TypeError thrown
 * by the DECLARATION before this file's or UIEvent's body is entered, and the slot record is what an accessor
 * reads once a receiver has passed it.
 *
 * TOUCH EVENTS ARE ABSENT FROM THIS AGENT AND THAT DOES NOT MAKE `firesTouchEvents` MEANINGLESS. The audit
 * excludes the GlobalEventHandlers touch members because Touch Events Level 2 says a user agent whose "expose
 * legacy touch event APIs" is false must not implement that mixin; it says nothing about this interface, and
 * §"firesTouchEvents" is a statement a page reads to decide whether a `mousedown` it received may already have
 * been handled — a question a page asks in code this engine drives whether or not the events exist. */
#include "check.h"
#include "quickjs.h"
#include "core/events/input_device_capabilities.h"
#include "core/idl_args.h"
#include "core/idl_slots.h"
#include "core/realm.h"

static JSValue   g_key;         /* the private Symbol this interface's own slots hang off */
static JSClassID g_idc_class;   /* the BRAND, worn by every instance, and the per-REALM prototype slot */
static int       g_ready;
static int       g_ctor_stepid = -1;

/* §"The InputDeviceCapabilities interface"'s two attributes, in ONE table so the member list, the dictionary
   and the slot names can never name different sets. Web IDL §3.2.17 Dictionary types reads a dictionary's own
   members lexicographically, which is the order below. */
enum { IDC_FIRES_TOUCH_EVENTS = 0, IDC_POINTER_MOVEMENT_SCROLLS, IDC_N };
static const char *const IDC_MEMBER[IDC_N] = { "firesTouchEvents", "pointerMovementScrolls" };

/* `boolean X = false` — IDL_BOOLEAN and not IDL_BOOLEAN_NO_DEFAULT, because these members HAVE a default and it
   is exactly what ToBoolean(undefined) answers, so there is no absent state for a body to tell apart. */
static const IdlDictMember IDC_INIT[IDC_N] = {
    { "firesTouchEvents", IDL_BOOLEAN },
    { "pointerMovementScrolls", IDL_BOOLEAN },
};
static const IdlArgType IDC_CTOR_ARGS[1] = { IDL_DICT };

JSClassID input_device_capabilities_class(void)
{
    DCHECK(g_ready, "the InputDeviceCapabilities brand was asked for before input_device_capabilities_init "
                    "ran — a dictionary that brands against a class id of zero has nothing to test, which the "
                    "conversion asserts on rather than admitting the value");
    return g_idc_class;
}

static JSValue idc_slots(JSContext *ctx, JSValueConst v)
{
    JSAtom k;
    JSValue slots;

    DCHECK(g_ready, "an InputDeviceCapabilities' slots were asked for before the interface was declared");
    if (!JS_IsObject(v))
        return JS_UNDEFINED;
    k = JS_ValueToAtom(ctx, g_key);
    if (k == JS_ATOM_NULL)
        return JS_UNDEFINED;
    if (JS_GetOwnSlot(ctx, &slots, v, k) <= 0)
        slots = JS_UNDEFINED;
    JS_FreeAtom(ctx, k);
    return slots;
}

JSValue input_device_capabilities_of_dict(JSContext *ctx, JSValueConst init, const char *member)
{
    JSValue v = idl_dict_get(ctx, init, member);

    DCHECK(member != NULL && *member, "an InputDeviceCapabilities? member was read with no member name");
    /* THE ABSENT DICTIONARY IS DOM §2.5 Constructing events' CREATE AN EVENT, NOT A PAGE'S OMISSION. A
       constructor's declaration has already placed the IDL's `= null` for a member the page left out, so the
       only `undefined` that reaches here is the one an engine-created event carries — Web IDL §3.2.17
       Dictionary types makes every member of an ABSENT dictionary absent, and this member's default IS its
       un-initialized value. */
    if (JS_IsUndefined(v)) {
        JS_FreeValue(ctx, v);
        return JS_NULL;
    }
    DCHECK(JS_IsNull(v) || JS_GetClassID(v) == g_idc_class,
           "an `InputDeviceCapabilities?` dictionary member reached its reader carrying something that is "
           "neither the IDL null nor an InputDeviceCapabilities — the BRAND is the declaration's "
           "(idl_iface_brand over IDL_INTERFACE_NULLABLE), so a wrong value is a TypeError thrown before any "
           "body is entered and cannot arrive here");
    return v;
}

/* ---- the attributes ------------------------------------------------------------------------------------- */

static JSValue js_idc_get(JSContext *ctx, JSValueConst this_val, int magic)
{
    JSValue slots = idc_slots(ctx, this_val), v;

    DCHECK(magic >= 0 && magic < IDC_N,
           "an InputDeviceCapabilities attribute was declared with a magic the member table does not name");
    if (!JS_IsObject(slots)) {
        JS_FreeValue(ctx, slots);
        return JS_ThrowTypeError(ctx, "an InputDeviceCapabilities attribute was read on something that is not "
                                      "an InputDeviceCapabilities");
    }
    v = JS_GetPropertyStr(ctx, slots, IDC_MEMBER[magic]);
    JS_FreeValue(ctx, slots);
    DCHECK(JS_IsBool(v), "an InputDeviceCapabilities slot held something that is not a boolean — both members "
                         "are `boolean` with a default, so the constructor writes a real true or false into "
                         "every one of them");
    return v;
}

/* ---- the constructor ------------------------------------------------------------------------------------ */

static JSValue js_idc_ctor(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    JSValueConst init = argc > 0 ? argv[0] : JS_UNDEFINED;
    JSValue obj, proto, slots;
    JSAtom k;
    int i;

    (void)magic;
    if (JS_IsUndefined(this_val))
        return JS_ThrowTypeError(ctx, "constructor InputDeviceCapabilities requires 'new'");
    proto = JS_GetClassProto(ctx, g_idc_class);
    DCHECK(!JS_IsNull(proto),
           "InputDeviceCapabilities.prototype was asked for in a realm that never ran "
           "input_device_capabilities_install_protos — a realm whose intrinsics were not all installed mints "
           "an instance chained to another document's prototype");
    obj = JS_NewObjectProtoClass(ctx, proto, g_idc_class);
    JS_FreeValue(ctx, proto);
    if (JS_IsException(obj))
        return obj;
    slots = idl_slots_new(ctx);
    k = JS_ValueToAtom(ctx, g_key);
    if (JS_IsException(slots) || k == JS_ATOM_NULL) {
        JS_FreeValue(ctx, slots);
        JS_FreeValue(ctx, obj);
        if (k != JS_ATOM_NULL) JS_FreeAtom(ctx, k);
        return JS_EXCEPTION;
    }
    /* The declaration has already converted both members, so these reads run none of the page's code. */
    for (i = 0; i < IDC_N; i++)
        JS_SetPropertyStr(ctx, slots, IDC_MEMBER[i],
                          JS_NewBool(ctx, idl_dict_bool(ctx, init, IDC_MEMBER[i])));
    JS_SetProperty(ctx, obj, k, slots);
    JS_FreeAtom(ctx, k);
    return obj;
}

/* ---- install --------------------------------------------------------------------------------------------- */

void input_device_capabilities_init(JSContext *ctx)
{
    JSClassDef d = { "InputDeviceCapabilities" };

    DCHECK(!g_ready, "input_device_capabilities_init ran twice — the interface is declared once per AGENT");
    g_key = JS_NewSymbol(ctx, "inputDeviceCapabilitiesSlots", false);
    CHECK(!JS_IsException(g_key), "the InputDeviceCapabilities slot key allocation failed");
    JS_NewClassID(JS_GetRuntime(ctx), &g_idc_class);
    JS_NewClass(JS_GetRuntime(ctx), g_idc_class, &d);
    g_ctor_stepid = idl_method_id_dict(ctx, IDC_CTOR_ARGS, 1, IDC_INIT, IDC_N, js_idc_ctor, 0);
    idl_optional_from(0);   /* `constructor(optional InputDeviceCapabilitiesInit deviceInitDict = {})` */
    g_ready = 1;
    realm_declare_intrinsic(input_device_capabilities_install_protos);
}

void input_device_capabilities_install_protos(JSContext *ctx)
{
    JSValue proto, prev, ctor, global;
    int i;

    DCHECK(g_ready, "a realm asked for InputDeviceCapabilities before it was declared");
    prev = JS_GetClassProto(ctx, g_idc_class);
    DCHECK(JS_IsNull(prev), "input_device_capabilities_install_protos ran twice in one realm — §3.7 gives a "
                            "realm ONE InputDeviceCapabilities.prototype, and a second leaves every instance "
                            "already chained to the first answering out of a discarded object");
    JS_FreeValue(ctx, prev);
    proto = JS_NewObject(ctx);
    CHECK(!JS_IsException(proto), "InputDeviceCapabilities.prototype could not be allocated");
    idl_interface_tag(ctx, proto, "InputDeviceCapabilities");
    for (i = 0; i < IDC_N; i++)
        idl_install_accessor(ctx, proto, IDC_MEMBER[i], js_idc_get, i, -1);
    JS_SetClassProto(ctx, g_idc_class, JS_DupValue(ctx, proto));

    /* §3.7.1's interface object, on THIS realm's global — declared into core/realm.h's ONE list rather than a
       host's hand-written list of globals, so a host cannot be missing it. */
    ctor = idl_step_constructor(ctx, "InputDeviceCapabilities", 0, g_ctor_stepid);
    CHECK(!JS_IsException(ctor), "the InputDeviceCapabilities interface object could not be allocated");
    JS_SetConstructor(ctx, ctor, proto);
    JS_FreeValue(ctx, proto);
    global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, "InputDeviceCapabilities", ctor);
    JS_FreeValue(ctx, global);
}

void input_device_capabilities_free(JSRuntime *rt)
{
    if (!g_ready) return;
    JS_FreeValueRT(rt, g_key);  /* the prototypes are the REALMS' — each is released with its context */
    g_key = JS_UNDEFINED;
    g_ready = 0;
    g_ctor_stepid = -1;
}
