/* INDEXED DATABASE §2.8.2's CONNECTION QUEUE, §5.1's OPEN A DATABASE CONNECTION, §5.7's UPGRADE A DATABASE and
 * §4.3's completion task — the algorithm a page reaches every other part of this standard through.
 *
 * WHY IT IS THREE MACHINES AND NOT ONE, WHICH IS THE WHOLE SHAPE OF THIS FILE. §5.1 is written as one algorithm
 * with FOUR waits in it, and the four are not the same kind of thing:
 *
 *   step 3   "wait until all previous requests in queue have been processed" — satisfied by the previous open
 *            request COMPLETING, which is §4.3's completion task and is arbitrarily far away.
 *   step 10.4 "wait for all of the events to be fired" — satisfied by the queue: the versionchange events are
 *            queued database tasks and this algorithm's own continuation is queued behind them. That is the
 *            same discharge idb_request.c makes for §5.6 step 5.1 and idb_transaction.c for §5.4 step 2.1, so
 *            it is a REST POINT inside one machine and not a hand-off.
 *   step 10.6 "wait until all connections in openConnections are closed" — satisfied by the PAGE calling
 *            `close()`, or by the last transaction of an already-close-pending connection finishing. Nothing
 *            the queue can order.
 *   step 10.7's own last step, §5.7 step 10 "wait for transaction to finish" — satisfied by the upgrade
 *            transaction's commit or abort task, which fires `complete` or `abort` at the page first.
 *
 * A machine RESTS at a stage and is resumed by the SCHEDULER; it cannot be resumed by an event that has not
 * happened yet. So the three waits that are not the queue's are exactly where one machine ends and the next is
 * enqueued, and each machine's `algorithm` names the SPAN of the standard it performs. What carries the state
 * across those hand-offs is the ENTRY — an internal-slot record holding §5.1's own locals (`db`, `version`,
 * `connection`, `openConnections`) — so the algorithm's variables live where a per-flow COW delta captures them
 * and where a park to the cold tier carries them, rather than in a C struct that would exist only while one
 * task was running.
 *
 * §2.8.2's CONNECTION QUEUE IS WHAT MAKES ALL OF THAT SAFE. "Open requests are processed in a connection queue.
 * The queue contains all open requests associated with a storage key and a name. Requests added to the
 * connection queue [are] processed in order and each request must run to completion before the next request is
 * processed." One storage key per agent (§Security makes an instance an origin-keyed agent cluster), so the
 * queue is keyed by NAME alone. It is why §2.1's "a database has AT MOST ONE upgrade transaction" is an
 * invariant rather than a hope, and why the DCHECKs below can assert that the entry being resumed is the head.
 *
 * WHAT IS ABSENT AND WHY. §5.3's DELETE A DATABASE and §4.3's `deleteDatabase` are not here: they are a second
 * algorithm over the same queue and the same `blocked` machinery, and writing them beside §5.1 before §5.1 has
 * run would be writing two things at once. `databases()` reports §2.1's set and needs a promise plus an
 * IDBDatabaseInfo sequence. Both are honestly ABSENT and the IDL gap auditor lists them. */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "quickjs-step.h"
#include "core/agent_state.h"
#include "core/events/event.h"
#include "core/events/event_target.h"
#include "core/idl_args.h"
#include "core/idl_slots.h"
#include "core/indexeddb/idb_connection.h"
#include "core/indexeddb/idb_database.h"
#include "core/indexeddb/idb_open.h"
#include "core/indexeddb/idb_request.h"
#include "core/indexeddb/idb_transaction.h"
#include "core/indexeddb/idb_version_change_event.h"

/* §5.1's OWN LOCALS, on the record that carries them across the algorithm's hand-offs. */
#define E_REQUEST      "request"          /* §2.8.1's open request */
#define E_NAME         "name"             /* the database name, which is also this entry's queue */
#define E_VERSION      "version"          /* §5.1's `version` once step 5 has decided it */
#define E_HAS_VERSION  "hasVersion"       /* whether §4.3's caller gave one — step 5 branches on it */
#define E_DB           "db"               /* §5.1 step 4's `db`, JS_NULL until step 6 */
#define E_CONNECTION   "connection"       /* §5.1 step 8's `connection`, JS_NULL until then */
#define E_OPEN_CONNS   "openConnections"  /* §5.1 step 10.1's set, snapshotted where the step computes it */
#define E_CURSOR       "cursor"           /* which of them step 10.2 is standing on */
#define E_OLD_VERSION  "oldVersion"       /* §5.7 step 7's `old version`, fired at step 9.5 */
#define E_BLOCKED      "blockedFired"     /* whether step 10.5 has already fired `blocked` */

static JSValue g_queues = JS_UNDEFINED;   /* §2.8.2: name -> Array of entries, in arrival order */
/* THE ENTRIES PARKED AT STEP 10.6. A list rather than a walk over every queue's head, because what wakes them
   is a CONNECTION closing and the question that connection answers is "is any open request waiting on me" —
   asking it of a list of waiters is the question; asking it of every queue would be a search for the answer. */
static JSValue g_blocked = JS_UNDEFINED;

static int g_open_stepid = -1, g_upgrade_stepid = -1, g_result_stepid = -1;

/* ---- the entry, and §2.8.2's queue ------------------------------------------------------------------------ */

static uint32_t open_list_len(JSContext *ctx, JSValueConst list)
{
    JSValue len = JS_GetPropertyStr(ctx, list, "length");
    uint32_t n = 0;
    int r;

    DCHECK(!JS_IsException(len), "reading the length of one of this component's own Arrays threw — they are "
                                 "engine-built and have no getters to run");
    r = JS_ToUint32(ctx, &n, len);
    DCHECK(r >= 0, "one of this component's own Arrays had a length that is not a number");
    (void)r;
    JS_FreeValue(ctx, len);
    return n;
}

static JSValue entry_get(JSContext *ctx, JSValueConst e, const char *field)
{
    JSValue v;

    DCHECK(JS_IsObject(e), "an §5.1 entry field was read off something that is not an entry");
    v = JS_GetPropertyStr(ctx, e, field);
    DCHECK(!JS_IsUndefined(v), "an §5.1 entry carried no such field — every field is placed by "
                               "idb_open_request before the algorithm starts, so an absent one is a field "
                               "this file reads under a name nothing writes");
    return v;
}

