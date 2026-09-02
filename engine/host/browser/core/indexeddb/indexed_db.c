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
 * `deleteDatabase` is the SAME two synchronous steps minus the version — the storage key and the request — and
 * for the same reason its algorithm is not here: §5.3 is a second walk over the SAME §2.8.2 connection queue
 * with the same `versionchange`/`blocked` machinery and a different ending, so it lives beside §5.1 in
 * core/indexeddb/idb_open.c and this file holds only the member. That siblinghood is not cosmetic: the two
 * share a queue, so `deleteDatabase(n)` followed by `open(n, 1)` is an ordering the standard guarantees, and
 * it is the first two statements of half this standard's own fixtures.
 *
 * `databases()` IS a member of this file and not a whole algorithm, for the reason its own section states: it
 * needs neither the queue nor a request, because there is no connection to wait for and nothing to block on.
 * What it needs instead is a promise and a TASK BOUNDARY, since the standard reads §2.1's set after the member
 * has returned.
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
#include "core/agent_state.h"
#include "quickjs.h"
#include "quickjs-step.h"
#include "core/idl_args.h"
#include "core/indexeddb/idb_database.h"
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
static int       g_id_delete = -1;
static int       g_id_databases = -1;
static int       g_id_databases_task = -1;

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

/* "The deleteDatabase(name) method steps are: let environment be this's relevant settings object. Let
 * storageKey be the result of running obtain a storage key given environment. If failure is returned, then
 * throw a SecurityError DOMException and abort these steps. Let request be a new open request. Run these steps
 * in parallel: ... Return a new IDBOpenDBRequest object for request."
 *
 * IT IS SHORTER THAN `open` BY EXACTLY ITS VERSION. There is no zero to refuse, no [EnforceRange] range to
 * test, and no ToNumber to perform — so nothing here runs the page's code and this is a plain member rather
 * than a machine. What is left is the storage key and the request, and everything after them is §5.3, which is
 * core/indexeddb/idb_open.c because it is processed in the same §2.8.2 connection queue `open` is. */
static JSValue js_idb_delete_database(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
                                      int magic)
{
    JSValue req;
    const char *name;

    (void)argc; (void)magic;
    if (!factory_brand(ctx, this_val)) return JS_EXCEPTION;
    /* The same refusal `open` makes and for the same reason: an OPAQUE ORIGIN has no storage key, so a
       sandboxed document must not reach — or empty — this agent's databases. */
    if (origin_is_opaque(window_proxy_origin(document_window_proxy(ctx))))
        return JS_ThrowDOMException(ctx, "SecurityError",
                                    "a document with an opaque origin has no storage key");
    name = JS_ToCString(ctx, argv[0]);
    if (name == NULL) return JS_EXCEPTION;
    req = idb_delete_request(ctx, name);
    JS_FreeCString(ctx, name);
    return req;
}

/* ---- §4.3's databases() ------------------------------------------------------------------------------------
 *
 * "Let environment be this's relevant settings object. Let storageKey be the result of running obtain a storage
 * key given environment. If failure is returned, then return a promise rejected with a SecurityError
 * DOMException. Let p be a new promise. Run these steps in parallel: ... Return p."
 *
 * IT IS THE ONE MEMBER OF §4.3 THAT NEEDS NEITHER THE CONNECTION QUEUE NOR A REQUEST. There is no connection to
 * wait for, no `versionchange` to fire and nothing to block on — it reads §2.1's set and answers — so it is a
 * member here rather than an algorithm in idb_open.c, and filing it in §2.8.2's queue would give it an ordering
 * against opens that §4.3 pointedly does not give it ("there are no guarantees about the sequencing of the
 * collection of the data ... with respect to requests to create, upgrade, or delete databases").
 *
 * TWO MACHINES because the standard puts a task boundary in the middle of one member. Step 4 is "in parallel"
 * and its last step is "queue a database task to resolve p with result", so the SET IS READ AFTER THE MEMBER
 * HAS RETURNED — the snapshot the page gets is of the moment the task ran, not of the call. Collapsing that
 * into the member would resolve the promise from a set read one turn too early, which is precisely the
 * sequencing the note above says is a snapshot. */

#define DBT_STAGES(X) \
    X(DBT_COLLECT, "Indexed Database §4.3 databases() steps 4.1-4.3 — ONE O(1) engine action: the set of " \
                   "databases in this storage key becomes a list of IDBDatabaseInfo, skipping every database " \
                   "whose version is 0") \
    X(DBT_RESOLVE, "Indexed Database §4.3 databases() step 4.4 (queue a database task to resolve p with " \
                   "result)")
