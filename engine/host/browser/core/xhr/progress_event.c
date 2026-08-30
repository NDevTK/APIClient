/* THE ProgressEvent INTERFACE — XMLHttpRequest Standard §5.
 *
 * `interface ProgressEvent : Event`, and a REAL subclass like ErrorEvent and MessageEvent: its prototype chains
 * to the realm's `Event.prototype`, so `e instanceof Event` is true and `initEvent` works on one. The base half
 * is event_new_derived's; the three own attributes hang off a private Symbol, which makes them slots a page
 * cannot forge and — because a slot written as a property write is captured by the COW delta — state that
 * time-travels with the flow that fired the event.
 *
 * IT LIVES BESIDE XMLHttpRequest BECAUSE §5 IS PART OF THAT STANDARD. Seven of the eight events XHR fires are
 * ProgressEvents, and §5.1's "fire a progress event" is the operation that mints them; putting the interface in
 * a general events bag would separate it from the one algorithm that defines how its attributes are filled. */
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "core/idl_slots.h"
#include "core/idl_args.h"
#include "core/realm.h"
#include "core/agent_state.h"
#include "core/events/event.h"
#include "core/xhr/progress_event.h"

static JSValue    g_key = JS_UNDEFINED;   /* the private Symbol this interface's own slots hang off */
static JSClassID  g_pe_class;   /* the class exists for its per-REALM prototype slot; nothing wears it */
static int        g_ready;
static int        g_ctor_stepid = -1;

enum { PE_LENGTH_COMPUTABLE = 0, PE_LOADED, PE_TOTAL };
static const char *const PE_SLOT[] = { "lengthComputable", "loaded", "total" };

