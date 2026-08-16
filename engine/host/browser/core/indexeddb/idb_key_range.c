/* INDEXED DATABASE §2.9's KEY RANGE, and §4.7's IDBKeyRange over it.
 *
 * WHY THIS IS SUBPROBLEM TWO. A key range is "a continuous interval over some data type used for keys" and
 * nothing else: its two bounds ARE keys, its membership test IS §2.4's compare, and it needs no database, no
 * connection, no transaction and no request to be complete. Every retrieval operation the standard defines
 * takes one — §6.2's "the first record in store's list of records whose key is in RANGE", §6.4's delete, §6.5's
 * count, §6.7's cursor iteration, and §4.5's `get`/`getKey`/`delete`/`count` each convert their `query`
 * argument to a key range before they do anything else. So the store's operations cannot be written before
 * this exists, which is why it comes before the store rather than after it.
 *
 *     [Exposed=(Window,Worker)]
 *     interface IDBKeyRange {
 *       readonly attribute any lower;
 *       readonly attribute any upper;
 *       readonly attribute boolean lowerOpen;
 *       readonly attribute boolean upperOpen;
 *       [NewObject] static IDBKeyRange only(any value);
 *       [NewObject] static IDBKeyRange lowerBound(any lower, optional boolean open = false);
 *       [NewObject] static IDBKeyRange upperBound(any upper, optional boolean open = false);
 *       [NewObject] static IDBKeyRange bound(any lower, any upper,
 *                                            optional boolean lowerOpen = false,
 *                                            optional boolean upperOpen = false);
 *       boolean includes(any key);
 *     };
 *
 * THE ROUND TRIP IS THE TEST THAT THIS IS COMPLETE, and it is page-visible: `IDBKeyRange.only(v).lower` runs
 * §7.4 on the way in and §7.3 on the way out and must answer with `v` again, for a number, a string, a Date and
 * an ArrayBuffer alike — while `IDBKeyRange.only(v).includes(v)` runs §2.4's compare over the same key. A key
 * conversion that is right in one direction and wrong in the other has nowhere to hide from those two lines.
 *
 * THE RECORD IS THE COMPONENT'S OWN AND IT TIME-TRAVELS. Two owned JSValues (the bounds, each a key record or
 * null) and two flags live behind the class opaque, so the capture goes in the ACCESSOR every member reaches —
 * a record a flow has reached is one it may write, and there is then no write site left to miss. §4.7 declares
 * every member of this interface readonly, so today there is no write; the capture is what makes that a fact
 * nothing has to re-check the day a member is added. The offset list is the SAME list the finalizer frees and
 * the gc_mark walks, which is what makes a field added to one and not the others impossible to miss. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include "check.h"
#include "quickjs.h"
#include "core/idl_args.h"
#include "core/indexeddb/idb_key.h"
#include "core/indexeddb/idb_key_range.h"
#include "core/realm.h"
#include "solver/cow.h"

/* §2.9's four fields: "an associated lower bound (null or a key)", "an associated upper bound (null or a
   key)", "an associated lower open flag", "an associated upper open flag". */
typedef struct IdbRangeData {
    JSValue lower;        /* a key record, or JS_NULL — OWNED */
    JSValue upper;        /* a key record, or JS_NULL — OWNED */
    uint8_t lower_open;
    uint8_t upper_open;
} IdbRangeData;

static const uint16_t RANGE_VALS[] = { (uint16_t)offsetof(IdbRangeData, lower),
                                       (uint16_t)offsetof(IdbRangeData, upper) };
static const CowRecord RANGE_REC = { sizeof(IdbRangeData), RANGE_VALS, 2 };

static JSClassID g_range_class;
static int g_id_only        = -1;
static int g_id_lower_bound = -1;
static int g_id_upper_bound = -1;
static int g_id_bound       = -1;
static int g_id_includes    = -1;

/* WHICH OF THE TWO BOUNDS A MEMBER MEANS. `lowerBound` and `upperBound` are one algorithm with the bounds
   swapped, and `lower`/`upper` and `lowerOpen`/`upperOpen` are one getter each — so the side is a magic rather
   than a second copy of a body that could drift from its twin. */
enum { IDB_SIDE_LOWER = 0, IDB_SIDE_UPPER };