enum { DBT_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const DBT_STEPS[] = { DBT_STAGES(JS_STEP_STAGE_LABEL) NULL };

typedef struct {
    JSStepHdr hdr;        /* FIRST: the driver writes the def and the operand bounds through it */
    uint8_t   started;    /* a step state is js_mallocz'd and a zeroed JSValue is the INTEGER 0 */
    uint8_t   phase;      /* the resolve call's own cursor */
    JSValue   result;     /* step 4.2's list (owned) */
    JSValue   cb[3];      /* the resolve call's operand buffer: [this, func, result] */
} IdbDatabasesTask;

static void js_idb_databases_task_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    IdbDatabasesTask *s = st;
    int i;

    v->val(ctx, &s->result);
    STEP_CB_FOREACH(s->cb, i) v->val(ctx, &s->cb[i]);
}

static int js_idb_databases_task(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    IdbDatabasesTask *s = st;
    JSValueConst resolve = JS_StepClosureData(&s->hdr, 0);
    int r;

    if (!s->started) {
        int i;

        s->started = 1;
        s->result = JS_UNDEFINED;
        STEP_CB_FOREACH(s->cb, i) s->cb[i] = JS_UNDEFINED;
    }
    STEP_DISPATCH(DBT_STAGES, s->hdr.stage, s->hdr.def->algorithm, JS_STEP_ABRUPT);

    STEP_ARM(DBT_COLLECT);
        JS_FreeValue(ctx, cb_result);
        {
            /* Step 4.1: "let databases be the set of databases in storageKey. If this cannot be determined for
               any reason, then ... reject p with an appropriate error." There is no reason it cannot be: the
               set is this agent's own record, so a failure to read it is this component disagreeing with
               itself and idb_database_list crashes rather than reporting an UnknownError. */
            JSValue set = idb_database_list(ctx), out = JS_NewArray(ctx);
            JSValue len = JS_GetPropertyStr(ctx, set, "length");
            uint32_t i, n = 0, k = 0;
            int cr = JS_ToUint32(ctx, &n, len);

            DCHECK(cr >= 0, "§2.1's set of databases was listed as something without a numeric length");
            (void)cr;
            JS_FreeValue(ctx, len);
            CHECK(!JS_IsException(out), "IndexedDB: §4.3 databases() step 4.2's list could not be allocated");
            for (i = 0; i < n; i++) {
                JSValue db = JS_GetPropertyUint32(ctx, set, i), info;
                double version = idb_database_version(ctx, db);

                /* Step 4.3.1: "If db's version is 0, then continue." A database is at version 0 between §5.1
                   step 6 creating it and §5.7 step 8 raising it — and it STAYS there when step 7 refuses the
                   open, so this is not a transient the page cannot observe. */
                if (version == 0) { JS_FreeValue(ctx, db); continue; }
                /* Steps 4.3.2-4.3.5: a new IDBDatabaseInfo whose two members are db's name and version.
                   `dictionary IDBDatabaseInfo { DOMString name; unsigned long long version; }` — a Web IDL
                   dictionary crosses into ECMAScript as own data properties in DECLARATION order. */
                info = JS_NewObject(ctx);
                CHECK(!JS_IsException(info), "IndexedDB: an IDBDatabaseInfo could not be allocated");
                JS_SetPropertyStr(ctx, info, "name", idb_database_name(ctx, db));
                JS_SetPropertyStr(ctx, info, "version", JS_NewFloat64(ctx, version));
                JS_DefinePropertyValueUint32(ctx, out, k++, info, JS_PROP_C_W_E);
                JS_FreeValue(ctx, db);
            }
            JS_FreeValue(ctx, set);
            s->result = out;
        }
        STEP_GOTO(s->hdr.stage, DBT_RESOLVE, &s->phase, NULL);
        return JS_STEP_YIELD;

    STEP_ARM(DBT_RESOLVE);
        {
            JSValue settled = JS_UNDEFINED;

            DCHECK(JS_IsFunction(ctx, resolve), "§4.3 databases()'s task carried no resolving function — the "
                                                "member takes the capability's first function with it, and it "
                                                "is the only thing the task is minted over");
            r = step_call_run(ctx, &s->phase, STEP_CB(s->cb), resolve, JS_UNDEFINED, 1,
                              (JSValueConst *)&s->result, cb_result, &settled, out_cb, out_argc);
            if (r > 0) return r;
            if (r < 0) return JS_STEP_ABRUPT;
            JS_FreeValue(ctx, settled);
        }
        return JS_STEP_DONE;
}

