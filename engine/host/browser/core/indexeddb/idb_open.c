/* INDEXED DATABASE §2.8.2's CONNECTION QUEUE and the algorithms PROCESSED IN IT — §5.1's OPEN A DATABASE
 * CONNECTION, §5.3's DELETE A DATABASE, §5.7's UPGRADE A DATABASE, and §4.3's two completion tasks.
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
 *   step 10.5 "wait until all connections in openConnections are closed" — satisfied by the PAGE calling
 *            `close()`, or by the last transaction of an already-close-pending connection finishing. Nothing
 *            the queue can order.
 *   step 10.6's own last step, §5.7 step 10 "wait for transaction to finish" — satisfied by the upgrade
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
 * WHY §5.3 IS IN THIS FILE AND NOT A COMPONENT OF ITS OWN. It is a SECOND ALGORITHM over the FIRST's state:
 * the same connection queue, the same entry record, the same step-10.5 blocked list, the same
 * `versionchange`-then-`blocked` walk, and the same §4.3 completion task and dequeue. Splitting it out would
 * export ten of this file's internals — the entry accessors, the queue, the blocked list, the shared step
 * state, the enqueue — which is a wider seam than the thing it separates, and each of those exports is a
 * second way to reach a queue whose whole contract is that there is one. So the file's contract is §2.8.2's
 * QUEUE and everything processed in it, and what distinguishes §5.1's entry from §5.3's is one field (E_KIND).
 * THE RULE THAT FIELD CARRIES, and the one thing sharing a record for two algorithms costs: every question of
 * the form "which of the two is this" is answered from E_KIND and from nothing else — which machine STARTS an
 * entry, which machine RESUMES a parked one, and which arm of §4.3's completion task settles it. A caller that
 * supplies its own answer to any of the three has made the record describe a request nobody made, and the
 * shape of that failure is that it SUCCEEDS: §5.1's steps run to completion over a delete's entry.
 *
 * §5.3 IS §5.1 WITH THREE DIFFERENCES AND NOT A COPY OF IT: its openConnections is ALL of the database's
 * connections (there is no connection of its own to except), its two version change events carry a NULL new
 * version rather than the requested one, and where §5.1 continues into §5.7's upgrade it continues into a
 * removal from §2.1's set. Everything else — the ordering, the selection-then-fire split, the once-only
 * `blocked`, the park — is the same mechanism reached through the same code.
 *
 * WHAT IS ABSENT AND WHY. Nothing of §4.3's is: `databases()` needs neither this queue nor an entry (it is a
 * promise over §2.1's set, with no connection to wait for), so it is a member in indexed_db.c beside `cmp`. */
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

/* §5.1's AND §5.3's OWN LOCALS, on the record that carries them across the algorithm's hand-offs. */
#define E_REQUEST      "request"          /* §2.8.1's open request */
/* WHICH OF §4.3's TWO ALGORITHMS THIS ENTRY IS. §2.8.1's open request is "used when opening a connection OR
   deleting a database", so §2.8.2's queue holds both interleaved and every place that has to tell them apart
   asks THIS: which machine starts an entry, which machine resumes a parked one, and which arm of §4.3's
   completion task settles it — whose success arm is a connection and a plain `success` for one and undefined
   and an IDBVersionChangeEvent for the other. It is on the ENTRY rather than derived from which fields are
   set, because "E_CONNECTION is null" is also true of an open that has not reached step 8 yet — and, the
   other way about, a delete entry that HAS one is not a delete that acquired a connection but an entry that
   was run through the wrong algorithm, which is what entry_enqueue's pairing assertion makes unreachable. */
#define E_KIND         "kind"
#define E_NAME         "name"             /* the database name, which is also this entry's queue */
#define E_VERSION      "version"          /* §5.1's `version` once step 5 has decided it */
#define E_HAS_VERSION  "hasVersion"       /* whether §4.3's caller gave one — step 5 branches on it */
#define E_DB           "db"               /* §5.1 step 4's `db`, JS_NULL until step 6 */
#define E_CONNECTION   "connection"       /* §5.1 step 8's `connection`, JS_NULL until then */
#define E_OPEN_CONNS   "openConnections"  /* §5.1 step 10.1's set, snapshotted where the step computes it */
#define E_CURSOR       "cursor"           /* which of them step 10.2 is standing on */
/* THE CONNECTION step 10.2's SELECTION PICKED, held across the fire that follows it — JS_NULL between turns.
   It is on the entry rather than in a C local because the fire is a SUSPENSION: the stage that holds it is
   re-entered with its request's answer, and re-deriving the target there would re-run the very filter the fire
   has just invalidated. See OPN_STAGES. */
#define E_TARGET       "versionChangeTarget"
/* THE VERSION THE DATABASE HELD BEFORE THIS ALGORITHM RAN — §5.7 step 7's `old version`, fired at step 9.5,
   and §5.3 step 10's `version`, which is that algorithm's RETURN VALUE and what §4.3's delete completion task
   fires as the success event's `oldVersion`. One field because it is one fact, and §5.3 step 4's "otherwise,
   return 0 (zero)" is the 0 idb_open_request leaves here. */
#define E_OLD_VERSION  "oldVersion"
#define E_BLOCKED      "blockedFired"     /* whether step 10.4 has already fired `blocked` */
/* §5.1 step 10.8's condition. §5.7 step 10 waits for the upgrade transaction to FINISH, and which of the two
   endings it reached is the whole of what that step asks — see idb_open_upgrade_finished. FALSE for an open
   that never ran an upgrade at all, which is the same answer. */
#define E_ABORTED      "upgradeAborted"

static JSValue g_queues = JS_UNDEFINED;   /* §2.8.2: name -> Array of entries, in arrival order */
/* THE ENTRIES PARKED AT STEP 10.5. A list rather than a walk over every queue's head, because what wakes them
   is a CONNECTION closing and the question that connection answers is "is any open request waiting on me" —
   asking it of a list of waiters is the question; asking it of every queue would be a search for the answer. */
static JSValue g_blocked = JS_UNDEFINED;

static int g_open_stepid = -1, g_upgrade_stepid = -1, g_result_stepid = -1;
static int g_delete_stepid = -1, g_delete_finish_stepid = -1;

/* E_KIND's two values. §2.8.1's open request is one type used by two algorithms; this is which. */
enum { IDB_REQ_OPEN, IDB_REQ_DELETE };

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