/* `v` is CONSUMED. */
static void entry_set(JSContext *ctx, JSValueConst e, const char *field, JSValue v)
{
    DCHECK(JS_IsObject(e), "an §5.1 entry field was written on something that is not an entry");
    JS_SetPropertyStr(ctx, (JSValue)e, field, v);
}

static double entry_num(JSContext *ctx, JSValueConst e, const char *field)
{
    JSValue v = entry_get(ctx, e, field);
    double out = 0;
    int r = JS_ToFloat64(ctx, &out, v);

    DCHECK(r >= 0, "an §5.1 entry's numeric field is not a number — only this file writes them");
    (void)r;
    JS_FreeValue(ctx, v);
    return out;
}

static bool entry_flag(JSContext *ctx, JSValueConst e, const char *field)
{
    JSValue v = entry_get(ctx, e, field);
    bool b;

    DCHECK(JS_IsBool(v), "an §5.1 entry's flag is not a boolean — only this file writes them");
    b = JS_ToBool(ctx, v);
    JS_FreeValue(ctx, v);
    return b;
}

/* The entry's own queue. OWNED; built on first use, because §2.8.2's queue exists for a (storage key, name)
   pair and a name nothing has ever opened has no requests to order. */
static JSValue open_queue(JSContext *ctx, const char *name, bool create)
{
    JSValue q;

    DCHECK(!JS_IsUndefined(g_queues), "§2.8.2's connection queues were asked for before idb_open_init built "
                                      "the set");
    q = JS_GetPropertyStr(ctx, g_queues, name);
    if (JS_IsUndefined(q)) {
        if (!create)
            return JS_NULL;
        q = JS_NewArray(ctx);
        CHECK(!JS_IsException(q), "IndexedDB: §2.8.2's connection queue could not be allocated");
        JS_SetPropertyStr(ctx, g_queues, name, JS_DupValue(ctx, q));
    }
    DCHECK(JS_IsArray(q), "§2.8.2's connection queue set held something that is not a queue");
    return q;
}

/* The entry's queue, named by the entry itself. OWNED. */
static JSValue entry_queue(JSContext *ctx, JSValueConst e)
{
    JSValue nv = entry_get(ctx, e, E_NAME), q;
    const char *name = JS_ToCString(ctx, nv);

    CHECK(name != NULL, "IndexedDB: an §5.1 entry could not report its own database name");
    q = open_queue(ctx, name, /*create*/ false);
    DCHECK(JS_IsArray(q), "an §5.1 entry's connection queue does not exist — every entry is added to one by "
                          "idb_open_request and removed only when its request completes");
    JS_FreeCString(ctx, name);
    JS_FreeValue(ctx, nv);
    return q;
}

/* THE ASSERTION EVERY MACHINE IN THIS FILE MAKES ON ENTRY: §2.8.2's "each request must run to completion
   before the next request is processed" means the entry a task is running for is the HEAD of its queue. It is
   what makes step 3's wait discharged rather than waited on, and it is what makes §2.1's at-most-one upgrade
   transaction an invariant. */
static void entry_assert_head(JSContext *ctx, JSValueConst e)
{
    JSValue q = entry_queue(ctx, e), head;

    DCHECK(open_list_len(ctx, q) > 0, "an §5.1 task ran for an entry whose connection queue is empty");
    head = JS_GetPropertyUint32(ctx, q, 0);
    DCHECK(JS_VALUE_GET_PTR(head) == JS_VALUE_GET_PTR(e),
           "an §5.1 task ran for an open request that is not the head of its connection queue — §2.8.2 says "
           "each request runs to completion before the next is processed, so a task for anything but the head "
           "means two opens of one name are in flight and §2.1's at-most-one upgrade transaction no longer "
           "holds");
    JS_FreeValue(ctx, head);
    JS_FreeValue(ctx, q);
}

/* Enqueue one of this file's machines over `entry`. Every hand-off in §5.1 is one of these. */
static void entry_enqueue(JSContext *ctx, int stepid, JSValueConst entry)
{
    JSValue fn = JS_NewStepClosure(ctx, stepid, 0, 1, &entry);

    CHECK(!JS_IsException(fn), "IndexedDB: an §5.1 task could not be minted");
    JS_EnqueueCallTask(ctx, fn, 0, NULL);
    JS_FreeValue(ctx, fn);
}

#define OPEN_CD_ENTRY 0

static JSValueConst task_entry(const JSStepHdr *hdr)
{
    JSValueConst e = JS_StepClosureData(hdr, OPEN_CD_ENTRY);

    DCHECK(JS_IsObject(e), "an §5.1 task was minted over something that is not an open-request entry");
    return e;
}

/* A DOMException as a VALUE, derived the way idb_request.c derives one: by throwing and taking it back, which
   is the engine's only constructor for the interface and runs none of the page's code. */
static JSValue open_dom_exception(JSContext *ctx, const char *name, const char *msg)
{
    JS_ThrowDOMException(ctx, name, "%s", msg);
    return JS_GetException(ctx);
}

/* §5.1's THREE ERROR RETURNS all end the same way: the error is written on the request and §4.3's completion
   task reports it. `error` is CONSUMED. */
static void open_fail(JSContext *ctx, JSValueConst entry, JSValue error)
{
    JSValue req = entry_get(ctx, entry, E_REQUEST);

    idb_request_set_error(ctx, req, error);
    JS_FreeValue(ctx, req);
    entry_enqueue(ctx, g_result_stepid, entry);
}

/* ---- the state both machines that fire an event carry ------------------------------------------------------ */

typedef struct {
    JSStepHdr   hdr;      /* FIRST: the driver writes the def and the operand bounds through it */
    uint8_t     started;  /* a step state is js_mallocz'd and a zeroed JSValue is the INTEGER 0 */
    uint8_t     phase;    /* the dispatch request's own cursor */
    JSValue     ev;       /* the event in flight (owned) */
    EventFireCb cb;       /* the dispatch request's operand buffer */
    uint8_t     did_throw;/* §4.2 step 7's legacyOutputDidListenersThrowFlag, which §5.7 step 9.6 reads */
} JSIdbOpenState;

static void js_idb_open_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSIdbOpenState *s = st;
    int i;

    v->val(ctx, &s->ev);
    STEP_CB_FOREACH(s->cb, i) v->val(ctx, &s->cb[i]);
}

/* Every value slot is placed before anything can read one — quickjs-step.h's note is that a machine can never
   answer "have I started?" from its stage, since the first stage's constant is also the entry stage. */