static const JSTrampStepDef js_idb_databases_task_def = {
    sizeof(IdbDatabasesTask), js_idb_databases_task, NULL, 0, .visit = js_idb_databases_task_visit,
    .algorithm = "Indexed Database §4.3 databases()'s in-parallel steps and the database task that resolves p",
    .steps = DBT_STEPS
};

#define DBS_STAGES(X) \
    X(DBS_RUN,    "Indexed Database §4.3 databases() steps 1-3 and 5 — ONE O(1) engine action: the storage " \
                  "key is obtained, p is a new promise, step 4 is queued as a database task, and p is " \
                  "returned") \
    X(DBS_REJECT, "Indexed Database §4.3 databases() step 2's failure (return a promise rejected with a " \
                  "SecurityError DOMException)")
enum { IDL_STEP_STAGE_BASE(DBS_STAGES) DBS_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const DBS_STEPS[] = { DBS_STAGES(JS_STEP_STAGE_LABEL) NULL };

typedef struct {
    uint8_t started;
    uint8_t phase;
    JSValue promise;   /* step 3's `p` (owned) */
    JSValue func;      /* the capability function step 2's failure calls (owned) */
    JSValue value;     /* the SecurityError it is called with (owned) */
    JSValue cb[3];
} IdbDatabasesState;

static void js_idb_databases_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    IdbDatabasesState *s = st;
    int i;

    v->val(ctx, &s->promise);
    v->val(ctx, &s->func);
    v->val(ctx, &s->value);
    STEP_CB_FOREACH(s->cb, i) v->val(ctx, &s->cb[i]);
}

static int js_idb_databases(JSContext *ctx, JSStepHdr *hdr, void *st, int argc, JSValueConst *argv,
                            JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    IdbDatabasesState *s = st;
    int r;

    (void)argc; (void)argv;
    *presult = JS_UNDEFINED;
    if (!s->started) {
        int i;

        s->started = 1;
        s->promise = s->func = s->value = JS_UNDEFINED;
        STEP_CB_FOREACH(s->cb, i) s->cb[i] = JS_UNDEFINED;
    }
    STEP_DISPATCH(DBS_STAGES, hdr->stage, hdr->def->algorithm, JS_STEP_ABRUPT);

    STEP_ARM(DBS_RUN);
        JS_FreeValue(ctx, cb_result);
        {
            JSValue funcs[2];

            if (!factory_brand(ctx, hdr->this_val)) return JS_STEP_ABRUPT;
            /* Step 3: "let p be a new promise." Minted BEFORE step 2's refusal is reported, because that
               refusal is a REJECTED PROMISE and not a throw — `indexedDB.databases()` in a sandboxed document
               returns something a page can `.catch`, which is the one way this member differs from `open` and
               `deleteDatabase` in how it says the same no. */
            s->promise = JS_NewPromiseCapability(ctx, funcs);
            if (JS_IsException(s->promise)) { s->promise = JS_UNDEFINED; return JS_STEP_ABRUPT; }
            if (origin_is_opaque(window_proxy_origin(document_window_proxy(ctx)))) {
                JS_ThrowDOMException(ctx, "SecurityError",
                                     "a document with an opaque origin has no storage key");
                s->value = JS_GetException(ctx);
                s->func = funcs[1];
                JS_FreeValue(ctx, funcs[0]);
                STEP_GOTO(hdr->stage, DBS_REJECT, &s->phase, NULL);
                return JS_STEP_YIELD;
            }
            /* Step 4: "run these steps in parallel". The task takes the RESOLVING FUNCTION with it and nothing
               else — the set it reads is read when it runs, which is what makes the answer a snapshot of that
               moment rather than of this one. */
            {
                JSValue task = JS_NewStepClosure(ctx, g_id_databases_task, 0, 1, (JSValueConst *)&funcs[0]);

                CHECK(!JS_IsException(task), "IndexedDB: §4.3 databases()'s database task could not be minted");
                JS_EnqueueCallTask(ctx, task, 0, NULL);
                JS_FreeValue(ctx, task);
            }
            JS_FreeValue(ctx, funcs[0]);
            JS_FreeValue(ctx, funcs[1]);   /* nothing rejects p: step 4.1's failure cannot arise — see the task */
        }
        *presult = s->promise;             /* step 5: "return p" */
        s->promise = JS_UNDEFINED;
        return JS_STEP_DONE;

    STEP_ARM(DBS_REJECT);
        {
            JSValue settled = JS_UNDEFINED;

            r = step_call_run(ctx, &s->phase, STEP_CB(s->cb), s->func, JS_UNDEFINED, 1,
                              (JSValueConst *)&s->value, cb_result, &settled, out_cb, out_argc);
            if (r > 0) return r;
            if (r < 0) return JS_STEP_ABRUPT;
            JS_FreeValue(ctx, settled);
        }
        *presult = s->promise;
        s->promise = JS_UNDEFINED;
        return JS_STEP_DONE;
}