static int entry_kind(JSContext *ctx, JSValueConst e)
{
    int kind = (int)entry_num(ctx, e, E_KIND);

    DCHECK(kind == IDB_REQ_OPEN || kind == IDB_REQ_DELETE,
           "an open request in §2.8.2's queue is neither §5.1's nor §5.3's — E_KIND is written once, by the "
           "entry's own constructor, and only those two exist");
    return kind;
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

/* WHICH OF THIS FILE'S MACHINES MAY RUN FOR WHICH KIND OF ENTRY. Four of the five perform a span of exactly
   ONE of the two standards — §5.1 Opening a database connection's steps 4-10.6 and §5.7 Upgrading a database
   serve an open, §5.3 Deleting a database's steps 4-9 and its steps 10-12 serve a delete — and the fifth is
   §4.3 The IDBFactory interface's two completion tasks, which are one machine BECAUSE they differ in one
   sentence. So (machine, kind) is a total function with one answer, and it is written down HERE rather than
   re-decided at each hand-off. */
static inline bool machine_serves(JSContext *ctx, int stepid, JSValueConst e)
{
    if (stepid == g_open_stepid || stepid == g_upgrade_stepid)
        return entry_kind(ctx, e) == IDB_REQ_OPEN;
    if (stepid == g_delete_stepid || stepid == g_delete_finish_stepid)
        return entry_kind(ctx, e) == IDB_REQ_DELETE;
    return stepid == g_result_stepid;   /* §4.3's completion tasks, the one machine both kinds end in */
}

/* WHICH MACHINE STARTS AN ENTRY — the same question entry_wait_continuation asks about a RESUMED one, and the
   same answer: a fact about which algorithm the entry IS, so it is read off E_KIND and never supplied by the
   caller. THE DEFECT SHAPE IT EXISTS TO END: two places answered this question independently, and the one that
   answered it at a HAND-OFF rather than from the entry named §5.1's machine as a constant — so a queue holding
   `open(n)` ahead of `deleteDatabase(n)`, which is how half of this standard's fixtures begin, dequeued the
   open and started §5.3's request into §5.1's steps. Those steps do not reject a delete; they look a name up,
   default a version, RE-CREATE the database the request was asked to remove, and open a connection to it, so
   the request succeeded at the wrong standard and nothing until §4.3's completion task had cause to look. */
static int entry_start_machine(JSContext *ctx, JSValueConst e)
{
    int id = entry_kind(ctx, e) == IDB_REQ_OPEN ? g_open_stepid : g_delete_stepid;

    DCHECK(id >= 0, "an open request was started before idb_open_init registered its algorithm's machine");
    return id;
}

/* Enqueue one of this file's machines over `entry`. Every hand-off in §5.1 is one of these — which is what
   makes this the ONE place the pairing above can be asserted, at the instant the task is MINTED rather than
   wherever the wrong machine's writes are first noticed. A machine that runs the other algorithm's steps over
   an entry does not fail; it SUCCEEDS at the wrong standard, so nothing downstream is obliged to notice. */
static void entry_enqueue(JSContext *ctx, int stepid, JSValueConst entry)
{
    JSValue fn;

    DCHECK(machine_serves(ctx, stepid, entry),
           "an §2.8.1 open request was handed to a machine that performs the OTHER algorithm's steps — §5.1 "
           "Opening a database connection and §5.3 Deleting a database share §2.8.2's queue and this entry "
           "record, and nothing else, so which machine runs an entry is decided by its kind and by no caller");
    fn = JS_NewStepClosure(ctx, stepid, 0, 1, &entry);
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

/* ---- §5.1's OPEN A DATABASE CONNECTION, steps 4 through 10.6 ---------------------------------------------- */

/* THE SELECTION AND THE FIRE ARE TWO STAGES, AND SPLITTING THEM IS THE WHOLE OF WHY step 10.2 IS TWO.
 *
 * Both of this machine's fires used to live in the same stage as the walk that DECIDED to fire — and the
 * decision is a fact about the connections, which is exactly what firing the event CHANGES. Step 10.2's own
 * NOTE says so: "firing this event might cause one or more of the other objects in openConnections to be
 * closed, in which case the versionchange event is not fired at those objects." A `versionchange` handler
 * calling `db.close()` sets the close pending flag on the connection just fired AT, so the resume leg of that
 * same fire re-walked the filter, SKIPPED that connection, and either collected the previous target's answer as
 * a different target's or ran off the end of the set and left the stage with `s->phase` at 1 — which is the
 * two-phase invariant quickjs-step.h's STEP_GOTO states, and it aborted there rather than silently.
 * Step 10.4's `blocked` fire had the identical shape: `open_still_blocked` is re-evaluated on its own resume
 * leg, and a handler that closed the connections flips it.
 *
 * So the rule this file now follows is §scheduler's: a stage that HOLDS a request does nothing but hold it, and
 * an operation that becomes a work item takes its INPUTS with it. The selection stage picks the target, records
 * it on the entry, and holds no cursor; the fire stage reads that one recorded target and reaches the SAME
 * idb_fire_version_change_event_run on every entry, resume included. */
#define OPN_STAGES(X) \
    X(OPN_FIND,    "Indexed Database §5.1 steps 4-6 — ONE O(1) engine action: the database named `name` is " \
                   "looked up in this storage key, `version` is defaulted from it, and a database that does " \
                   "not exist is created with version 0") \
    X(OPN_VERSION, "Indexed Database §5.1 step 7 (if db's version is greater than version, return a " \
                   "VersionError)") \
    X(OPN_CONNECT, "Indexed Database §5.1 steps 8-9 (let connection be a new connection to db; set " \
                   "connection's version to version)") \
    X(OPN_VERSIONCHANGE, "Indexed Database §5.1 step 10.2's SELECTION, one connection per turn (the next " \
                         "entry of openConnections that does not have its close pending flag set)") \
    X(OPN_VERSIONCHANGE_FIRE, "Indexed Database §5.1 step 10.2's FIRE (fire a version change event named " \
                              "versionchange at the selected entry with db's version and version); step " \
                              "10.3's \"wait for all of the events to be fired\" is this walk's own turns") \
    X(OPN_BLOCKED, "Indexed Database §5.1 step 10.4's DECISION (whether any of the connections in " \
                   "openConnections are still not closed, asked once with every cursor at rest)") \
    X(OPN_BLOCKED_FIRE, "Indexed Database §5.1 step 10.4's FIRE (fire a version change event named blocked " \
                        "at request with db's version and version)") \
    X(OPN_WAIT,    "Indexed Database §5.1 step 10.5 (wait until all connections in openConnections are " \
                   "closed)") \
    X(OPN_UPGRADE, "Indexed Database §5.1 step 10.6 (run upgrade a database using connection, version and " \
                   "request)")
enum { OPN_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const OPN_STEPS[] = { OPN_STAGES(JS_STEP_STAGE_LABEL) NULL };

/* §5.1 step 10.5's question, asked of the set step 10.1 snapshotted. A connection that has become closed has
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
        /* THE MIRROR OF DEL_FIND's, AND ITS ABSENCE IS WHY THIS FILE HAD A DEFECT. A delete entry started here
           does not fail — §5.1's steps 4-6 look a name up, default a version and CREATE the database, all of
           which succeed for any entry — so it ran four stages, opened a connection at OPN_CONNECT and was
           first noticed at §4.3's completion task, by which point the record no longer described any request
           the page made. entry_enqueue's pairing makes it unreachable; this is what says so where the steps
           are. */
        DCHECK(entry_kind(ctx, entry) == IDB_REQ_OPEN,
               "§5.1's machine ran for an entry that is not an open request");
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
               later is not one this open waits for, and step 10.5 asks about THESE. */
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

    /* Step 10.2's SELECTION. "For each entry of openConnections that does not have its close pending flag set
       to true, [fire] a version change event named versionchange at entry with db's version and version." The
       flag is re-read at each TURN of the walk rather than filtered once, which is what the step's own note
       requires — "firing this event might cause one or more of the other objects in openConnections to be
       closed, in which case the versionchange event is not fired at those objects" — and it is re-read HERE,
       where no request is in flight, rather than on the resume leg of a fire it has already begun.
       Step 10.3's "wait for all of the events to be fired" is discharged by this walk, whose every turn is a
       REST POINT — the same discharge idb_request.c makes for §5.6 step 5.1. */
    STEP_ARM(OPN_VERSIONCHANGE);
        JS_FreeValue(ctx, cb_result);
        {
            JSValue set = entry_get(ctx, entry, E_OPEN_CONNS), target = JS_NULL;
            uint32_t i = (uint32_t)entry_num(ctx, entry, E_CURSOR), n = open_list_len(ctx, set);

            while (i < n) {
                JS_FreeValue(ctx, target);
                target = JS_GetPropertyUint32(ctx, set, i);
                DCHECK(idb_connection_is(target), "§5.1 step 10.1's openConnections held something that is "
                                                  "not a connection");
                if (!idb_connection_close_pending(ctx, target))
                    break;
                i++;
            }
            JS_FreeValue(ctx, set);
            entry_set(ctx, entry, E_CURSOR, JS_NewInt32(ctx, (int32_t)i));
            if (i >= n) {
                JS_FreeValue(ctx, target);
                STEP_GOTO(s->hdr.stage, OPN_BLOCKED, &s->phase, NULL);
                return JS_STEP_YIELD;
            }
            /* THE FIRE'S INPUT, TAKEN WITH IT. The connection this turn fires at is decided here and read back
               there, because the fire is what makes the decision untrue. */
            entry_set(ctx, entry, E_TARGET, target);
            STEP_GOTO(s->hdr.stage, OPN_VERSIONCHANGE_FIRE, &s->phase, NULL);
            return JS_STEP_YIELD;
        }

    /* Step 10.2's FIRE, holding that one request and deciding nothing. */
    STEP_ARM(OPN_VERSIONCHANGE_FIRE);
        {
            JSValue target = entry_get(ctx, entry, E_TARGET);

            DCHECK(idb_connection_is(target), "§5.1 step 10.2's fire was entered with no connection selected — "
                                              "OPN_VERSIONCHANGE records the one it picked and is the only "
                                              "stage that points here");
            db = entry_get(ctx, entry, E_DB);
            db_version = idb_database_version(ctx, db);
            JS_FreeValue(ctx, db);
            version = entry_num(ctx, entry, E_VERSION);
            r = idb_fire_version_change_event_run(ctx, &s->phase, STEP_CB(s->cb), target, "versionchange",
                                                  db_version, version, /*has_new*/ true, &s->ev, cb_result,
                                                  NULL, out_cb, out_argc);
            JS_FreeValue(ctx, target);
            if (r > 0) return r;
            if (r < 0) return JS_STEP_ABRUPT;
            /* The event is this connection's; the next one gets its own, which §4.2 step 1 creates. */
            JS_FreeValue(ctx, s->ev);
            s->ev = JS_UNDEFINED;
            entry_set(ctx, entry, E_TARGET, JS_NULL);
            entry_set(ctx, entry, E_CURSOR,
                      JS_NewInt32(ctx, (int32_t)((uint32_t)entry_num(ctx, entry, E_CURSOR) + 1)));
            STEP_GOTO(s->hdr.stage, OPN_VERSIONCHANGE, &s->phase, NULL);
            return JS_STEP_YIELD;
        }

    STEP_ARM(OPN_BLOCKED);
        /* Step 10.4's DECISION: "If any of the connections in openConnections are still not closed, queue a
           database task to fire a version change event named blocked at request with db's version and
           version." ONCE — the flag is what stops a second `blocked` when a close arrives and the wait resumes,
           and it is SET HERE rather than after the fire, because a `blocked` handler that closes the
           connections would otherwise flip `open_still_blocked` under the fire's own resume leg. */
        JS_FreeValue(ctx, cb_result);
        if (!open_still_blocked(ctx, entry) || entry_flag(ctx, entry, E_BLOCKED)) {
            STEP_GOTO(s->hdr.stage, OPN_WAIT, &s->phase, NULL);
            return JS_STEP_YIELD;
        }
        entry_set(ctx, entry, E_BLOCKED, JS_TRUE);
        STEP_GOTO(s->hdr.stage, OPN_BLOCKED_FIRE, &s->phase, NULL);
        return JS_STEP_YIELD;

    /* Step 10.4's FIRE, holding that one request and deciding nothing. */
    STEP_ARM(OPN_BLOCKED_FIRE);
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
        JS_FreeValue(ctx, s->ev);
        s->ev = JS_UNDEFINED;
        STEP_GOTO(s->hdr.stage, OPN_WAIT, &s->phase, NULL);
        return JS_STEP_YIELD;

    STEP_ARM(OPN_WAIT);
        JS_FreeValue(ctx, cb_result);
        /* Step 10.5: "Wait until all connections in openConnections are closed." THIS is the wait no queue can
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
        /* Step 10.6: "Run upgrade a database using connection, version and request." §5.7's own step 10 is a
           wait for the transaction to finish, so it is a machine of its own and steps 10.7-10.8 continue in
           §4.3's completion task, which idb_open_upgrade_finished enqueues. */
        entry_enqueue(ctx, g_upgrade_stepid, entry);
        return JS_STEP_DONE;
}

static const JSTrampStepDef js_idb_open_def = {
    sizeof(JSIdbOpenState), js_idb_open_step, NULL, 0, .visit = js_idb_open_visit,
    .algorithm = "Indexed Database §5.1's open a database connection, steps 4 through 10.6",
    .steps = OPN_STEPS
};

/* WHICH MACHINE CONTINUES AN ENTRY THAT HAS FINISHED WAITING FOR CONNECTIONS TO CLOSE. §5.1 step 10.5's
   continuation is step 10.6, "run upgrade a database"; §5.3 step 9's is step 10, "let version be db's version".
   Both are machines, and which one an entry resumes into is a fact about WHICH ALGORITHM the entry is — asked
   of E_KIND rather than stored as a step id, because a step id is a handle this process handed out and the
   entry outlives the process (it parks to the cold tier and resumes in a later session), while the kind is
   what §2.8.1 says the request is. */
static int entry_wait_continuation(JSContext *ctx, JSValueConst e)
{
    int id = entry_kind(ctx, e) == IDB_REQ_OPEN ? g_upgrade_stepid : g_delete_finish_stepid;

    DCHECK(id >= 0, "an entry finished waiting before idb_open_init registered the machine that continues it");
    return id;
}

/* §5.2's rendezvous — a connection has become CLOSED, so every open request parked at §5.1 step 10.5 or §5.3
   step 9 asks its own question again. It runs no page code (it reads flags the connection wrote) and it is not
   a machine, because each of those two continuations IS one. */
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
        entry_enqueue(ctx, entry_wait_continuation(ctx, e), e);
        JS_FreeValue(ctx, e);
        n--;
        i--;
    }
}