static void open_state_start(JSIdbOpenState *s)
{
    int i;

    if (s->started)
        return;
    s->started = 1;
    s->ev = JS_UNDEFINED;
    STEP_CB_FOREACH(s->cb, i) s->cb[i] = JS_UNDEFINED;
}

/* ---- §5.1's OPEN A DATABASE CONNECTION, steps 4 through 10.7 ---------------------------------------------- */

#define OPN_STAGES(X) \
    X(OPN_FIND,    "Indexed Database §5.1 steps 4-6 — ONE O(1) engine action: the database named `name` is " \
                   "looked up in this storage key, `version` is defaulted from it, and a database that does " \
                   "not exist is created with version 0") \
    X(OPN_VERSION, "Indexed Database §5.1 step 7 (if db's version is greater than version, return a " \
                   "VersionError)") \
    X(OPN_CONNECT, "Indexed Database §5.1 steps 8-9 (let connection be a new connection to db; set " \
                   "connection's version to version)") \
    X(OPN_VERSIONCHANGE, "Indexed Database §5.1 steps 10.1-10.4, one connection per turn (fire a version " \
                         "change event named versionchange at each other open connection, and wait for all " \
                         "of the events to be fired)") \
    X(OPN_BLOCKED, "Indexed Database §5.1 step 10.5 (if any of the connections in openConnections are still " \
                   "not closed, fire a version change event named blocked at request)") \
    X(OPN_WAIT,    "Indexed Database §5.1 step 10.6 (wait until all connections in openConnections are " \
                   "closed)") \
    X(OPN_UPGRADE, "Indexed Database §5.1 step 10.7 (run upgrade a database using connection, version and " \
                   "request)")
enum { OPN_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const OPN_STEPS[] = { OPN_STAGES(JS_STEP_STAGE_LABEL) NULL };

/* §5.1 step 10.6's question, asked of the set step 10.1 snapshotted. A connection that has become closed has
   left its database's connection set and carries the flag, so this reads the flag rather than re-deriving the
   set — which is the point of snapshotting it: a connection opened AFTER step 10.1 is not one this open is
   waiting for. */
static bool open_still_blocked(JSContext *ctx, JSValueConst entry)
{
    JSValue set = entry_get(ctx, entry, E_OPEN_CONNS);
    uint32_t i, n = open_list_len(ctx, set);
    bool blocked = false;

    for (i = 0; i < n && !blocked; i++) {
        JSValue c = JS_GetPropertyUint32(ctx, set, i);

        DCHECK(idb_connection_is(c), "§5.1 step 10.1's openConnections held something that is not a connection");
        blocked = !idb_connection_is_closed(ctx, c);
        JS_FreeValue(ctx, c);
    }
    JS_FreeValue(ctx, set);
    return blocked;
}

static int js_idb_open_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    JSIdbOpenState *s = st;
    JSValueConst entry = task_entry(&s->hdr);
    JSValue db, req, conn;
    double version, db_version;
    int r;

    open_state_start(s);
    STEP_DISPATCH(OPN_STAGES, s->hdr.stage, s->hdr.def->algorithm, JS_STEP_ABRUPT);

    STEP_ARM(OPN_FIND);
        JS_FreeValue(ctx, cb_result);
        entry_assert_head(ctx, entry);
        {
            JSValue nv = entry_get(ctx, entry, E_NAME);
            const char *name = JS_ToCString(ctx, nv);

            CHECK(name != NULL, "IndexedDB: an §5.1 entry could not report its own database name");
            /* Step 4: "Let db be the database named name in storageKey, or null otherwise." */
            db = idb_database_find(ctx, name);
            /* Step 5: "If version is undefined, let version be 1 if db is null, or db's version otherwise."
               The two are different requests and the standard says so — `open(name)` on an existing database
               opens it AT its version and upgrades nothing. */
            if (!entry_flag(ctx, entry, E_HAS_VERSION)) {
                version = JS_IsNull(db) ? 1 : idb_database_version(ctx, db);
                entry_set(ctx, entry, E_VERSION, JS_NewFloat64(ctx, version));
                entry_set(ctx, entry, E_HAS_VERSION, JS_TRUE);
            }
            /* Step 6: "If db is null, let db be a new database with name name, version 0 (zero), and with no
               object stores." A database created here is at version 0 until §5.7 step 8 raises it, which is
               what makes step 7's comparison always fall through for a brand-new one. */
            if (JS_IsNull(db)) {
                JS_FreeValue(ctx, db);
                db = idb_database_create(ctx, name);
            }
            JS_FreeCString(ctx, name);
            JS_FreeValue(ctx, nv);
        }
        entry_set(ctx, entry, E_DB, db);
        STEP_GOTO(s->hdr.stage, OPN_VERSION, &s->phase, NULL);
        return JS_STEP_YIELD;

    STEP_ARM(OPN_VERSION);
        JS_FreeValue(ctx, cb_result);
        db = entry_get(ctx, entry, E_DB);
        db_version = idb_database_version(ctx, db);
        version = entry_num(ctx, entry, E_VERSION);
        JS_FreeValue(ctx, db);
        /* Step 7: "If db's version is greater than version, return a newly created VersionError DOMException
           and abort these steps." Opening at a LOWER version than the database is at is the one request this
           algorithm refuses outright — there is no downgrade. */
        if (db_version > version) {
            open_fail(ctx, entry, open_dom_exception(ctx, "VersionError",
                                                     "the database is at a higher version than the one "
                                                     "requested"));
            return JS_STEP_DONE;
        }
        STEP_GOTO(s->hdr.stage, OPN_CONNECT, &s->phase, NULL);
        return JS_STEP_YIELD;