static IdbRangeData *range_of(JSValueConst v)
{
    IdbRangeData *r = JS_GetOpaque(v, g_range_class);

    if (r) cow_capture_host_record(v, r, &RANGE_REC);
    return r;
}

/* WEB IDL §3.7.5's BRAND CHECK. `IDBKeyRange.prototype.includes.call({}, 1)` is a TypeError, and a page tells
   that apart from `false`. */
static IdbRangeData *range_here(JSContext *ctx, JSValueConst v)
{
    IdbRangeData *r = range_of(v);

    if (!r) {
        JS_ThrowTypeError(ctx, "an IDBKeyRange member was reached on something that is not an IDBKeyRange");
        return NULL;
    }
    return r;
}

static void range_finalizer(JSRuntime *rt, JSValue val)
{
    IdbRangeData *r = JS_GetOpaque(val, g_range_class);

    if (!r) return;
    JS_FreeValueRT(rt, r->lower);
    JS_FreeValueRT(rt, r->upper);
    free(r);
}

static void range_gc_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func)
{
    IdbRangeData *r = JS_GetOpaque(val, g_range_class);

    if (!r) return;
    JS_MarkValue(rt, r->lower, mark_func);
    JS_MarkValue(rt, r->upper, mark_func);
}

/* §2.9's "create a new key range". `lower` and `upper` are CONSUMED — each a key record or JS_NULL. */
static JSValue idb_key_range_new(JSContext *ctx, JSValue lower, JSValue upper, bool lower_open, bool upper_open)
{
    JSValue proto, obj;
    IdbRangeData *r;

    DCHECK(g_range_class != 0, "a key range was built before idb_key_range_init declared the interface");
    proto = JS_GetClassProto(ctx, g_range_class);
    DCHECK(!JS_IsNull(proto), "a key range was built in a realm with no IDBKeyRange.prototype");
    obj = JS_NewObjectProtoClass(ctx, proto, g_range_class);
    JS_FreeValue(ctx, proto);
    CHECK(!JS_IsException(obj), "IndexedDB: an IDBKeyRange could not be allocated");
    r = calloc(1, sizeof *r);
    CHECK(r != NULL, "IndexedDB: a key range's record allocation failed");
    r->lower = lower;
    r->upper = upper;
    r->lower_open = lower_open ? 1 : 0;
    r->upper_open = upper_open ? 1 : 0;
    JS_SetOpaque(obj, r);
    return obj;
}

/* §2.9's "A key is IN a key range range if both of the following conditions are fulfilled: the range's lower
   bound is null, or it is less than key, or it is both equal to key and the range's lower open flag is false;
   [and the mirror for the upper bound]." Written as the standard states it — as the two conditions rather than
   as an interval arithmetic that would have to be re-derived to be checked. */
static bool range_contains(JSContext *ctx, const IdbRangeData *r, JSValueConst key)
{
    int c;

    if (!JS_IsNull(r->lower)) {
        c = idb_key_compare(ctx, r->lower, key);
        if (!(c < 0 || (c == 0 && !r->lower_open))) return false;
    }
    if (!JS_IsNull(r->upper)) {
        c = idb_key_compare(ctx, r->upper, key);
        if (!(c > 0 || (c == 0 && !r->upper_open))) return false;
    }
    return true;
}

/* ---- §4.7's four static construction methods --------------------------------------------------------------- */

/* "The only(value) method steps are: let key be the result of converting a value to a key with value. Rethrow
   any exceptions. If key is 'invalid value' or 'invalid type', throw a 'DataError' DOMException. Create and
   return a new key range containing only key."
   §2.9: "A key range containing only key has both lower bound and upper bound EQUAL TO key" — one key on both
   sides, and both open flags false, which is what makes `only(k).includes(k)` true. */
static JSValue js_range_only(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    JSValue key;

    (void)this_val; (void)argc; (void)magic;
    if (idb_key_from_value(ctx, argv[0], &key) < 0) return JS_EXCEPTION;
    return idb_key_range_new(ctx, JS_DupValue(ctx, key), key, false, false);
}

/* "The lowerBound(lower, open) method steps ... create and return a new key range with lower bound set to
   lowerKey, lower open flag set to open, upper bound set to null, and upper open flag set to TRUE" — and the
   mirror for upperBound, whose LOWER open flag is the one set to true. The flag on the absent side is not
   cosmetic: §2.9's membership test reads it only when the bound is non-null, and §2.10's cursor iteration
   compares ranges, so a range that states the standard's value is a range that answers those the same way. */