/* ---- §5.3's DELETE A DATABASE, steps 4 through 9 ----------------------------------------------------------- */

/* THE SELECTION AND THE FIRE ARE TWO STAGES here for the reason OPN_STAGES states at length and does not
   restate: step 6's own note is that firing the event may close the very connections the walk is standing on,
   so the stage that DECIDES holds no request and the stage that HOLDS one decides nothing. */
#define DEL_STAGES(X) \
    X(DEL_FIND,    "Indexed Database §5.3 steps 4-5 — ONE O(1) engine action: the database named `name` is " \
                   "looked up in this storage key (a name this storage key has no database for is step 4's " \
                   "\"otherwise, return 0 (zero)\") and openConnections is the set of ALL connections " \
                   "associated with it") \
    X(DEL_VERSIONCHANGE, "Indexed Database §5.3 step 6's SELECTION, one connection per turn (the next entry " \
                         "of openConnections that does not have its close pending flag set)") \
    X(DEL_VERSIONCHANGE_FIRE, "Indexed Database §5.3 step 6's FIRE (fire a version change event named " \
                              "versionchange at the selected entry with db's version and NULL); step 7's " \
                              "\"wait for all of the events to be fired\" is this walk's own turns") \
    X(DEL_BLOCKED, "Indexed Database §5.3 step 8's DECISION (whether any of the connections in " \
                   "openConnections are still not closed, asked once with every cursor at rest)") \
    X(DEL_BLOCKED_FIRE, "Indexed Database §5.3 step 8's FIRE (fire a version change event named blocked at " \
                        "request with db's version and null)") \
    X(DEL_WAIT,    "Indexed Database §5.3 step 9 (wait until all connections in openConnections are closed)")