    STEP_ARM(OPN_CONNECT);
        JS_FreeValue(ctx, cb_result);
        db = entry_get(ctx, entry, E_DB);
        version = entry_num(ctx, entry, E_VERSION);
        /* Steps 8-9: "Let connection be a new connection to db. Set connection's version to version." */
        conn = idb_connection_open(ctx, db);
        idb_connection_set_version(ctx, conn, version);
        entry_set(ctx, entry, E_CONNECTION, JS_DupValue(ctx, conn));
        db_version = idb_database_version(ctx, db);
        /* Step 10 is entered only when "db's version is less than version". Equal versions are an open with no
           upgrade at all — no versionchange, no blocked, no upgradeneeded — which is what makes the second
           `open(name)` of a session cheap and is why the page gets `success` and never `upgradeneeded`. */
        if (db_version < version) {
            /* Step 10.1: "Let openConnections be the set of all connections, EXCEPT connection, associated
               with db." Snapshotted here because that is where the step computes it: a connection opened
               later is not one this open waits for, and step 10.6 asks about THESE. */
            JSValue all = idb_database_connections(ctx, db), set = JS_NewArray(ctx);
            uint32_t i, n = open_list_len(ctx, all), k = 0;

            CHECK(!JS_IsException(set), "IndexedDB: §5.1 step 10.1's openConnections could not be allocated");
            for (i = 0; i < n; i++) {
                JSValue c = JS_GetPropertyUint32(ctx, all, i);

                if (JS_VALUE_GET_PTR(c) == JS_VALUE_GET_PTR(conn)) { JS_FreeValue(ctx, c); continue; }
                JS_DefinePropertyValueUint32(ctx, set, k++, c, JS_PROP_C_W_E);
            }
            JS_FreeValue(ctx, all);
            entry_set(ctx, entry, E_OPEN_CONNS, set);
            entry_set(ctx, entry, E_CURSOR, JS_NewInt32(ctx, 0));
            JS_FreeValue(ctx, conn);
            JS_FreeValue(ctx, db);
            STEP_GOTO(s->hdr.stage, OPN_VERSIONCHANGE, &s->phase, NULL);
            return JS_STEP_YIELD;
        }
        JS_FreeValue(ctx, conn);
        JS_FreeValue(ctx, db);
        /* Step 11: "Return connection." */
        entry_enqueue(ctx, g_result_stepid, entry);
        return JS_STEP_DONE;

    STEP_ARM(OPN_VERSIONCHANGE);
        {
            JSValue set = entry_get(ctx, entry, E_OPEN_CONNS), target;
            uint32_t i = (uint32_t)entry_num(ctx, entry, E_CURSOR), n = open_list_len(ctx, set);

            /* Step 10.2: "For each entry of openConnections that does not have its close pending flag set to
               true, queue a database task to fire a version change event named versionchange at entry with
               db's version and version." The note is why the flag is re-read at each turn rather than filtered
               once: "firing this event might cause one or more of the other objects in openConnections to be
               closed, in which case the versionchange event is not fired at those objects."
               Step 10.3's "wait for all of the events to be fired" is discharged by this walk, whose every
               turn is a REST POINT — the same discharge idb_request.c makes for §5.6 step 5.1. */
            while (i < n) {
                target = JS_GetPropertyUint32(ctx, set, i);
                DCHECK(idb_connection_is(target), "§5.1 step 10.1's openConnections held something that is "
                                                  "not a connection");
                if (!idb_connection_close_pending(ctx, target))
                    break;
                JS_FreeValue(ctx, target);
                i++;
            }
            if (i >= n) {
                JS_FreeValue(ctx, set);
                entry_set(ctx, entry, E_CURSOR, JS_NewInt32(ctx, (int32_t)i));
                JS_FreeValue(ctx, cb_result);
                STEP_GOTO(s->hdr.stage, OPN_BLOCKED, &s->phase, NULL);
                return JS_STEP_YIELD;
            }
            db = entry_get(ctx, entry, E_DB);
            db_version = idb_database_version(ctx, db);
            JS_FreeValue(ctx, db);
            version = entry_num(ctx, entry, E_VERSION);
            r = idb_fire_version_change_event_run(ctx, &s->phase, STEP_CB(s->cb), target, "versionchange",
                                                  db_version, version, /*has_new*/ true, &s->ev, cb_result,
                                                  NULL, out_cb, out_argc);
            JS_FreeValue(ctx, target);
            JS_FreeValue(ctx, set);
            if (r > 0) return r;
            if (r < 0) return JS_STEP_ABRUPT;
            /* The event is this connection's; the next one gets its own, which §4.2 step 1 creates. */
            JS_FreeValue(ctx, s->ev);
            s->ev = JS_UNDEFINED;
            entry_set(ctx, entry, E_CURSOR, JS_NewInt32(ctx, (int32_t)(i + 1)));
            return JS_STEP_YIELD;   /* the walk's own rest point, at the same stage */
        }

    STEP_ARM(OPN_BLOCKED);
        /* Step 10.5: "If any of the connections in openConnections are still not closed, queue a database task
           to fire a version change event named blocked at request with db's version and version." ONCE — the
           flag is what stops a second `blocked` when a close arrives and the wait resumes. */
        if (!open_still_blocked(ctx, entry) || entry_flag(ctx, entry, E_BLOCKED)) {
            JS_FreeValue(ctx, cb_result);
            STEP_GOTO(s->hdr.stage, OPN_WAIT, &s->phase, NULL);
            return JS_STEP_YIELD;
        }
        req = entry_get(ctx, entry, E_REQUEST);
        db = entry_get(ctx, entry, E_DB);
        db_version = idb_database_version(ctx, db);
        JS_FreeValue(ctx, db);
        version = entry_num(ctx, entry, E_VERSION);
        r = idb_fire_version_change_event_run(ctx, &s->phase, STEP_CB(s->cb), req, "blocked", db_version,
                                              version, /*has_new*/ true, &s->ev, cb_result, NULL, out_cb,
                                              out_argc);
        JS_FreeValue(ctx, req);
        if (r > 0) return r;
        if (r < 0) return JS_STEP_ABRUPT;
        entry_set(ctx, entry, E_BLOCKED, JS_TRUE);
        JS_FreeValue(ctx, s->ev);
        s->ev = JS_UNDEFINED;
        STEP_GOTO(s->hdr.stage, OPN_WAIT, &s->phase, NULL);
        return JS_STEP_YIELD;

    STEP_ARM(OPN_WAIT);
        JS_FreeValue(ctx, cb_result);
        /* Step 10.6: "Wait until all connections in openConnections are closed." THIS is the wait no queue can
           discharge — a `versionchange` handler that ignores the event leaves its connection open until the
           page decides otherwise. The entry PARKS on the blocked list and idb_open_connection_closed brings it
           back; §5.2's close is the only thing that can, which is what §5.2's own note says. */
        if (open_still_blocked(ctx, entry)) {
            JS_DefinePropertyValueUint32(ctx, g_blocked, open_list_len(ctx, g_blocked),
                                         JS_DupValue(ctx, entry), JS_PROP_C_W_E);
            return JS_STEP_DONE;
        }
        STEP_GOTO(s->hdr.stage, OPN_UPGRADE, &s->phase, NULL);
        return JS_STEP_YIELD;