static JSValue js_range_one_bound(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    JSValue key;
    bool open;

    (void)this_val;
    DCHECK(magic == IDB_SIDE_LOWER || magic == IDB_SIDE_UPPER,
           "an IDBKeyRange bound constructor was declared with a magic naming neither of §2.9's two bounds");
    if (idb_key_from_value(ctx, argv[0], &key) < 0) return JS_EXCEPTION;
    /* `optional boolean open = false`: the declaration converted a value the page passed, and an absent
       argument IS the IDL's default, which is what ToBoolean of an absent one already answers. */
    open = argc > 1 && JS_ToBool(ctx, argv[1]);
    if (magic == IDB_SIDE_LOWER)
        return idb_key_range_new(ctx, key, JS_NULL, open, true);
    return idb_key_range_new(ctx, JS_NULL, key, true, open);
}

/* "The bound(lower, upper, lowerOpen, upperOpen) method steps are: [convert lower, throw DataError if invalid;
   convert upper, throw DataError if invalid;] if lowerKey is GREATER THAN upperKey, throw a 'DataError'
   DOMException; create and return a new key range ..."
   THE ORDER IS THE SPEC'S AND IS OBSERVABLE: an invalid lower is reported before an invalid upper is even
   looked at, and the greater-than test runs only once both are keys. */
static JSValue js_range_bound(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    JSValue lo, hi;
    bool lower_open, upper_open;

    (void)this_val; (void)magic;
    if (idb_key_from_value(ctx, argv[0], &lo) < 0) return JS_EXCEPTION;
    if (idb_key_from_value(ctx, argv[1], &hi) < 0) {
        JS_FreeValue(ctx, lo);
        return JS_EXCEPTION;
    }
    if (idb_key_compare(ctx, lo, hi) > 0) {
        JS_FreeValue(ctx, lo);
        JS_FreeValue(ctx, hi);
        JS_ThrowDOMException(ctx, "DataError", "the lower key of an IDBKeyRange is greater than its upper key");
        return JS_EXCEPTION;
    }
    lower_open = argc > 2 && JS_ToBool(ctx, argv[2]);
    upper_open = argc > 3 && JS_ToBool(ctx, argv[3]);
    return idb_key_range_new(ctx, lo, hi, lower_open, upper_open);
}

/* ---- §4.7's instance members --------------------------------------------------------------------------------- */

/* "The lower getter steps are to return the result of converting a key to a value with this's lower bound if it
   is not null, or undefined otherwise" — and the mirror for `upper`. */
static JSValue js_range_bound_get(JSContext *ctx, JSValueConst this_val, int magic)
{
    IdbRangeData *r = range_here(ctx, this_val);
    JSValueConst bound;

    if (!r) return JS_EXCEPTION;
    DCHECK(magic == IDB_SIDE_LOWER || magic == IDB_SIDE_UPPER,
           "an IDBKeyRange bound getter was installed with a magic naming neither of §2.9's two bounds");
    bound = magic == IDB_SIDE_LOWER ? r->lower : r->upper;
    if (JS_IsNull(bound)) return JS_UNDEFINED;
    return idb_key_to_value(ctx, bound);
}

/* "The lowerOpen getter steps are to return this's lower open flag" — and the mirror for `upperOpen`. */
static JSValue js_range_open_get(JSContext *ctx, JSValueConst this_val, int magic)
{
    IdbRangeData *r = range_here(ctx, this_val);

    if (!r) return JS_EXCEPTION;
    DCHECK(magic == IDB_SIDE_LOWER || magic == IDB_SIDE_UPPER,
           "an IDBKeyRange open-flag getter was installed with a magic naming neither of §2.9's two bounds");
    return JS_NewBool(ctx, magic == IDB_SIDE_LOWER ? r->lower_open : r->upper_open);
}

/* "The includes(key) method steps are: let k be the result of converting a value to a key with key. Rethrow any
   exceptions. If k is 'invalid value' or 'invalid type', throw a 'DataError' DOMException. Return true if k is
   in this range, and false otherwise." */