enum { DEL_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const DEL_STEPS[] = { DEL_STAGES(JS_STEP_STAGE_LABEL) NULL };

static int js_idb_delete_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    JSIdbOpenState *s = st;
    JSValueConst entry = task_entry(&s->hdr);
    JSValue db, req;
    double db_version;
    int r;

    open_state_start(s);
    STEP_DISPATCH(DEL_STAGES, s->hdr.stage, s->hdr.def->algorithm, JS_STEP_ABRUPT);

    STEP_ARM(DEL_FIND);
        JS_FreeValue(ctx, cb_result);
        entry_assert_head(ctx, entry);
        DCHECK(entry_kind(ctx, entry) == IDB_REQ_DELETE,
               "§5.3's machine ran for an entry that is not a delete request");
        {
            JSValue nv = entry_get(ctx, entry, E_NAME);
            const char *name = JS_ToCString(ctx, nv);

            CHECK(name != NULL, "IndexedDB: an §5.3 entry could not report its own database name");
            /* Step 4: "Let db be the database named name in storageKey, if one exists. Otherwise, return 0
               (zero)." The 0 is what idb_delete_request already left in E_OLD_VERSION, and it is a SUCCESS —
               `deleteDatabase` of a name that was never opened fires `success` with oldVersion 0, which is
               what makes it the first statement of half the corpus's fixtures. */
            db = idb_database_find(ctx, name);
            JS_FreeCString(ctx, name);
            JS_FreeValue(ctx, nv);
        }
        if (JS_IsNull(db)) {
            JS_FreeValue(ctx, db);
            entry_enqueue(ctx, g_result_stepid, entry);
            return JS_STEP_DONE;
        }
        {
            /* Step 5: "Let openConnections be the set of all connections associated with db." SNAPSHOTTED
               here, where the step computes it, for the reason §5.1 step 10.1 is: step 9 waits for THESE, and
               §2.1.1's live set loses each connection as it closes. Unlike §5.1's there is nothing to except —
               this algorithm has no connection of its own. */
            JSValue all = idb_database_connections(ctx, db), set = JS_NewArray(ctx);
            uint32_t i, n = open_list_len(ctx, all);

            CHECK(!JS_IsException(set), "IndexedDB: §5.3 step 5's openConnections could not be allocated");
            for (i = 0; i < n; i++)
                JS_DefinePropertyValueUint32(ctx, set, i, JS_GetPropertyUint32(ctx, all, i), JS_PROP_C_W_E);
            JS_FreeValue(ctx, all);
            entry_set(ctx, entry, E_OPEN_CONNS, set);
            entry_set(ctx, entry, E_CURSOR, JS_NewInt32(ctx, 0));
        }
        entry_set(ctx, entry, E_DB, db);
        STEP_GOTO(s->hdr.stage, DEL_VERSIONCHANGE, &s->phase, NULL);
        return JS_STEP_YIELD;