    STEP_ARM(OPN_UPGRADE);
        JS_FreeValue(ctx, cb_result);
        /* Step 10.7: "Run upgrade a database using connection, version and request." §5.7's own step 10 is a
           wait for the transaction to finish, so it is a machine of its own and steps 10.8-10.9 continue in
           §4.3's completion task, which idb_open_upgrade_finished enqueues. */
        entry_enqueue(ctx, g_upgrade_stepid, entry);
        return JS_STEP_DONE;
}

static const JSTrampStepDef js_idb_open_def = {
    sizeof(JSIdbOpenState), js_idb_open_step, NULL, 0, .visit = js_idb_open_visit,
    .algorithm = "Indexed Database §5.1's open a database connection, steps 4 through 10.7",
    .steps = OPN_STEPS
};

/* §5.2's rendezvous — a connection has become CLOSED, so every open request parked at step 10.6 asks its own
   question again. It runs no page code (it reads flags the connection wrote) and it is not a machine, because
   step 10.6's continuation is step 10.7, which IS one. */
static void idb_open_connection_closed(JSContext *ctx, JSValueConst connection)
{
    uint32_t i, n;

    (void)connection;   /* the question is per-ENTRY: which set an entry is waiting on is the entry's own */
    DCHECK(!JS_IsUndefined(g_blocked), "a connection closed before idb_open_init built the blocked list");
    n = open_list_len(ctx, g_blocked);
    for (i = 0; i < n; i++) {
        JSValue e = JS_GetPropertyUint32(ctx, g_blocked, i);
        uint32_t k;

        DCHECK(JS_IsObject(e), "§5.1's blocked list held something that is not an entry");
        if (open_still_blocked(ctx, e)) { JS_FreeValue(ctx, e); continue; }
        for (k = i; k + 1 < n; k++)
            JS_DefinePropertyValueUint32(ctx, g_blocked, k, JS_GetPropertyUint32(ctx, g_blocked, k + 1),
                                         JS_PROP_C_W_E);
        JS_SetPropertyStr(ctx, g_blocked, "length", JS_NewUint32(ctx, n - 1));
        entry_enqueue(ctx, g_upgrade_stepid, e);
        JS_FreeValue(ctx, e);
        n--;
        i--;
    }
}

/* ---- §5.7's UPGRADE A DATABASE ----------------------------------------------------------------------------- */

#define UPG_STAGES(X) \
    X(UPG_CREATE,   "Indexed Database §5.7 steps 1-8 — ONE O(1) engine action: the upgrade transaction is " \
                    "created over the connection's object store set, the database points at it, it is made " \
                    "inactive and started, db's version is set to the new version, and the request's " \
                    "processed flag is set") \
    X(UPG_DELIVER,  "Indexed Database §5.7 steps 9.1-9.4 — ONE O(1) engine action: the request's result is " \
                    "the connection, its transaction is the upgrade transaction, its done flag is set, and " \
                    "the transaction is made active for the dispatch") \
    X(UPG_FIRE,     "Indexed Database §5.7 step 9.5 (fire a version change event named upgradeneeded at " \
                    "request with old version and version)") \
    X(UPG_SETTLE,   "Indexed Database §5.7 step 9.6 (if transaction's state is active, set it inactive and " \
                    "abort it when the listeners threw)")
enum { UPG_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const UPG_STEPS[] = { UPG_STAGES(JS_STEP_STAGE_LABEL) NULL };

static int js_idb_upgrade_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    JSIdbOpenState *s = st;
    JSValueConst entry = task_entry(&s->hdr);
    JSValue db, req, conn, tx;
    double version, old_version;
    int r;

    open_state_start(s);
    STEP_DISPATCH(UPG_STAGES, s->hdr.stage, s->hdr.def->algorithm, JS_STEP_ABRUPT);

    STEP_ARM(UPG_CREATE);
        JS_FreeValue(ctx, cb_result);
        entry_assert_head(ctx, entry);
        conn = entry_get(ctx, entry, E_CONNECTION);
        db = idb_connection_database(ctx, conn);          /* step 1: "let db be connection's database" */
        req = entry_get(ctx, entry, E_REQUEST);
        version = entry_num(ctx, entry, E_VERSION);
        {
            /* Steps 2-3: "let transaction be a new upgrade transaction with connection used as connection.
               Set transaction's scope to connection's object store set." The scope is an Array of the store
               RECORDS that set holds; it is EMPTY for a database being created by this very upgrade, and
               idb_transaction_scope_add keeps it true as `createObjectStore` adds to the set. */
            JSValue stores = idb_connection_store_set(ctx, conn), scope = JS_NewArray(ctx);
            JSPropertyEnum *names = NULL;
            uint32_t count = 0, i;

            CHECK(!JS_IsException(scope), "IndexedDB: the upgrade transaction's scope could not be allocated");
            CHECK(JS_GetOwnPropertyNames(ctx, &names, &count, stores, JS_GPN_STRING_MASK) == 0,
                  "IndexedDB: a connection's object store set could not be enumerated");
            for (i = 0; i < count; i++) {
                JSValue store = JS_GetProperty(ctx, stores, names[i].atom);

                DCHECK(JS_IsObject(store), "a connection's object store set held something that is not a store");
                JS_DefinePropertyValueUint32(ctx, scope, i, store, JS_PROP_C_W_E);
            }
            for (i = 0; i < count; i++)
                JS_FreeAtom(ctx, names[i].atom);
            js_free(ctx, names);
            JS_FreeValue(ctx, stores);
            /* §2.7.3: "an upgrade transaction is a transaction with mode versionchange". Its durability is the
               default — §2.7's hint is set when a transaction is created and §5.7 names none, which IS the
               default the enumeration declares. */
            tx = idb_transaction_new(ctx, conn, scope, IDB_TX_VERSIONCHANGE, IDB_DUR_DEFAULT);
        }
        /* Step 4: "Set db's upgrade transaction to transaction." Step 5: "Set transaction's state to
           inactive." Step 6 "start transaction" is what §2.7.2's constraint already decided at creation.
           §5.7 gives the transaction NO cleanup event loop, and that omission is load-bearing: §2.7.1's
           cleanup would deactivate it at the end of this task, and its lifetime is the `upgradeneeded`
           dispatch (idb_transaction.h states the same thing from the other side). */
        idb_transaction_set_request(ctx, tx, req);
        idb_database_set_upgrade_transaction(ctx, db, JS_DupValue(ctx, tx));
        idb_transaction_set_state(ctx, tx, IDB_TX_INACTIVE);
        /* Steps 7-8: "Let old version be db's version. Set db's version to version." The old version is
           carried on the entry because step 9.5 fires it at the page AFTER this task has ended. */
        old_version = idb_database_version(ctx, db);
        entry_set(ctx, entry, E_OLD_VERSION, JS_NewFloat64(ctx, old_version));
        idb_database_set_version(ctx, tx, db, version);
        /* Step 8's last line: "Set request's processed flag to true." */
        idb_request_set_processed(ctx, req, true);
        JS_FreeValue(ctx, tx);
        JS_FreeValue(ctx, req);
        JS_FreeValue(ctx, db);
        JS_FreeValue(ctx, conn);
        STEP_GOTO(s->hdr.stage, UPG_DELIVER, &s->phase, NULL);
        return JS_STEP_YIELD;

