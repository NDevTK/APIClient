/* INDEXED DATABASE §4.3's IDBFactory — the door onto the standard.
 *
 *     partial interface mixin WindowOrWorkerGlobalScope {
 *       [SameObject] readonly attribute IDBFactory indexedDB;
 *     };
 *     [Exposed=(Window,Worker)]
 *     interface IDBFactory {
 *       [NewObject] IDBOpenDBRequest open(DOMString name, optional [EnforceRange] unsigned long long version);
 *       [NewObject] IDBOpenDBRequest deleteDatabase(DOMString name);
 *       Promise<sequence<IDBDatabaseInfo>> databases();
 *       short cmp(any first, any second);
 *     };
 *
 * WHAT IS HERE AND WHY IT IS ONLY `cmp`. §4.3's other three members are the DATABASE — `open` runs §5.1's
 * opening algorithm (a connection queue, an upgrade transaction, an `upgradeneeded` event fired at an open
 * request), `deleteDatabase` runs §5.3's, `databases()` reports what §2.1 holds. Every one of those is an
 * ASYNCHRONOUS operation that fires an event at the page, so each is a declared step machine over a request
 * state machine over a transaction state machine over a store that does not exist yet, and writing any of them
 * before those exist would be writing the surface first. They are honestly ABSENT: `indexedDB.open(...)` is a
 * TypeError naming the member, which is the forcing function, and Web IDL is the auditor that lists them.
 *
 * `cmp` IS COMPLETE, and it is complete because it is §2.4's compare exposed and nothing else: "let a be the
 * result of converting a value to a key with first ... return the results of comparing two keys with a and b".
 * It needs no database, no connection, no transaction and no request — so it is exactly the member of this
 * interface that subproblem one finishes, and it is what makes the key ordering something a page (and a test)
 * can read back rather than something only the store's internals would ever exercise.
 *
 * THE INSTANCE IS THE REALM'S. `[SameObject]` means `indexedDB === indexedDB`, and a factory built once into a
 * module static would be ONE realm's object answering every document — §3.7's per-realm rule, which in this
 * engine decides ANSWERS and not just identities, because a C member runs in the realm that DEFINED it. So the
 * object lives in quickjs's own per-context slot (core/realm.h), built eagerly with the realm. */
#include <stdbool.h>

#include "check.h"
#include "quickjs.h"
#include "core/idl_args.h"
#include "core/indexeddb/idb_key.h"
#include "core/indexeddb/indexed_db.h"
#include "core/realm.h"

static JSClassID g_factory_class;
static int       g_obj_slot = -1;
static int       g_id_cmp   = -1;

/* WEB IDL §3.7.5's BRAND CHECK. `IDBFactory.prototype.cmp.call({}, 1, 2)` is a TypeError, and a page tells that
   apart from a "DataError". */
static bool factory_brand(JSContext *ctx, JSValueConst this_val)
{
    DCHECK(g_factory_class != 0, "an IDBFactory member ran before indexed_db_init declared the class");
    if (JS_GetClassID(this_val) == g_factory_class) return true;
    JS_ThrowTypeError(ctx, "an IDBFactory member was reached on something that is not an IDBFactory");
    return false;
}

/* "The cmp(first, second) method steps are: let a be the result of converting a value to a key with first.
   Rethrow any exceptions. If a is 'invalid value' or 'invalid type', throw a 'DataError' DOMException. Let b be
   [the same for second]. Return the results of comparing two keys with a and b."
   THE ORDER IS OBSERVABLE: `cmp(undefined, undefined)` reports the FIRST argument's DataError, and a body that
   converted both before testing either would report the second's. */
static JSValue js_idb_cmp(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    JSValue a, b;
    int r;

    (void)argc; (void)magic;
    if (!factory_brand(ctx, this_val)) return JS_EXCEPTION;
    if (idb_key_from_value(ctx, argv[0], &a) < 0) return JS_EXCEPTION;
    if (idb_key_from_value(ctx, argv[1], &b) < 0) {
        JS_FreeValue(ctx, a);
        return JS_EXCEPTION;
    }
    r = idb_key_compare(ctx, a, b);
    JS_FreeValue(ctx, a);
    JS_FreeValue(ctx, b);
    DCHECK(r >= -1 && r <= 1, "§2.4's compare two keys answered something that is not -1, 0 or 1");
    return JS_NewInt32(ctx, r);   /* `short cmp(...)` — the three values the algorithm returns */
}

/* `[SameObject] readonly attribute IDBFactory indexedDB` — the guarantee comes from the realm slot rather than
   from a cache in the getter, the same way §6.4.4's UserActivation and Storage §2's StorageManager do. */
static JSValue js_idb_get_factory(JSContext *ctx, JSValueConst this_val, int magic)
{
    (void)this_val; (void)magic;
    return realm_value_get(ctx, g_obj_slot);
}

static void indexed_db_install_realm(JSContext *ctx)
{
    JSValue proto, prev, ctor, obj, global;

    DCHECK(g_factory_class != 0, "a realm asked for IDBFactory before the interface was declared");
    prev = JS_GetClassProto(ctx, g_factory_class);
    DCHECK(JS_IsNull(prev), "indexed_db_install_realm ran twice in one realm");
    JS_FreeValue(ctx, prev);
    proto = JS_NewObject(ctx);
    CHECK(!JS_IsException(proto), "IDBFactory.prototype could not be allocated");
    idl_interface_tag(ctx, proto, "IDBFactory");
    idl_install_method(ctx, proto, "cmp", 2, g_id_cmp);
    JS_SetClassProto(ctx, g_factory_class, JS_DupValue(ctx, proto));

    /* §3.7.1's interface object — §4.3 declares no constructor, so it throws a TypeError when called or
       constructed, which is what `new IDBFactory()` must get. */
    ctor = idl_interface_object(ctx, "IDBFactory", proto);
    CHECK(!JS_IsException(ctor), "the IDBFactory interface object could not be allocated");
    global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, "IDBFactory", ctor);

    obj = JS_NewObjectProtoClass(ctx, proto, g_factory_class);
    JS_FreeValue(ctx, proto);
    CHECK(!JS_IsException(obj), "the realm's IDBFactory could not be allocated");
    realm_value_set(ctx, g_obj_slot, obj);

    idl_install_accessor(ctx, global, "indexedDB", js_idb_get_factory, 0, -1);
    JS_FreeValue(ctx, global);
}

void indexed_db_init(JSContext *ctx)
{
    JSClassDef d = { "IDBFactory" };
    static const IdlArgType CMP_ARGS[2] = { IDL_ANY, IDL_ANY };

    DCHECK(g_obj_slot < 0, "indexed_db_init ran twice — the class and the slot are declared once per AGENT");
    JS_NewClassID(JS_GetRuntime(ctx), &g_factory_class);
    CHECK(JS_NewClass(JS_GetRuntime(ctx), g_factory_class, &d) == 0,
          "IDBFactory: the per-realm prototype slot could not be declared");
    g_obj_slot = realm_value_declare(ctx, "Indexed Database §4.3 the realm's IDBFactory");
    g_id_cmp = idl_method_id(ctx, CMP_ARGS, 2, js_idb_cmp, 0);
    realm_declare_intrinsic(indexed_db_install_realm);
}