static const IdlStepDecl DBS_STEP = {
    js_idb_databases, sizeof(IdbDatabasesState), js_idb_databases_visit, NULL,
    "Indexed Database §4.3 IDBFactory.databases()", DBS_STEPS
};

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
    idl_install_method(ctx, proto, "open", g_id_open);
    idl_install_method(ctx, proto, "deleteDatabase", g_id_delete);
    idl_install_method(ctx, proto, "databases", g_id_databases);
    idl_install_method(ctx, proto, "cmp", g_id_cmp);
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
    static const IdlArgType DELETE_ARGS[1] = { IDL_DOMSTRING };

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
    /* `[NewObject] IDBOpenDBRequest deleteDatabase(DOMString name)` — one required argument, no optional
       version, which is the whole of what its declaration differs from `open`'s by. */
    g_id_delete = idl_method_id(ctx, DELETE_ARGS, 1, js_idb_delete_database, 0);
    /* `Promise<sequence<IDBDatabaseInfo>> databases()` — no arguments, and the machine is what the REJECTION
       needs: settling a promise is calling the capability's function, which is the page's code path and cannot
       be a JS_Call from C. */
    g_id_databases = idl_method_id_step(ctx, NULL, 0, NULL, 0, &DBS_STEP, 0);
    /* Web IDL §3.7.7 Operations' create an operation function rejects for the whole call. The member takes no
       arguments, so what this covers is the BRAND CHECK the body asks first — `IDBFactory.prototype.databases
       .call({})` threw a TypeError where a browser hands back a rejected promise, and that is the one throw a
       page cannot tell from `databases()` simply refusing. */
    idl_returns_promise();
    g_id_databases_task = JS_RegisterStepDef(JS_GetRuntime(ctx), &js_idb_databases_task_def);
    /* WHAT THIS COMPONENT HOLDS FOR THE AGENT, DECLARED — core/agent_state.h. Its header used to state that it
       "holds no agent-lifetime JS value, so there is nothing to release", and the conclusion did not follow
       from the premise: no JS VALUE, but a class id, a realm-value slot and five declarations, every one of
       them made in and against THIS runtime. This row was on core/platform.c's list with an EMPTY release
       column and no release function existed anywhere, so all seven survived — invisible to both of
       JS_FreeRuntime's censuses, since each is an int, and read by nothing until the next agent's
       `indexed_db_init` consults `g_obj_slot` to decide it need not run. */
    agent_state_class("indexed_db", &g_factory_class, "Indexed Database §4.3's IDBFactory class");
    agent_state_id("indexed_db", &g_obj_slot,
                   "Indexed Database §4.3's realm-value slot for the realm's one [SameObject] IDBFactory, "
                   "and this component's declaration latch");
    agent_state_id("indexed_db", &g_id_cmp, "Indexed Database §4.3's cmp declaration");
    agent_state_id("indexed_db", &g_id_open, "Indexed Database §4.3's open declaration");
    agent_state_id("indexed_db", &g_id_delete, "Indexed Database §4.3's deleteDatabase declaration");
    agent_state_id("indexed_db", &g_id_databases, "Indexed Database §4.3's databases declaration");
    agent_state_id("indexed_db", &g_id_databases_task,
                   "Indexed Database §4.3's databases task step definition");
    realm_declare_intrinsic(indexed_db_install_realm);
}

/* THE INVERSE OF THE DECLARATION ABOVE, WHICH DID NOT EXIST. Every prototype, every interface object and every
   realm's one IDBFactory are the REALMS' and go with their contexts — the realm-value POOL is released by
   realm_intrinsics_free, so what this owes is the SLOT NUMBER it holds into that pool, which would otherwise
   index a pool the next agent has not built yet. The class goes back to 0 because a class is registered in a
   RUNTIME (core/agent_state.h's one policy). */
void indexed_db_free(void)
{
    DCHECK(g_obj_slot >= 0, "§4.3's IDBFactory was released in an agent that never declared it");
    g_id_cmp = g_id_open = g_id_delete = g_id_databases = g_id_databases_task = -1;
    g_obj_slot = -1;
    g_factory_class = 0;
}
