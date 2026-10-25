/* THE DragEvent INTERFACE — HTML §6.11.4 The DragEvent interface.
 *
 *     [Exposed=Window]
 *     interface DragEvent : MouseEvent {
 *       constructor(DOMString type, optional DragEventInit eventInitDict = {});
 *       readonly attribute DataTransfer? dataTransfer;
 *     };
 *     dictionary DragEventInit : MouseEventInit {
 *       DataTransfer? dataTransfer = null;
 *     };
 *
 * "The dataTransfer attribute of the DragEvent interface must return the value it was initialized to." The
 * user agent's events are minted by fire a DND event (drag_event_new_for_dnd); a page's by the constructor.
 * The slot is an own property under a private Symbol, so the event's state time-travels with its flow. */
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "core/agent_state.h"
#include "core/clipboard/data_transfer.h"
#include "core/events/drag_event.h"
#include "core/events/event.h"
#include "core/events/input_device_capabilities.h"
#include "core/events/mouse_event.h"
#include "core/events/ui_event.h"
#include "core/frame/window_proxy.h"
#include "core/idl_args.h"
#include "core/idl_slots.h"
#include "core/realm.h"

static JSValue   g_key = JS_UNDEFINED;   /* the private Symbol this interface's slot record hangs off */
static JSClassID g_de_class;             /* exists for its per-realm prototype slot; nothing wears it */
static int       g_ready;
static int       g_ctor_stepid = -1;

static JSValue drag_event_proto(JSContext *ctx)
{
    JSValue proto = JS_GetClassProto(ctx, g_de_class);

    DCHECK(!JS_IsNull(proto), "DragEvent.prototype was asked for in a realm that never ran drag_event_install_protos");
    return proto;   /* owned */
}

static int de_set_data_transfer(JSContext *ctx, JSValueConst ev, JSValue dt)
{
    JSValue slots = idl_slots_new(ctx);
    JSAtom k = JS_ValueToAtom(ctx, g_key);

    if (JS_IsException(slots) || k == JS_ATOM_NULL) {
        JS_FreeValue(ctx, slots);
        JS_FreeValue(ctx, dt);
        if (k != JS_ATOM_NULL) JS_FreeAtom(ctx, k);
        return -1;
    }
    DCHECK(JS_IsNull(dt) || data_transfer_is(ctx, dt),
           "a DragEvent's dataTransfer is neither null nor a DataTransfer — DragEventInit's member brands it");
    JS_SetPropertyStr(ctx, slots, "dataTransfer", dt);
    JS_SetProperty(ctx, (JSValue)ev, k, slots);
    JS_FreeAtom(ctx, k);
    return 0;
}

/* The receiver's `dataTransfer`. Web IDL §3.7.6 makes a receiver that is not a DragEvent a TypeError. */
static JSValue js_de_get(JSContext *ctx, JSValueConst this_val, int magic)
{
    JSAtom k;
    JSValue slots = JS_UNDEFINED, v;

    (void)magic;
    if (!JS_IsObject(this_val)) return JS_ThrowTypeError(ctx, "a DragEvent attribute was read on something that is not one");
    k = JS_ValueToAtom(ctx, g_key);
    if (JS_GetOwnSlot(ctx, &slots, this_val, k) <= 0) slots = JS_UNDEFINED;
    JS_FreeAtom(ctx, k);
    if (!JS_IsObject(slots)) {
        JS_FreeValue(ctx, slots);
        return JS_ThrowTypeError(ctx, "a DragEvent attribute was read on something that is not one");
    }
    v = JS_GetPropertyStr(ctx, slots, "dataTransfer");
    JS_FreeValue(ctx, slots);
    return v;
}

/* DragEventInit splices the three inherited member lists and appends its own at level 4, the depth
   mouse_event.h names for a `: MouseEventInit` dictionary. */
static const IdlArgType DE_CTOR_ARGS[2] = { IDL_DOMSTRING, IDL_DICT };
static const IdlDictMember DE_INIT[] = {
    UI_EVENT_INIT_MEMBERS,
    EVENT_MODIFIER_INIT_MEMBERS,
    MOUSE_EVENT_INIT_MEMBERS,
    { "dataTransfer", IDL_INTERFACE_NULLABLE, false, NULL, 4, NULL, IDL_DEFAULT_NULL,
      .iface_is = data_transfer_is, .iface_name = "DataTransfer" },
};

static JSValue js_de_ctor(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    JSValueConst init = argc > 1 ? argv[1] : JS_UNDEFINED;
    JSValue ev, dt;

    (void)magic;
    if (JS_IsUndefined(this_val)) return JS_ThrowTypeError(ctx, "constructor DragEvent requires 'new'");
    DCHECK(argc >= 1, "the DragEvent constructor body ran with no type argument — Web IDL §3.6 step 5 is the "
                      "declaration's and throws before any body is entered");
    ev = mouse_event_new_derived(ctx, drag_event_proto(ctx), argv[0], init, /*trusted*/ false);
    if (JS_IsException(ev)) return ev;
    dt = JS_IsUndefined(init) ? JS_NULL : idl_dict_get(ctx, init, "dataTransfer");
    if (de_set_data_transfer(ctx, ev, JS_IsUndefined(dt) ? JS_NULL : dt) < 0) {
        JS_FreeValue(ctx, ev);
        return JS_EXCEPTION;
    }
    return ev;
}