    STEP_ARM(DEL_VERSIONCHANGE);
        JS_FreeValue(ctx, cb_result);
        {
            JSValue set = entry_get(ctx, entry, E_OPEN_CONNS), target = JS_NULL;
            uint32_t i = (uint32_t)entry_num(ctx, entry, E_CURSOR), n = open_list_len(ctx, set);

            while (i < n) {
                JS_FreeValue(ctx, target);
                target = JS_GetPropertyUint32(ctx, set, i);
                DCHECK(idb_connection_is(target), "§5.3 step 5's openConnections held something that is not a "
                                                  "connection");
                if (!idb_connection_close_pending(ctx, target))
                    break;
                i++;
            }
            JS_FreeValue(ctx, set);
            entry_set(ctx, entry, E_CURSOR, JS_NewInt32(ctx, (int32_t)i));
            if (i >= n) {
                JS_FreeValue(ctx, target);
                STEP_GOTO(s->hdr.stage, DEL_BLOCKED, &s->phase, NULL);
                return JS_STEP_YIELD;
            }
            entry_set(ctx, entry, E_TARGET, target);
            STEP_GOTO(s->hdr.stage, DEL_VERSIONCHANGE_FIRE, &s->phase, NULL);
            return JS_STEP_YIELD;
        }

    STEP_ARM(DEL_VERSIONCHANGE_FIRE);
        {
            JSValue target = entry_get(ctx, entry, E_TARGET);

            DCHECK(idb_connection_is(target), "§5.3 step 6's fire was entered with no connection selected — "
                                              "DEL_VERSIONCHANGE records the one it picked and is the only "
                                              "stage that points here");
            db = entry_get(ctx, entry, E_DB);
            db_version = idb_database_version(ctx, db);
            JS_FreeValue(ctx, db);
            /* "…with db's version and NULL." The null new version is the whole of what a page's
               `versionchange` handler tells a delete from an upgrade by, so `has_new` is false and not a 0. */
            r = idb_fire_version_change_event_run(ctx, &s->phase, STEP_CB(s->cb), target, "versionchange",
                                                  db_version, 0, /*has_new*/ false, &s->ev, cb_result,
                                                  NULL, out_cb, out_argc);
            JS_FreeValue(ctx, target);
            if (r > 0) return r;
            if (r < 0) return JS_STEP_ABRUPT;
            JS_FreeValue(ctx, s->ev);
            s->ev = JS_UNDEFINED;
            entry_set(ctx, entry, E_TARGET, JS_NULL);
            entry_set(ctx, entry, E_CURSOR,
                      JS_NewInt32(ctx, (int32_t)((uint32_t)entry_num(ctx, entry, E_CURSOR) + 1)));
            STEP_GOTO(s->hdr.stage, DEL_VERSIONCHANGE, &s->phase, NULL);
            return JS_STEP_YIELD;
        }

    STEP_ARM(DEL_BLOCKED);
        /* Step 8, ONCE — for the reason OPN_BLOCKED states: the flag is set BEFORE the fire, because a
           `blocked` handler that closes the connections would otherwise flip the decision under the fire's own
           resume leg. */
        JS_FreeValue(ctx, cb_result);
        if (!open_still_blocked(ctx, entry) || entry_flag(ctx, entry, E_BLOCKED)) {
            STEP_GOTO(s->hdr.stage, DEL_WAIT, &s->phase, NULL);
            return JS_STEP_YIELD;
        }
        entry_set(ctx, entry, E_BLOCKED, JS_TRUE);
        STEP_GOTO(s->hdr.stage, DEL_BLOCKED_FIRE, &s->phase, NULL);
        return JS_STEP_YIELD;

    STEP_ARM(DEL_BLOCKED_FIRE);
        req = entry_get(ctx, entry, E_REQUEST);
        db = entry_get(ctx, entry, E_DB);
        db_version = idb_database_version(ctx, db);
        JS_FreeValue(ctx, db);
        r = idb_fire_version_change_event_run(ctx, &s->phase, STEP_CB(s->cb), req, "blocked", db_version, 0,
                                              /*has_new*/ false, &s->ev, cb_result, NULL, out_cb, out_argc);
        JS_FreeValue(ctx, req);
        if (r > 0) return r;
        if (r < 0) return JS_STEP_ABRUPT;
        JS_FreeValue(ctx, s->ev);
        s->ev = JS_UNDEFINED;
        STEP_GOTO(s->hdr.stage, DEL_WAIT, &s->phase, NULL);
        return JS_STEP_YIELD;

    STEP_ARM(DEL_WAIT);
        JS_FreeValue(ctx, cb_result);
        /* Step 9: "Wait until all connections in openConnections are closed." The SAME wait §5.1 step 10.5 is,
           discharged by the SAME park — a `versionchange` handler that ignores the event leaves its connection
           open until the page decides otherwise, and no queue can order that. The entry parks on the blocked
           list at ~zero CPU and idb_open_connection_closed resumes it into the continuation its kind names. */
        if (open_still_blocked(ctx, entry)) {
            JS_DefinePropertyValueUint32(ctx, g_blocked, open_list_len(ctx, g_blocked),
                                         JS_DupValue(ctx, entry), JS_PROP_C_W_E);
            return JS_STEP_DONE;
        }
        entry_enqueue(ctx, g_delete_finish_stepid, entry);
        return JS_STEP_DONE;
}

static const JSTrampStepDef js_idb_delete_def = {
    sizeof(JSIdbOpenState), js_idb_delete_step, NULL, 0, .visit = js_idb_open_visit,
    .algorithm = "Indexed Database §5.3's delete a database, steps 4 through 9", .steps = DEL_STEPS
};

/* ---- §5.3's DELETE A DATABASE, steps 10 through 12 --------------------------------------------------------- */

#define DELFIN_STAGES(X) \
    X(DELFIN_DELETE, "Indexed Database §5.3 steps 10-12 — ONE O(1) engine action: version is db's version, db " \
                     "is deleted from §2.1's set, and that version is what the algorithm returns")
