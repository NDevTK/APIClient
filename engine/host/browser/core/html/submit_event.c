/* THE SubmitEvent INTERFACE — HTML §4.10.22.10.
 *
 *     [Exposed=Window]
 *     interface SubmitEvent : Event {
 *       constructor(DOMString type, optional SubmitEventInit eventInitDict = {});
 *       readonly attribute HTMLElement? submitter;
 *     };
 *     dictionary SubmitEventInit : EventInit { HTMLElement? submitter = null; };
 *
 * WHY IT IS NOT DECORATION. §4.10.22.3 step 5.6 fires the `submit` event USING THIS INTERFACE, and `submitter`
 * is the only thing that tells a handler WHICH button submitted the form — which is what decides the request:
 * a submit button carries `formaction`, `formmethod`, `formenctype` and `formtarget`, so one form posts to two
 * endpoints and the handler reads which one it is off this attribute. Firing a plain Event there does not
 * merely lose a property, it makes `e.submitter.name` a TypeError in every bundle that routes on it — the
 * handler throws, and the code after it (the fetch this engine exists to find) never runs.
 *
 * A REAL SUBCLASS, like FormDataEvent: its prototype chains to the realm's `Event.prototype`, so
 * `e instanceof Event` holds and `initEvent` works on one. The base half is event_new_derived's; the one own
 * attribute hangs off a private Symbol, which makes it a slot a page cannot forge and — because a slot written
 * as a property write is captured by the COW delta — state that time-travels with the flow that fired it.
 *
 * THE INTERFACE OBJECT IS A PER-REALM INTRINSIC, declared into realm.h's one list beside the prototype rather
 * than installed from a host's hand-written list of globals. §3.7 gives each realm its own interface OBJECT for
 * the same reason it gives each its own prototype, and realm.h exists precisely because a list of per-realm
 * installs that is hand-copied is a list that stops being maintained: a component missing from one copy is
 * silently absent in that realm with nothing to say so. */
#include <string.h>

#include <lexbor/dom/dom.h>

#include "check.h"
#include "quickjs.h"
#include "core/dom/node.h"
#include "core/events/event.h"
#include "core/idl_args.h"
#include "core/idl_slots.h"
#include "core/html/submit_event.h"
#include "core/realm.h"

static JSValue   g_key;         /* the private Symbol this interface's own slot hangs off */
static JSClassID g_se_class;    /* the class exists for its per-REALM prototype slot; nothing wears it */
static int       g_ready;
static int       g_ctor_stepid = -1;

static void submit_event_install_realm(JSContext *ctx);

static JSValue se_proto(JSContext *ctx)
{
    JSValue proto = JS_GetClassProto(ctx, g_se_class);

    DCHECK(!JS_IsNull(proto),
           "SubmitEvent.prototype was asked for in a realm that never ran its per-realm install");
    return proto;   /* OWNED */
}

/* `HTMLElement?` — the type of the attribute and of the init dictionary's member. Every HTML element wrapper in
   this engine is one interface's object at the class level (§16.5a's gap: one class for every node), so the
   narrowing is the NODE's own two facts: it is an element, and it is in the HTML namespace. That is exactly what
   "implements HTMLElement" means to this engine — HTMLUnknownElement included, which is right, because the IDL
   type is satisfied by it. */
static bool se_is_html_element(JSValueConst v)
{
    lxb_dom_node_t *n = node_of(v);

    return n != NULL && n->type == LXB_DOM_NODE_TYPE_ELEMENT && n->ns == LXB_NS_HTML;
}

/* §4.10.22.10: "The submitter attribute must return the value it was initialized to." */
static JSValue js_se_get_submitter(JSContext *ctx, JSValueConst this_val, int magic)
{
    JSAtom k;
    JSValue slots, v;

    (void)magic;
    DCHECK(g_ready, "a SubmitEvent attribute was read before submit_event_init ran");
    if (!JS_IsObject(this_val))
        return JS_ThrowTypeError(ctx, "a SubmitEvent attribute was read on something that is not one");
    k = JS_ValueToAtom(ctx, g_key);
    if (k == JS_ATOM_NULL) return JS_EXCEPTION;
    if (JS_GetOwnSlot(ctx, &slots, this_val, k) <= 0) slots = JS_UNDEFINED;
    JS_FreeAtom(ctx, k);
    if (!JS_IsObject(slots)) {
        JS_FreeValue(ctx, slots);
        return JS_ThrowTypeError(ctx, "a SubmitEvent attribute was read on something that is not one");
    }
    v = JS_GetPropertyStr(ctx, slots, "submitter");
    JS_FreeValue(ctx, slots);
    return v;
}