JSValue drag_event_new(JSContext *ctx)
{
    JSValue type = JS_NewString(ctx, ""), ev;

    if (JS_IsException(type)) return type;
    ev = mouse_event_new_derived(ctx, drag_event_proto(ctx), type, JS_UNDEFINED, /*trusted*/ true);
    JS_FreeValue(ctx, type);
    if (!JS_IsException(ev) && de_set_data_transfer(ctx, ev, JS_NULL) < 0) {
        JS_FreeValue(ctx, ev);
        return JS_EXCEPTION;
    }
    return ev;
}

JSValue drag_event_new_for_dnd(JSContext *ctx, const char *type, JSValueConst view, JSValueConst data_transfer)
{
    JSValue t = JS_NewString(ctx, type), ev;
    bool cancelable = strcmp(type, "dragleave") != 0 && strcmp(type, "dragend") != 0;   /* step 8 */

    CHECK(!JS_IsException(t), "DragEvent: OOM naming a DND event");
    DCHECK(data_transfer_is(ctx, data_transfer), "a DND event was fired with no DataTransfer — step 5 makes one");
    DCHECK(window_proxy_is_window(ctx, view), "a DND event was fired with a view that is not a Window — step 3's "
                                              "window is the target document's relevant global object");
    /* Step 7: create an event using DragEvent, its mouse attributes at their un-initialized 0 (step 9's "no
       relevant pointing device"), then type, bubbles, cancelable and view. */
    ev = mouse_event_new_derived(ctx, drag_event_proto(ctx), t, JS_UNDEFINED, /*trusted*/ true);
    CHECK(!JS_IsException(ev), "DragEvent: OOM creating a DND event");
    CHECK(ui_event_reinit(ctx, ev, t, true, cancelable, JS_DupValue(ctx, view)),
          "a DND event just created was already being dispatched");
    JS_FreeValue(ctx, t);
    CHECK(de_set_data_transfer(ctx, ev, JS_DupValue(ctx, data_transfer)) == 0, "DragEvent: OOM setting dataTransfer");
    return ev;
}

static const JSCFunctionListEntry js_de_proto[] = {
    JS_CGETSET_MAGIC_DEF("dataTransfer", js_de_get, NULL, 0),
};

void drag_event_init(JSContext *ctx)
{
    JSClassDef d = { "DragEvent" };

    DCHECK(!g_ready, "drag_event_init ran twice — the interface is declared once per agent");
    g_key = JS_NewSymbol(ctx, "dragEventSlots", false);
    CHECK(!JS_IsException(g_key), "the DragEvent slot key allocation failed");
    JS_NewClassID(JS_GetRuntime(ctx), &g_de_class);
    JS_NewClass(JS_GetRuntime(ctx), g_de_class, &d);
    g_ctor_stepid = idl_method_id_dict(ctx, DE_CTOR_ARGS, 2, DE_INIT, (int)(sizeof(DE_INIT) / sizeof(DE_INIT[0])),
                                       js_de_ctor, 0);
    idl_optional_from(1);
    /* UIEventInit's `sourceCapabilities`; `view`, `relatedTarget` and `dataTransfer` state their own predicates. */
    idl_iface_brand(input_device_capabilities_class());
    g_ready = 1;
    agent_state_flag("event", &g_ready, "HTML §6.11.4 DragEvent's declaration latch");
    agent_state_class("event", &g_de_class, "HTML §6.11.4 DragEvent's class, held for its per-realm prototype slot");
    agent_state_value("event", &g_key, "the private Symbol HTML §6.11.4 DragEvent's slot record hangs off");
    agent_state_id("event", &g_ctor_stepid,
                   "HTML §6.11.4 DragEvent's `constructor(DOMString type, optional DragEventInit eventInitDict = {})`");
    realm_declare_intrinsic(drag_event_install_protos);
}

void drag_event_install_protos(JSContext *ctx)
{
    JSValue proto, prev, base, ctor, global;

    DCHECK(g_ready, "a realm asked for DragEvent before drag_event_init declared it");
    prev = JS_GetClassProto(ctx, g_de_class);
    DCHECK(JS_IsNull(prev), "drag_event_install_protos ran twice in one realm");
    JS_FreeValue(ctx, prev);
    base = mouse_event_proto(ctx);   /* `interface DragEvent : MouseEvent`, this realm's */
    proto = JS_NewObjectProto(ctx, base);
    JS_FreeValue(ctx, base);
    CHECK(!JS_IsException(proto), "DragEvent.prototype could not be allocated");
    idl_interface_tag(ctx, proto, "DragEvent");
    JS_SetPropertyFunctionList(ctx, proto, js_de_proto, (int)(sizeof(js_de_proto) / sizeof(js_de_proto[0])));
    JS_SetClassProto(ctx, g_de_class, JS_DupValue(ctx, proto));
    ctor = idl_step_constructor(ctx, "DragEvent", g_ctor_stepid);
    CHECK(!JS_IsException(ctor), "the DragEvent interface object could not be allocated");
    JS_SetConstructor(ctx, ctor, proto);
    JS_FreeValue(ctx, proto);
    global = JS_GetGlobalObject(ctx);
    idl_define_global_property_reference(ctx, global, "DragEvent", ctor);
    JS_FreeValue(ctx, global);
}

void drag_event_free(JSRuntime *rt)
{
    DCHECK(g_ready, "HTML §6.11.4 DragEvent was released in an agent that never declared it");
    JS_FreeValueRT(rt, g_key);
    g_key = JS_UNDEFINED;
    g_ready = 0;
    g_de_class = 0;
    g_ctor_stepid = -1;
}
