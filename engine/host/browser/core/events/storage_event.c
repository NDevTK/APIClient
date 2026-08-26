/* THE StorageEvent INTERFACE — HTML §12.2.4 "The StorageEvent interface". The IDL, and what its absence cost,
 * are in storage_event.h.
 *
 * §12.2.4's PROSE IS ONE SENTENCE AND IT IS THE WHOLE OF THE BEHAVIOUR: "The key, oldValue, newValue, url, and
 * storageArea attributes must return the values they were initialized to." So this file is a mint, five slots
 * and five readers — and everything interesting about it is WHICH VALUES reach the slots and in what form,
 * which is what the header's note on taint is about.
 *
 * `initStorageEvent` IS NOT A STUB EITHER. §12.2.4 says it "must initialize the event in a manner analogous to
 * the similarly-named initEvent() method", and initEvent's own steps are DOM §2.2's initialise-an-existing-event
 * — including the early return for an event whose dispatch flag is set, which a derived initializer must honour
 * before touching a single slot of its own. That prefix is event_reinit's, shared rather than copied, exactly as
 * core/events/ui_event.c's initUIEvent shares it. */
#include <stdbool.h>

#include "check.h"
#include "quickjs.h"
#include "core/events/event.h"
#include "core/events/storage_event.h"
#include "core/idl_args.h"
#include "core/idl_slots.h"
#include "core/realm.h"
#include "core/storage/storage.h"
#include "solver/concolic.h"

static JSValue   g_key;         /* the private Symbol this interface's own slots hang off */
static JSClassID g_se_class;    /* the class exists for its per-REALM prototype slot; nothing wears it */
static int       g_ready;
static int       g_ctor_stepid = -1;
static int       g_id_init = -1;

static JSValue se_proto(JSContext *ctx)
{
    JSValue proto = JS_GetClassProto(ctx, g_se_class);

    DCHECK(!JS_IsNull(proto),
           "StorageEvent.prototype was asked for in a realm that never ran its per-realm install");
    return proto;   /* OWNED */
}

/* The five attributes, read out of the one slot record. `magic` IS the member, so there is one body rather than
   five copies of the same brand check. */
enum { SE_KEY = 0, SE_OLD_VALUE, SE_NEW_VALUE, SE_URL, SE_STORAGE_AREA, SE_N };
static const char *const SE_SLOT_NAME[SE_N] = { "key", "oldValue", "newValue", "url", "storageArea" };

/* This event's own slot record, or JS_UNDEFINED for anything that is not a StorageEvent. OWNED. */
static JSValue se_slots(JSContext *ctx, JSValueConst ev)
{
    JSAtom k;
    JSValue slots;

    if (!JS_IsObject(ev)) return JS_UNDEFINED;
    k = JS_ValueToAtom(ctx, g_key);
    CHECK(k != JS_ATOM_NULL, "storage event: the slot key could not be interned");
    if (JS_GetOwnSlot(ctx, &slots, ev, k) <= 0) slots = JS_UNDEFINED;
    JS_FreeAtom(ctx, k);
    if (!JS_IsObject(slots)) { JS_FreeValue(ctx, slots); return JS_UNDEFINED; }
    return slots;
}

static JSValue js_se_get(JSContext *ctx, JSValueConst this_val, int magic)
{
    JSValue slots, v;

    DCHECK(g_ready, "a StorageEvent attribute was read before storage_event_init ran");
    DCHECK(magic >= 0 && magic < SE_N,
           "a StorageEvent accessor was installed with a magic this interface has no member for");
    slots = se_slots(ctx, this_val);
    if (!JS_IsObject(slots))
        return JS_ThrowTypeError(ctx, "a StorageEvent attribute was read on something that is not one");
    v = JS_GetPropertyStr(ctx, slots, SE_SLOT_NAME[magic]);
    JS_FreeValue(ctx, slots);
    return v;
}

/* The five own slots, placed on an event whose Event half is already built, or overwritten by the legacy
   initializer. They are VALUES rather than C strings: a constructor's arrive already converted by the IDL
   declaration, and a broadcast's may be unknown external input that must cross as itself (storage_event.h).
   Returns -1 with the throw live. */
