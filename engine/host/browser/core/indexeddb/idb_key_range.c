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
#include "quickjs-step.h"
#include "core/idl_args.h"
#include "core/indexeddb/idb_key.h"
#include "core/indexeddb/idb_key_array.h"
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

/* WHICH OF THE TWO BOUNDS A GETTER MEANS. `lower`/`upper` and `lowerOpen`/`upperOpen` are one getter each — so
   the side is a magic rather than a second copy of a body that could drift from its twin. The two CONSTRUCTION
   methods that are also one algorithm with the bounds swapped are magic'd by RANGE_M_* below, which numbers the
   four one-key members rather than the two bounds. */
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

/* ---- §2.9's two algorithms, as the operations over a store are stated in them ------------------------------- */

/* §2.9's STEPS 1-2 — the two arms that convert nothing, and the whole of what this algorithm decides before
   §7.4 runs. 1 with *prange the answer, 0 to go on to step 3, -1 with step 2's "DataError" live. It is one
   function because the C entry and the walk below are two ENTRIES to one algorithm and not two algorithms: a
   second copy of these two sentences is a second place for the unbounded range to be built differently. */
static int range_convert_pre(JSContext *ctx, JSValueConst value, bool null_disallowed, JSValue *prange)
{
    *prange = JS_UNDEFINED;
    /* "If value is a key range, return value." The brand is the class, which is the same question §4.7's
       members ask and the reason a page cannot hand a shape-alike object in. */
    if (JS_GetClassID(value) == g_range_class) {
        *prange = JS_DupValue(ctx, value);
        return 1;
    }
    /* "If value is undefined or is null, then throw a DataError DOMException if null disallowed flag is true,
       or return an UNBOUNDED key range otherwise" — §2.9's unbounded range is the one whose both bounds are
       null, and "All keys are in an unbounded key range", which range_contains answers without a special
       case because it reads a bound only where the bound is not null. */
    if (JS_IsUndefined(value) || JS_IsNull(value)) {
        if (null_disallowed) {
            JS_ThrowDOMException(ctx, "DataError", "this member requires a key or a key range, not null");
            return -1;
        }
        *prange = idb_key_range_new(ctx, JS_NULL, JS_NULL, false, false);
        return 1;
    }
    return 0;
}

/* §2.9's STEP 5: "Return a key range containing only key" — the same two-bounds-equal range §4.7's `only`
   builds, and §2.9's own sentence for why ("a key range containing only key has both lower bound and upper
   bound EQUAL TO key"). `key` is CONSUMED. */
static JSValue range_only(JSContext *ctx, JSValue key)
{
    return idb_key_range_new(ctx, JS_DupValue(ctx, key), key, false, false);
}

JSValue idb_key_range_only_key(JSContext *ctx, JSValueConst key)
{
    DCHECK(JS_IsObject(key), "a key range containing only a key was asked for over something that is not a "
                             "§2.4 key record");
    return range_only(ctx, JS_DupValue(ctx, key));
}

int idb_key_range_from_value(JSContext *ctx, JSValueConst value, bool null_disallowed, JSValue *prange)
{
    JSValue key;
    int pre = range_convert_pre(ctx, value, null_disallowed, prange);

    if (pre != 0)
        return pre < 0 ? -1 : 0;
    if (idb_key_from_value(ctx, value, &key) < 0)   /* STEPS 3-4 */
        return -1;
    *prange = range_only(ctx, key);                 /* STEP 5 */
    return 0;
}

int idb_key_range_walk_start(JSContext *ctx, JSStepHdr *hdr, IdbRangeWalk *rw, JSValueConst value,
                             bool null_disallowed, int base, int after)
{
    int pre;

    /* A ZEROED JSValue IS THE INTEGER 0 AND NOT JS_UNDEFINED, so the slot is PLACED here rather than assumed:
       every later reader of it tells "steps 1-2 answered" from "step 3 decides" by exactly that value. */
    JS_FreeValue(ctx, rw->range);
    rw->range = JS_UNDEFINED;
    pre = range_convert_pre(ctx, value, null_disallowed, &rw->range);
    if (pre < 0)
        return JS_STEP_ABRUPT;
    if (pre > 0) {
        hdr->stage = (uint16_t)after;
        return JS_STEP_YIELD;
    }
    idb_key_walk_start(ctx, hdr, &rw->w, value, base, after);   /* STEP 3 */
    return JS_STEP_YIELD;
}