/* The one own slot, placed on an event whose Event half is already built. Returns -1 with the throw live. */
static int se_init_slot(JSContext *ctx, JSValueConst ev, JSValueConst submitter)
{
    JSValue slots = idl_slots_new(ctx);
    JSAtom k = JS_ValueToAtom(ctx, g_key);

    if (JS_IsException(slots) || k == JS_ATOM_NULL) {
        JS_FreeValue(ctx, slots);
        if (k != JS_ATOM_NULL) JS_FreeAtom(ctx, k);
        return -1;
    }
    JS_SetPropertyStr(ctx, slots, "submitter", JS_DupValue(ctx, submitter));
    JS_SetProperty(ctx, (JSValue)ev, k, slots);
    JS_FreeAtom(ctx, k);
    return 0;
}

JSValue submit_event_new(JSContext *ctx, JSValueConst submitter)
{
    JSValue tv = JS_NewString(ctx, "submit");
    /* §4.10.22.3 step 5.6 names three initialisers and no others: `submitter`, `bubbles` true and `cancelable`
       true. It is NOT composed, and it IS trusted — the user agent fired it. */
    JSValue ev = event_new_derived(ctx, se_proto(ctx), tv, /*bubbles*/ true, /*cancelable*/ true,
                                   /*composed*/ false, /*trusted*/ true);

    JS_FreeValue(ctx, tv);
    if (JS_IsException(ev))
        return ev;
    DCHECK(JS_IsNull(submitter) || se_is_html_element(submitter),
           "§4.10.22.3 step 5.5's submitterButton was neither null nor an HTMLElement — the attribute is typed "
           "`HTMLElement?` and step 5.5 produces exactly those two");
    if (se_init_slot(ctx, ev, submitter) < 0) {
        JS_FreeValue(ctx, ev);
        return JS_EXCEPTION;
    }
    return ev;
}

/* ---- the constructor ----------------------------------------------------------------------------------------
 *
 * `constructor(DOMString type, optional SubmitEventInit eventInitDict = {})`. SubmitEventInit INHERITS
 * EventInit, and Web IDL converts a dictionary's members with the INHERITED ones first and each level
 * lexicographically among itself — which is the order this list is in, and the order a page pins by throwing
 * from one member's getter.
 *
 * `submitter` IS DECLARED `IDL_ANY` AND CHECKED HERE, and that placement is the declaration surface's gap
 * rather than this member's choice: `HTMLElement?` is a NULLABLE INTERFACE type, and IDL_INTERFACE brands
 * against a CLASS — it refuses null, which the IDL's `= null` default makes the ordinary value, and every node
 * wrapper is one class so it could not say HTMLElement anyway. The rule performed here is the type's, whole:
 * null and undefined are the IDL null, an HTML element crosses as itself, and anything else is a TypeError. */
static const IdlArgType SE_CTOR_ARGS[2] = { IDL_DOMSTRING, IDL_DICT };
static const IdlDictMember SE_INIT[] = {
    { "bubbles", IDL_BOOLEAN }, { "cancelable", IDL_BOOLEAN }, { "composed", IDL_BOOLEAN },
    { "submitter", IDL_ANY, false, NULL, 1 },
};

