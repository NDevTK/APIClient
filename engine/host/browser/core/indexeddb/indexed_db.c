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
 * WHAT IS HERE. `open` is §5.1's opening algorithm — a connection queue, an upgrade transaction, an
 * `upgradeneeded` event fired at an open request — and this file holds only its TWO SYNCHRONOUS STEPS, because
 * everything after them is "run these steps in parallel" and belongs to the three step machines
 * core/indexeddb/idb_open.c is. The member is therefore short and the algorithm is not: what is here is the
 * TypeError for a zero version, the storage key, and the IDBOpenDBRequest the page holds while the rest runs.
 *
 * `deleteDatabase` (§5.3) and `databases()` are still ABSENT, and each is a whole algorithm rather than a
 * member: §5.3 is a second walk over the same connection queue with the same `versionchange`/`blocked`
 * machinery and a different ending, and `databases()` is a promise over §2.1's set. Both are honestly absent —
 * `indexedDB.deleteDatabase(...)` is a TypeError naming the member — and Web IDL is the auditor that lists
 * them.
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
#include <math.h>
#include <stdbool.h>

#include "check.h"
#include "quickjs.h"
#include "quickjs-step.h"
#include "core/idl_args.h"
#include "core/indexeddb/idb_key.h"
#include "core/indexeddb/idb_key_array.h"
#include "core/indexeddb/idb_open.h"
#include "core/indexeddb/indexed_db.h"
#include "core/dom/document.h"
#include "core/frame/window_proxy.h"
#include "core/realm.h"
#include "core/url/origin.h"

static JSClassID g_factory_class;
static int       g_obj_slot = -1;
static int       g_id_cmp   = -1;
static int       g_id_open  = -1;

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
 * Rethrow any exceptions. If a is 'invalid value' or 'invalid type', throw a 'DataError' DOMException. Let b be
 * [the same for second]. Return the results of comparing two keys with a and b."
 *
 * THE ORDER IS OBSERVABLE: `cmp(undefined, undefined)` reports the FIRST argument's DataError, and a body that
 * converted both before testing either would report the second's.
 *
 * A MACHINE, because §7.4 IS ONE. `cmp([1, 2], [1, 3])` converts two Array exotic objects, and that arm's steps
 * are the page's own code — one `? Get` per element, on a structure the page sized and nested. So each of this
 * member's two conversions declares §7.4's stage block and drives one walk, and the SECOND reuses the record the
 * first left at rest. */
#define CMP_STAGES(X) \
    X(CMP_FIRST,  "Indexed Database §4.3 cmp step 1 (let a be the result of converting a value to a key with " \
                  "first)") \
    IDB_KEY_ARRAY_ALGO_STAGES(X, CMP_A, "Indexed Database §4.3 cmp step 1") \
    X(CMP_TOOK_A, "Indexed Database §4.3 cmp steps 3-4 — ONE O(1) engine action: an `a` that is \"invalid " \
                  "value\" or \"invalid type\" is a DataError, and b's conversion begins (step 2's rethrow is " \
                  "the conversion's own abrupt completion and rests nowhere)") \
    IDB_KEY_ARRAY_ALGO_STAGES(X, CMP_B, "Indexed Database §4.3 cmp step 4") \
    X(CMP_TOOK_B, "Indexed Database §4.3 cmp step 6 (if b is \"invalid value\" or \"invalid type\", throw a " \
                  "DataError)") \
    X(CMP_RETURN, "Indexed Database §4.3 cmp step 7 (return the results of comparing two keys with a and b)")
enum { IDL_STEP_STAGE_BASE(CMP_STAGES) CMP_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const CMP_STEPS[] = { CMP_STAGES(JS_STEP_STAGE_LABEL) NULL };

typedef struct {
    IdbKeyWalk w;      /* §7.4 in flight — one record, used for `first` and then for `second` */
    JSValue    a, b;   /* the two keys (owned) */
} IdbCmpState;

static void js_idb_cmp_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    IdbCmpState *s = st;