int idb_key_range_walk_run(JSContext *ctx, JSStepHdr *hdr, IdbRangeWalk *rw, JSValue in, int base,
                           JSValue **out_cb, int *out_argc)
{
    DCHECK(JS_IsUndefined(rw->range), "§2.9's step 3 is running on a record whose steps 1-2 already answered — "
                                      "the conversion is begun only where neither did, and the stage points at "
                                      "the caller's own otherwise");
    return idb_key_walk_run(ctx, hdr, &rw->w, in, base, out_cb, out_argc);
}

void idb_key_range_walk_visit(JSContext *ctx, IdbRangeWalk *rw, JSStepVisit *v)
{
    idb_key_walk_visit(ctx, &rw->w, v);
    v->val(ctx, &rw->range);
}

int idb_key_range_walk_take(JSContext *ctx, IdbRangeWalk *rw, JSValue *prange)
{
    JSValue key;

    DCHECK(JS_IsUndefined(rw->range) || JS_GetClassID(rw->range) == g_range_class,
           "§2.9's answer was taken from a record its own start never placed — a zeroed JSValue is the integer 0 "
           "and not undefined, so a record that skipped idb_key_range_walk_start reads as \"steps 1-2 answered\" "
           "and hands a number back as a key range");
    if (!JS_IsUndefined(rw->range)) {   /* STEP 1's or STEP 2's answer */
        *prange = rw->range;
        rw->range = JS_UNDEFINED;
        return 0;
    }
    *prange = JS_UNDEFINED;
    if (idb_key_walk_take(ctx, &rw->w, &key) < 0)   /* STEP 4 */
        return -1;
    *prange = range_only(ctx, key);                 /* STEP 5 */
    return 0;
}

bool idb_key_range_contains(JSContext *ctx, JSValueConst range, JSValueConst key)
{
    const IdbRangeData *r = range_of(range);

    DCHECK(r != NULL, "§2.9's membership test was asked of something that is not a key range — every range in "
                      "this engine is built by this component, and an operation over a store has converted its "
                      "query through §2.9's convert-a-value-to-a-key-range before it reaches the store");
    return range_contains(ctx, r, key);
}

/* ---- §4.7's four static construction methods, and `includes` ----------------------------------------------- */

/* ALL FIVE ARE MACHINES, BECAUSE §7.4 IS ONE. Every one of them begins "let key be the result of converting a
 * value to a key with <argument>", and where that argument is an Array exotic object the conversion's steps are
 * the PAGE'S OWN CODE — one `? Get` per element over a structure the page sized and nested. So each member
 * declares §7.4's stage block (core/indexeddb/idb_key_array.h) and drives one walk.
 *
 * FOUR OF THEM SHARE ONE DECLARATION, which is what a magic is for: `only`, `lowerBound`, `upperBound` and
 * `includes` are one conversion followed by an O(1) tail, and four copies of the same six-stage block would be
 * four places for §7.4's rest points to drift apart. `bound` has its own, because it converts TWO values and
 * the ORDER of its two refusals is observable. */

/* WHICH OF THE FOUR ONE-ARGUMENT MEMBERS a shared invocation is. Its own enumeration rather than IDB_SIDE_*,
   because that one names §2.9's two BOUNDS and two of these members name neither. */
enum { RANGE_M_ONLY = 0, RANGE_M_LOWER_BOUND, RANGE_M_UPPER_BOUND, RANGE_M_INCLUDES };

#define RG1_STAGES(X) \
    X(RG1_CONVERT, "Indexed Database §4.7 only / lowerBound / upperBound / includes step 1 (let key be the " \
                   "result of converting a value to a key with this member's one argument)") \
    IDB_KEY_ARRAY_ALGO_STAGES(X, RG1_K, "Indexed Database §4.7 only / lowerBound / upperBound / includes step 1") \
    X(RG1_TAKE,    "Indexed Database §4.7 only / lowerBound / upperBound / includes steps 3-4 — ONE O(1) " \
                   "engine action: an \"invalid value\" or \"invalid type\" key is a DataError, and otherwise " \
                   "the member's own answer is built (step 2's rethrow is the conversion's own abrupt " \
                   "completion and rests nowhere)")