static int se_set_slots(JSContext *ctx, JSValueConst ev, JSValueConst key, JSValueConst old_value,
                        JSValueConst new_value, JSValueConst url, JSValueConst storage_area)
{
    JSValue slots = se_slots(ctx, ev);
    JSAtom k;

    if (JS_IsObject(slots)) {                       /* the initializer's path: the record already exists */
        JS_SetPropertyStr(ctx, slots, "key", JS_DupValue(ctx, key));
        JS_SetPropertyStr(ctx, slots, "oldValue", JS_DupValue(ctx, old_value));
        JS_SetPropertyStr(ctx, slots, "newValue", JS_DupValue(ctx, new_value));
        JS_SetPropertyStr(ctx, slots, "url", JS_DupValue(ctx, url));
        JS_SetPropertyStr(ctx, slots, "storageArea", JS_DupValue(ctx, storage_area));
        JS_FreeValue(ctx, slots);
        return 0;
    }
    slots = idl_slots_new(ctx);
    k = JS_ValueToAtom(ctx, g_key);
    if (JS_IsException(slots) || k == JS_ATOM_NULL) {
        JS_FreeValue(ctx, slots);
        if (k != JS_ATOM_NULL) JS_FreeAtom(ctx, k);
        return -1;
    }
    JS_SetPropertyStr(ctx, slots, "key", JS_DupValue(ctx, key));
    JS_SetPropertyStr(ctx, slots, "oldValue", JS_DupValue(ctx, old_value));
    JS_SetPropertyStr(ctx, slots, "newValue", JS_DupValue(ctx, new_value));
    JS_SetPropertyStr(ctx, slots, "url", JS_DupValue(ctx, url));
    JS_SetPropertyStr(ctx, slots, "storageArea", JS_DupValue(ctx, storage_area));
    JS_SetProperty(ctx, (JSValue)ev, k, slots);
    JS_FreeAtom(ctx, k);
    return 0;
}

/* The shared body of the two makers and of the constructor: an event of `type` carrying the five values. */
static JSValue se_new(JSContext *ctx, JSValueConst type, bool bubbles, bool cancelable, bool composed,
                      bool trusted, JSValueConst key, JSValueConst old_value, JSValueConst new_value,
                      JSValueConst url, JSValueConst storage_area)
{
    JSValue ev;

    DCHECK(g_ready, "a StorageEvent was minted before storage_event_init declared the interface");
    ev = event_new_derived(ctx, se_proto(ctx), type, bubbles, cancelable, composed, trusted);
    if (JS_IsException(ev)) return ev;
    if (se_set_slots(ctx, ev, key, old_value, new_value, url, storage_area) < 0) {
        JS_FreeValue(ctx, ev);
        return JS_EXCEPTION;
    }
    return ev;
}

/* DOM §4.5 steps 6-8 overwrite type and isTrusted and unset the initialized flag afterwards, so what a maker
   owes is only "a default instance of this interface" — here, the five attributes at the values §12.2.4's
   dictionary gives them. */
JSValue storage_event_new(JSContext *ctx)
{
    JSValue empty = JS_NewString(ctx, "");
    JSValue ev;

    CHECK(!JS_IsException(empty), "§4.5 createEvent: the default StorageEvent's url could not be allocated");
    ev = se_new(ctx, empty, /*bubbles*/ false, /*cancelable*/ false, /*composed*/ false, /*trusted*/ false,
                JS_NULL, JS_NULL, JS_NULL, empty, JS_NULL);
    JS_FreeValue(ctx, empty);
    return ev;
}

JSValue storage_event_new_to_fire(JSContext *ctx, JSValueConst key, JSValueConst old_value,
                                  JSValueConst new_value, const char *url, JSValueConst storage_area)
{
    JSValue type, uv, ev;

    DCHECK(url != NULL, "a StorageEvent was minted with no url — §12.2.4 types the attribute USVString with an "
                        "empty-string default, so there is no absent value for it to carry, and §12.2.1 step 2 "
                        "computes it as the serialization of the broadcasting Document's URL");
    DCHECK(JS_IsObject(storage_area) && storage_is(storage_area),
           "a StorageEvent was minted with a storageArea that is not a Storage — §12.2.1 step 4 initializes it "
           "to remoteStorage, which is one of the objects broadcast step 3 collected");
    type = JS_NewString(ctx, "storage");
    uv = JS_NewString(ctx, url);
    if (JS_IsException(type) || JS_IsException(uv)) {
        JS_FreeValue(ctx, type);
        JS_FreeValue(ctx, uv);
        return JS_EXCEPTION;
    }
    /* DOM's fire-an-event with none of the three flags set, because §12.2.1 step 4 sets none of them. */
    ev = se_new(ctx, type, /*bubbles*/ false, /*cancelable*/ false, /*composed*/ false, /*trusted*/ true,
                key, old_value, new_value, uv, storage_area);
    JS_FreeValue(ctx, type);
    JS_FreeValue(ctx, uv);
    return ev;
}