    idb_key_walk_visit(ctx, &s->w, v);
    v->val(ctx, &s->a);
    v->val(ctx, &s->b);
}

static int js_idb_cmp(JSContext *ctx, JSStepHdr *hdr, void *st, int argc, JSValueConst *argv,
                      JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    IdbCmpState *s = st;
    int r;

    (void)argc;
    STEP_DISPATCH(CMP_STAGES, hdr->stage, hdr->def->algorithm, JS_STEP_ABRUPT);

    STEP_ARM(CMP_FIRST);
        JS_FreeValue(ctx, cb_result);
        s->a = JS_UNDEFINED;
        s->b = JS_UNDEFINED;
        if (!factory_brand(ctx, hdr->this_val)) return JS_STEP_ABRUPT;
        idb_key_walk_start(ctx, hdr, &s->w, argv[0], CMP_A_LENGTH, CMP_TOOK_A);
        return JS_STEP_YIELD;

    /* STEP 1's conversion, whose six rest points are this member's. Named individually for the reason
       core/dom/node.c names `clone a node`'s: a stage added to the algorithm does not compile until it has an
       arm here, where a negation or a partial list would silently route it into a neighbour. */
    STEP_ARM(CMP_A_LENGTH);
    STEP_ARM(CMP_A_BEGIN);
    STEP_ARM(CMP_A_HOP);
    STEP_ARM(CMP_A_ENTRY);
    STEP_ARM(CMP_A_SUBKEY);
    STEP_ARM(CMP_A_LEAVE);
        return idb_key_walk_run(ctx, hdr, &s->w, cb_result, CMP_A_LENGTH, out_cb, out_argc);

    STEP_ARM(CMP_TOOK_A);
        JS_FreeValue(ctx, cb_result);
        if (idb_key_walk_take(ctx, &s->w, &s->a) < 0) return JS_STEP_ABRUPT;   /* STEP 3 */
        idb_key_walk_start(ctx, hdr, &s->w, argv[1], CMP_B_LENGTH, CMP_TOOK_B);   /* STEP 4 */
        return JS_STEP_YIELD;

    STEP_ARM(CMP_B_LENGTH);
    STEP_ARM(CMP_B_BEGIN);
    STEP_ARM(CMP_B_HOP);
    STEP_ARM(CMP_B_ENTRY);
    STEP_ARM(CMP_B_SUBKEY);
    STEP_ARM(CMP_B_LEAVE);
        return idb_key_walk_run(ctx, hdr, &s->w, cb_result, CMP_B_LENGTH, out_cb, out_argc);

    STEP_ARM(CMP_TOOK_B);
        JS_FreeValue(ctx, cb_result);
        if (idb_key_walk_take(ctx, &s->w, &s->b) < 0) return JS_STEP_ABRUPT;   /* STEP 6 */
        STEP_GOTO(hdr->stage, CMP_RETURN, &hdr->get_phase, &hdr->desc_phase, NULL);
        return JS_STEP_YIELD;

    STEP_ARM(CMP_RETURN);
        JS_FreeValue(ctx, cb_result);
        r = idb_key_compare(ctx, s->a, s->b);                                  /* STEP 7 */
        DCHECK(r >= -1 && r <= 1, "§2.4's compare two keys answered something that is not -1, 0 or 1");
        *presult = JS_NewInt32(ctx, r);   /* `short cmp(...)` — the three values the algorithm returns */
        return JS_STEP_DONE;
}

static const IdlStepDecl CMP_STEP = {
    /* No release: the walk's level stack is idb_key_walk_visit's, and the teardown discharges that one list. */
    js_idb_cmp, sizeof(IdbCmpState), js_idb_cmp_visit, NULL,
    "Indexed Database §4.3 IDBFactory.cmp", CMP_STEPS
};

