/* THE HashChangeEvent INTERFACE — HTML §7.2.7.3.
 *
 *     [Exposed=Window]
 *     interface HashChangeEvent : Event {
 *       constructor(DOMString type, optional HashChangeEventInit eventInitDict = {});
 *       readonly attribute USVString oldURL;
 *       readonly attribute USVString newURL;
 *     };
 *     dictionary HashChangeEventInit : EventInit {
 *       USVString oldURL = "";
 *       USVString newURL = "";
 *     };
 *
 * WHY IT EXISTS AND WHAT WAS BLOCKED ON IT. §7.4.6.2's update-document-for-history-step-application ends with
 * "if oldURL's fragment is not equal to entry's URL's fragment, then queue a global task on the DOM manipulation
 * task source … to fire an event named hashchange … using HashChangeEvent", and there was no such interface — so
 * a traversal between two entries that differ only in their fragment had nothing to fire. That is the shape of
 * every `#/route` router ever shipped, and of every in-page anchor navigation: `onhashchange` is the whole of
 * how such a bundle learns that the user went back.
 *
 * THE TWO ATTRIBUTES ARE THE EVENT'S ENTIRE CONTENT and a handler reads both: §7.2.7.3 says `oldURL` "represents
 * … the URL of the session history entry that was traversed FROM" and `newURL` "the URL of the session history
 * entry that is now current". A router splits `newURL` on `#` to get its route and diffs it against `oldURL` to
 * decide direction, so firing a plain Event here would not lose two properties quietly — it would make both
 * `undefined` and send the handler's `split` to a TypeError.
 *
 * IT IS A REAL SUBCLASS: `HashChangeEvent.prototype.__proto__ === Event.prototype`, so `e instanceof Event`
 * holds and `initEvent` works on one. The base half is event_new_derived's.
 *
 * IT HAS A createEvent ROW, unlike PopStateEvent, and that asymmetry is DOM §4.5's table rather than a choice
 * here: the table names HashChangeEvent and not PopStateEvent. core/events/create_event.c asserts the pairing
 * from both sides, so the row and this interface arrive together or the first `createEvent('HashChangeEvent')`
 * crashes naming the one that did not.
 *
 * THE SLOTS ARE OWN PROPERTIES UNDER A PRIVATE SYMBOL, for the reason event.c gives: a slot written as a
 * property write is captured by the COW delta, so the event's state time-travels with the flow that fired it,
 * and the symbol is a brand a page cannot forge.
 *
 * THE INTERFACE OBJECT IS A PER-REALM INTRINSIC, declared into realm.h's one list beside the prototype — §3.7
 * gives each realm its own interface OBJECT for the same reason it gives each its own prototype. */
#include <stdbool.h>

#include "check.h"
#include "quickjs.h"
#include "core/events/event.h"
#include "core/events/hash_change_event.h"
#include "core/idl_args.h"
#include "core/idl_slots.h"
#include "core/realm.h"

static JSValue   g_key;         /* the private Symbol this interface's own slots hang off */
static JSClassID g_hce_class;   /* the class exists for its per-REALM prototype slot; nothing wears it */
static int       g_ready;
static int       g_ctor_stepid = -1;

static JSValue hce_proto(JSContext *ctx)
{
    JSValue proto = JS_GetClassProto(ctx, g_hce_class);

    DCHECK(!JS_IsNull(proto),
           "HashChangeEvent.prototype was asked for in a realm that never ran its per-realm install");
    return proto;   /* OWNED */
}

/* Both attributes "must return the value it was initialized to", read out of the one slot record. `magic` IS the
   member, so there is one body rather than two copies of the same brand check. */
enum { HCE_OLD_URL = 0, HCE_NEW_URL };
static const char *const HCE_SLOT_NAME[] = { "oldURL", "newURL" };

static JSValue js_hce_get(JSContext *ctx, JSValueConst this_val, int magic)
{
    JSAtom k;
    JSValue slots, v;

    DCHECK(g_ready, "a HashChangeEvent attribute was read before hash_change_event_init ran");
    DCHECK(magic >= HCE_OLD_URL && magic <= HCE_NEW_URL,
           "a HashChangeEvent accessor was installed with a magic this interface has no member for");
    if (!JS_IsObject(this_val))
        return JS_ThrowTypeError(ctx, "a HashChangeEvent attribute was read on something that is not one");
    k = JS_ValueToAtom(ctx, g_key);
    if (k == JS_ATOM_NULL) return JS_EXCEPTION;
    if (JS_GetOwnSlot(ctx, &slots, this_val, k) <= 0) slots = JS_UNDEFINED;
    JS_FreeAtom(ctx, k);
    if (!JS_IsObject(slots)) {
        JS_FreeValue(ctx, slots);
        return JS_ThrowTypeError(ctx, "a HashChangeEvent attribute was read on something that is not one");
    }
    v = JS_GetPropertyStr(ctx, slots, HCE_SLOT_NAME[magic]);
    JS_FreeValue(ctx, slots);
    return v;
}