    STEP_ARM(UPG_DELIVER);
        JS_FreeValue(ctx, cb_result);
        req = entry_get(ctx, entry, E_REQUEST);
        conn = entry_get(ctx, entry, E_CONNECTION);
        db = idb_connection_database(ctx, conn);
        tx = idb_database_upgrade_transaction(ctx, db);
        DCHECK(idb_transaction_is(tx), "§5.7 step 9 ran with no upgrade transaction on the database — step 4 "
                                       "set one and only §5.4 step 2.5.1 and §5.5 step 7.1 clear it, neither "
                                       "of which can have run before the transaction has finished");
        /* Steps 9.1-9.3: the request now REPORTS the connection, and §2.8.1's "the transaction of an open
           request is null unless an upgradeneeded event has been fired" becomes true here. */
        idb_request_set_result(ctx, req, JS_DupValue(ctx, conn));
        idb_request_set_transaction(ctx, req, JS_DupValue(ctx, tx));
        idb_request_set_done(ctx, req, true);
        /* Step 9.4: "Set transaction's state to active." This is what lets the handler call
           `createObjectStore` and place requests against the transaction. */
        idb_transaction_set_state(ctx, tx, IDB_TX_ACTIVE);
        JS_FreeValue(ctx, tx);
        JS_FreeValue(ctx, db);
        JS_FreeValue(ctx, conn);
        JS_FreeValue(ctx, req);
        STEP_GOTO(s->hdr.stage, UPG_FIRE, &s->phase, NULL);
        return JS_STEP_YIELD;

    STEP_ARM(UPG_FIRE);
        req = entry_get(ctx, entry, E_REQUEST);
        old_version = entry_num(ctx, entry, E_OLD_VERSION);
        version = entry_num(ctx, entry, E_VERSION);
        {
            bool threw = false;

            r = idb_fire_version_change_event_run(ctx, &s->phase, STEP_CB(s->cb), req, "upgradeneeded",
                                                  old_version, version, /*has_new*/ true, &s->ev, cb_result,
                                                  &threw, out_cb, out_argc);
            JS_FreeValue(ctx, req);
            if (r > 0) return r;
            if (r < 0) return JS_STEP_ABRUPT;
            s->did_throw = threw ? 1 : 0;
        }
        STEP_GOTO(s->hdr.stage, UPG_SETTLE, &s->phase, NULL);
        return JS_STEP_YIELD;

    STEP_ARM(UPG_SETTLE);
        JS_FreeValue(ctx, cb_result);
        conn = entry_get(ctx, entry, E_CONNECTION);
        db = idb_connection_database(ctx, conn);
        tx = idb_database_upgrade_transaction(ctx, db);
        /* Step 9.6: "If transaction's state is active, then: set transaction's state to inactive. If didThrow
           is true, run abort a transaction with transaction and a newly created AbortError DOMException."
           A handler that called `transaction.abort()` has already moved it, and then none of this runs. */
        if (idb_transaction_is(tx) && idb_transaction_state(ctx, tx) == IDB_TX_ACTIVE) {
            idb_transaction_set_state(ctx, tx, IDB_TX_INACTIVE);
            if (s->did_throw) {
                idb_transaction_abort(ctx, tx, open_dom_exception(ctx, "AbortError",
                                                                  "a listener threw during upgradeneeded"));
            } else if (idb_transaction_requests_empty(ctx, tx)) {
                /* §2.7.1's lifetime: "the implementation must attempt to commit an inactive transaction when
                   all requests placed against the transaction have completed ... no new requests have been
                   placed against the transaction, and the transaction has not been aborted." An upgrade
                   transaction has NO cleanup event loop, so §2.7.1's cleanup will never see it — this is the
                   other site that sentence applies at, and without it a migration that placed no request
                   would leave the transaction inactive forever and §5.7 step 10 would never return. A
                   transaction the handler DID place requests against commits from §5.9 step 9.3 instead. */
                idb_transaction_commit(ctx, tx);
            }
        }
        JS_FreeValue(ctx, tx);
        JS_FreeValue(ctx, db);
        JS_FreeValue(ctx, conn);
        /* Step 10: "Wait for transaction to finish." The transaction's commit or abort task tells this
           component through the rendezvous idb_transaction.h declares, and §5.1 continues in §4.3's
           completion task. */
        return JS_STEP_DONE;
}

static const JSTrampStepDef js_idb_upgrade_def = {
    sizeof(JSIdbOpenState), js_idb_upgrade_step, NULL, 0, .visit = js_idb_open_visit,
    .algorithm = "Indexed Database §5.7's upgrade a database", .steps = UPG_STEPS
};

/* §5.7 step 10's rendezvous. The transaction names its own open request; the ENTRY is the head of that
   database's connection queue, which §2.8.2 guarantees is this request's. */
static void idb_open_upgrade_finished(JSContext *ctx, JSValueConst tx)
{
    JSValue conn = idb_transaction_connection(ctx, tx), db = idb_connection_database(ctx, conn);
    JSValue name = idb_database_name(ctx, db), req = idb_transaction_request(ctx, tx);
    const char *cname = JS_ToCString(ctx, name);
    JSValue q, entry, entry_req;

    CHECK(cname != NULL, "IndexedDB: a finishing upgrade transaction's database could not report its name");
    q = open_queue(ctx, cname, /*create*/ false);
    DCHECK(JS_IsArray(q) && open_list_len(ctx, q) > 0,
           "an upgrade transaction finished with no open request queued for its database — §2.8.2's queue is "
           "what §5.7 was reached through, and an entry leaves it only when its request completes");
    entry = JS_GetPropertyUint32(ctx, q, 0);
    entry_req = entry_get(ctx, entry, E_REQUEST);
    DCHECK(JS_VALUE_GET_PTR(entry_req) == JS_VALUE_GET_PTR(req),
           "the head of a connection queue is not the open request the upgrade transaction was created for — "
           "§2.8.2 says each request runs to completion before the next is processed, so the two disagreeing "
           "means an entry left the queue early");
    JS_FreeValue(ctx, entry_req);
    /* §5.1 steps 10.8-10.9 and §4.3's completion task, in the one machine that performs them. */
    entry_enqueue(ctx, g_result_stepid, entry);
    JS_FreeValue(ctx, entry);
    JS_FreeValue(ctx, q);
    JS_FreeCString(ctx, cname);
    JS_FreeValue(ctx, name);
    JS_FreeValue(ctx, req);
    JS_FreeValue(ctx, db);
    JS_FreeValue(ctx, conn);
}

