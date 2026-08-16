/* THE ToggleEvent INTERFACE — HTML §4.11.5.
 *
 *     [Exposed=Window]
 *     interface ToggleEvent : Event {
 *       constructor(DOMString type, optional ToggleEventInit eventInitDict = {});
 *       readonly attribute DOMString oldState;
 *       readonly attribute DOMString newState;
 *       readonly attribute Element? source;
 *     };
 *     dictionary ToggleEventInit : EventInit {
 *       DOMString oldState = "";
 *       DOMString newState = "";
 *       Element? source = null;
 *     };
 *
 * WHY IT EXISTS RATHER THAN A PLAIN Event. §4.11.4's CLOSE THE DIALOG fires `beforetoggle` and then queues a
 * `toggle`, and the two attributes are the whole of what a handler reads: a page that watches one dialog for
 * both directions branches on `e.newState === "closed"`, and firing a plain Event there does not lose a
 * property quietly — it makes that comparison `undefined === "closed"`, so the handler takes the wrong arm and
 * every line after it runs in the wrong world. `source` is the element that caused the change, which for a
 * `method=dialog` submission is the submitter button.
 *
 * A REAL SUBCLASS, like SubmitEvent: its prototype chains to this realm's `Event.prototype`, so
 * `e instanceof Event` holds. The base half is event_new_derived's; the three own attributes hang off a private
 * Symbol, which makes them slots a page cannot forge and — because a slot written as a property write is
 * captured by the COW delta — state that time-travels with the flow that fired it.
 *
 * THE INTERFACE OBJECT IS A PER-REALM INTRINSIC, declared into realm.h's one list beside the prototype, for the
 * reason submit_event.c states: a hand-copied list of per-realm installs is a list that stops being
 * maintained. */
#include <string.h>

#include <lexbor/dom/dom.h>

#include "check.h"
#include "quickjs.h"
#include "core/dom/node.h"
#include "core/events/event.h"
#include "core/html/toggle_event.h"
#include "core/idl_args.h"
#include "core/idl_slots.h"
#include "core/realm.h"

static JSValue   g_key;         /* the private Symbol this interface's own slots hang off */
static JSClassID g_te_class;    /* the class exists for its per-REALM prototype slot; nothing wears it */
static int       g_ready;
static int       g_ctor_stepid = -1;

static void toggle_event_install_realm(JSContext *ctx);

static JSValue te_proto(JSContext *ctx)
{
    JSValue proto = JS_GetClassProto(ctx, g_te_class);

    DCHECK(!JS_IsNull(proto),
           "ToggleEvent.prototype was asked for in a realm that never ran its per-realm install");
    return proto;   /* OWNED */
}

/* `Element?` — the type of the `source` attribute and of the init dictionary's member. Every element wrapper in
   this engine is one class at the class level, so the narrowing is the NODE's own fact: it is an element. The
   type is Element and not HTMLElement, so no namespace test belongs here — an SVG element satisfies it. */
static bool te_is_element(JSValueConst v)
{
    lxb_dom_node_t *n = node_of(v);

    return n != NULL && n->type == LXB_DOM_NODE_TYPE_ELEMENT;
}

/* The three attributes "must return the value it was initialized to", read out of the one slot record. `magic`
   IS the member, so there is one body rather than three copies of the same brand check. */
enum { TE_OLD_STATE = 0, TE_NEW_STATE, TE_SOURCE };
static const char *const TE_SLOT_NAME[] = { "oldState", "newState", "source" };

static JSValue js_te_get(JSContext *ctx, JSValueConst this_val, int magic)
{
    JSAtom k;
    JSValue slots, v;

    DCHECK(g_ready, "a ToggleEvent attribute was read before toggle_event_init ran");
    DCHECK(magic >= TE_OLD_STATE && magic <= TE_SOURCE,
           "a ToggleEvent accessor was installed with a magic this interface has no member for");
    if (!JS_IsObject(this_val))
        return JS_ThrowTypeError(ctx, "a ToggleEvent attribute was read on something that is not one");
    k = JS_ValueToAtom(ctx, g_key);
    if (k == JS_ATOM_NULL) return JS_EXCEPTION;
    if (JS_GetOwnSlot(ctx, &slots, this_val, k) <= 0) slots = JS_UNDEFINED;
    JS_FreeAtom(ctx, k);
    if (!JS_IsObject(slots)) {
        JS_FreeValue(ctx, slots);
        return JS_ThrowTypeError(ctx, "a ToggleEvent attribute was read on something that is not one");
    }
    v = JS_GetPropertyStr(ctx, slots, TE_SLOT_NAME[magic]);
    JS_FreeValue(ctx, slots);
    return v;
}