enum { IDL_STEP_STAGE_BASE(RG1_STAGES) RG1_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const RG1_STEPS[] = { RG1_STAGES(JS_STEP_STAGE_LABEL) NULL };

#define RGB_STAGES(X) \
    X(RGB_LOWER, "Indexed Database §4.7 bound step 1 (let lowerKey be the result of converting a value to a " \
                 "key with lower)") \
    IDB_KEY_ARRAY_ALGO_STAGES(X, RGB_L, "Indexed Database §4.7 bound step 1") \
    X(RGB_TOOK_LOWER, "Indexed Database §4.7 bound steps 3-4 — ONE O(1) engine action: an invalid lowerKey is " \
                      "a DataError, and upper's conversion begins") \
    IDB_KEY_ARRAY_ALGO_STAGES(X, RGB_U, "Indexed Database §4.7 bound step 4") \
    X(RGB_TOOK_UPPER, "Indexed Database §4.7 bound step 6 (if upperKey is \"invalid value\" or \"invalid " \
                      "type\", throw a DataError)") \
    X(RGB_BUILD, "Indexed Database §4.7 bound steps 7-8 — ONE O(1) engine action beside §2.4's own compare: a " \
                 "lowerKey greater than upperKey is a DataError, and otherwise the key range is created")
enum { IDL_STEP_STAGE_BASE(RGB_STAGES) RGB_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const RGB_STEPS[] = { RGB_STAGES(JS_STEP_STAGE_LABEL) NULL };

typedef struct { IdbKeyWalk w; } IdbRange1State;
typedef struct { IdbKeyWalk w; JSValue lo, hi; } IdbRangeBoundState;

static void js_range_one_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    idb_key_walk_visit(ctx, &((IdbRange1State *)st)->w, v);
}

static void js_range_bound_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    IdbRangeBoundState *s = st;

    idb_key_walk_visit(ctx, &s->w, v);
    v->val(ctx, &s->lo);
    v->val(ctx, &s->hi);
}

/* "The only(value) method steps are: [convert; rethrow; DataError if invalid;] create and return a new key range
 *  containing only key."
 * "The lowerBound(lower, open) method steps are: ... create and return a new key range with lower bound set to
 *  lowerKey, lower open flag set to open, upper bound set to null, and upper open flag set to TRUE" — and the
 *  mirror for upperBound, whose LOWER open flag is the one set to true. The flag on the absent side is not
 *  cosmetic: §2.9's membership test reads it only when the bound is non-null, and §2.10's cursor iteration
 *  compares ranges, so a range that states the standard's value answers those the same way.
 * "The includes(key) method steps are: ... return true if k is in this range, and false otherwise."
 * §2.9: "A key range containing only key has both lower bound and upper bound EQUAL TO key" — one key on both
 * sides, and both open flags false, which is what makes `only(k).includes(k)` true. */