/* ---- §12.2.4's legacy initializer ----------------------------------------------------------------------------
 *
 * `initStorageEvent(type, bubbles, cancelable, key, oldValue, newValue, url, storageArea)` — "a manner
 * analogous to the similarly-named initEvent() method", which means DOM §2.2's initialise-an-existing-event and
 * then this interface's five attributes. It is how a bundle old enough to use `createEvent` finishes making one,
 * and without it `createEvent('StorageEvent')` answers an event that can never be dispatched: §4.5 leaves the
 * initialized flag unset and only an initializer sets it. */
static JSValue js_se_init_storage_event(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
                                        int magic)
{
    JSValue slots;
    bool mine;

    (void)magic;
    DCHECK(g_ready, "initStorageEvent ran before storage_event_init declared the interface");
    /* THE BRAND IS THIS INTERFACE'S OWN SLOT RECORD, which every StorageEvent carries and nothing else can
       forge — the same test §3.7.7 "Operations" states, asked of the receiver before any step runs. */
    slots = se_slots(ctx, this_val);
    mine = JS_IsObject(slots);
    JS_FreeValue(ctx, slots);
    if (!mine)
        return JS_ThrowTypeError(ctx, "initStorageEvent called on something that is not a StorageEvent");
    DCHECK(event_is(ctx, this_val), "an object carrying StorageEvent's slot record is not an Event — the record "
                                    "is placed only on an event this component's mint already built");
    /* EVERY POSITION IS THERE, INCLUDING THE SEVEN OPTIONAL ONES: the declaration states §3.6 step 14.2's
       default for each, so the machine PLACES the IDL's value and this body never reads an absence as one.
       A shorter argc means those defaults were not declared. */
    DCHECK(argc >= 8, "initStorageEvent's body ran with fewer positions than its IDL lists — every optional "
                      "argument of this member carries a declared default, so the argument machine materializes "
                      "all eight");
    /* §2.2 step 1's early return, which the derived initializer must honour BEFORE writing a slot of its own:
       an event the dispatch walk currently owns is left exactly as it is. */
    if (!event_reinit(ctx, this_val, argv[0], JS_ToBool(ctx, argv[1]) != 0, JS_ToBool(ctx, argv[2]) != 0))
        return JS_UNDEFINED;
    if (se_set_slots(ctx, this_val, argv[3], argv[4], argv[5], argv[6], argv[7]) < 0)
        return JS_EXCEPTION;
    return JS_UNDEFINED;
}

/* ---- the constructor ----------------------------------------------------------------------------------------
 *
 * `constructor(DOMString type, optional StorageEventInit eventInitDict = {})`. StorageEventInit INHERITS
 * EventInit, and Web IDL §3.2.17 "Dictionary types" converts a dictionary's members with the INHERITED ones
 * first and each level lexicographically among itself — which is the order this list is in, and the order a page
 * pins by throwing from one member's getter. Within the derived level that is key, newValue, oldValue,
 * storageArea, url, which is NOT the order §12.2.4 prints them in; the IDL's print order is not its conversion
 * order, and writing the printed one here would have run a page's getters in the wrong sequence.
 *
 * EVERY MEMBER CARRIES THE IDL'S OWN DEFAULT, so `new StorageEvent("storage").key` is null and `.url` is the
 * empty string without this body deciding anything. */
static const IdlArgType SE_CTOR_ARGS[2] = { IDL_DOMSTRING, IDL_DICT };
static const IdlDictMember SE_INIT[] = {
    { "bubbles", IDL_BOOLEAN }, { "cancelable", IDL_BOOLEAN }, { "composed", IDL_BOOLEAN },
    { "key",         IDL_DOMSTRING_NULLABLE, false, NULL, 1, NULL, IDL_DEFAULT_NULL },
    { "newValue",    IDL_DOMSTRING_NULLABLE, false, NULL, 1, NULL, IDL_DEFAULT_NULL },
    { "oldValue",    IDL_DOMSTRING_NULLABLE, false, NULL, 1, NULL, IDL_DEFAULT_NULL },
    { "storageArea", IDL_INTERFACE_NULLABLE, false, NULL, 1, NULL, IDL_DEFAULT_NULL },
    { "url",         IDL_USVSTRING,          false, NULL, 1, NULL, IDL_DEFAULT_STRING, "" },
};