static JSValue js_range_includes(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    IdbRangeData *r = range_here(ctx, this_val);
    JSValue key;
    bool in;

    (void)argc; (void)magic;
    if (!r) return JS_EXCEPTION;
    if (idb_key_from_value(ctx, argv[0], &key) < 0) return JS_EXCEPTION;
    in = range_contains(ctx, r, key);
    JS_FreeValue(ctx, key);
    return JS_NewBool(ctx, in);
}

/* ---- the declaration and the per-realm install --------------------------------------------------------------- */

static void idb_key_range_install_realm(JSContext *ctx)
{
    JSValue proto, prev, ctor, global;

    DCHECK(g_range_class != 0, "a realm asked for IDBKeyRange before the interface was declared");
    prev = JS_GetClassProto(ctx, g_range_class);
    DCHECK(JS_IsNull(prev), "idb_key_range_install_realm ran twice in one realm");
    JS_FreeValue(ctx, prev);
    proto = JS_NewObject(ctx);
    CHECK(!JS_IsException(proto), "IDBKeyRange.prototype could not be allocated");
    idl_interface_tag(ctx, proto, "IDBKeyRange");
    idl_install_accessor(ctx, proto, "lower",     js_range_bound_get, IDB_SIDE_LOWER, -1);
    idl_install_accessor(ctx, proto, "upper",     js_range_bound_get, IDB_SIDE_UPPER, -1);
    idl_install_accessor(ctx, proto, "lowerOpen", js_range_open_get,  IDB_SIDE_LOWER, -1);
    idl_install_accessor(ctx, proto, "upperOpen", js_range_open_get,  IDB_SIDE_UPPER, -1);
    idl_install_method(ctx, proto, "includes", 1, g_id_includes);
    JS_SetClassProto(ctx, g_range_class, JS_DupValue(ctx, proto));

    /* §3.7.1's INTERFACE OBJECT — §4.7 declares no constructor, so `new IDBKeyRange()` is a TypeError, which
       is also what tells a feature-detecting bundle the interface EXISTS. The four construction methods are
       STATIC, so §3.7.2 puts them on this object and not on the prototype. */
    ctor = idl_interface_object(ctx, "IDBKeyRange", proto);
    CHECK(!JS_IsException(ctor), "the IDBKeyRange interface object could not be allocated");
    JS_FreeValue(ctx, proto);
    idl_install_method(ctx, ctor, "only",       1, g_id_only);
    idl_install_method(ctx, ctor, "lowerBound", 1, g_id_lower_bound);
    idl_install_method(ctx, ctor, "upperBound", 1, g_id_upper_bound);
    idl_install_method(ctx, ctor, "bound",      2, g_id_bound);
    global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, "IDBKeyRange", ctor);
    JS_FreeValue(ctx, global);
}

void idb_key_range_init(JSContext *ctx)
{
    JSClassDef d = { "IDBKeyRange", range_finalizer, range_gc_mark };
    static const IdlArgType ONE_ANY[1]    = { IDL_ANY };
    static const IdlArgType ANY_OPEN[2]   = { IDL_ANY, IDL_BOOLEAN };
    static const IdlArgType BOUND_ARGS[4] = { IDL_ANY, IDL_ANY, IDL_BOOLEAN, IDL_BOOLEAN };

    DCHECK(g_range_class == 0, "idb_key_range_init ran twice — the interface is declared once per AGENT, and a "
                               "second class id would leave every range already built branded with the first");
    JS_NewClassID(JS_GetRuntime(ctx), &g_range_class);
    CHECK(JS_NewClass(JS_GetRuntime(ctx), g_range_class, &d) == 0,
          "IDBKeyRange: the class could not be declared");
    g_id_only        = idl_method_id(ctx, ONE_ANY,    1, js_range_only,      0);
    g_id_lower_bound = idl_method_id(ctx, ANY_OPEN,   2, js_range_one_bound, IDB_SIDE_LOWER);
    idl_optional_from(1);
    g_id_upper_bound = idl_method_id(ctx, ANY_OPEN,   2, js_range_one_bound, IDB_SIDE_UPPER);
    idl_optional_from(1);
    g_id_bound       = idl_method_id(ctx, BOUND_ARGS, 4, js_range_bound,     0);
    idl_optional_from(2);
    g_id_includes    = idl_method_id(ctx, ONE_ANY,    1, js_range_includes,  0);
    realm_declare_intrinsic(idb_key_range_install_realm);
}
