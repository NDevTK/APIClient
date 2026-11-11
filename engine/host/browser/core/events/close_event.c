/* WebSockets §6 "The CloseEvent interface": `[Exposed=(Window,Worker)] interface CloseEvent : Event` with
   `wasClean`, `code` and `reason`, each returning the value it was initialized to. The three live in one
   slot record under a private symbol, as every Event subclass here keeps its own members. */
#include "check.h"
#include "quickjs.h"
#include "core/agent_state.h"
#include "core/events/close_event.h"
#include "core/events/event.h"
#include "core/idl_args.h"
#include "core/idl_slots.h"
#include "core/realm.h"

static JSValue   g_key = JS_UNDEFINED;   /* the private symbol the slot record hangs off */
static JSClassID g_ce_class;             /* held for its per-realm prototype slot; no object wears it */
static int       g_ready;
static int       g_ctor_stepid = -1;

enum { CE_WAS_CLEAN = 0, CE_CODE, CE_REASON };
static const char *const CE_SLOT_NAME[] = { "wasClean", "code", "reason" };

static JSValue ce_proto(JSContext *ctx)
{
    JSValue proto = JS_GetClassProto(ctx, g_ce_class);

    DCHECK(!JS_IsNull(proto), "CloseEvent.prototype was asked for in a realm that never ran its per-realm install");
    return proto;   /* owned */
}

static JSValue js_ce_get(JSContext *ctx, JSValueConst this_val, int magic)
{
    JSAtom k;
    JSValue slots, v;

    DCHECK(g_ready, "a CloseEvent attribute was read before close_event_init ran");
    DCHECK(magic >= CE_WAS_CLEAN && magic <= CE_REASON,
           "a CloseEvent accessor was installed with a magic this interface has no member for");
    if (!JS_IsObject(this_val))
        return JS_ThrowTypeError(ctx, "a CloseEvent attribute was read on something that is not one");
    k = JS_ValueToAtom(ctx, g_key);
    if (k == JS_ATOM_NULL) return JS_EXCEPTION;
    if (JS_GetOwnSlot(ctx, &slots, this_val, k) <= 0) slots = JS_UNDEFINED;
    JS_FreeAtom(ctx, k);
    if (!JS_IsObject(slots)) {
        JS_FreeValue(ctx, slots);
        return JS_ThrowTypeError(ctx, "a CloseEvent attribute was read on something that is not one");
    }
    v = JS_GetPropertyStr(ctx, slots, CE_SLOT_NAME[magic]);
    JS_FreeValue(ctx, slots);
    return v;
}

/* Places the slot record on an event whose Event half is built. Consumes the three values. */
static int ce_init_slots(JSContext *ctx, JSValueConst ev, JSValue was_clean, JSValue code, JSValue reason)
{
    JSValue slots = idl_slots_new(ctx);
    JSAtom k = JS_ValueToAtom(ctx, g_key);

    if (JS_IsException(slots) || k == JS_ATOM_NULL) {
        JS_FreeValue(ctx, slots);
        if (k != JS_ATOM_NULL) JS_FreeAtom(ctx, k);
        JS_FreeValue(ctx, was_clean); JS_FreeValue(ctx, code); JS_FreeValue(ctx, reason);
        return -1;
    }
    JS_SetPropertyStr(ctx, slots, CE_SLOT_NAME[CE_WAS_CLEAN], was_clean);
    JS_SetPropertyStr(ctx, slots, CE_SLOT_NAME[CE_CODE], code);
    JS_SetPropertyStr(ctx, slots, CE_SLOT_NAME[CE_REASON], reason);
    JS_SetProperty(ctx, (JSValue)ev, k, slots);
    JS_FreeAtom(ctx, k);
    return 0;
}

JSValue close_event_new(JSContext *ctx, bool was_clean, uint16_t code, const char *reason)
{
    JSValue tv, rv, ev;

    DCHECK(g_ready, "a close event was minted before close_event_init declared the interface");
    DCHECK(reason != NULL, "a close event was minted with no reason; §6 types it USVString, so absent is \"\"");
    tv = JS_NewString(ctx, "close");
    rv = JS_NewString(ctx, reason);
    if (JS_IsException(tv) || JS_IsException(rv)) {
        JS_FreeValue(ctx, tv); JS_FreeValue(ctx, rv);
        return JS_EXCEPTION;
    }
    ev = event_new_derived(ctx, ce_proto(ctx), tv, /*bubbles*/ false, /*cancelable*/ false, /*composed*/ false,
                           /*trusted*/ true);
    JS_FreeValue(ctx, tv);
    if (JS_IsException(ev)) { JS_FreeValue(ctx, rv); return ev; }
    if (ce_init_slots(ctx, ev, JS_NewBool(ctx, was_clean), JS_NewInt32(ctx, code), rv) < 0) {
        JS_FreeValue(ctx, ev);
        return JS_EXCEPTION;
    }
    return ev;
}

/* `constructor(DOMString type, optional CloseEventInit eventInitDict = {})`. The dictionary's own members are
   level 1 in Web IDL §3.2.17's read order (code, reason, wasClean), each with §6's default. */