enum { DELFIN_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const DELFIN_STEPS[] = { DELFIN_STAGES(JS_STEP_STAGE_LABEL) NULL };

static int js_idb_delete_finish_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb,
                                     int *out_argc)
{
    JSIdbOpenState *s = st;
    JSValueConst entry = task_entry(&s->hdr);
    JSValue db;

    (void)out_cb; (void)out_argc;
    open_state_start(s);
    STEP_DISPATCH(DELFIN_STAGES, s->hdr.stage, s->hdr.def->algorithm, JS_STEP_ABRUPT);

    STEP_ARM(DELFIN_DELETE);
        JS_FreeValue(ctx, cb_result);
        entry_assert_head(ctx, entry);
        DCHECK(entry_kind(ctx, entry) == IDB_REQ_DELETE,
               "§5.3's second machine ran for an entry that is not a delete request");
        DCHECK(!open_still_blocked(ctx, entry),
               "§5.3 step 10 was reached with a connection in openConnections still not closed — step 9 is the "
               "wait, and the only two ways here are that wait finding nothing left and the rendezvous that "
               "asks the same question");
        db = entry_get(ctx, entry, E_DB);
        DCHECK(JS_IsObject(db), "§5.3 step 10 was reached with no database — step 4's \"otherwise, return 0\" "
                                "ends the algorithm at DEL_FIND and never reaches this machine");
        /* Steps 10-12: "Let version be db's version. Delete db. Return version." The version is recorded
           BEFORE the deletion because it is what §4.3's completion task fires as the success event's
           `oldVersion`, and after the deletion there is no record to read it from. */
        entry_set(ctx, entry, E_OLD_VERSION, JS_NewFloat64(ctx, idb_database_version(ctx, db)));
        idb_database_destroy(ctx, db);
        JS_FreeValue(ctx, db);
        entry_set(ctx, entry, E_DB, JS_NULL);
        entry_enqueue(ctx, g_result_stepid, entry);
        return JS_STEP_DONE;
}

static const JSTrampStepDef js_idb_delete_finish_def = {
    sizeof(JSIdbOpenState), js_idb_delete_finish_step, NULL, 0, .visit = js_idb_open_visit,
    .algorithm = "Indexed Database §5.3's delete a database, steps 10 through 12", .steps = DELFIN_STEPS
};

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
           inactive." Step 6: "Start transaction" — DIRECTLY, which is why the creation does not queue
           §2.7.1's start task for an upgrade transaction: step 9's `upgradeneeded` handler places requests
           against this transaction inside the same task, and a start one queue hop away would hold every one
           of them. §2.7.2's constraints are asserted inside the start, where §5.1 step 10's "wait until all
           connections are closed" is what makes them hold.
           §5.7 gives the transaction NO cleanup event loop, and that omission is load-bearing: §2.7.1's
           cleanup would deactivate it at the end of this task, and its lifetime is the `upgradeneeded`
           dispatch (idb_transaction.h states the same thing from the other side). */
        idb_transaction_set_request(ctx, tx, req);
        idb_database_set_upgrade_transaction(ctx, db, JS_DupValue(ctx, tx));
        idb_transaction_set_state(ctx, tx, IDB_TX_INACTIVE);
        idb_transaction_start(ctx, tx);
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
                   transaction the handler DID place requests against commits from §5.9 step 8.3 instead. */
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
   database's connection queue, which §2.8.2 guarantees is this request's. `aborted` is WHICH of §2.7.1's two
   endings it reached, which is what §5.1 step 10.8 is about and what nothing else here can re-derive. */
static void idb_open_upgrade_finished(JSContext *ctx, JSValueConst tx, bool aborted)
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
    /* §5.1 STEP 10.8's CONDITION, RECORDED WHERE THE ANSWER IS KNOWN. That step reads "if request's error is
       set", and NOTHING in §5.5 or §5.7 writes one on an open request: §5.5 step 6 sets an AbortError on each
       request of the transaction's REQUEST LIST, and an open request is never placed against a transaction by
       §5.6, so it is in no list; §5.5 step 7.3 then walks the open request's other four fields back and
       pointedly leaves `error` alone. What the observable is is not in doubt — an upgrade that aborted fires
       ONE `error` at the open request carrying step 10.8's own newly created "AbortError", and `success` with
       the connection would be the WRONG answer rather than a missing one — so what the condition is really
       asking is whether this upgrade transaction ABORTED, which is what the rendezvous now says. It cannot be
       re-derived from the transaction's error either: §2.7's own note is that "the value null is considered an
       error, as it is set from abort()", so a null error means committed OR explicitly aborted. §4.10's
       `error` getter goes on reporting the transaction's — the ConstraintError of §4.5's worked example. */
    entry_set(ctx, entry, E_ABORTED, JS_NewBool(ctx, aborted));
    /* §5.1 steps 10.7-10.8 and §4.3's completion task, in the one machine that performs them. */
    entry_enqueue(ctx, g_result_stepid, entry);
    JS_FreeValue(ctx, entry);
    JS_FreeValue(ctx, q);
    JS_FreeCString(ctx, cname);
    JS_FreeValue(ctx, name);
    JS_FreeValue(ctx, req);
    JS_FreeValue(ctx, db);
    JS_FreeValue(ctx, conn);
}

/* ---- §5.1 steps 10.7-10.8 and §4.3's TWO COMPLETION TASKS --------------------------------------------------
 *
 * §4.3's own question-box is why these are not §5.9's and §5.10's steps: "there is no transaction associated
 * with the request (at this point), so those steps — which activate an associated transaction before dispatch
 * and deactivate the transaction after dispatch — do not apply." So the event is a plain §2.9 dispatch and
 * nothing follows it but the queue moving on.
 *
 * ONE MACHINE FOR BOTH BECAUSE THE TWO TASKS DIFFER IN ONE SENTENCE. `open`'s and `deleteDatabase`'s completion
 * tasks are written out separately in §4.3 and are the same three steps with the same error arm — result
 * undefined, error set, done set, an `error` event with bubbles and cancelable both true. Their SUCCESS arms
 * are what differ: `open` reports the connection and fires a plain `success`, while `deleteDatabase` reports
 * undefined and fires "a version change event named success at request with result and null" — an
 * IDBVersionChangeEvent, because §4.3's second question-box says the page is owed the `oldVersion` the deleted
 * database had. E_KIND is the sentence; everything around it is shared, including the dequeue that lets the
 * next request in the queue be processed, which is the one thing neither task may have a second copy of. */