static JSValue js_se_ctor(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    JSValueConst init = argc > 1 ? argv[1] : JS_UNDEFINED;
    JSValue key, old_value, new_value, url, area, ev;

    (void)magic;
    if (JS_IsUndefined(this_val))
        return JS_ThrowTypeError(ctx, "constructor StorageEvent requires 'new'");
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "StorageEvent constructor requires a type");
    key = idl_dict_get(ctx, init, "key");
    old_value = idl_dict_get(ctx, init, "oldValue");
    new_value = idl_dict_get(ctx, init, "newValue");
    url = idl_dict_get(ctx, init, "url");
    area = idl_dict_get(ctx, init, "storageArea");
    /* THE DECLARATION'S FIVE VALUES, READ AND NEVER DEFAULTED. `optional StorageEventInit eventInitDict = {}`
       means an OMITTED second argument is a dictionary carrying every member's declared default, which the
       argument machine materializes — so an `undefined` here is not "the page wrote nothing", it is the
       conversion having failed to place §12.2.4's own `= null` / `= ""`, and a `||` at this line would turn
       that into a plausible datum. Unknown external input crosses a declared position as itself, which is why
       each test admits a concolic beside the IDL's type. */
    DCHECK(JS_IsNull(key) || JS_IsString(key) || concolic_is(key),
           "StorageEventInit's `key` reached the constructor as neither a string, null, nor unknown external "
           "input — the member is `DOMString? key = null` and its default is the declaration's to place");
    DCHECK(JS_IsNull(old_value) || JS_IsString(old_value) || concolic_is(old_value),
           "StorageEventInit's `oldValue` reached the constructor as neither a string, null, nor unknown "
           "external input — the member is `DOMString? oldValue = null`");
    DCHECK(JS_IsNull(new_value) || JS_IsString(new_value) || concolic_is(new_value),
           "StorageEventInit's `newValue` reached the constructor as neither a string, null, nor unknown "
           "external input — the member is `DOMString? newValue = null`");
    DCHECK(JS_IsString(url) || concolic_is(url),
           "StorageEventInit's `url` reached the constructor as neither a string nor unknown external input — "
           "the member is `USVString url = \"\"`, so an absent one is the empty string the declaration places");
    DCHECK(JS_IsNull(area) || storage_is(area),
           "StorageEventInit's `storageArea` reached the constructor as neither a Storage nor null — the "
           "member is `Storage? storageArea = null` and §3.2.15's brand is the declaration's");
    /* §2.2's constructor steps with THIS interface's prototype — an event the PAGE constructs is untrusted. */
    ev = se_new(ctx, argv[0], idl_dict_bool(ctx, init, "bubbles"), idl_dict_bool(ctx, init, "cancelable"),
                idl_dict_bool(ctx, init, "composed"), /*trusted*/ false,
                key, old_value, new_value, url, area);
    JS_FreeValue(ctx, key);
    JS_FreeValue(ctx, old_value);
    JS_FreeValue(ctx, new_value);
    JS_FreeValue(ctx, url);
    JS_FreeValue(ctx, area);
    return ev;
}

/* ---- install ------------------------------------------------------------------------------------------------ */

static const JSCFunctionListEntry js_se_proto[] = {
    JS_CGETSET_MAGIC_DEF("key", js_se_get, NULL, SE_KEY),
    JS_CGETSET_MAGIC_DEF("oldValue", js_se_get, NULL, SE_OLD_VALUE),
    JS_CGETSET_MAGIC_DEF("newValue", js_se_get, NULL, SE_NEW_VALUE),
    JS_CGETSET_MAGIC_DEF("url", js_se_get, NULL, SE_URL),
    JS_CGETSET_MAGIC_DEF("storageArea", js_se_get, NULL, SE_STORAGE_AREA),
};