static const IdlArgType CE_CTOR_ARGS[2] = { IDL_DOMSTRING, IDL_DICT };
static const IdlDictMember CE_INIT[] = {
    { "bubbles", IDL_BOOLEAN }, { "cancelable", IDL_BOOLEAN }, { "composed", IDL_BOOLEAN },
    { "code", IDL_UNSIGNED_SHORT, false, NULL, 1, NULL, IDL_DEFAULT_ZERO },
    { "reason", IDL_USVSTRING, false, NULL, 1, NULL, IDL_DEFAULT_STRING, "" },
    { "wasClean", IDL_BOOLEAN, false, NULL, 1, NULL, IDL_DEFAULT_FALSE },
};

static JSValue js_ce_ctor(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    JSValueConst init = argc > 1 ? argv[1] : JS_UNDEFINED;
    JSValue ev;

    (void)magic;
    if (JS_IsUndefined(this_val))
        return JS_ThrowTypeError(ctx, "constructor CloseEvent requires 'new'");
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "CloseEvent constructor requires a type");
    /* DOM §2.5 "Constructing events" with this interface's prototype; a page-built event is untrusted. */
    ev = event_new_derived(ctx, ce_proto(ctx), argv[0], idl_dict_bool(ctx, init, "bubbles"),
                           idl_dict_bool(ctx, init, "cancelable"), idl_dict_bool(ctx, init, "composed"),
                           /*trusted*/ false);
    if (JS_IsException(ev)) return ev;
    if (ce_init_slots(ctx, ev, JS_NewBool(ctx, idl_dict_bool(ctx, init, "wasClean")),
                      idl_dict_get(ctx, init, "code"), idl_dict_get(ctx, init, "reason")) < 0) {
        JS_FreeValue(ctx, ev);
        return JS_EXCEPTION;
    }
    return ev;
}

static const JSCFunctionListEntry js_ce_proto[] = {
    JS_CGETSET_MAGIC_DEF("wasClean", js_ce_get, NULL, CE_WAS_CLEAN),
    JS_CGETSET_MAGIC_DEF("code", js_ce_get, NULL, CE_CODE),
    JS_CGETSET_MAGIC_DEF("reason", js_ce_get, NULL, CE_REASON),
};

void close_event_init(JSContext *ctx)
{
    JSClassDef d = { "CloseEvent" };

    DCHECK(!g_ready, "close_event_init ran twice; the interface is declared once per agent");
    g_key = JS_NewSymbol(ctx, "closeEventSlots", false);
    CHECK(!JS_IsException(g_key), "the CloseEvent slot key allocation failed");
    JS_NewClassID(JS_GetRuntime(ctx), &g_ce_class);
    JS_NewClass(JS_GetRuntime(ctx), g_ce_class, &d);
    g_ctor_stepid = idl_method_id_dict(ctx, CE_CTOR_ARGS, 2, CE_INIT, (int)(sizeof(CE_INIT) / sizeof(CE_INIT[0])),
                                       js_ce_ctor, 0);
    idl_optional_from(1);
    g_ready = 1;
    /* An Event subclass's slots are released by the `event` row, whose init and free call this component's. */
    agent_state_flag("event", &g_ready, "WebSockets §6 The CloseEvent interface's declaration latch");
    agent_state_class("event", &g_ce_class,
                      "WebSockets §6 The CloseEvent interface's class, held for its per-realm prototype slot");
    agent_state_value("event", &g_key, "the private symbol WebSockets §6 The CloseEvent interface's slots hang off");
    agent_state_id("event", &g_ctor_stepid, "WebSockets §6 The CloseEvent interface's constructor");
    realm_declare_intrinsic(close_event_install_protos);
}

void close_event_install_protos(JSContext *ctx)
{
    JSValue proto, prev, base, ctor, global;

    DCHECK(g_ready, "a realm asked for CloseEvent before close_event_init declared it");
    prev = JS_GetClassProto(ctx, g_ce_class);
    DCHECK(JS_IsNull(prev), "close_event_install_protos ran twice in one realm");
    JS_FreeValue(ctx, prev);
    base = event_proto(ctx);
    proto = JS_NewObjectProto(ctx, base);
    JS_FreeValue(ctx, base);
    CHECK(!JS_IsException(proto), "CloseEvent.prototype could not be allocated");
    idl_interface_tag(ctx, proto, "CloseEvent");
    JS_SetPropertyFunctionList(ctx, proto, js_ce_proto, (int)(sizeof(js_ce_proto) / sizeof(js_ce_proto[0])));
    JS_SetClassProto(ctx, g_ce_class, JS_DupValue(ctx, proto));
    ctor = idl_step_constructor(ctx, "CloseEvent", g_ctor_stepid);
    CHECK(!JS_IsException(ctor), "the CloseEvent interface object could not be allocated");
    JS_SetConstructor(ctx, ctor, proto);
    JS_FreeValue(ctx, proto);
    global = JS_GetGlobalObject(ctx);
    idl_define_global_property_reference(ctx, global, "CloseEvent", ctor);
    JS_FreeValue(ctx, global);
}

void close_event_free(JSRuntime *rt)
{
    DCHECK(g_ready, "WebSockets §6 The CloseEvent interface was released in an agent that never declared it");
    JS_FreeValueRT(rt, g_key);   /* the prototypes are the realms', released with their contexts */
    g_key = JS_UNDEFINED;
    g_ready = 0;
    g_ce_class = 0;
    g_ctor_stepid = -1;
}