/* ---- §5.1 steps 10.8-10.9 and §4.3's COMPLETION TASK -------------------------------------------------------
 *
 * §4.3's own question-box is why these are not §5.9's and §5.10's steps: "there is no transaction associated
 * with the request (at this point), so those steps — which activate an associated transaction before dispatch
 * and deactivate the transaction after dispatch — do not apply." So the event is a plain §2.9 dispatch and
 * nothing follows it but the queue moving on. */

#define RES_STAGES(X) \
    X(RES_CHECK,   "Indexed Database §5.1 steps 10.8-10.9 (a connection that was closed, or a request whose " \
                   "error is set, is an AbortError — and the second closes the connection first)") \
    X(RES_SETTLE,  "Indexed Database §4.3's open() completion task, steps 1-3 of either arm — ONE O(1) " \
                   "engine action: the request's result or its error is written and its done flag is set") \
    X(RES_FIRE,    "Indexed Database §4.3's open() completion task, the fire (an event named success at " \
                   "request, or one named error with its bubbles and cancelable attributes initialized to " \
                   "true)") \
    X(RES_DEQUEUE, "Indexed Database §2.8.2 (this open request has run to completion, so the next request in " \
                   "the connection queue is processed)")
enum { RES_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const RES_STEPS[] = { RES_STAGES(JS_STEP_STAGE_LABEL) NULL };

static int js_idb_result_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    JSIdbOpenState *s = st;
    JSValueConst entry = task_entry(&s->hdr);
    JSValue req, conn, err;
    int r;

    open_state_start(s);
    STEP_DISPATCH(RES_STAGES, s->hdr.stage, s->hdr.def->algorithm, JS_STEP_ABRUPT);

    STEP_ARM(RES_CHECK);
        JS_FreeValue(ctx, cb_result);
        entry_assert_head(ctx, entry);
        req = entry_get(ctx, entry, E_REQUEST);
        conn = entry_get(ctx, entry, E_CONNECTION);
        err = idb_request_error(ctx, req);
        if (JS_IsNull(err) && idb_connection_is(conn)) {
            /* Step 10.8: "If connection was closed, return a newly created AbortError DOMException." A
               `versionchange` or `upgradeneeded` handler that closed the connection it was handed gets this. */
            if (idb_connection_is_closed(ctx, conn))
                idb_request_set_error(ctx, req, open_dom_exception(ctx, "AbortError",
                                                                   "the connection was closed during the "
                                                                   "upgrade"));
        } else if (!JS_IsNull(err) && idb_connection_is(conn)) {
            /* Step 10.9: "If request's error is set, run the steps to close a database connection with
               connection, return a newly created AbortError DOMException." The connection is closed FIRST —
               an aborted upgrade must not leave one open, or the next open of that name is blocked forever. */
            idb_connection_close(ctx, conn);
            idb_request_set_error(ctx, req, open_dom_exception(ctx, "AbortError",
                                                               "the upgrade transaction was aborted"));
        }
        JS_FreeValue(ctx, err);
        JS_FreeValue(ctx, conn);
        JS_FreeValue(ctx, req);
        STEP_GOTO(s->hdr.stage, RES_SETTLE, &s->phase, NULL);
        return JS_STEP_YIELD;

    STEP_ARM(RES_SETTLE);
        JS_FreeValue(ctx, cb_result);
        req = entry_get(ctx, entry, E_REQUEST);
        err = idb_request_error(ctx, req);
        if (JS_IsNull(err)) {
            conn = entry_get(ctx, entry, E_CONNECTION);
            DCHECK(idb_connection_is(conn), "§4.3's completion task found neither a connection nor an error — "
                                            "§5.1 answers with one or the other on every path");
            idb_request_set_result(ctx, req, conn);
            idb_request_set_error(ctx, req, JS_UNDEFINED);
        } else {
            /* "Set request's result to undefined. Set request's error to result." */
            idb_request_set_result(ctx, req, JS_UNDEFINED);
        }
        idb_request_set_done(ctx, req, true);
        s->did_throw = JS_IsNull(err) ? 0 : 1;   /* which of the two events the next stage fires */
        JS_FreeValue(ctx, err);
        JS_FreeValue(ctx, req);
        STEP_GOTO(s->hdr.stage, RES_FIRE, &s->phase, NULL);
        return JS_STEP_YIELD;

    STEP_ARM(RES_FIRE);
        req = entry_get(ctx, entry, E_REQUEST);
        if (JS_IsUndefined(s->ev)) {
            /* An `error` here BUBBLES and is CANCELABLE, and a `success` is neither — §4.3 spells both out,
               and the difference is what lets a page cancel the default the error would otherwise carry. */
            s->ev = event_new(ctx, s->did_throw ? "error" : "success", s->did_throw != 0, s->did_throw != 0);
            if (JS_IsException(s->ev)) {
                s->ev = JS_UNDEFINED;
                JS_FreeValue(ctx, req);
                JS_FreeValue(ctx, cb_result);
                return JS_STEP_ABRUPT;
            }
        }
        r = event_target_fire_run(ctx, &s->phase, STEP_CB(s->cb), req, s->ev, JS_UNDEFINED, cb_result, NULL,
                                  out_cb, out_argc);
        JS_FreeValue(ctx, req);
        if (r > 0) return r;
        if (r < 0) return JS_STEP_ABRUPT;
        STEP_GOTO(s->hdr.stage, RES_DEQUEUE, &s->phase, NULL);
        return JS_STEP_YIELD;