#define RES_STAGES(X) \
    X(RES_CHECK,   "Indexed Database §5.1 steps 10.7-10.8 (a connection that was closed, or a request whose " \
                   "error is set, is an AbortError — and the second closes the connection first). §5.3 has no " \
                   "connection and no such steps, so a delete passes through") \
    X(RES_SETTLE,  "Indexed Database §4.3's completion task, steps 1-3 of either arm — ONE O(1) engine " \
                   "action: the request's result (open's connection, delete's undefined) or its error is " \
                   "written and its done flag is set") \
    X(RES_FIRE,    "Indexed Database §4.3's completion task, the fire (an event named success at request, a " \
                   "version change event named success with the deleted database's version and null, or one " \
                   "named error with its bubbles and cancelable attributes initialized to true)") \
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
        /* §5.1's steps 10.7-10.8 are about a CONNECTION, and §5.3 Deleting a database's twelve steps make
           none — so the delete arm falls THROUGH both tests below rather than being branched around them, and
           this is what says so. Which also makes the converse assertable: an entry that is a delete and holds
           a connection is not a delete that acquired one, it is an entry driven through §5.1's step 8, and its
           KIND is not what is wrong (E_KIND is written once, by the entry's own constructor, from which of
           §4.3's two members the page called). entry_start_machine and entry_enqueue's pairing make that
           unreachable at the MINT; this is the consumer that would otherwise hand a `deleteDatabase` a
           connection as its result. */
        DCHECK(entry_kind(ctx, entry) == IDB_REQ_OPEN || JS_IsNull(conn),
               "a §5.3 delete request reached §4.3's completion task holding a connection — §5.3 has no step "
               "that makes one, so this entry ran §5.1's steps instead of its own");
        err = idb_request_error(ctx, req);
        if (JS_IsNull(err) && !entry_flag(ctx, entry, E_ABORTED) && idb_connection_is(conn)) {
            /* Step 10.7: "If connection was closed, return a newly created AbortError DOMException." A
               `versionchange` or `upgradeneeded` handler that closed the connection it was handed gets this. */
            if (idb_connection_is_closed(ctx, conn))
                idb_request_set_error(ctx, req, open_dom_exception(ctx, "AbortError",
                                                                   "the connection was closed during the "
                                                                   "upgrade"));
        } else if (idb_connection_is(conn)) {
            /* Step 10.8: "If request's error is set, run the steps to close a database connection with
               connection, return a newly created AbortError DOMException." The connection is closed FIRST —
               an aborted upgrade must not leave one open, or the next open of that name is blocked forever.
               THE UPGRADE-ABORTED FLAG IS THAT CONDITION and the request's own error is not, because nothing
               in §5.5 or §5.7 sets one on an OPEN request — idb_open_upgrade_finished states the whole of
               that argument. The `err` arm stays beside it for the one thing that DOES set a request error
               here: §5.1's own earlier steps, whose VersionError §4.3 delivers through this same task. */
            idb_connection_close(ctx, conn);
            idb_request_set_error(ctx, req, open_dom_exception(ctx, "AbortError",
                                                               "the upgrade transaction was aborted"));
        }
        /* §4.3 step 5.2, for BOTH members: "set request's processed flag to true", between the in-parallel
           algorithm answering and the completion task running. §5.7 step 9 states it again for the one open
           that runs an upgrade, which is why an open that did NOT — the second `open(name)` of a session, and
           every delete — reached its completion task without it. */
        idb_request_set_processed(ctx, req, true);
        JS_FreeValue(ctx, err);
        JS_FreeValue(ctx, conn);
        JS_FreeValue(ctx, req);
        STEP_GOTO(s->hdr.stage, RES_SETTLE, &s->phase, NULL);
        return JS_STEP_YIELD;

    STEP_ARM(RES_SETTLE);
        JS_FreeValue(ctx, cb_result);
        req = entry_get(ctx, entry, E_REQUEST);
        err = idb_request_error(ctx, req);
        if (JS_IsNull(err) && entry_kind(ctx, entry) == IDB_REQ_OPEN) {
            conn = entry_get(ctx, entry, E_CONNECTION);
            DCHECK(idb_connection_is(conn), "§4.3's completion task found neither a connection nor an error — "
                                            "§5.1 answers with one or the other on every path");
            idb_request_set_result(ctx, req, conn);
            idb_request_set_error(ctx, req, JS_UNDEFINED);
        } else if (JS_IsNull(err)) {
            /* `deleteDatabase`'s success arm: "set request's result to UNDEFINED". §5.3's answer — the version
               the database had, or 0 — is not the result; it is the success event's `oldVersion`, which
               RES_FIRE reads off the entry. */
            idb_request_set_result(ctx, req, JS_UNDEFINED);
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
        if (!s->did_throw && entry_kind(ctx, entry) == IDB_REQ_DELETE) {
            /* `deleteDatabase`'s success: "fire a version change event named success at request with result
               and null." The result is §5.3's return — the version the database HELD — and §4.3's second
               question-box is why it is an IDBVersionChangeEvent rather than a plain one: the page is owed
               that `oldVersion`, and a `newVersion` of null is what says the database is gone rather than
               upgraded. This event does NOT bubble and is NOT cancelable; only the error arm below is. */
            r = idb_fire_version_change_event_run(ctx, &s->phase, STEP_CB(s->cb), req, "success",
                                                  entry_num(ctx, entry, E_OLD_VERSION), 0, /*has_new*/ false,
                                                  &s->ev, cb_result, NULL, out_cb, out_argc);
            JS_FreeValue(ctx, req);
            if (r > 0) return r;
            if (r < 0) return JS_STEP_ABRUPT;
            STEP_GOTO(s->hdr.stage, RES_DEQUEUE, &s->phase, NULL);
            return JS_STEP_YIELD;
        }
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

                /* THE NEXT ENTRY'S OWN ALGORITHM, not this one's. §2.8.1 Open requests is one request type
                   "used when opening a connection or deleting a database", so the queue interleaves both and
                   what a dequeue starts is whatever the next entry IS — asked of entry_start_machine, which
                   is the only thing that answers it. */
                entry_enqueue(ctx, entry_start_machine(ctx, next), next);
                JS_FreeValue(ctx, next);
            }
            JS_FreeValue(ctx, q);
        }
        return JS_STEP_DONE;
}

static const JSTrampStepDef js_idb_result_def = {
    sizeof(JSIdbOpenState), js_idb_result_step, NULL, 0, .visit = js_idb_open_visit,
    .algorithm = "Indexed Database §5.1 steps 10.7-10.8 and §4.3's open() completion task",
    .steps = RES_STEPS
};

/* ---- §4.3's open() and deleteDatabase(), everything after their synchronous steps -------------------------- */