static JSValue js_se_ctor(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    JSValueConst init = argc > 1 ? argv[1] : JS_UNDEFINED;
    JSValue sub, ev;

    (void)magic;
    if (JS_IsUndefined(this_val))
        return JS_ThrowTypeError(ctx, "constructor SubmitEvent requires 'new'");
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "SubmitEvent constructor requires a type");
    sub = idl_dict_get(ctx, init, "submitter");
    if (JS_IsUndefined(sub) || JS_IsNull(sub)) {
        JS_FreeValue(ctx, sub);
        sub = JS_NULL;                       /* `HTMLElement? submitter = null` */
    } else if (!se_is_html_element(sub)) {
        JS_FreeValue(ctx, sub);
        return JS_ThrowTypeError(ctx, "SubmitEventInit member `submitter` is not an HTMLElement");
    }
    /* §2.2's constructor steps with THIS interface's prototype — an event the PAGE constructs is untrusted. */
    ev = event_new_derived(ctx, se_proto(ctx), argv[0],
                           idl_dict_bool(ctx, init, "bubbles"),
                           idl_dict_bool(ctx, init, "cancelable"),
                           idl_dict_bool(ctx, init, "composed"), /*trusted*/ false);
    if (JS_IsException(ev)) {
        JS_FreeValue(ctx, sub);
        return ev;
    }
    if (se_init_slot(ctx, ev, sub) < 0) {
        JS_FreeValue(ctx, sub);
        JS_FreeValue(ctx, ev);
        return JS_EXCEPTION;
    }
    JS_FreeValue(ctx, sub);
    return ev;
}

/* ---- install ------------------------------------------------------------------------------------------------ */

static const JSCFunctionListEntry js_se_proto[] = {
    JS_CGETSET_MAGIC_DEF("submitter", js_se_get_submitter, NULL, 0),
};

void submit_event_init(JSContext *ctx)
{
    JSClassDef d = { "SubmitEvent" };

    DCHECK(!g_ready, "submit_event_init ran twice — the interface is declared once per AGENT");
    g_key = JS_NewSymbol(ctx, "submitEventSlots", false);
    CHECK(!JS_IsException(g_key), "the SubmitEvent slot key allocation failed");
    JS_NewClassID(JS_GetRuntime(ctx), &g_se_class);
    JS_NewClass(JS_GetRuntime(ctx), g_se_class, &d);
    g_ctor_stepid = idl_method_id_dict(ctx, SE_CTOR_ARGS, 2, SE_INIT,
                                       (int)(sizeof(SE_INIT) / sizeof(SE_INIT[0])), js_se_ctor, 0);
    g_ready = 1;
    realm_declare_intrinsic(submit_event_install_realm);
}

static void submit_event_install_realm(JSContext *ctx)
{
    JSValue proto, prev, base, ctor, global;

    DCHECK(g_ready, "a realm asked for SubmitEvent before submit_event_init declared it");
    prev = JS_GetClassProto(ctx, g_se_class);
    DCHECK(JS_IsNull(prev), "submit_event_install_realm ran twice in one realm");
    JS_FreeValue(ctx, prev);
    base = event_proto(ctx);
    proto = JS_NewObjectProto(ctx, base);
    JS_FreeValue(ctx, base);
    CHECK(!JS_IsException(proto), "SubmitEvent.prototype could not be allocated");
    idl_interface_tag(ctx, proto, "SubmitEvent");
    JS_SetPropertyFunctionList(ctx, proto, js_se_proto, (int)(sizeof(js_se_proto) / sizeof(js_se_proto[0])));
    JS_SetClassProto(ctx, g_se_class, JS_DupValue(ctx, proto));

    /* §3.7.1's interface object, on THIS realm's global — one `SubmitEvent` per realm, whose `prototype` is the
       prototype this same install just built. */
    ctor = idl_step_constructor(ctx, "SubmitEvent", 1, g_ctor_stepid);
    CHECK(!JS_IsException(ctor), "the SubmitEvent interface object could not be allocated");
    JS_SetConstructor(ctx, ctor, proto);
    JS_FreeValue(ctx, proto);
    global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, "SubmitEvent", ctor);
    JS_FreeValue(ctx, global);
}

void submit_event_free(JSContext *ctx)
{
    if (!g_ready) return;
    JS_FreeValue(ctx, g_key);   /* the prototypes are the REALMS' — each is released with its context */
    g_key = JS_UNDEFINED;
    g_ready = 0;
    g_ctor_stepid = -1;
}