void storage_event_init(JSContext *ctx)
{
    /* §12.2.4's `initStorageEvent`, position for position. The seven optional ones each carry §3.6 step 14.2's
       declared default, so an omitted argument arrives as the IDL's value rather than as a hole this body would
       have to fill. */
    static const IdlArgType SE_INIT_ARGS[8] = {
        IDL_DOMSTRING, IDL_BOOLEAN, IDL_BOOLEAN, IDL_DOMSTRING_NULLABLE, IDL_DOMSTRING_NULLABLE,
        IDL_DOMSTRING_NULLABLE, IDL_USVSTRING, IDL_INTERFACE_NULLABLE,
    };
    JSClassDef d = { "StorageEvent" };

    DCHECK(!g_ready, "storage_event_init ran twice — the interface is declared once per AGENT");
    /* §3.2.15's BRAND FOR `Storage? storageArea`, READ HERE rather than assumed. core/platform.c declares the
       `storage` row before the `event` row this interface is declared from, so the class id exists — and this
       is the assert that says so, because a reordering would otherwise brand every storageArea against class 0
       and turn `new StorageEvent("storage", {storageArea: localStorage})` into a TypeError. */
    DCHECK(storage_class_id() != 0,
           "StorageEvent was declared before HTML §12.2.1's Storage class existed — §12.2.4's `storageArea` "
           "brands against it, so core/platform.c's `storage` row must precede its `event` row");
    g_key = JS_NewSymbol(ctx, "storageEventSlots", false);
    CHECK(!JS_IsException(g_key), "the StorageEvent slot key allocation failed");
    JS_NewClassID(JS_GetRuntime(ctx), &g_se_class);
    JS_NewClass(JS_GetRuntime(ctx), g_se_class, &d);

    g_ctor_stepid = idl_method_id_dict(ctx, SE_CTOR_ARGS, 2, SE_INIT,
                                       (int)(sizeof(SE_INIT) / sizeof(SE_INIT[0])), js_se_ctor, 0);
    idl_iface_brand(storage_class_id());        /* StorageEventInit's one interface-typed member */
    idl_optional_from(1);                       /* `optional StorageEventInit eventInitDict = {}` */

    g_id_init = idl_method_id(ctx, SE_INIT_ARGS, 8, js_se_init_storage_event, 0);
    idl_iface_brand(storage_class_id());        /* `optional Storage? storageArea = null` */
    idl_optional_from(1);
    idl_arg_default(1, IDL_DEFAULT_FALSE, NULL);        /* `optional boolean bubbles = false` */
    idl_arg_default(2, IDL_DEFAULT_FALSE, NULL);        /* `optional boolean cancelable = false` */
    idl_arg_default(3, IDL_DEFAULT_NULL, NULL);         /* `optional DOMString? key = null` */
    idl_arg_default(4, IDL_DEFAULT_NULL, NULL);         /* `optional DOMString? oldValue = null` */
    idl_arg_default(5, IDL_DEFAULT_NULL, NULL);         /* `optional DOMString? newValue = null` */
    idl_arg_default(6, IDL_DEFAULT_STRING, "");         /* `optional USVString url = ""` */
    idl_arg_default(7, IDL_DEFAULT_NULL, NULL);         /* `optional Storage? storageArea = null` */
    g_ready = 1;
    realm_declare_intrinsic(storage_event_install_protos);
}

void storage_event_install_protos(JSContext *ctx)
{
    JSValue proto, prev, base, ctor, global;

    DCHECK(g_ready, "a realm asked for StorageEvent before storage_event_init declared it");
    prev = JS_GetClassProto(ctx, g_se_class);
    DCHECK(JS_IsNull(prev), "storage_event_install_protos ran twice in one realm");
    JS_FreeValue(ctx, prev);
    base = event_proto(ctx);
    proto = JS_NewObjectProto(ctx, base);
    JS_FreeValue(ctx, base);
    CHECK(!JS_IsException(proto), "StorageEvent.prototype could not be allocated");
    idl_interface_tag(ctx, proto, "StorageEvent");
    JS_SetPropertyFunctionList(ctx, proto, js_se_proto, (int)(sizeof(js_se_proto) / sizeof(js_se_proto[0])));
    /* Web IDL §3.7.4.1's length is the number of REQUIRED arguments — one, `type`. */
    idl_install_method(ctx, proto, "initStorageEvent", 1, g_id_init);
    JS_SetClassProto(ctx, g_se_class, JS_DupValue(ctx, proto));

    /* §3.7.1's interface object, on THIS realm's global. It is also what core/events/create_event.c's row asks
       about: §4.5 step 4 refuses an interface the realm does not EXPOSE, so the row and this line are two halves
       of one fact and the factory asserts they agree. */
    ctor = idl_step_constructor(ctx, "StorageEvent", 1, g_ctor_stepid);
    CHECK(!JS_IsException(ctor), "the StorageEvent interface object could not be allocated");
    JS_SetConstructor(ctx, ctor, proto);
    JS_FreeValue(ctx, proto);
    global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, "StorageEvent", ctor);
    JS_FreeValue(ctx, global);
}

void storage_event_free(JSContext *ctx)
{
    if (!g_ready) return;
    JS_FreeValue(ctx, g_key);   /* the prototypes are the REALMS' — each is released with its context */
    g_key = JS_UNDEFINED;
    g_ready = 0;
    g_ctor_stepid = -1;
    g_id_init = -1;
}