static JSValue pe_slots(JSContext *ctx, JSValueConst ev)
{
    JSAtom k;
    JSValue slots;

    DCHECK(g_ready, "a ProgressEvent's slots were asked for before progress_event_init ran");
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

/* §5: "The lengthComputable, loaded, and total getter steps are to return the value they were initialized
   to." */
static JSValue js_pe_get(JSContext *ctx, JSValueConst this_val, int magic)
{
    JSValue slots = pe_slots(ctx, this_val), v;

    DCHECK(magic >= 0 && magic < (int)(sizeof(PE_SLOT) / sizeof(PE_SLOT[0])),
           "a ProgressEvent attribute was declared with a magic the slot table does not name");
    if (!JS_IsObject(slots)) {
        JS_FreeValue(ctx, slots);
        return JS_ThrowTypeError(ctx, "a ProgressEvent attribute was read on something that is not one");
    }
    v = JS_GetPropertyStr(ctx, slots, PE_SLOT[magic]);
    JS_FreeValue(ctx, slots);
    return v;
}

/* The three own slots, placed on an event whose Event half is already built. Returns -1 with the throw live. */
static int pe_init_slots(JSContext *ctx, JSValueConst ev, bool length_computable, double loaded, double total)
{
    JSValue slots = idl_slots_new(ctx);
    JSAtom k = JS_ValueToAtom(ctx, g_key);

    if (JS_IsException(slots) || k == JS_ATOM_NULL) {
        JS_FreeValue(ctx, slots);
        if (k != JS_ATOM_NULL) JS_FreeAtom(ctx, k);
        return -1;
    }
    JS_SetPropertyStr(ctx, slots, "lengthComputable", JS_NewBool(ctx, length_computable));
    JS_SetPropertyStr(ctx, slots, "loaded", JS_NewFloat64(ctx, loaded));
    JS_SetPropertyStr(ctx, slots, "total", JS_NewFloat64(ctx, total));
    JS_SetProperty(ctx, (JSValue)ev, k, slots);
    JS_FreeAtom(ctx, k);
    return 0;
}

JSValue progress_event_new(JSContext *ctx, const char *type, double transmitted, double length)
{
    JSValue tv = JS_NewString(ctx, type);
    /* §5.1: the event is fired with `loaded` = transmitted, and — only when length is not 0 —
       `lengthComputable` = true and `total` = length. It does not bubble and is not cancelable (§5.1 names no
       initialiser for either), and it IS trusted: the user agent fired it. */
    JSValue ev = event_new_derived(ctx, progress_event_proto(ctx), tv, /*bubbles*/ false, /*cancelable*/ false,
                                   /*composed*/ false, /*trusted*/ true);

    JS_FreeValue(ctx, tv);
    if (JS_IsException(ev))
        return ev;
    if (pe_init_slots(ctx, ev, length != 0, transmitted, length != 0 ? length : 0) < 0) {
        JS_FreeValue(ctx, ev);
        return JS_EXCEPTION;
    }
    return ev;
}

/* ---- the constructor -------------------------------------------------------------------------------------- */

/* `constructor(DOMString type, optional ProgressEventInit eventInitDict = {})`. ProgressEventInit INHERITS
   EventInit, and Web IDL converts a dictionary's members with the INHERITED ones first and each level
   lexicographically among itself — which is the order this list is in, and the order a page pins by throwing
   from one getter. `lengthComputable` sorts before `loaded` and `total`, and all three sort after EventInit's. */
static const IdlArgType PE_CTOR_ARGS[2] = { IDL_DOMSTRING, IDL_DICT };
static const IdlDictMember PE_INIT[] = {
    { "bubbles", IDL_BOOLEAN }, { "cancelable", IDL_BOOLEAN }, { "composed", IDL_BOOLEAN },
    { "lengthComputable", IDL_BOOLEAN, false, NULL, 1 },
    { "loaded", IDL_UNRESTRICTED_DOUBLE, false, NULL, 1 },
    { "total", IDL_UNRESTRICTED_DOUBLE, false, NULL, 1 },
};

static double pe_dict_double(JSContext *ctx, JSValueConst init, const char *name)
{
    JSValue v = idl_dict_get(ctx, init, name);
    double d = 0;

    /* The declaration has already converted the member to a NUMBER, so this runs none of the page's code; an
       absent member is the IDL's `= 0` default. */
    if (!JS_IsUndefined(v))
        JS_ToFloat64(ctx, &d, v);
    JS_FreeValue(ctx, v);
    return d;
}

static JSValue js_pe_ctor(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    JSValueConst init = argc > 1 ? argv[1] : JS_UNDEFINED;
    JSValue ev;

    (void)magic;
    if (JS_IsUndefined(this_val))
        return JS_ThrowTypeError(ctx, "constructor ProgressEvent requires 'new'");
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "ProgressEvent constructor requires a type");
    /* §2.2's constructor steps with THIS interface's prototype — an event the PAGE constructs is untrusted. */
    ev = event_new_derived(ctx, progress_event_proto(ctx), argv[0],
                           idl_dict_bool(ctx, init, "bubbles"),
                           idl_dict_bool(ctx, init, "cancelable"),
                           idl_dict_bool(ctx, init, "composed"), /*trusted*/ false);
    if (JS_IsException(ev))
        return ev;
    /* A CONSTRUCTED ProgressEvent takes its three members AS WRITTEN — unlike §5.1's fire, which derives
       lengthComputable from the length. `new ProgressEvent("x", {total: 5})` has lengthComputable false. */
    if (pe_init_slots(ctx, ev, idl_dict_bool(ctx, init, "lengthComputable"),
                      pe_dict_double(ctx, init, "loaded"), pe_dict_double(ctx, init, "total")) < 0) {
        JS_FreeValue(ctx, ev);
        return JS_EXCEPTION;
    }
    return ev;
}

/* ---- install ----------------------------------------------------------------------------------------------- */

static const JSCFunctionListEntry js_pe_proto[] = {
    JS_CGETSET_MAGIC_DEF("lengthComputable", js_pe_get, NULL, PE_LENGTH_COMPUTABLE),
    JS_CGETSET_MAGIC_DEF("loaded", js_pe_get, NULL, PE_LOADED),
    JS_CGETSET_MAGIC_DEF("total", js_pe_get, NULL, PE_TOTAL),
};