/* The two own slots, placed on an event whose Event half is already built. They are VALUES rather than C strings
   because the constructor's arrive already converted by the IDL declaration — re-stringifying one would run the
   page's toString a second time, and a USVString's unpaired-surrogate replacement has already happened by then.
   Returns -1 with the throw live. */
static int hce_init_slots(JSContext *ctx, JSValueConst ev, JSValueConst old_url, JSValueConst new_url)
{
    JSValue slots = idl_slots_new(ctx);
    JSAtom k = JS_ValueToAtom(ctx, g_key);

    if (JS_IsException(slots) || k == JS_ATOM_NULL) {
        JS_FreeValue(ctx, slots);
        if (k != JS_ATOM_NULL) JS_FreeAtom(ctx, k);
        return -1;
    }
    JS_SetPropertyStr(ctx, slots, "oldURL", JS_DupValue(ctx, old_url));
    JS_SetPropertyStr(ctx, slots, "newURL", JS_DupValue(ctx, new_url));
    JS_SetProperty(ctx, (JSValue)ev, k, slots);
    JS_FreeAtom(ctx, k);
    return 0;
}

/* The shared body of the two makers: an event of `type` carrying the two URLs. `old_url` and `new_url` are the
   already-serialized addresses; `trusted` is what separates the engine's fire from §4.5's factory instance. */
static JSValue hce_new(JSContext *ctx, const char *type, const char *old_url, const char *new_url, bool trusted)
{
    JSValue tv, ov, nv, ev;

    DCHECK(g_ready, "a HashChangeEvent was minted before hash_change_event_init declared the interface");
    DCHECK(old_url != NULL && new_url != NULL,
           "a HashChangeEvent was minted with a null URL — §7.2.7.3 types both attributes USVString with an "
           "empty-string default, so there is no absent value for one to carry");
    tv = JS_NewString(ctx, type);
    ov = JS_NewString(ctx, old_url);
    nv = JS_NewString(ctx, new_url);
    if (JS_IsException(tv) || JS_IsException(ov) || JS_IsException(nv)) {
        JS_FreeValue(ctx, tv); JS_FreeValue(ctx, ov); JS_FreeValue(ctx, nv);
        return JS_EXCEPTION;
    }
    /* DOM's fire-an-event with none of the three flags set, because §7.4.6.2 sets none of them. */
    ev = event_new_derived(ctx, hce_proto(ctx), tv, /*bubbles*/ false, /*cancelable*/ false,
                           /*composed*/ false, trusted);
    JS_FreeValue(ctx, tv);
    if (!JS_IsException(ev) && hce_init_slots(ctx, ev, ov, nv) < 0) {
        JS_FreeValue(ctx, ev);
        ev = JS_EXCEPTION;
    }
    JS_FreeValue(ctx, ov);
    JS_FreeValue(ctx, nv);
    return ev;
}

/* DOM §4.5 steps 6-8 overwrite type and isTrusted and unset the initialized flag afterwards, so what a maker
   owes is only "a default instance of this interface" — here, both USVStrings at their un-initialized value. */
JSValue hash_change_event_new(JSContext *ctx)
{
    return hce_new(ctx, "", "", "", /*trusted*/ false);
}

JSValue hash_change_event_new_to_fire(JSContext *ctx, const char *old_url, const char *new_url)
{
    return hce_new(ctx, "hashchange", old_url, new_url, /*trusted*/ true);
}

/* ---- the constructor ----------------------------------------------------------------------------------------
 *
 * `constructor(DOMString type, optional HashChangeEventInit eventInitDict = {})`. HashChangeEventInit INHERITS
 * EventInit, and Web IDL converts a dictionary's members with the INHERITED ones first and each level
 * lexicographically among itself — which is the order this list is in, and the order a page pins by throwing
 * from one member's getter. `newURL` sorts before `oldURL` within the derived dictionary, and both sort after
 * the base's three. Here the two orders happen to agree on the derived pair, which is exactly why THE LEVEL is
 * still declared: it is what states the fact, rather than leaving it to a coincidence of spelling that the next
 * member added to this dictionary would break.
 *
 * BOTH MEMBERS CARRY THE IDL'S OWN `= ""` DEFAULT, so `new HashChangeEvent("hashchange").oldURL` is the empty
 * string without this body deciding anything. */
static const IdlArgType HCE_CTOR_ARGS[2] = { IDL_DOMSTRING, IDL_DICT };
static const IdlDictMember HCE_INIT[] = {
    { "bubbles", IDL_BOOLEAN }, { "cancelable", IDL_BOOLEAN }, { "composed", IDL_BOOLEAN },
    { "newURL", IDL_USVSTRING, false, NULL, 1, NULL, IDL_DEFAULT_STRING, "" },
    { "oldURL", IDL_USVSTRING, false, NULL, 1, NULL, IDL_DEFAULT_STRING, "" },
};