static int js_range_one_key(JSContext *ctx, JSStepHdr *hdr, void *st, int argc, JSValueConst *argv,
                            JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    IdbRange1State *s = st;
    int magic = idl_step_magic(hdr);

    STEP_DISPATCH(RG1_STAGES, hdr->stage, hdr->def->algorithm, JS_STEP_ABRUPT);

    STEP_ARM(RG1_CONVERT);
        JS_FreeValue(ctx, cb_result);
        DCHECK(magic >= RANGE_M_ONLY && magic <= RANGE_M_INCLUDES,
               "an IDBKeyRange one-key member was declared with a magic naming none of §4.7's four");
        /* `includes` is the only one of the four with a receiver, and Web IDL §3.7.5's brand check is asked
           before its step 1 — a page tells that TypeError apart from `false`. */
        if (magic == RANGE_M_INCLUDES && !range_here(ctx, hdr->this_val))
            return JS_STEP_ABRUPT;
        idb_key_walk_start(ctx, hdr, &s->w, argv[0], RG1_K_LENGTH, RG1_TAKE);
        return JS_STEP_YIELD;

    /* STEP 1's conversion, whose six rest points are this member's. Named individually for the reason
       core/dom/node.c names `clone a node`'s: a stage added to the algorithm does not compile until it has an
       arm here, where a negation or a partial list would silently route it into a neighbour. */
    STEP_ARM(RG1_K_LENGTH);
    STEP_ARM(RG1_K_BEGIN);
    STEP_ARM(RG1_K_HOP);
    STEP_ARM(RG1_K_ENTRY);
    STEP_ARM(RG1_K_SUBKEY);
    STEP_ARM(RG1_K_LEAVE);
        return idb_key_walk_run(ctx, hdr, &s->w, cb_result, RG1_K_LENGTH, out_cb, out_argc);

    STEP_ARM(RG1_TAKE);
    {
        JSValue key;
        bool open;

        JS_FreeValue(ctx, cb_result);
        if (idb_key_walk_take(ctx, &s->w, &key) < 0) return JS_STEP_ABRUPT;   /* STEP 3 */
        switch (magic) {                                                      /* STEP 4 */
        case RANGE_M_ONLY:
            *presult = range_only(ctx, key);   /* the same sentence §2.9's step 5 is, written once */
            return JS_STEP_DONE;
        case RANGE_M_LOWER_BOUND:
        case RANGE_M_UPPER_BOUND:
            /* `optional boolean open = false`: the declaration converted a value the page passed, and an absent
               argument IS the IDL's default, which is what ToBoolean of an absent one already answers. */
            open = argc > 1 && JS_ToBool(ctx, argv[1]);
            if (magic == RANGE_M_LOWER_BOUND)
                *presult = idb_key_range_new(ctx, key, JS_NULL, open, true);
            else
                *presult = idb_key_range_new(ctx, JS_NULL, key, true, open);
            return JS_STEP_DONE;
        default: {
            /* STEP 4 is asked of `this` AFTER the conversion, which is where the standard asks it — a fact
               about the range and not about the operation, so it is read here rather than carried. */
            IdbRangeData *r = range_here(ctx, hdr->this_val);
            bool in;

            if (!r) { JS_FreeValue(ctx, key); return JS_STEP_ABRUPT; }
            in = range_contains(ctx, r, key);
            JS_FreeValue(ctx, key);
            *presult = JS_NewBool(ctx, in);
            return JS_STEP_DONE;
        }
        }
    }
}

/* "The bound(lower, upper, lowerOpen, upperOpen) method steps are: [convert lower, rethrow, DataError if
   invalid;] [the same for upper;] if lowerKey is GREATER THAN upperKey, throw a 'DataError' DOMException; create
   and return a new key range ..."
   THE ORDER IS THE SPEC'S AND IS OBSERVABLE: an invalid lower is reported before an invalid upper is even
   looked at, and the greater-than test runs only once both are keys. */