/* The three own slots, placed on an event whose Event half is already built. `old_state` and `new_state` are
   VALUES rather than C strings because the constructor's arrive already converted by the IDL declaration and
   re-stringifying one would run the page's toString a second time. Returns -1 with the throw live. */
static int te_init_slots(JSContext *ctx, JSValueConst ev, JSValueConst old_state, JSValueConst new_state,
                         JSValueConst source)
{
    JSValue slots = idl_slots_new(ctx);
    JSAtom k = JS_ValueToAtom(ctx, g_key);

    if (JS_IsException(slots) || k == JS_ATOM_NULL) {
        JS_FreeValue(ctx, slots);
        if (k != JS_ATOM_NULL) JS_FreeAtom(ctx, k);
        return -1;
    }
    JS_SetPropertyStr(ctx, slots, "oldState", JS_DupValue(ctx, old_state));
    JS_SetPropertyStr(ctx, slots, "newState", JS_DupValue(ctx, new_state));
    JS_SetPropertyStr(ctx, slots, "source", JS_DupValue(ctx, source));
    JS_SetProperty(ctx, (JSValue)ev, k, slots);
    JS_FreeAtom(ctx, k);
    return 0;
}

JSValue toggle_event_new(JSContext *ctx, const char *type, const char *old_state, const char *new_state,
                         JSValueConst source, bool bubbles, bool cancelable)
{
    JSValue tv, ov, nv, ev;

    DCHECK(g_ready, "a ToggleEvent was minted before toggle_event_init declared the interface");
    DCHECK(JS_IsNull(source) || te_is_element(source),
           "a ToggleEvent was minted with a `source` that is neither null nor an Element — the attribute is "
           "typed `Element?` and every algorithm that fires one produces exactly those two");
    tv = JS_NewString(ctx, type);
    ov = JS_NewString(ctx, old_state);
    nv = JS_NewString(ctx, new_state);
    if (JS_IsException(tv) || JS_IsException(ov) || JS_IsException(nv)) {
        JS_FreeValue(ctx, tv); JS_FreeValue(ctx, ov); JS_FreeValue(ctx, nv);
        return JS_EXCEPTION;
    }
    /* TRUSTED — the user agent fired it. `composed` is false: no algorithm in HTML fires this one composed. */
    ev = event_new_derived(ctx, te_proto(ctx), tv, bubbles, cancelable, /*composed*/ false, /*trusted*/ true);
    JS_FreeValue(ctx, tv);
    if (!JS_IsException(ev) && te_init_slots(ctx, ev, ov, nv, source) < 0) {
        JS_FreeValue(ctx, ev);
        ev = JS_EXCEPTION;
    }
    JS_FreeValue(ctx, ov);
    JS_FreeValue(ctx, nv);
    return ev;
}

/* ---- the constructor ----------------------------------------------------------------------------------------
 *
 * ToggleEventInit INHERITS EventInit, and Web IDL converts a dictionary's members with the INHERITED ones first
 * and each level lexicographically among itself — which is the order this list is in, and the order a page pins
 * by throwing from one member's getter.
 *
 * `source` IS DECLARED `IDL_ANY` AND CHECKED HERE for the reason SubmitEvent's `submitter` is: `Element?` is a
 * NULLABLE INTERFACE type, IDL_INTERFACE brands against a CLASS, and every node wrapper in this engine is one
 * class. The rule performed here is the type's, whole: null and undefined are the IDL null, an element crosses
 * as itself, and anything else is a TypeError. */
static const IdlArgType TE_CTOR_ARGS[2] = { IDL_DOMSTRING, IDL_DICT };
static const IdlDictMember TE_INIT[] = {
    { "bubbles", IDL_BOOLEAN }, { "cancelable", IDL_BOOLEAN }, { "composed", IDL_BOOLEAN },
    { "newState", IDL_DOMSTRING, false, NULL, 1, NULL, IDL_DEFAULT_STRING, "" },
    { "oldState", IDL_DOMSTRING, false, NULL, 1, NULL, IDL_DEFAULT_STRING, "" },
    { "source",   IDL_ANY,       false, NULL, 1, NULL, IDL_DEFAULT_NULL, NULL },
};