    STEP_ARM(RES_DEQUEUE);
        JS_FreeValue(ctx, cb_result);
        {
            /* §2.8.2: "each request must run to completion before the next request is processed." This is that
               completion, and the next entry's §5.1 starts here — which is the whole of how step 3's wait is
               discharged rather than waited on. */
            JSValue q = entry_queue(ctx, entry), head;
            uint32_t i, n = open_list_len(ctx, q);

            DCHECK(n > 0, "an open request completed with an empty connection queue");
            head = JS_GetPropertyUint32(ctx, q, 0);
            DCHECK(JS_VALUE_GET_PTR(head) == JS_VALUE_GET_PTR(entry),
                   "the open request that completed is not the head of its connection queue");
            JS_FreeValue(ctx, head);
            for (i = 0; i + 1 < n; i++)
                JS_DefinePropertyValueUint32(ctx, q, i, JS_GetPropertyUint32(ctx, q, i + 1), JS_PROP_C_W_E);
            JS_SetPropertyStr(ctx, q, "length", JS_NewUint32(ctx, n - 1));
            if (n > 1) {
                JSValue next = JS_GetPropertyUint32(ctx, q, 0);

                entry_enqueue(ctx, g_open_stepid, next);
                JS_FreeValue(ctx, next);
            }
            JS_FreeValue(ctx, q);
        }
        return JS_STEP_DONE;
}

static const JSTrampStepDef js_idb_result_def = {
    sizeof(JSIdbOpenState), js_idb_result_step, NULL, 0, .visit = js_idb_open_visit,
    .algorithm = "Indexed Database §5.1 steps 10.8-10.9 and §4.3's open() completion task",
    .steps = RES_STEPS
};

/* ---- §4.3's open(), everything after its two synchronous steps -------------------------------------------- */

JSValue idb_open_request(JSContext *ctx, const char *name, double version, bool has_version)
{
    JSValue req, entry, q;
    uint32_t n;

    DCHECK(name != NULL, "§4.3's open was given no database name");
    DCHECK(!has_version || version >= 1, "§4.3's open reached §5.1 with a zero version — its first step is "
                                         "\"if version is 0 (zero), throw a TypeError\", reported by the "
                                         "member before this");
    req = idb_request_new_open(ctx);
    entry = idl_slots_new(ctx);
    CHECK(!JS_IsException(entry), "IndexedDB: §5.1's entry could not be allocated");
    entry_set(ctx, entry, E_REQUEST, JS_DupValue(ctx, req));
    entry_set(ctx, entry, E_NAME, JS_NewString(ctx, name));
    entry_set(ctx, entry, E_VERSION, JS_NewFloat64(ctx, has_version ? version : 0));
    entry_set(ctx, entry, E_HAS_VERSION, JS_NewBool(ctx, has_version));
    entry_set(ctx, entry, E_DB, JS_NULL);
    entry_set(ctx, entry, E_CONNECTION, JS_NULL);
    entry_set(ctx, entry, E_OPEN_CONNS, JS_NewArray(ctx));
    entry_set(ctx, entry, E_CURSOR, JS_NewInt32(ctx, 0));
    entry_set(ctx, entry, E_OLD_VERSION, JS_NewFloat64(ctx, 0));
    entry_set(ctx, entry, E_BLOCKED, JS_FALSE);

    /* §5.1 steps 1-2: "let queue be the connection queue for storageKey and name. Add request to queue." */
    q = open_queue(ctx, name, /*create*/ true);
    n = open_list_len(ctx, q);
    JS_DefinePropertyValueUint32(ctx, q, n, JS_DupValue(ctx, entry), JS_PROP_C_W_E);
    JS_FreeValue(ctx, q);
    /* Step 3: "Wait until all previous requests in queue have been processed." An entry that arrives at the
       HEAD starts now; one behind another starts when that one's §4.3 completion task dequeues it. Which is
       what makes the wait an ORDERING rather than a loop. */
    if (n == 0)
        entry_enqueue(ctx, g_open_stepid, entry);
    JS_FreeValue(ctx, entry);
    return req;   /* "Return a new IDBOpenDBRequest object for request." */
}

void idb_open_init(JSContext *ctx)
{
    JSRuntime *rt = JS_GetRuntime(ctx);

    DCHECK(JS_IsUndefined(g_queues), "idb_open_init ran twice — §2.8.2's queues belong to the AGENT, and a "
                                     "second set would let two opens of one name run at once");
    g_queues = idl_slots_new(ctx);
    CHECK(!JS_IsException(g_queues), "IndexedDB: §2.8.2's connection queues could not be allocated");
    g_blocked = JS_NewArray(ctx);
    CHECK(!JS_IsException(g_blocked), "IndexedDB: §5.1 step 10.6's blocked list could not be allocated");
    g_open_stepid = JS_RegisterStepDef(rt, &js_idb_open_def);
    g_upgrade_stepid = JS_RegisterStepDef(rt, &js_idb_upgrade_def);
    g_result_stepid = JS_RegisterStepDef(rt, &js_idb_result_def);
    /* The two rendezvous this algorithm's un-dischargeable waits need. Registered HERE, in the declaration of
       the component that performs §5.1, which is what makes each of them one question with one asker. */
    idb_connection_set_closed_hook(idb_open_connection_closed);
    idb_transaction_set_upgrade_finished_hook(idb_open_upgrade_finished);
    agent_state_value("idb_open", &g_queues, "§2.8.2's connection queues, and the declaration latch");
    agent_state_value("idb_open", &g_blocked, "§5.1 step 10.6's blocked list");
    agent_state_id("idb_open", &g_open_stepid, "§5.1's open machine");
    agent_state_id("idb_open", &g_upgrade_stepid, "§5.1's run-an-upgrade-transaction machine");
    agent_state_id("idb_open", &g_result_stepid, "§5.1's deliver-the-result machine");
}

void idb_open_free(JSRuntime *rt)
{
    /* NOT `if (JS_IsUndefined(g_queues)) return;` — the declare pass of core/platform.c's one list is
       unconditional and its table asserts a release row has a declare, so an undeclared release is a host
       tearing this component down with something that is not that list. */
    DCHECK(!JS_IsUndefined(g_queues), "§2.8.2's queues were released in an agent that never declared them");
    idb_connection_set_closed_hook(NULL);
    idb_transaction_set_upgrade_finished_hook(NULL);
    JS_FreeValueRT(rt, g_queues);
    JS_FreeValueRT(rt, g_blocked);
    g_queues = JS_UNDEFINED;
    g_blocked = JS_UNDEFINED;
    g_open_stepid = g_upgrade_stepid = g_result_stepid = -1;
}