static JSValue js_hce_ctor(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    JSValueConst init = argc > 1 ? argv[1] : JS_UNDEFINED;
    JSValue old_url, new_url, ev;

    (void)magic;
    if (JS_IsUndefined(this_val))
        return JS_ThrowTypeError(ctx, "constructor HashChangeEvent requires 'new'");
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "HashChangeEvent constructor requires a type");
    old_url = idl_dict_get(ctx, init, "oldURL");
    new_url = idl_dict_get(ctx, init, "newURL");
    /* §2.2's constructor steps with THIS interface's prototype — an event the PAGE constructs is untrusted. */
    ev = event_new_derived(ctx, hce_proto(ctx), argv[0],
                           idl_dict_bool(ctx, init, "bubbles"),
                           idl_dict_bool(ctx, init, "cancelable"),
                           idl_dict_bool(ctx, init, "composed"), /*trusted*/ false);
    if (!JS_IsException(ev) && hce_init_slots(ctx, ev, old_url, new_url) < 0) {
        JS_FreeValue(ctx, ev);
        ev = JS_EXCEPTION;
    }
    JS_FreeValue(ctx, old_url);
    JS_FreeValue(ctx, new_url);
    return ev;
}

/* ---- install ------------------------------------------------------------------------------------------------ */

static const JSCFunctionListEntry js_hce_proto[] = {
    JS_CGETSET_MAGIC_DEF("oldURL", js_hce_get, NULL, HCE_OLD_URL),
    JS_CGETSET_MAGIC_DEF("newURL", js_hce_get, NULL, HCE_NEW_URL),
};

void hash_change_event_init(JSContext *ctx)
{
    JSClassDef d = { "HashChangeEvent" };

    DCHECK(!g_ready, "hash_change_event_init ran twice — the interface is declared once per AGENT");
    g_key = JS_NewSymbol(ctx, "hashChangeEventSlots", false);
    CHECK(!JS_IsException(g_key), "the HashChangeEvent slot key allocation failed");
    JS_NewClassID(JS_GetRuntime(ctx), &g_hce_class);
    JS_NewClass(JS_GetRuntime(ctx), g_hce_class, &d);
    g_ctor_stepid = idl_method_id_dict(ctx, HCE_CTOR_ARGS, 2, HCE_INIT,
                                       (int)(sizeof(HCE_INIT) / sizeof(HCE_INIT[0])), js_hce_ctor, 0);
    idl_optional_from(1);                       /* `optional HashChangeEventInit eventInitDict = {}` */
    g_ready = 1;
    realm_declare_intrinsic(hash_change_event_install_protos);
}

void hash_change_event_install_protos(JSContext *ctx)
{
    JSValue proto, prev, base, ctor, global;

    DCHECK(g_ready, "a realm asked for HashChangeEvent before hash_change_event_init declared it");
    prev = JS_GetClassProto(ctx, g_hce_class);
    DCHECK(JS_IsNull(prev), "hash_change_event_install_protos ran twice in one realm");
    JS_FreeValue(ctx, prev);
    base = event_proto(ctx);
    proto = JS_NewObjectProto(ctx, base);
    JS_FreeValue(ctx, base);
    CHECK(!JS_IsException(proto), "HashChangeEvent.prototype could not be allocated");
    idl_interface_tag(ctx, proto, "HashChangeEvent");
    JS_SetPropertyFunctionList(ctx, proto, js_hce_proto, (int)(sizeof(js_hce_proto) / sizeof(js_hce_proto[0])));
    JS_SetClassProto(ctx, g_hce_class, JS_DupValue(ctx, proto));

    /* §3.7.1's interface object, on THIS realm's global — one `HashChangeEvent` per realm, whose `prototype` is
       the prototype this same install just built. It is also what core/events/create_event.c's row asks about:
       §4.5 step 4 refuses an interface the realm does not EXPOSE, so the row and this line are two halves of one
       fact and the factory asserts they agree. */
    ctor = idl_step_constructor(ctx, "HashChangeEvent", 1, g_ctor_stepid);
    CHECK(!JS_IsException(ctor), "the HashChangeEvent interface object could not be allocated");
    JS_SetConstructor(ctx, ctor, proto);
    JS_FreeValue(ctx, proto);
    global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, "HashChangeEvent", ctor);
    JS_FreeValue(ctx, global);
}

void hash_change_event_free(JSContext *ctx)
{
    if (!g_ready) return;
    JS_FreeValue(ctx, g_key);   /* the prototypes are the REALMS' — each is released with its context */
    g_key = JS_UNDEFINED;
    g_ready = 0;
    g_ctor_stepid = -1;
}