/* THE ONE PLACE AN OPEN REQUEST IS FILED IN §2.8.2's QUEUE. §5.1 steps 1-3 and §5.3 steps 1-3 are the same
   three sentences — "let queue be the connection queue for storageKey and name; add request to queue; wait
   until all previous requests in queue have been processed" — and a second copy of them would be a second
   answer to "who is the head", which is the invariant every machine in this file asserts on entry. Which
   algorithm the entry starts is NOT a parameter: E_KIND is placed below, before the enqueue, so by the time
   step 3 has anything to start there is an entry to ask — and a `stepid` argument here would be a SECOND
   answer to a question entry_start_machine already answers, which is exactly how the dequeue came to answer
   it wrong. Returns the REQUEST, owned; the member hands it straight back to the page. */
static JSValue open_request_enqueue(JSContext *ctx, const char *name, int kind, double version,
                                    bool has_version)
{
    JSValue req, entry, q;
    uint32_t n;

    req = idb_request_new_open(ctx);   /* §2.8.1: one type of request, "used when opening a connection OR
                                          deleting a database" */
    entry = idl_slots_new(ctx);
    CHECK(!JS_IsException(entry), "IndexedDB: §5.1's entry could not be allocated");
    entry_set(ctx, entry, E_KIND, JS_NewInt32(ctx, kind));
    entry_set(ctx, entry, E_REQUEST, JS_DupValue(ctx, req));
    entry_set(ctx, entry, E_NAME, JS_NewString(ctx, name));
    entry_set(ctx, entry, E_VERSION, JS_NewFloat64(ctx, has_version ? version : 0));
    entry_set(ctx, entry, E_HAS_VERSION, JS_NewBool(ctx, has_version));
    entry_set(ctx, entry, E_DB, JS_NULL);
    entry_set(ctx, entry, E_CONNECTION, JS_NULL);
    entry_set(ctx, entry, E_OPEN_CONNS, JS_NewArray(ctx));
    entry_set(ctx, entry, E_CURSOR, JS_NewInt32(ctx, 0));
    entry_set(ctx, entry, E_TARGET, JS_NULL);
    entry_set(ctx, entry, E_OLD_VERSION, JS_NewFloat64(ctx, 0));
    entry_set(ctx, entry, E_BLOCKED, JS_FALSE);
    entry_set(ctx, entry, E_ABORTED, JS_FALSE);

    /* §5.1 steps 1-2 and §5.3 steps 1-2: "let queue be the connection queue for storageKey and name. Add
       request to queue." */
    q = open_queue(ctx, name, /*create*/ true);
    n = open_list_len(ctx, q);
    JS_DefinePropertyValueUint32(ctx, q, n, JS_DupValue(ctx, entry), JS_PROP_C_W_E);
    JS_FreeValue(ctx, q);
    /* Step 3: "Wait until all previous requests in queue have been processed." An entry that arrives at the
       HEAD starts now; one behind another starts when that one's §4.3 completion task dequeues it. Which is
       what makes the wait an ORDERING rather than a loop — and what makes `deleteDatabase(n)` immediately
       followed by `open(n, 1)`, which is how half this standard's fixtures begin, run in that order without
       either of them knowing the other exists. */
    if (n == 0)
        entry_enqueue(ctx, entry_start_machine(ctx, entry), entry);
    JS_FreeValue(ctx, entry);
    return req;   /* "Return a new IDBOpenDBRequest object for request." */
}

JSValue idb_open_request(JSContext *ctx, const char *name, double version, bool has_version)
{
    DCHECK(name != NULL, "§4.3's open was given no database name");
    DCHECK(!has_version || version >= 1, "§4.3's open reached §5.1 with a zero version — its first step is "
                                         "\"if version is 0 (zero), throw a TypeError\", reported by the "
                                         "member before this");
    return open_request_enqueue(ctx, name, IDB_REQ_OPEN, version, has_version);
}

JSValue idb_delete_request(JSContext *ctx, const char *name)
{
    DCHECK(name != NULL, "§4.3's deleteDatabase was given no database name");
    /* §5.3 takes NO version — the algorithm's `version` is its own step 10, read off the database it is about
       to delete — so the entry's requested version is absent and E_HAS_VERSION is false. §5.1's step 5 is the
       only reader of that pair and it is not on any path a delete takes. */
    return open_request_enqueue(ctx, name, IDB_REQ_DELETE, 0, /*has_version*/ false);
}

void idb_open_init(JSContext *ctx)
{
    JSRuntime *rt = JS_GetRuntime(ctx);

    DCHECK(JS_IsUndefined(g_queues), "idb_open_init ran twice — §2.8.2's queues belong to the AGENT, and a "
                                     "second set would let two opens of one name run at once");
    g_queues = idl_slots_new(ctx);
    CHECK(!JS_IsException(g_queues), "IndexedDB: §2.8.2's connection queues could not be allocated");
    g_blocked = JS_NewArray(ctx);
    CHECK(!JS_IsException(g_blocked), "IndexedDB: §5.1 step 10.5's blocked list could not be allocated");
    g_open_stepid = JS_RegisterStepDef(rt, &js_idb_open_def);
    g_upgrade_stepid = JS_RegisterStepDef(rt, &js_idb_upgrade_def);
    g_result_stepid = JS_RegisterStepDef(rt, &js_idb_result_def);
    g_delete_stepid = JS_RegisterStepDef(rt, &js_idb_delete_def);
    g_delete_finish_stepid = JS_RegisterStepDef(rt, &js_idb_delete_finish_def);
    /* The two rendezvous this algorithm's un-dischargeable waits need. Registered HERE, in the declaration of
       the component that performs §5.1, which is what makes each of them one question with one asker. */
    idb_connection_set_closed_hook(idb_open_connection_closed);
    idb_transaction_set_upgrade_finished_hook(idb_open_upgrade_finished);
    agent_state_value("idb_open", &g_queues, "§2.8.2's connection queues, and the declaration latch");
    agent_state_value("idb_open", &g_blocked, "§5.1 step 10.5's blocked list");
    agent_state_id("idb_open", &g_open_stepid, "§5.1's open machine");
    agent_state_id("idb_open", &g_upgrade_stepid, "§5.1's run-an-upgrade-transaction machine");
    agent_state_id("idb_open", &g_result_stepid, "§5.1's deliver-the-result machine");
    agent_state_id("idb_open", &g_delete_stepid, "§5.3's delete-a-database machine");
    agent_state_id("idb_open", &g_delete_finish_stepid, "§5.3's remove-the-database machine");
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
    g_delete_stepid = g_delete_finish_stepid = -1;
}