static JSValue js_te_ctor(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    JSValueConst init = argc > 1 ? argv[1] : JS_UNDEFINED;
    JSValue old_state, new_state, source, ev;

    (void)magic;
    if (JS_IsUndefined(this_val))
        return JS_ThrowTypeError(ctx, "constructor ToggleEvent requires 'new'");
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "ToggleEvent constructor requires a type");
    source = idl_dict_get(ctx, init, "source");
    if (JS_IsUndefined(source) || JS_IsNull(source)) {
        JS_FreeValue(ctx, source);
        source = JS_NULL;                    /* `Element? source = null` */
    } else if (!te_is_element(source)) {
        JS_FreeValue(ctx, source);
        return JS_ThrowTypeError(ctx, "ToggleEventInit member `source` is not an Element");
    }
    old_state = idl_dict_get(ctx, init, "oldState");
    new_state = idl_dict_get(ctx, init, "newState");
    /* §2.2's constructor steps with THIS interface's prototype — an event the PAGE constructs is untrusted. */
    ev = event_new_derived(ctx, te_proto(ctx), argv[0],
                           idl_dict_bool(ctx, init, "bubbles"),
                           idl_dict_bool(ctx, init, "cancelable"),
                           idl_dict_bool(ctx, init, "composed"), /*trusted*/ false);
    if (!JS_IsException(ev) && te_init_slots(ctx, ev, old_state, new_state, source) < 0) {
        JS_FreeValue(ctx, ev);
        ev = JS_EXCEPTION;
    }
    JS_FreeValue(ctx, old_state);
    JS_FreeValue(ctx, new_state);
    JS_FreeValue(ctx, source);
    return ev;
}

/* ---- install ------------------------------------------------------------------------------------------------ */

static const JSCFunctionListEntry js_te_proto[] = {
    JS_CGETSET_MAGIC_DEF("oldState", js_te_get, NULL, TE_OLD_STATE),
    JS_CGETSET_MAGIC_DEF("newState", js_te_get, NULL, TE_NEW_STATE),
    JS_CGETSET_MAGIC_DEF("source", js_te_get, NULL, TE_SOURCE),
};

void toggle_event_init(JSContext *ctx)
{
    JSClassDef d = { "ToggleEvent" };

    DCHECK(!g_ready, "toggle_event_init ran twice — the interface is declared once per AGENT");
    g_key = JS_NewSymbol(ctx, "toggleEventSlots", false);
    CHECK(!JS_IsException(g_key), "the ToggleEvent slot key allocation failed");
    JS_NewClassID(JS_GetRuntime(ctx), &g_te_class);
    JS_NewClass(JS_GetRuntime(ctx), g_te_class, &d);
    g_ctor_stepid = idl_method_id_dict(ctx, TE_CTOR_ARGS, 2, TE_INIT,
                                       (int)(sizeof(TE_INIT) / sizeof(TE_INIT[0])), js_te_ctor, 0);
    g_ready = 1;
    realm_declare_intrinsic(toggle_event_install_realm);
}

static void toggle_event_install_realm(JSContext *ctx)
{
    JSValue proto, prev, base, ctor, global;

    DCHECK(g_ready, "a realm asked for ToggleEvent before toggle_event_init declared it");
    prev = JS_GetClassProto(ctx, g_te_class);
    DCHECK(JS_IsNull(prev), "toggle_event_install_realm ran twice in one realm");
    JS_FreeValue(ctx, prev);
    base = event_proto(ctx);
    proto = JS_NewObjectProto(ctx, base);
    JS_FreeValue(ctx, base);
    CHECK(!JS_IsException(proto), "ToggleEvent.prototype could not be allocated");
    idl_interface_tag(ctx, proto, "ToggleEvent");
    JS_SetPropertyFunctionList(ctx, proto, js_te_proto, (int)(sizeof(js_te_proto) / sizeof(js_te_proto[0])));
    JS_SetClassProto(ctx, g_te_class, JS_DupValue(ctx, proto));

    /* §3.7.1's interface object, on THIS realm's global — one `ToggleEvent` per realm, whose `prototype` is the
       prototype this same install just built. */
    ctor = idl_step_constructor(ctx, "ToggleEvent", 1, g_ctor_stepid);
    CHECK(!JS_IsException(ctor), "the ToggleEvent interface object could not be allocated");
    JS_SetConstructor(ctx, ctor, proto);
    JS_FreeValue(ctx, proto);
    global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, "ToggleEvent", ctor);
    JS_FreeValue(ctx, global);
}

void toggle_event_free(JSRuntime *rt)
{
    if (!g_ready) return;
    JS_FreeValueRT(rt, g_key);   /* the prototypes are the REALMS' — each is released with its context */
    g_key = JS_UNDEFINED;
    g_ready = 0;
    g_ctor_stepid = -1;
}