/* "The open(name, version) method steps are: if version is 0 (zero), throw a TypeError. Let environment be
   this's relevant settings object. Let storageKey be the result of running obtain a storage key given
   environment. If failure is returned, then throw a SecurityError DOMException and abort these steps. Let
   request be a new open request. Run these steps in parallel: ... Return a new IDBOpenDBRequest object for
   request."
   THE ZERO IS A TypeError AND NOT A DOMException, which is the one thing about this member a page can tell
   apart from every other refusal in this standard — it is a Web IDL-level rejection of the argument, reported
   before any storage is touched. `[EnforceRange] unsigned long long` is the declaration, so a negative or
   non-integral version is already a TypeError before this body runs. */
static JSValue js_idb_open(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    JSValue req;
    const char *name;
    double version = 0;
    bool has_version = argc > 1 && !JS_IsUndefined(argv[1]);

    (void)magic;
    if (!factory_brand(ctx, this_val)) return JS_EXCEPTION;
    if (has_version) {
        /* The declaration has already run ToNumber; what is left of `[EnforceRange] unsigned long long` is its
           RANGE — a fractional, negative, non-finite or too-large version is a TypeError, never a clamp and
           never a modulo. */
        int r = JS_ToFloat64(ctx, &version, argv[1]);

        DCHECK(r >= 0, "§4.3's `open` was handed a version its own declaration did not convert to a number");
        (void)r;
        if (!isfinite(version) || version != trunc(version) || version < 0 ||
            version > 18446744073709551615.0)
            return JS_ThrowTypeError(ctx, "the database version is outside the range of an unsigned long long");
        /* §4.3 step 1: "If version is 0 (zero), throw a TypeError." */
        if (version == 0)
            return JS_ThrowTypeError(ctx, "the database version may not be 0");
    }
    /* "Obtain a storage key given environment. If failure is returned, then throw a SecurityError." An OPAQUE
       ORIGIN has no storage key, so a sandboxed document must not reach this agent's databases — the same
       refusal core/file/storage_manager.c makes for the bucket map, asked the same way (§7.1.1 step 1 makes an
       origin same origin with ITSELF, opaque included, so the same-origin check is the WRONG predicate). */
    if (origin_is_opaque(window_proxy_origin(document_window_proxy(ctx))))
        return JS_ThrowDOMException(ctx, "SecurityError",
                                    "a document with an opaque origin has no storage key");
    name = JS_ToCString(ctx, argv[0]);
    if (name == NULL) return JS_EXCEPTION;
    req = idb_open_request(ctx, name, version, has_version);
    JS_FreeCString(ctx, name);
    return req;
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
    idl_install_method(ctx, proto, "open", 1, g_id_open);
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
    static const IdlArgType OPEN_ARGS[2] = { IDL_DOMSTRING, IDL_UNRESTRICTED_DOUBLE };

    DCHECK(g_obj_slot < 0, "indexed_db_init ran twice — the class and the slot are declared once per AGENT");
    JS_NewClassID(JS_GetRuntime(ctx), &g_factory_class);
    CHECK(JS_NewClass(JS_GetRuntime(ctx), g_factory_class, &d) == 0,
          "IDBFactory: the per-realm prototype slot could not be declared");
    g_obj_slot = realm_value_declare(ctx, "Indexed Database §4.3 the realm's IDBFactory");
    g_id_cmp = idl_method_id_step(ctx, CMP_ARGS, 2, NULL, 0, &CMP_STEP, 0);
    /* `open(DOMString name, optional [EnforceRange] unsigned long long version)`, as the composition
       core/streams/readable_stream.c states the same extended attribute with: the DECLARATION performs
       §3.2.7's ToNumber — which is the page's `valueOf` and therefore has to be a request rather than a read
       from C — and the BODY performs [EnforceRange]'s test on the double it produced. Declaring plain
       IDL_UNSIGNED_LONG_LONG instead would be the WRONG type: that conversion is a modulo, so
       `open("db", -1)` would open version 18446744073709551615 where every browser throws. */
    g_id_open = idl_method_id(ctx, OPEN_ARGS, 2, js_idb_open, 0);
    idl_optional_from(1);
    realm_declare_intrinsic(indexed_db_install_realm);
}