void progress_event_init(JSContext *ctx)
{
    JSClassDef d = { "ProgressEvent" };

    DCHECK(!g_ready, "progress_event_init ran twice — the interface is declared once per AGENT");
    g_key = JS_NewSymbol(ctx, "progressEventSlots", false);
    CHECK(!JS_IsException(g_key), "the ProgressEvent slot key allocation failed");
    JS_NewClassID(JS_GetRuntime(ctx), &g_pe_class);
    JS_NewClass(JS_GetRuntime(ctx), g_pe_class, &d);
    g_ctor_stepid = idl_method_id_dict(ctx, PE_CTOR_ARGS, 2, PE_INIT,
                                       (int)(sizeof(PE_INIT) / sizeof(PE_INIT[0])), js_pe_ctor, 0);
    idl_optional_from(1);
    g_ready = 1;
    /* A SUB-COMPONENT NAMES THE ROW THAT RELEASES IT, which core/agent_state.h states is a CLAIM about which
       release undoes this and not a spelling of this file: §5 has no row of core/platform.c's own, because
       xhr_init calls this init and xhr_free calls this release. Nothing here was declared at all, so the
       pairing's own arm — does anybody release this? — was never asked about any of the four, and the row
       that really owns them read as one that holds exactly seven slots. */
    agent_state_flag("xml_http_request", &g_ready, "§5's declaration latch");
    agent_state_class("xml_http_request", &g_pe_class, "§5's ProgressEvent class");
    agent_state_id("xml_http_request", &g_ctor_stepid, "§5's ProgressEvent constructor");
    agent_state_value("xml_http_request", &g_key, "the private Symbol §5's three slots hang off");
    realm_declare_intrinsic(progress_event_install_proto);
}

void progress_event_install_proto(JSContext *ctx)
{
    JSValue proto, prev, base;

    DCHECK(g_ready, "a realm asked for ProgressEvent.prototype before progress_event_init declared it");
    prev = JS_GetClassProto(ctx, g_pe_class);
    DCHECK(JS_IsNull(prev), "progress_event_install_proto ran twice in one realm");
    JS_FreeValue(ctx, prev);
    base = event_proto(ctx);
    proto = JS_NewObjectProto(ctx, base);
    JS_FreeValue(ctx, base);
    CHECK(!JS_IsException(proto), "ProgressEvent.prototype could not be allocated");
    idl_interface_tag(ctx, proto, "ProgressEvent");
    JS_SetPropertyFunctionList(ctx, proto, js_pe_proto, (int)(sizeof(js_pe_proto) / sizeof(js_pe_proto[0])));
    JS_SetClassProto(ctx, g_pe_class, proto);
}

void progress_event_install(JSContext *ctx, JSValueConst global)
{
    JSValue ctor, proto;

    DCHECK(g_ready, "ProgressEvent was installed before progress_event_init declared it");
    ctor = idl_step_constructor(ctx, "ProgressEvent", 1, g_ctor_stepid);
    CHECK(!JS_IsException(ctor), "the ProgressEvent interface object could not be allocated");
    proto = progress_event_proto(ctx);
    JS_SetConstructor(ctx, ctor, proto);
    JS_FreeValue(ctx, proto);
    JS_SetPropertyStr(ctx, (JSValue)global, "ProgressEvent", ctor);
}

JSValue progress_event_proto(JSContext *ctx)
{
    JSValue proto = JS_GetClassProto(ctx, g_pe_class);
    DCHECK(!JS_IsNull(proto),
           "ProgressEvent.prototype was asked for in a realm that never ran progress_event_install_proto");
    return proto;   /* OWNED */
}

void progress_event_free(JSRuntime *rt)
{
    /* NOT `if (!g_ready) return;`, for the reason xhr_init states one file over: xhr_init calls
       progress_event_init unconditionally and xhr_free — which has already asserted its own latch — calls this
       unconditionally, so the test could never be true and what it could do was hide a release that left the
       latch set. */
    DCHECK(g_ready, "ProgressEvent was released in an agent that never declared it — xhr_init calls "
                    "progress_event_init on the one declaration pass, so there is no arm that reaches here "
                    "undeclared");
    JS_FreeValueRT(rt, g_key);   /* the prototypes are the REALMS' — each is released with its context */
    g_key = JS_UNDEFINED;
    g_ready = 0;
    g_ctor_stepid = -1;
    /* core/agent_state.h's one policy: a class id is given back like every other slot. Nothing wears this
       class — it exists for its per-realm prototype slot — so there is no finalizer or gc_mark here to owe the
       JS_GetAnyOpaque the zeroing costs xml_http_request.c. */
    g_pe_class = 0;
}