static int js_range_bound(JSContext *ctx, JSStepHdr *hdr, void *st, int argc, JSValueConst *argv,
                          JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    IdbRangeBoundState *s = st;

    STEP_DISPATCH(RGB_STAGES, hdr->stage, hdr->def->algorithm, JS_STEP_ABRUPT);

    STEP_ARM(RGB_LOWER);
        JS_FreeValue(ctx, cb_result);
        s->lo = JS_UNDEFINED;
        s->hi = JS_UNDEFINED;
        idb_key_walk_start(ctx, hdr, &s->w, argv[0], RGB_L_LENGTH, RGB_TOOK_LOWER);
        return JS_STEP_YIELD;

    STEP_ARM(RGB_L_LENGTH);
    STEP_ARM(RGB_L_BEGIN);
    STEP_ARM(RGB_L_HOP);
    STEP_ARM(RGB_L_ENTRY);
    STEP_ARM(RGB_L_SUBKEY);
    STEP_ARM(RGB_L_LEAVE);
        return idb_key_walk_run(ctx, hdr, &s->w, cb_result, RGB_L_LENGTH, out_cb, out_argc);

    STEP_ARM(RGB_TOOK_LOWER);
        JS_FreeValue(ctx, cb_result);
        if (idb_key_walk_take(ctx, &s->w, &s->lo) < 0) return JS_STEP_ABRUPT;   /* STEP 3 */
        idb_key_walk_start(ctx, hdr, &s->w, argv[1], RGB_U_LENGTH, RGB_TOOK_UPPER);   /* STEP 4 */
        return JS_STEP_YIELD;

    STEP_ARM(RGB_U_LENGTH);
    STEP_ARM(RGB_U_BEGIN);
    STEP_ARM(RGB_U_HOP);
    STEP_ARM(RGB_U_ENTRY);
    STEP_ARM(RGB_U_SUBKEY);
    STEP_ARM(RGB_U_LEAVE);
        return idb_key_walk_run(ctx, hdr, &s->w, cb_result, RGB_U_LENGTH, out_cb, out_argc);

    STEP_ARM(RGB_TOOK_UPPER);
        JS_FreeValue(ctx, cb_result);
        if (idb_key_walk_take(ctx, &s->w, &s->hi) < 0) return JS_STEP_ABRUPT;   /* STEP 6 */
        STEP_GOTO(hdr->stage, RGB_BUILD, &hdr->get_phase, &hdr->desc_phase, NULL);
        return JS_STEP_YIELD;

    STEP_ARM(RGB_BUILD);
        JS_FreeValue(ctx, cb_result);
        if (idb_key_compare(ctx, s->lo, s->hi) > 0) {                           /* STEP 7 */
            JS_ThrowDOMException(ctx, "DataError",
                                 "the lower key of an IDBKeyRange is greater than its upper key");
            return JS_STEP_ABRUPT;
        }
        {                                                                       /* STEP 8 */
            bool lower_open = argc > 2 && JS_ToBool(ctx, argv[2]);
            bool upper_open = argc > 3 && JS_ToBool(ctx, argv[3]);
            JSValue lo = s->lo, hi = s->hi;

            s->lo = JS_UNDEFINED;
            s->hi = JS_UNDEFINED;
            *presult = idb_key_range_new(ctx, lo, hi, lower_open, upper_open);
            return JS_STEP_DONE;
        }
}

static const IdlStepDecl RANGE_ONE_KEY_STEP = {
    /* No release: the walk's level stack is idb_key_walk_visit's, and the teardown discharges that one list. */
    js_range_one_key, sizeof(IdbRange1State), js_range_one_visit, NULL,
    "Indexed Database §4.7 IDBKeyRange.only / .lowerBound / .upperBound / .includes", RG1_STEPS
};

static const IdlStepDecl RANGE_BOUND_STEP = {
    js_range_bound, sizeof(IdbRangeBoundState), js_range_bound_visit, NULL,
    "Indexed Database §4.7 IDBKeyRange.bound", RGB_STEPS
};

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

/* `includes` is a MACHINE and lives with the four §7.4 conversions above, whose declaration it shares. */

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
    g_id_only        = idl_method_id_step(ctx, ONE_ANY,    1, NULL, 0, &RANGE_ONE_KEY_STEP, RANGE_M_ONLY);
    g_id_lower_bound = idl_method_id_step(ctx, ANY_OPEN,   2, NULL, 0, &RANGE_ONE_KEY_STEP,
                                         RANGE_M_LOWER_BOUND);
    idl_optional_from(1);
    g_id_upper_bound = idl_method_id_step(ctx, ANY_OPEN,   2, NULL, 0, &RANGE_ONE_KEY_STEP,
                                         RANGE_M_UPPER_BOUND);
    idl_optional_from(1);
    g_id_bound       = idl_method_id_step(ctx, BOUND_ARGS, 4, NULL, 0, &RANGE_BOUND_STEP, 0);
    idl_optional_from(2);
    g_id_includes    = idl_method_id_step(ctx, ONE_ANY,    1, NULL, 0, &RANGE_ONE_KEY_STEP,
                                         RANGE_M_INCLUDES);
    realm_declare_intrinsic(idb_key_range_install_realm);
}
