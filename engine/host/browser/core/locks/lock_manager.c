/* WEB LOCKS API §2 "Concepts", §3 "API" and §4 "Algorithms". See lock_manager.h for why this member and why
 * all three sections are one landing.
 *
 * THE FIVE ALGORITHMS OF §4 ARE WRITTEN ONCE EACH AND CALL EACH OTHER exactly as the standard has them:
 * §4.1 "Request a lock" enqueues, §4.4 "Process a lock request queue for a given resource name" grants,
 * Web Locks API §4.2 "Release a lock" and §4.3 "Abort a request" both END in §4.4, and §4.5 "Snapshot the
 * lock state" reads.
 * That shape is why the queue machinery is real rather than a fast path for the uncontended case: a page that
 * calls request() twice for one name from two handlers — which the programs this was built against do — has the
 * second request WAIT, and §4.4 is what wakes it when the first callback's promise settles.
 *
 * WHERE THE LOCK TASK QUEUE'S STEPS RUN. §2 gives the user agent a "lock task queue" which is "the result of
 * starting a new parallel queue", and §4.1, Web Locks API §4.2, §4.3 and §4.5 all assert they are running on it. A parallel
 * queue is not a task source and nothing in this engine can be on this one but this component: every step it
 * carries mutates engine state and runs NONE of the page's code, and everything that DOES touch the page is
 * enqueued from there as a TASK (§4.1's ifAvailable arm and §4.4's grant both say "enqueue the following steps
 * on callback's relevant settings object's responsible event loop"). So they run inline, at the point the
 * standard enqueues them, and the ordering a real parallel queue would impose is unobservable because there is
 * no second producer to order against. The ASSERTS are what keeps that honest: each of the four says in its own
 * DCHECK that it may not run the page's code, so the day one of them gains a read that could, it fires.
 *
 * THE TASK SOURCE IS THE STANDARD'S OWN AND NOT A GENERIC ONE — §2 names it in the same breath as the queue
 * ("The task source for steps enqueued below is the web-locks tasks source"), so it is a row of
 * core/timing/task_source.h rather than one of HTML §8.1.7.4's five. §8.1.7.1's ordering guarantee is about a
 * source, and putting these on a generic one would order them against unrelated features by this file's
 * decision rather than by anybody's standard.
 *
 * WHY THE CALLBACK IS INVOKED BY A STEP MACHINE ENQUEUED AS A TASK, and not by a JS_Call from the grant. §4.4's
 * enqueued steps end "Let r be the result of invoking callback with a new Lock object … Resolve waiting with
 * r", so the algorithm needs the callback's RESULT — and the callback is the page's code with loops, awaits and
 * forks in it, which §C-stack forbids a C activation from hosting. A step closure over JS_EnqueueCallTask is
 * both halves at once: the task runs as a call-root flow on the one frontier, preemptible per opcode and
 * parkable at any depth, and `step_call_run` hands the machine the result when the flow comes back.
 * ONE MACHINE SERVES BOTH ENQUEUES, because they are the same operation. §4.1's ifAvailable arm invokes the
 * callback with NULL and resolves the REQUEST's promise with r; §4.4's grant invokes it with a Lock and resolves
 * the WAITING promise with r. Written twice they would drift on the one thing they share, which is what Web IDL
 * §3.12's "Invoking callback functions" does with a throw.
 *
 * WHAT `Resolve waiting with r` IS, AND WHY §2.4's SETTLE READS NO `then`. §2.4 "Locks" says "When lock lock's
 * waiting promise settles (fulfills or rejects), enqueue the following steps on the lock task queue: Release the
 * lock lock. Resolve lock's released promise with lock's waiting promise." Resolving a capability WITH a promise
 * is 27.5.1.3's thenable path, which reads `then` off it — the page's code if Promise.prototype.then is
 * patched. It is not needed: this reacts to `waiting` through Web IDL's own PerformPromiseThen (never a
 * page-visible `.then`), so by the time the settle runs the state and the value are IN HAND, and a fulfilment
 * value that has been through resolution is never a thenable. Calling the released capability's resolve with
 * the value, or its reject with the reason, is therefore the same observable outcome with nothing of the page's
 * read. The one place a thenable IS resolved is `Resolve waiting with r`, which is the page's own return value
 * and where the standard means that read to happen.
 *
 * THE CLIENT ID IS MINTED PER REALM AND IS NOT A Client's `id`. §3.2.2 says the field "corresponds to a unique
 * context (frame or worker), and is the same value returned by Client's id attribute" — this engine has no
 * Service Worker Client component, so there is no id to be the same as. A UUID per environment is a unique
 * context, which is the property §4.5's snapshot and §3.2.2's example both rest on, and the residual at the
 * bottom of this file says what it is not. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "quickjs-step.h"
#include "core/agent_state.h"
#include "core/idl_args.h"
#include "core/idl_slots.h"
#include "core/realm.h"
#include "core/crypto/crypto.h"
#include "core/dom/abort.h"
#include "core/dom/document.h"
#include "core/locks/lock_manager.h"
#include "core/storage/storage_shed.h"
#include "core/timing/task_source.h"

/* §2.2's BOTTLE IDENTIFIER — the string this component passes to "obtaining a local storage bottle map given
   environment and" it. It is the Web Locks bottle and nothing else reads it, which is what makes the manager
   record below this component's alone to put there. */
#define LK_BOTTLE "web-locks"

/* THE FIELDS OF §2.4's LOCK, §2.5's LOCK REQUEST AND §2.2's MANAGER, as the property names of internal-slot
   records. They are spelled once here because the same five names are written by the grant and read by the
   snapshot, and a name written in one place and read in another is the defect core/idl_slots.h is about. */
#define LKF_HELD     "held"        /* §2.4: the manager's held lock set — an Array of lock records */
#define LKF_QUEUES   "queues"      /* §2.5: its lock request queue map — a record of name -> Array of requests */
#define LKF_NAME     "name"        /* §2.4/§2.5: the resource name */
#define LKF_MODE     "mode"        /* §2.3's mode, as this file's LkMode */
#define LKF_CLIENT   "clientId"    /* §2.4/§2.5: the opaque string §3.2.2's snapshot reports */
#define LKF_WAITING  "waiting"     /* §2.4's waiting promise */
#define LKF_RES      "res"         /* the RELEASED promise's capability — §2.4's "released promise" */
#define LKF_REJ      "rej"
#define LKF_CALLBACK "callback"    /* §2.5's callback */
#define LKF_SIGNAL   "signal"      /* §2.5's signal */
#define LKF_ALGO     "algo"        /* §4.1's "signal to abort the request" algorithm, so §4.4 can remove it */
#define LKF_MANAGER  "manager"     /* the realm's LockManager object -> §2.2's manager, or null for failure */
#define LKF_WREJ     "wrej"        /* §4.4's WAITING promise's reject, for Web IDL §3.12's rejected arm */

/* §2.3 "Modes and Scheduling": "A mode is either 'exclusive' or 'shared'." The IDL's `enum LockMode` lists
   them in the other order, and THAT order is the one §3.2.18's conversion checks against — so the array is the
   IDL's and this enum indexes into it. */
IDL_ENUM_VALUES(LK_MODE_VALUES, "shared", "exclusive");
typedef enum { LK_SHARED = 0, LK_EXCLUSIVE = 1 } LkMode;

static JSClassID g_manager_class;
static JSClassID g_lock_class;
static JSClassID g_obj_slot = JS_INVALID_CLASS_ID;
static JSValue   g_lock_key;        /* §3.3: "A Lock object has an associated lock" — its internal-slot key */
static int g_id_request = -1;
static int g_id_query = -1;
static int g_grant_stepid = -1;
static int g_settled_stepid = -1;
static int g_abort_stepid = -1;
static JSRuntime *g_rt;
static int g_ready;

/* ---- the records ------------------------------------------------------------------------------------------ */

static JSValue lk_get(JSContext *ctx, JSValueConst rec, const char *field)
{
    DCHECK(JS_IsObject(rec), "a Web Locks record field was read off something that is not a record — every one "
                             "of them is an internal-slot record this file minted, so a non-object here is a "
                             "field that was never written rather than a page value");
    return JS_GetPropertyStr(ctx, rec, field);   /* OWNED; the record has a null prototype, so no page code */
}

static int32_t lk_get_int(JSContext *ctx, JSValueConst rec, const char *field)
{
    JSValue v = lk_get(ctx, rec, field);
    int32_t n = -1;

    JS_ToInt32(ctx, &n, v);
    JS_FreeValue(ctx, v);
    return n;
}

static uint32_t lk_len(JSContext *ctx, JSValueConst arr)
{
    JSValue v = JS_GetPropertyStr(ctx, arr, "length");
    uint32_t n = 0;

    JS_ToUint32(ctx, &n, v);
    JS_FreeValue(ctx, v);
    return n;
}

/* §2.5: "To get the lock request queue from lock request queue map queueMap from resource name name: If
   queueMap[name] does not exist, set queueMap[name] to a new empty lock request queue. Return queueMap[name]."
   The map is an internal-slot record, so `name` lands as an ordinary own property whatever the page called the
   resource — a null prototype is what makes `__proto__` and `constructor` ordinary names here. OWNED. */
static JSValue lk_queue_of(JSContext *ctx, JSValueConst manager, JSValueConst name)
{
    JSValue map = lk_get(ctx, manager, LKF_QUEUES);
    JSAtom a = JS_ValueToAtom(ctx, name);
    JSValue q;

    CHECK(a != JS_ATOM_NULL, "a Web Locks resource name could not be interned");
    q = JS_GetProperty(ctx, map, a);
    if (!JS_IsArray(q)) {
        JS_FreeValue(ctx, q);
        q = JS_NewArray(ctx);
        CHECK(!JS_IsException(q), "§2.5's new empty lock request queue could not be allocated");
        JS_SetProperty(ctx, map, a, JS_DupValue(ctx, q));
    }
    JS_FreeAtom(ctx, a);
    JS_FreeValue(ctx, map);
    return q;
}

/* Remove one element of a JS array by IDENTITY, which is what every "Remove X from Y" step over these lists
   means. There is no Infra list primitive here, so it is `splice` performed in C — and it is asked of SameValue
   rather than of a field, because two requests for one name with one mode are distinct structs. */
static bool lk_remove(JSContext *ctx, JSValueConst arr, JSValueConst item)
{
    uint32_t n = lk_len(ctx, arr), i, j;
    bool found = false;

    for (i = 0; i < n; i++) {
        JSValue e = JS_GetPropertyUint32(ctx, arr, i);
        bool same = JS_IsSameValue(ctx, e, item);

        JS_FreeValue(ctx, e);
        if (!same)
            continue;
        for (j = i + 1; j < n; j++)
            JS_SetPropertyUint32(ctx, arr, j - 1, JS_GetPropertyUint32(ctx, arr, j));
        JS_SetPropertyUint32(ctx, arr, n - 1, JS_UNDEFINED);
        JS_SetPropertyStr(ctx, arr, "length", JS_NewUint32(ctx, n - 1));
        found = true;
        break;
    }
    return found;
}

/* §2.5's GRANTABLE, verbatim: "If queue is not empty and request is not the first item in queue, then return
   false. If mode is 'exclusive', then return true if no lock in held has name equal to name, and false
   otherwise. Otherwise, mode is 'shared'; return true if no lock in held has mode 'exclusive' and has name
   equal to name, and false otherwise."
   `index` IS THE REQUEST'S POSITION IN `queue`, or -1 FOR A REQUEST THAT IS NOT IN ONE, and the two callers are
   why it is a parameter rather than an identity search. §4.4 asks it of the queue's own members, where "the
   first item" is index 0; §4.1's ifAvailable arm asks it of a request it has NOT YET enqueued, where the
   standard's own test makes a non-empty queue answer false because a request in no queue is not its first
   item. Searching for the request would answer the second case wrongly by not finding it. */
static bool lk_grantable(JSContext *ctx, JSValueConst manager, JSValueConst queue, JSValueConst name,
                         LkMode mode, int index)
{
    JSValue held;
    uint32_t n, i;
    bool ok = true;

    if (lk_len(ctx, queue) != 0 && index != 0)
        return false;
    held = lk_get(ctx, manager, LKF_HELD);
    n = lk_len(ctx, held);
    for (i = 0; i < n && ok; i++) {
        JSValue lock = JS_GetPropertyUint32(ctx, held, i);
        JSValue ln = lk_get(ctx, lock, LKF_NAME);

        if (JS_IsSameValue(ctx, ln, name) &&
            (mode == LK_EXCLUSIVE || (LkMode)lk_get_int(ctx, lock, LKF_MODE) == LK_EXCLUSIVE))
            ok = false;
        JS_FreeValue(ctx, ln);
        JS_FreeValue(ctx, lock);
    }
    JS_FreeValue(ctx, held);
    return ok;
}

/* ---- §4.4's grant, and §4.1's ifAvailable arm, as one task machine ---------------------------------------- */

enum { LK_CD_MANAGER = 0, LK_CD_LOCK, LK_CD_REQUEST, LK_CD_RESOLVE, LK_CD_N };

#define LK_GRANT_STAGES(X)                                                                                     \
    X(LKG_CALL, "Web Locks API §4.4 Process a lock request queue for a given resource name's enqueued steps "   \
                "(the signal test, removing the abort algorithm, and invoking callback with a new Lock "        \
                "object), or §4.1 Request a lock's ifAvailable arm (invoking callback with null)")
enum { LK_GRANT_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const LK_GRANT_STEPS[] = { LK_GRANT_STAGES(JS_STEP_STAGE_LABEL) NULL };

typedef struct {
    JSStepHdr hdr;        /* FIRST — the driver writes the def and the operand bounds through it */
    uint8_t   phase;      /* step_call_run's own, held across the callback's suspension */
    JSValue   cb[3];      /* [this, func, arg] — §4.4 invokes the callback with exactly one argument */
    /* §4.4's SIGNAL TEST HELD WHERE THE SIBLING'S SNAPSHOT CARRIES IT — abort_signal_aborted_step's operand.
       An `AbortSignal.timeout()` the page passed as §3.2.1's `signal` has an UNKNOWN `aborted`, so this test
       FORKS: one world where the grant is released and one where the callback is invoked, and both lead to code
       worth reaching. The seam borrows the flag for the length of the request and the state is BYTE-COPIED at a
       deep fork, so a flag in a C local would be gone in the arm that resumes — which is why it is a field and
       why `lk_grant_visit` names it. JS_UNINITIALIZED is the EMPTY value and is written explicitly, because a
       zeroed step state's JSValue is the INTEGER 0 rather than JS_UNDEFINED. */
    JSValue   sig_flag;
} LkGrant;

static void lk_grant_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    LkGrant *s = st;
    int i;

    STEP_CB_FOREACH(s->cb, i) v->val(ctx, &s->cb[i]);
    v->val(ctx, &s->sig_flag);
}

static void lk_release(JSContext *ctx, JSValueConst manager, JSValueConst lock);

/* §3.3: a Lock object over a lock record. §4.4 mints a NEW one per grant ("a new Lock object associated with
   lock"), so two grants of one name never hand the page one object. */
static JSValue lk_new_lock_object(JSContext *ctx, JSValueConst lock)
{
    JSValue proto = JS_GetClassProto(ctx, g_lock_class);
    JSValue obj;
    JSAtom k;

    DCHECK(!JS_IsNull(proto), "§3.3's Lock.prototype was asked for in a realm that never ran the Web Locks "
                              "install — a grant runs in the realm whose LockManager took the request, and "
                              "that realm built this prototype before the member existed to be called");
    obj = JS_NewObjectProtoClass(ctx, proto, g_lock_class);
    JS_FreeValue(ctx, proto);
    CHECK(!JS_IsException(obj), "§3.3's Lock object could not be allocated — a grant that cannot hand the page "
                                "its lock is a callback that never runs and a promise nothing settles");
    k = JS_ValueToAtom(ctx, g_lock_key);
    CHECK(k != JS_ATOM_NULL, "the Lock internal-slot key could not be interned");
    JS_SetProperty(ctx, obj, k, JS_DupValue(ctx, lock));
    JS_FreeAtom(ctx, k);
    return obj;
}

static int lk_grant_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    LkGrant *s = st;
    JSValueConst manager = JS_StepClosureData(&s->hdr, LK_CD_MANAGER);
    JSValueConst lock = JS_StepClosureData(&s->hdr, LK_CD_LOCK);
    JSValueConst request = JS_StepClosureData(&s->hdr, LK_CD_REQUEST);
    JSValueConst resolve = JS_StepClosureData(&s->hdr, LK_CD_RESOLVE);
    JSValue arg, r;
    int rc;

    STEP_DISPATCH(LK_GRANT_STAGES, s->hdr.stage, s->hdr.def->algorithm, JS_STEP_ABRUPT);

    STEP_ARM(LKG_CALL);
    if (s->phase == 0) {
        /* ONCE PER INVOCATION, NOT ONCE PER ENTRY — and `phase == 0` is not that test. A fork re-enters this
           arm AT ITS TOP TWICE, once in the parent carrying the answer and once in the sibling re-asking, and
           `phase` is written only by `step_call_run` BELOW the ask, so it still reads 0 on both. An unguarded
           init here therefore runs again on each of them and OVERWRITES the owned reference that
           `abort_signal_aborted_step` is holding in `sig_flag` across the park, with nothing left to free it:
           about two leaked references to the concolic flag per fork, which §Testing counts as a failure.
           `step_fork_pending` is the fact that separates a fork entry from a first entry, and it is declared in
           `quickjs-step.h` precisely because every machine that reached for `fork_phase` instead got it wrong.
           THE GUARD IS ON THE INIT AND NEVER ON THE BLOCK: §4.4's signal test below must be RE-ASKED on the
           sibling's entry, because the arm it takes is replayed from the flow's own decision vector at the ask
           and a clone with the answer baked in would be a second, weaker answer to a settled question. */
        if (!step_fork_pending(&s->hdr)) {
            int i;

            STEP_CB_FOREACH(s->cb, i) s->cb[i] = JS_UNDEFINED;
            /* STATED, never read off the slot: a zeroed step state's JSValue is the INTEGER 0. */
            s->sig_flag = JS_UNINITIALIZED;
        }
        /* §4.4's TWO SIGNAL STEPS, which §4.1's ifAvailable arm does not have — and cannot, because §3.2.1
           rejects a call that passes both a signal and ifAvailable before any request exists. `lock` is null
           for that arm, which is what tells the two enqueues apart. */
        if (!JS_IsNull(lock)) {
            JSValue signal = lk_get(ctx, request, LKF_SIGNAL);

            if (!JS_IsUndefined(signal)) {
                int aborted = 0, r;

                /* THE PARKING FORM, because this ask CAN fork and this machine CAN carry the sibling. §3.2.1
                   lets the page pass any AbortSignal, so an `AbortSignal.timeout()` makes this test's operand
                   unknown input; the plain `abort_signal_aborted` returns a bool and therefore cannot say "I
                   forked", so it reached solver/engine.c's seam from inside a C activation with nowhere for the
                   sibling to resume and ABORTED. There is nothing to build here — this machine already holds
                   the resume point the driver clones at, so the whole repair is asking through the seam that
                   can return the fork code. */
                r = abort_signal_aborted_step(ctx, &s->hdr, signal, &s->sig_flag, &aborted);
                if (r) {
                    /* PARKED. `signal` is released and `cb_result` is NOT: the driver re-enters this body with
                       the same cb_result, and the stage is unchanged, so the re-read of §3.2.1's signal below
                       is the same read with the same answer. */
                    JS_FreeValue(ctx, signal);
                    return r;
                }
                if (aborted) {
                    /* "If signal is aborted, then run these steps: Enqueue the following step to the lock task
                       queue: Release the lock lock. Return." The release is the queue's step and runs where
                       every other one of that queue's steps runs. The REQUEST's promise is not settled here:
                       §4.3's "signal to abort the request" rejected it when the signal fired, which is the
                       algorithm that got this request out of the queue in the first place. */
                    JS_FreeValue(ctx, signal);
                    JS_FreeValue(ctx, cb_result);
                    lk_release(ctx, manager, lock);
                    return JS_STEP_DONE;
                }
                /* "Remove the algorithm signal to abort the request request from signal." The lock is granted,
                   so §3.2.1's "Once the lock has been granted, the signal is ignored" holds by there being no
                   algorithm left on it. */
                {
                    JSValue algo = lk_get(ctx, request, LKF_ALGO);

                    if (JS_IsFunction(ctx, algo))
                        abort_signal_remove_algorithm(ctx, signal, algo);
                    JS_FreeValue(ctx, algo);
                }
            }
            JS_FreeValue(ctx, signal);
        }
    }
    /* "Let r be the result of invoking callback with a new Lock object associated with lock as the only
       argument" — or, for §4.1's arm, "with null as the only argument". The callback is the page's, so it is a
       CALL REQUEST: the flow parks here and is re-entered with the result. */
    /* MINTED ONLY ON THE FIRST ENTRY. step_call_run holds the receiver, the function and the argument in the
       machine's own `cb` buffer from the moment it parks, so a re-entry after the callback suspended must not
       build a SECOND Lock — §4.4 says "a new Lock object" once per grant, and a fresh one per resume would
       read the realm's prototype and allocate for a value nothing can ever see. */
    arg = (s->phase != 0 || JS_IsNull(lock)) ? JS_NULL : lk_new_lock_object(ctx, lock);
    {
        JSValue callback = lk_get(ctx, request, LKF_CALLBACK);
        JSValueConst argv[1] = { arg };

        rc = step_call_run(ctx, &s->phase, s->cb, (int)(sizeof s->cb / sizeof s->cb[0]), callback,
                           JS_UNDEFINED, 1, argv, cb_result, &r, out_cb, out_argc);
        JS_FreeValue(ctx, callback);
        JS_FreeValue(ctx, arg);
        if (rc > 0)
            return rc;
    }
    /* WEB IDL §3.12 "Invoking callback functions" over a callback whose return type is `Promise<any>`: an abrupt
       completion becomes a REJECTED PROMISE rather than propagating, which is why §3.2.1's own prose can say
       "The lock will be released when the callback exits for any reason — either when the code returns, or if
       it throws". Rejecting the capability with the exception is that promise's only observable effect: nothing
       holds the intermediate promise, and resolving a capability with a rejected promise rejects it with the
       same reason. */
    if (JS_IsException(r)) {
        JSValue exc = JS_GetException(ctx);
        JSValue rej = lk_get(ctx, request, LKF_REJ);

        DCHECK(JS_IsFunction(ctx, rej) || !JS_IsNull(lock),
               "§4.4's grant caught the callback's throw with no reject function to hand it to — the request "
               "record carries the released capability from §3.2.1, and the grant is reached only through a "
               "request that has one");
        if (JS_IsNull(lock)) {
            /* §4.1's arm resolves the REQUEST's promise with r, and `r` is the rejected promise. */
            if (JS_CallAsFlow(ctx, rej, exc) < 0)
                JS_FreeValue(ctx, JS_GetException(ctx));
        } else {
            /* §4.4 resolves WAITING with r; §2.4's settle then releases the lock and hands the rejection on to
               the released promise. `resolve` is waiting's, so the rejection goes to waiting's REJECT — which
               this machine does not carry, so it is read off the lock record where the grant put it. */
            JSValue wrej = lk_get(ctx, lock, LKF_WREJ);

            if (JS_CallAsFlow(ctx, wrej, exc) < 0)
                JS_FreeValue(ctx, JS_GetException(ctx));
            JS_FreeValue(ctx, wrej);
        }
        JS_FreeValue(ctx, rej);
        JS_FreeValue(ctx, exc);
        return JS_STEP_DONE;
    }
    /* "Resolve waiting with r" (§4.4), or "Resolve promise with r" (§4.1's arm). THIS is the one resolution in
       this component that may read `then` off the page's value, and it is where the standard means it to
       happen — so it goes through JS_CallAsFlow, which gives that read a flow base. */
    if (JS_CallAsFlow(ctx, resolve, r) < 0)
        JS_FreeValue(ctx, JS_GetException(ctx));
    JS_FreeValue(ctx, r);
    return JS_STEP_DONE;
}

static const JSTrampStepDef lk_grant_def = {
    sizeof(LkGrant), lk_grant_step, NULL, 1, .visit = lk_grant_visit,
    .algorithm = "Web Locks API §4.4's enqueued grant steps / §4.1's ifAvailable arm",
    .steps = LK_GRANT_STEPS
};

/* ENQUEUE ONE OF THEM. `lock` is JS_NULL for §4.1's ifAvailable arm and the lock record for §4.4's grant;
   `resolve` is the capability §4.4 resolves (waiting's) or the one §4.1 resolves (the request's). */
static void lk_enqueue_grant(JSContext *ctx, JSValueConst manager, JSValueConst lock, JSValueConst request,
                             JSValueConst resolve)
{
    JSValueConst data[LK_CD_N];
    JSValue fn;

    DCHECK(g_grant_stepid >= 0, "a Web Locks grant was enqueued before lock_manager_init declared its machine");
    data[LK_CD_MANAGER] = manager;
    data[LK_CD_LOCK] = lock;
    data[LK_CD_REQUEST] = request;
    data[LK_CD_RESOLVE] = resolve;
    /* MINTED IN THE REALM THAT TOOK THE REQUEST, which is `ctx` — a closure held in a module static would hand
       every document the Lock.prototype of whichever realm built it first, which is the defect core/realm.h
       exists to abolish. */
    fn = JS_NewStepClosure(ctx, g_grant_stepid, 0, LK_CD_N, data);
    CHECK(!JS_IsException(fn), "a Web Locks grant task could not be allocated — a dropped one is a lock held "
                               "for ever and a callback the page asked for and never gets");
    JS_EnqueueCallTask(ctx, fn, 0, NULL, TASK_SOURCE_WEB_LOCKS);
    JS_FreeValue(ctx, fn);
}

/* ---- §2.4's settle, as the reaction on the waiting promise ------------------------------------------------ */

enum { LK_SD_MANAGER = 0, LK_SD_LOCK, LK_SD_REJECTED, LK_SD_N };

#define LK_SETTLED_STAGES(X)                                                                                   \
    X(LKS_SETTLE, "Web Locks API §2.4 Locks' steps for a waiting promise that has settled (release the lock, "  \
                  "and resolve the lock's released promise with the waiting promise)")
enum { LK_SETTLED_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const LK_SETTLED_STEPS[] = { LK_SETTLED_STAGES(JS_STEP_STAGE_LABEL) NULL };

typedef struct { JSStepHdr hdr; } LkSettled;

/* WHAT THIS MACHINE AND §4.3'S OWN: NOTHING BEYOND THE HEADER — which is why one function serves both of them
   and why it visits nothing. Each state is the header alone, and every value either step touches is read
   through JS_StepClosureData (the closure's, which outlives the state) or step_arg (the header's operands,
   which tramp_step_state_clone dups itself on the line above the one where it calls this) — so a v->val here
   would take a second reference to something this state never took a first one to, which is an over-count in
   the clone and an over-free in the teardown.
   THE TWO _Static_asserts BESIDE THE STRUCTS ARE WHAT KEEP THAT TRUE, rather than this paragraph: a JSValue
   added to either state stops this file compiling AT that struct, instead of silently handing a forked arm a
   reference nobody dup'd.
   A NULL VISIT WAS THE ONE SPELLING OF THIS FACT THE RUNTIME REFUSES, AND RIGHTLY — a definition that names no
   visit cannot be told apart from one whose author forgot to write it, so it is refused at the door
   (js_step_def_check's CHECK, fatal in release as well as dev) rather than at the fork that would corrupt.
   Both of these definitions were registered that way, so §3 aborted this agent's init in every regime on the
   first build after it landed, before any document ran. */
static void lk_hdr_only_visit(JSContext *ctx, void *st, JSStepVisit *v) { (void)ctx; (void)st; (void)v; }
_Static_assert(sizeof(LkSettled) == sizeof(JSStepHdr),
               "§2.4's settle state gained a field and lk_hdr_only_visit declares it owns none — a JSValue "
               "here is one a deep fork would hand to two flows and both teardowns would free. Give this "
               "machine a visit of its own naming the field, rather than widening the struct under the shared "
               "one");

static int lk_settled_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    LkSettled *s = st;
    JSValueConst manager = JS_StepClosureData(&s->hdr, LK_SD_MANAGER);
    JSValueConst lock = JS_StepClosureData(&s->hdr, LK_SD_LOCK);
    bool rejected = JS_ToBool(ctx, JS_StepClosureData(&s->hdr, LK_SD_REJECTED)) != 0;
    JSValue fn;

    (void)out_cb; (void)out_argc;
    STEP_DISPATCH(LK_SETTLED_STAGES, s->hdr.stage, s->hdr.def->algorithm, JS_STEP_ABRUPT);

    STEP_ARM(LKS_SETTLE);
    JS_FreeValue(ctx, cb_result);
    /* "Release the lock lock." FIRST, so the queue has already moved on by the time the page's `.then` on the
       released promise runs — which is the order §2.4 writes and is what lets a page request the same name
       again from inside that handler. */
    lk_release(ctx, manager, lock);
    /* "Resolve lock's released promise with lock's waiting promise." See this file's banner for why the value
       rather than the promise: `waiting` has settled, its fulfilment value is not a thenable, and reading
       `then` off a promise is the page's code where the standard needs none run. */
    fn = lk_get(ctx, lock, rejected ? LKF_REJ : LKF_RES);
    DCHECK(JS_IsFunction(ctx, fn), "§2.4's settle found no released capability on the lock — §4.4 builds the "
                                   "lock with the released promise §3.2.1 created, so a lock without one is a "
                                   "grant that never carried the promise it was supposed to settle");
    if (JS_CallAsFlow(ctx, fn, step_arg(&s->hdr, 0)) < 0)
        JS_FreeValue(ctx, JS_GetException(ctx));
    JS_FreeValue(ctx, fn);
    return JS_STEP_DONE;
}

static const JSTrampStepDef lk_settled_def = {
    sizeof(LkSettled), lk_settled_step, NULL, 0, .visit = lk_hdr_only_visit,
    .algorithm = "Web Locks API §2.4 Locks' waiting-promise settle steps",
    .steps = LK_SETTLED_STEPS
};

/* ---- Web Locks API §4.2 "Release a lock", §4.3 "Abort a request", §4.4 "Process a lock request queue" ----- */

/* §4.4, verbatim. Its own NOTE is what makes the loop a `while the first is grantable` rather than a scan:
   "Only the first item in a queue is grantable. Therefore, if something is not grantable then all the following
   items are automatically not grantable." */
static void lk_process_queue(JSContext *ctx, JSValueConst manager, JSValueConst queue)
{
    DCHECK(!JS_IsNull(manager), "§4.4 was asked to process a queue of a manager that is §2.2's failure — a "
                                "realm whose storage key is failure never enqueues, so there is no queue here "
                                "to process");
    for (;;) {
        JSValue request, name, waiting, lock, wfuncs[2], held, onf, onr, cap;
        JSValueConst rdata[LK_SD_N];
        LkMode mode;
        bool grantable;

        if (lk_len(ctx, queue) == 0)
            return;
        request = JS_GetPropertyUint32(ctx, queue, 0);
        name = lk_get(ctx, request, LKF_NAME);
        mode = (LkMode)lk_get_int(ctx, request, LKF_MODE);
        grantable = lk_grantable(ctx, manager, queue, name, mode, 0);
        if (!grantable) {
            JS_FreeValue(ctx, name);
            JS_FreeValue(ctx, request);
            return;
        }
        /* "Remove request from queue." */
        lk_remove(ctx, queue, request);
        /* "Let waiting be a new promise." */
        waiting = JS_NewPromiseCapability(ctx, wfuncs);
        CHECK(!JS_IsException(waiting),
              "§4.4's waiting promise could not be allocated — a grant without one is a lock nothing can ever "
              "release");
        /* "Let lock be a new lock with agent agent, clientId clientId, manager manager, mode mode, name name,
           released promise p, and waiting promise waiting." The AGENT is not a field here: this engine has one
           agent per runtime, so every lock in this manager has the same one and §2.6's per-agent termination
           has nothing to distinguish — which is the residual at the bottom of this file. */
        lock = idl_slots_new(ctx);
        CHECK(!JS_IsException(lock), "§4.4's lock record could not be allocated");
        JS_SetPropertyStr(ctx, lock, LKF_NAME, JS_DupValue(ctx, name));
        JS_SetPropertyStr(ctx, lock, LKF_MODE, JS_NewInt32(ctx, (int)mode));
        JS_SetPropertyStr(ctx, lock, LKF_CLIENT, lk_get(ctx, request, LKF_CLIENT));
        JS_SetPropertyStr(ctx, lock, LKF_WAITING, JS_DupValue(ctx, waiting));
        JS_SetPropertyStr(ctx, lock, LKF_RES, lk_get(ctx, request, LKF_RES));
        JS_SetPropertyStr(ctx, lock, LKF_REJ, lk_get(ctx, request, LKF_REJ));
        /* WAITING'S OWN REJECT, so §4.4's "Resolve waiting with r" has a reject arm for Web IDL §3.12's
           rejected-promise case. It is on the LOCK and not on the grant machine's captures because the grant is
           re-entered after a suspension and a capability copied into a machine's state would be copied again by
           a fork; the lock record is the one thing both halves already share. */
        JS_SetPropertyStr(ctx, lock, LKF_WREJ, JS_DupValue(ctx, wfuncs[1]));
        /* "Append lock to manager's held lock set." */
        held = lk_get(ctx, manager, LKF_HELD);
        JS_SetPropertyUint32(ctx, held, lk_len(ctx, held), JS_DupValue(ctx, lock));
        JS_FreeValue(ctx, held);
        /* §2.4: "When lock lock's waiting promise settles (fulfills or rejects), enqueue the following steps on
           the lock task queue". Attached through Web IDL's own PerformPromiseThen and never through a
           page-visible `.then`, so a page that replaces Promise.prototype.then does not thereby change what
           this algorithm does. */
        rdata[LK_SD_MANAGER] = manager;
        rdata[LK_SD_LOCK] = lock;
        rdata[LK_SD_REJECTED] = JS_FALSE;
        onf = JS_NewStepClosure(ctx, g_settled_stepid, 1, LK_SD_N, rdata);
        rdata[LK_SD_REJECTED] = JS_TRUE;
        onr = JS_NewStepClosure(ctx, g_settled_stepid, 1, LK_SD_N, rdata);
        CHECK(!JS_IsException(onf) && !JS_IsException(onr),
              "§2.4's settle reactions could not be allocated — a lock whose waiting promise has no reaction is "
              "never released and its released promise never settles");
        cap = JS_PerformPromiseThen(ctx, waiting, onf, onr);
        JS_FreeValue(ctx, cap);
        JS_FreeValue(ctx, onf);
        JS_FreeValue(ctx, onr);
        /* "Enqueue the following steps on callback's relevant settings object's responsible event loop". */
        lk_enqueue_grant(ctx, manager, lock, request, wfuncs[0]);
        JS_FreeValue(ctx, wfuncs[0]);
        JS_FreeValue(ctx, wfuncs[1]);
        JS_FreeValue(ctx, waiting);
        JS_FreeValue(ctx, lock);
        JS_FreeValue(ctx, name);
        JS_FreeValue(ctx, request);
        /* AND ROUND AGAIN, which is §4.4's "For each request of queue" continuing: a shared lock just granted
           leaves the next SHARED request grantable, and that is the readers-half of §2.3's readers-writer
           pattern. The loop terminates because every turn either returns or removes an element. */
    }
}

/* Web Locks API §4.2 "Release a lock", verbatim. */
static void lk_release(JSContext *ctx, JSValueConst manager, JSValueConst lock)
{
    JSValue name = lk_get(ctx, lock, LKF_NAME);
    JSValue queue = lk_queue_of(ctx, manager, name);
    JSValue held = lk_get(ctx, manager, LKF_HELD);

    /* "Remove lock from the manager's held lock set." THE REMOVAL IS A STATEMENT AND THE ASSERT IS ABOUT ITS
       ANSWER, which is not a style choice: a DCHECK condition is compiled out in release, so a removal
       performed INSIDE one would never happen there — the lock would stay in the held set for ever and every
       later request for that name would wait on a callback that had already returned. That is the
       side-effect-in-a-condition §Offensive-programming forbids by name, and it is invisible in a dev build
       because dev is exactly where the condition IS evaluated. */
    bool was_held = lk_remove(ctx, held, lock);

    /* A lock released twice would let the next request in while the first callback still believes it holds the
       resource, so the removal is asserted to have FOUND something rather than being allowed to be a no-op. */
    DCHECK(was_held,
           "Web Locks API §4.2 released a lock that is not in its manager's held lock set — the two callers are "
           "§2.4's settle "
           "(once per waiting promise) and §4.4's aborted-signal arm (which returns without settling), so a "
           "lock reaching here twice is one whose waiting promise settled after it had already been released");
    JS_FreeValue(ctx, held);
    /* "Process the lock request queue queue." */
    lk_process_queue(ctx, manager, queue);
    JS_FreeValue(ctx, queue);
    JS_FreeValue(ctx, name);
}

/* §4.3 "Abort a request", verbatim — the half that runs on the lock task queue. */
static void lk_abort_request(JSContext *ctx, JSValueConst manager, JSValueConst request)
{
    JSValue name = lk_get(ctx, request, LKF_NAME);
    JSValue queue = lk_queue_of(ctx, manager, name);

    /* "Remove request from queue." NOT asserted to have found it: §4.3's "signal to abort the request" enqueues
       these steps and the grant may have taken the request off the queue first, which is exactly the race
       §4.4's own signal test exists for — it releases the lock it just granted. */
    lk_remove(ctx, queue, request);
    /* "Process the lock request queue queue." */
    lk_process_queue(ctx, manager, queue);
    JS_FreeValue(ctx, queue);
    JS_FreeValue(ctx, name);
}

/* ---- §4.3's "signal to abort the request", as the algorithm added to the AbortSignal ----------------------- */

enum { LK_AD_MANAGER = 0, LK_AD_REQUEST, LK_AD_SIGNAL, LK_AD_N };

#define LK_ABORT_STAGES(X)                                                                                     \
    X(LKA_ABORT, "Web Locks API §4.3 Abort a request's signal to abort the request (enqueue the steps to abort " \
                 "the request to the lock task queue, and reject the request's promise with the signal's abort " \
                 "reason)")
enum { LK_ABORT_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const LK_ABORT_STEPS[] = { LK_ABORT_STAGES(JS_STEP_STAGE_LABEL) NULL };

typedef struct { JSStepHdr hdr; } LkAbort;
/* OWNS NOTHING BEYOND THE HEADER — see lk_hdr_only_visit, which this definition shares, for why that is a
   declaration rather than an omission. */
_Static_assert(sizeof(LkAbort) == sizeof(JSStepHdr),
               "§4.3's abort state gained a field and lk_hdr_only_visit declares it owns none — a JSValue "
               "here is one a deep fork would hand to two flows and both teardowns would free. Give this "
               "machine a visit of its own naming the field, rather than widening the struct under the shared "
               "one");

static int lk_abort_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    LkAbort *s = st;
    JSValueConst manager = JS_StepClosureData(&s->hdr, LK_AD_MANAGER);
    JSValueConst request = JS_StepClosureData(&s->hdr, LK_AD_REQUEST);
    JSValueConst signal = JS_StepClosureData(&s->hdr, LK_AD_SIGNAL);
    JSValue reason, rej;

    (void)out_cb; (void)out_argc;
    STEP_DISPATCH(LK_ABORT_STAGES, s->hdr.stage, s->hdr.def->algorithm, JS_STEP_ABRUPT);

    STEP_ARM(LKA_ABORT);
    JS_FreeValue(ctx, cb_result);
    /* "Enqueue the steps to abort the request request to the lock task queue." */
    lk_abort_request(ctx, manager, request);
    /* "Reject request's promise with signal's abort reason." The reason is read off the SIGNAL rather than taken
       as an argument, because DOM's "signal abort" runs each abort algorithm with none. */
    reason = abort_signal_reason(ctx, signal);
    rej = lk_get(ctx, request, LKF_REJ);
    DCHECK(JS_IsFunction(ctx, rej), "§4.3 rejected a request that carries no reject function — §3.2.1 creates "
                                    "the capability before §4.1 builds the request, so every request in a "
                                    "queue has one");
    if (JS_CallAsFlow(ctx, rej, reason) < 0)
        JS_FreeValue(ctx, JS_GetException(ctx));
    JS_FreeValue(ctx, rej);
    JS_FreeValue(ctx, reason);
    return JS_STEP_DONE;
}

static const JSTrampStepDef lk_abort_def = {
    sizeof(LkAbort), lk_abort_step, NULL, 0, .visit = lk_hdr_only_visit,
    .algorithm = "Web Locks API §4.3 Abort a request's signal to abort the request",
    .steps = LK_ABORT_STEPS
};

/* ---- §3.2's interface: the brand, §2.2's manager, and the two methods ------------------------------------- */

static bool lk_manager_brand(JSContext *ctx, JSValueConst this_val, const char *member)
{
    DCHECK(g_manager_class != 0, "a LockManager member ran before lock_manager_init declared the class");
    if (JS_GetClassID(this_val) == g_manager_class)
        return true;
    JS_ThrowTypeError(ctx, "LockManager.%s was called on something that is not a LockManager object", member);
    return false;
}

/* THE INTERNAL-SLOT RECORD OF A LockManager OR A Lock. Read as an OWN SLOT and never as a lookup: a miss on a
   lookup is the solver's absent-state seam and would mint a concolic for an internal slot, which is right for
   the page's own reads and wrong here. OWNED. */
static JSValue lk_slots_of(JSContext *ctx, JSValueConst v)
{
    JSAtom k = JS_ValueToAtom(ctx, g_lock_key);
    JSValue st;

    DCHECK(g_ready, "a Web Locks internal slot was asked for before the key existed");
    CHECK(k != JS_ATOM_NULL, "the Web Locks internal-slot key could not be interned");
    if (JS_GetOwnSlot(ctx, &st, v, k) <= 0)
        st = JS_UNDEFINED;
    JS_FreeAtom(ctx, k);
    DCHECK(JS_IsObject(st), "a Web Locks platform object has no internal-slot record — the realm's install mints "
                            "the LockManager and its record together and §4.4 mints a Lock with its own, so an "
                            "object without one is not this component's and its brand check has gone wrong");
    return st;
}

/* §2.2 "Lock Managers"' OBTAIN A LOCK MANAGER, and the one place §2.2's failure is decided.
 *
 * IT IS OBTAINED ONCE PER REALM AND CACHED ON THE LockManager OBJECT, not once per call, and the reason is both
 * halves of what the standard asks for. §4.6's obtain-a-storage-bottle-map APPENDS the proxy map it mints to the
 * bottle's proxy map reference set, so a call per request() would grow that set without bound for a value that
 * cannot change: a realm's storage key is fixed for the realm's life, so §3.2.1 step 4's answer is too. And
 * §2.2's own editorial note says the integration with Storage is unrefined, so what is stable is the pair of
 * answers — the manager, or failure — rather than the number of times the map is obtained.
 * JS_NULL IS §2.2's FAILURE AND IS A REAL ANSWER: obtain-a-storage-key answers failure for an OPAQUE ORIGIN, and
 * §3.2.1 and §3.2.2 both turn that into a "SecurityError" DOMException. BORROWED. */
static JSValueConst lk_manager_of(JSContext *ctx, JSValueConst self)
{
    JSValue slots = lk_slots_of(ctx, self);
    JSValue cached = lk_get(ctx, slots, LKF_MANAGER);
    JSValueConst borrowed;

    if (JS_IsUndefined(cached)) {
        /* "Let map be the result of obtaining a local storage bottle map given environment and" this
           component's identifier. "If map is failure, then return failure." */
        JSValue map = storage_shed_obtain_bottle_map(ctx, STORAGE_TYPE_LOCAL, LK_BOTTLE);

        if (JS_IsUndefined(map)) {
            JS_SetPropertyStr(ctx, slots, LKF_MANAGER, JS_NULL);
        } else {
            /* "Let bottle be map's associated storage bottle. Return bottle's associated lock manager." The
               bottle's BACKING MAP is what §4.7 says every operation on a proxy map is performed on, so it is
               the per-bucket identity §2.2's Note requires — every environment with this storage key obtains a
               different proxy map over the SAME backing, and therefore the same manager. */
            JSValue backing = storage_shed_backing_map(ctx, map);
            JSValue manager = lk_get(ctx, backing, LKF_MANAGER);

            if (!JS_IsObject(manager)) {
                JS_FreeValue(ctx, manager);
                manager = idl_slots_new(ctx);
                CHECK(!JS_IsException(manager), "§2.2's lock manager could not be allocated");
                JS_SetPropertyStr(ctx, manager, LKF_HELD, JS_NewArray(ctx));
                JS_SetPropertyStr(ctx, manager, LKF_QUEUES, idl_slots_new(ctx));
                JS_SetPropertyStr(ctx, backing, LKF_MANAGER, JS_DupValue(ctx, manager));
            }
            JS_SetPropertyStr(ctx, slots, LKF_MANAGER, manager);
            JS_FreeValue(ctx, backing);
        }
        JS_FreeValue(ctx, map);
        JS_FreeValue(ctx, cached);
        cached = lk_get(ctx, slots, LKF_MANAGER);
    }
    /* BORROWED off the record, which keeps it alive for as long as the LockManager object does — and the object
       is the realm's, so that is the realm's whole life. permission_status.c's descriptor borrows the same way
       and for the same reason: an algorithm's stack value never outlives the record it read. */
    borrowed = cached;
    JS_FreeValue(ctx, cached);
    JS_FreeValue(ctx, slots);
    return borrowed;
}

static JSValueConst lk_client_id(JSContext *ctx, JSValueConst self)
{
    JSValue slots = lk_slots_of(ctx, self);
    JSValue id = lk_get(ctx, slots, LKF_CLIENT);
    JSValueConst borrowed = id;

    DCHECK(JS_IsString(id), "a LockManager's client id is not a string — the realm's install mints one with the "
                            "object, and §3.2.2's snapshot reports it as a DOMString");
    JS_FreeValue(ctx, id);
    JS_FreeValue(ctx, slots);
    return borrowed;
}

/* ---- §3.2.1 "The request() method", and §4.1 "Request a lock" --------------------------------------------- */

/* `LockOptions` IN WEB IDL §3.2.17's OWN READ ORDER, which is LEXICOGRAPHICAL over the member names and not the
   order the IDL block lists them in — `ifAvailable`, `mode`, `signal`, `steal`. That is the order a page's
   getters observe, and a declaration in the IDL's order would make a page with four getters see them in the
   wrong one. `signal` declares no default, so §3.2.1 step 9's "If options['signal'] EXISTS" is the presence
   test an absent member answers; the other three carry the IDL's defaults, which is §3.2.1 step 1's "let
   options be a new LockOptions dictionary with default members" performed by the declaration. */
/* THE §3.2.15 TAIL IS NAMED AND NOT REACHED POSITIONALLY, which core/idl_args.h states as a rule rather than a
   style: the struct has gained fields more than once, and a positional list running to the end re-aims every
   value after the next field added — with no diagnostic at all where the neighbours are both pointers.
   `signal` STATES ITS INTERFACE AS A PREDICATE AND NOT AS A CLASS, because `AbortSignal` is asked of the
   REALM's AbortSignal.prototype rather than of one class id — which is the first of the three shapes
   IdlDictMember::iface_is exists for. */
static const IdlDictMember LK_OPTIONS[] = {
    { "ifAvailable", IDL_BOOLEAN, false, NULL, 0, NULL, IDL_DEFAULT_FALSE },
    { "mode",        IDL_ENUM,    false, LK_MODE_VALUES, 0, NULL, IDL_DEFAULT_STRING, "exclusive" },
    { "signal",      IDL_INTERFACE, false, NULL, 0, NULL, IDL_DEFAULT_NONE, NULL,
      .iface_is = abort_signal_is, .iface_name = "AbortSignal" },
    { "steal",       IDL_BOOLEAN, false, NULL, 0, NULL, IDL_DEFAULT_FALSE },
};
#define LK_OPTIONS_N ((int)(sizeof LK_OPTIONS / sizeof LK_OPTIONS[0]))

/* §3.2's TWO OVERLOADS AS ONE DECLARATION — the longest type list the effective overload set has, with the
   position the entries split at carrying that split as its type:

       Promise<any> request(DOMString name, LockGrantedCallback callback);
       Promise<any> request(DOMString name, LockOptions options, LockGrantedCallback callback);

   §3.6 steps 3-4 remove one of them from the ARGUMENT COUNT alone — the type lists are two and three long, so
   exactly one survives at every arity and step 12 never runs. See IDL_CALLBACK_OR_DICT, which is that fact as a
   type rather than as a shape test in this body. */
static const IdlArgType LK_REQUEST_ARGS[3] = { IDL_DOMSTRING, IDL_CALLBACK_OR_DICT, IDL_CALLBACK };

#define LK_REQ_STAGES(X)                                                                                       \
    X(LKR_RUN, "Web Locks API §3.2.1 The request() method steps 2-12 and §4.1 Request a lock (the refusals, "   \
               "the new promise, and the request enqueued on the lock task queue)")
enum { IDL_STEP_STAGE_BASE(LK_REQ_STAGES) LK_REQ_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const LK_REQ_STEPS[] = { LK_REQ_STAGES(JS_STEP_STAGE_LABEL) NULL };

typedef struct {
    JSValue promise;
    JSValue funcs[2];   /* the released promise's capability — every refusal below settles it */
    /* §3.2.1 STEP 9's SIGNAL TEST HELD WHERE THE SIBLING'S SNAPSHOT CARRIES IT — abort_signal_aborted_step's
       operand, for the reason `LkGrant.sig_flag` gives at length: the page may pass any AbortSignal, an
       `AbortSignal.timeout()` has an UNKNOWN `aborted`, the seam BORROWS the flag for the length of the request,
       and a deep fork byte-copies this state and re-takes only what `lk_req_visit` names. JS_UNINITIALIZED is
       the EMPTY value and is written explicitly in the prologue, because a zeroed step state's JSValue is the
       INTEGER 0 rather than JS_UNDEFINED. `query` has no signal and never writes it, which is why it is stated
       beside the capability both members build rather than at the one member's ask. */
    JSValue sig_flag;
    uint8_t started;
} LkReqState;

static void lk_req_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    LkReqState *s = st;

    if (!s->started)
        return;
    v->val(ctx, &s->promise);
    v->val(ctx, &s->funcs[0]);
    v->val(ctx, &s->funcs[1]);
    /* NAMED HERE AND THEREFORE BEHIND THE `started` RETURN, which is correct rather than lucky: the prologue
       writes this slot in the same guarded block that sets `started`, so a state that has not begun holds the
       INTEGER 0 here and there is nothing for a visit to take or release. */
    v->val(ctx, &s->sig_flag);
}

/* EVERY REFUSAL OF §3.2.1 AND §3.2.2, as the one operation all of them are: settle the promise the algorithm
   already created with the exception that is live. "Return a promise rejected with X" is a value and not a
   completion, which is what makes `request(…).catch(f)` run f. */
static int lk_reject(JSContext *ctx, LkReqState *s, JSValue *presult)
{
    JSValue exc = JS_GetException(ctx);

    DCHECK(s->started, "a Web Locks member rejected before it created its promise — §3.2.1 and §3.2.2 settle "
                       "the promise their own later steps create, so the capability is built before the first "
                       "thing that can fail rather than at the step the standard numbers it");
    if (JS_CallAsFlow(ctx, s->funcs[1], exc) < 0)
        JS_FreeValue(ctx, JS_GetException(ctx));
    JS_FreeValue(ctx, exc);
    *presult = s->promise;
    s->promise = JS_UNDEFINED;
    return JS_STEP_DONE;
}

/* THE PROLOGUE §3.2.1 AND §3.2.2 SHARE, in the standard's own order: the capability first (so every refusal
   below it is a rejection), Web IDL §3.7.7's brand check, step 3's fully-active test and step 4's manager.
   `*prejected` IS THE ANSWER AND THE RETURN VALUE IS NOT, because a JSValue has no spare sentinel: §2.2's own
   failure is JS_NULL and that is a REFUSAL the caller must tell apart from a brand check's, so the two are
   separate outputs rather than one value doing both jobs. */
static JSValueConst lk_prologue(JSContext *ctx, LkReqState *s, JSValueConst self, const char *member,
                                JSValue *presult, int *prejected)
{
    *prejected = 0;
    /* ONCE PER INVOCATION, NOT ONCE PER ENTRY. §3.2.1's step 9 is a SIGNAL TEST and it can PARK, so a fork
       re-enters this member's one stage AT ITS TOP TWICE — once in the parent carrying the answer and once in
       the sibling re-asking — and everything above the ask runs again. Re-minting the capability there would
       replace a promise and two resolving functions the arm's own snapshot has already re-taken, leaking three
       references per fork and handing the page a promise that is not the one the first pass built; re-stating
       `sig_flag` would drop the very reference the seam is holding across the park.
       `started` IS THE RIGHT GUARD HERE AND IT IS NOT A PHASE COUNTER BEING MISUSED. abort.h warns that a
       machine's own phase byte is not the fork test, and the reason is that a phase is written BELOW the ask so
       it still reads its initial value on both fork entries. This byte is written ABOVE it, and the question it
       answers is not "is this a fork entry" but "does the capability already exist" — which is the question the
       mint actually has, and is right however the re-entry came about. The fork question is asked separately,
       by `step_fork_pending`, at the one place that needs it: the assert in `lk_request_step`. */
    if (!s->started) {
        s->promise = s->funcs[0] = s->funcs[1] = JS_UNDEFINED;
        s->promise = JS_NewPromiseCapability(ctx, s->funcs);
        CHECK(!JS_IsException(s->promise),
              "a Web Locks member's promise capability could not be allocated — a call that answers with neither "
              "a promise nor a throw is one a page can only hang on");
        /* STATED, never read off the slot: a zeroed step state's JSValue is the INTEGER 0. */
        s->sig_flag = JS_UNINITIALIZED;
        s->started = 1;
    }
    /* WEB IDL §3.7.7 "Operations": "If jsValue does not implement the interface target, throw a TypeError." That
       section makes it a REJECTION here rather than a throw, because the return type is a promise — which is
       why it is asked AFTER the capability exists. */
    if (!lk_manager_brand(ctx, self, member)) {
        *prejected = 1;
        return JS_NULL;
    }
    /* STEP 3: "If environment's relevant global object's associated Document is not fully active, then return a
       promise rejected with a 'InvalidStateError' DOMException." */
    if (!document_fully_active(ctx)) {
        JS_ThrowDOMException(ctx, "InvalidStateError", "%s",
                             "the document is not fully active, so its locks cannot be requested");
        *prejected = 1;
        return JS_NULL;
    }
    /* STEP 4: "Let manager be the result of obtaining a lock manager given environment. If that returned
       failure, then return a promise rejected with a 'SecurityError' DOMException." */
    {
        JSValueConst manager = lk_manager_of(ctx, self);

        if (JS_IsNull(manager)) {
            JS_ThrowDOMException(ctx, "SecurityError", "%s",
                                 "this document's storage key is failure, so it has no lock manager");
            *prejected = 1;
            return JS_NULL;
        }
        return manager;
    }
}

static int lk_request_step(JSContext *ctx, JSStepHdr *hdr, void *st, int argc, JSValueConst *argv,
                           JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    LkReqState *s = st;
    JSValueConst manager, name, options, callback, signal;
    JSValue request, queue, mode_v, id;
    LkMode mode;
    bool if_available, steal;
    int rejected;

    (void)out_cb; (void)out_argc;
    /* ONE STAGE, AND THE REASON IS NO LONGER THAT NOTHING BELOW RUNS THE PAGE'S CODE. This comment used to say
       exactly that, and the engine's own `js_step_labels_check` bans that sentence FROM A LABEL for the reason
       it is wrong here too: a stage boundary is a rest point because the ENGINE may have to park there — RAM
       pressure paging the low-value tail to the cold tier, a cross-session resume, a flow that outranks this
       one, and now step 9's FORK — and none of those consult the page, so the page's quiet never bought this
       stage the right to span eleven steps. It spans them because every one of steps 2-8 and 10-12 is an O(1)
       engine action over values already converted, which is the one ground the check admits; step 9 is the only
       one that suspends, and it suspends INTO THIS STAGE rather than out of it, which is what makes the arm
       idempotent above the ask the thing that has to be true. The argument conversion that does run the page's
       code — §3.2.17's member reads over the options object — is the prologue the declaration owns, two stages
       of its own, which is where it rests. */
    DCHECK(hdr->stage == LKR_RUN, "§3.2.1's request resumed into a stage it does not have");
    JS_FreeValue(ctx, cb_result);
    /* THE ONLY RE-ENTRY AFTER THIS BODY HAS BEGUN IS A FORK'S, AND THAT IS NOW A REAL INVARIANT RATHER THAN A
       VACUOUS ONE. This assert used to read `!s->started` and say `its one stage makes no request, so there is
       no suspension for it to come back from` — true of the machine as it stood, because step 9's signal test
       answered a `bool` and could not park, and FALSE the moment it could. It is kept in its own words because
       the reasoning is the reasoning a reader re-derives: a member whose one stage issues no request really does
       have nothing to come back from, and the thing that made it wrong is a FORK, which is a re-entry no request
       is outstanding for. `step_fork_pending` is that fact (quickjs-step.h) and it is true across BOTH entries a
       fork produces, so the pair below still refuses every re-entry that is neither a first one nor a fork's. */
    DCHECK(!s->started || step_fork_pending(hdr),
           "§3.2.1's request was re-entered after it had begun and no fork of its own is outstanding — its one "
           "stage makes no request, so the only re-entry it has is the pair step 9's signal test produces when "
           "it forks, and the ARGUMENT conversion that can suspend completes before this body is entered at all");
    manager = lk_prologue(ctx, s, hdr->this_val, "request", presult, &rejected);
    if (rejected)
        return lk_reject(ctx, s, presult);
    /* §3.6 HAS ALREADY CHOSEN THE ENTRY: at argc 2 position 1 is the callback and there is no options object, at
       argc 3 position 1 is the dictionary and position 2 is the callback. The type at position 1 states both,
       so this reads the ARITY the conversion already resolved rather than testing the value again. */
    name = argv[0];
    options = (argc >= 3) ? argv[1] : JS_UNDEFINED;
    callback = (argc >= 3) ? argv[2] : argv[1];
    DCHECK(JS_IsFunction(ctx, callback),
           "§3.2.1's request reached its steps with a callback that is not callable — Web IDL §3.2.19's "
           "callback function conversion refuses that at the argument boundary, so the declaration's type list "
           "and this body have come apart about which position the callback is at");
    /* STEP 1: "If options was not passed, then let options be a new LockOptions dictionary with default
       members." An absent dictionary has every member absent, and reading each as its IDL default IS that step
       — which is why the member rows above carry the defaults and this body reads values rather than presence. */
    if_available = idl_dict_bool(ctx, options, "ifAvailable");
    steal = idl_dict_bool(ctx, options, "steal");
    /* `LockMode mode = "exclusive"` — the DEFAULT is the declaration's, so an absent member arrives as the
       string the IDL writes rather than as `undefined`, and this reads a value rather than testing presence.
       §3.2.18's conversion has already refused anything that is not one of the two, which is what the assert
       says: a third string here would be the declaration's value list and this file's enum disagreeing. */
    mode_v = idl_dict_get(ctx, options, "mode");
    {
        const char *m = JS_ToCString(ctx, mode_v);

        DCHECK(m != NULL && (strcmp(m, LK_MODE_VALUES[LK_SHARED]) == 0 ||
                             strcmp(m, LK_MODE_VALUES[LK_EXCLUSIVE]) == 0),
               "§3.2.1's `mode` is not one of `LockMode`'s two values — Web IDL §3.2.18's enumeration "
               "conversion throws for anything else at the argument boundary and the member declares a "
               "default, so neither an absent member nor a page value can reach here as a third one");
        mode = (m && strcmp(m, LK_MODE_VALUES[LK_SHARED]) == 0) ? LK_SHARED : LK_EXCLUSIVE;
        if (m) JS_FreeCString(ctx, m);
    }
    JS_FreeValue(ctx, mode_v);
    signal = JS_UNDEFINED;
    {
        JSValue sv = idl_dict_get(ctx, options, "signal");

        if (abort_signal_is(ctx, sv))
            signal = sv;               /* BORROWED below and released with the dictionary */
        else
            DCHECK(JS_IsUndefined(sv), "§3.2.1's `signal` member is neither absent nor an AbortSignal — Web IDL "
                                       "§3.2.15's interface-type conversion refuses that at the boundary");
        JS_FreeValue(ctx, sv);
    }
    /* STEP 5: "If name starts with U+002D HYPHEN-MINUS (-), then return a promise rejected with a
       'NotSupportedError' DOMException." The names beginning with it are reserved for the user agent. */
    {
        const char *n = JS_ToCString(ctx, name);
        bool hyphen = n && n[0] == '-';

        if (n) JS_FreeCString(ctx, n);
        if (hyphen) {
            JS_ThrowDOMException(ctx, "NotSupportedError", "%s",
                                 "a lock name beginning with U+002D HYPHEN-MINUS is reserved");
            return lk_reject(ctx, s, presult);
        }
    }
    /* STEPS 6, 7 AND 8 — the three combinations §3.2.1 refuses outright, each one its own sentence and each one
       a "NotSupportedError". They are separate tests rather than one because the standard's reasons differ:
       stealing while only asking for availability is contradictory, stealing a shared lock is not defined, and a
       signal has nothing to abort where the request either wins immediately or does not wait. */
    if (steal && if_available) {
        JS_ThrowDOMException(ctx, "NotSupportedError", "%s",
                             "`steal` and `ifAvailable` may not both be true");
        return lk_reject(ctx, s, presult);
    }
    if (steal && mode != LK_EXCLUSIVE) {
        JS_ThrowDOMException(ctx, "NotSupportedError", "%s",
                             "`steal` is only defined for an exclusive lock");
        return lk_reject(ctx, s, presult);
    }
    if (!JS_IsUndefined(signal) && (steal || if_available)) {
        JS_ThrowDOMException(ctx, "NotSupportedError", "%s",
                             "`signal` may not be combined with `steal` or `ifAvailable`");
        return lk_reject(ctx, s, presult);
    }
    /* STEP 9: "If options["signal"] exists and is aborted, then return a promise rejected with
       options["signal"]'s abort reason." The REASON and not an AbortError this file invents — a page that
       aborted with its own value reads that value back.
       THE PARKING FORM, which is what the prologue's guard above was the prerequisite for. `abort_signal_aborted`
       answers a `bool` and therefore cannot say "I forked", so an `AbortSignal.timeout()` passed here reached
       solver/engine.c's seam with no resume point and ABORTED — and §4.4's test one stage over had already been
       converted, so this was the same question answered two ways in one component. Nothing had to be built in
       the fork machinery: this machine holds the resume point the driver clones at, and every statement between
       the prologue and this line is an idempotent READ of a dictionary Web IDL §3.2.17 converted before the body
       was entered — `idl_dict_bool`, an `idl_dict_get` that frees what it took, a `JS_ToCString` that frees its
       string — so the two entries a fork produces re-derive them with the same answers and re-ask the same
       question. What was NOT idempotent was the capability, which is why the guard is up there and not here:
       THE GUARD GOES ON THE INIT AND NEVER ON THE ASK, because the sibling must re-ask and the arm it takes is
       replayed from the flow's own decision vector at the ask. */
    if (!JS_IsUndefined(signal)) {
        int aborted = 0, fr;

        fr = abort_signal_aborted_step(ctx, hdr, signal, &s->sig_flag, &aborted);
        /* PARKED. `signal` is BORROWED from the dictionary and released with it, so there is nothing here to let
           go of; the stage is unchanged and the sibling re-enters AT this ask. */
        if (fr)
            return fr;
        if (aborted) {
            JSValue reason = abort_signal_reason(ctx, signal);

            if (JS_CallAsFlow(ctx, s->funcs[1], reason) < 0)
                JS_FreeValue(ctx, JS_GetException(ctx));
            JS_FreeValue(ctx, reason);
            *presult = s->promise;
            s->promise = JS_UNDEFINED;
            return JS_STEP_DONE;
        }
    }
    /* STEP 11: "Request a lock with promise, the current agent, environment's id, manager, callback, name,
       options['mode'], options['ifAvailable'], options['steal'], and options['signal']" — §4.1, which is the
       rest of this function. Step 10's promise is the capability the prologue built. */
    id = JS_DupValue(ctx, lk_client_id(ctx, hdr->this_val));
    request = idl_slots_new(ctx);
    CHECK(!JS_IsException(request), "§4.1's lock request could not be allocated");
    JS_SetPropertyStr(ctx, request, LKF_NAME, JS_DupValue(ctx, name));
    JS_SetPropertyStr(ctx, request, LKF_MODE, JS_NewInt32(ctx, (int)mode));
    JS_SetPropertyStr(ctx, request, LKF_CLIENT, id);
    JS_SetPropertyStr(ctx, request, LKF_CALLBACK, JS_DupValue(ctx, callback));
    JS_SetPropertyStr(ctx, request, LKF_RES, JS_DupValue(ctx, s->funcs[0]));
    JS_SetPropertyStr(ctx, request, LKF_REJ, JS_DupValue(ctx, s->funcs[1]));
    JS_SetPropertyStr(ctx, request, LKF_SIGNAL, JS_DupValue(ctx, signal));
    /* §4.1: "If signal is present, then add the algorithm signal to abort the request request with signal to
       signal." The function is kept ON THE REQUEST so §4.4 can remove exactly it when the lock is granted —
       DOM's remove takes the algorithm, and a second closure over the same steps is a different algorithm. */
    if (!JS_IsUndefined(signal)) {
        JSValueConst adata[LK_AD_N];
        JSValue algo;

        adata[LK_AD_MANAGER] = manager;
        adata[LK_AD_REQUEST] = request;
        adata[LK_AD_SIGNAL] = signal;
        algo = JS_NewStepClosure(ctx, g_abort_stepid, 0, LK_AD_N, adata);
        CHECK(!JS_IsException(algo), "§4.1's signal-to-abort algorithm could not be allocated — a request whose "
                                     "signal cannot reach it is one an abort silently never aborts");
        abort_signal_add_algorithm(ctx, signal, algo);
        JS_SetPropertyStr(ctx, request, LKF_ALGO, algo);
    }
    /* §4.1's "Enqueue the following steps to the lock task queue" — see this file's banner for why they run
       here. They touch this component's records and nothing else. */
    queue = lk_queue_of(ctx, manager, name);
    if (steal) {
        /* "If steal is true, then run these steps: For each lock of held: If lock's name is name, then run
           these steps: Remove lock from held. Reject lock's released promise with an 'AbortError'
           DOMException. Prepend request in queue."
           THE REJECTION IS NOT Web Locks API §4.2's RELEASE, which is why the loop is written out rather
           than calling it: §4.2 would process the queue for each removed lock and grant this request before
           the prepend,
           and the standard removes them all first. */
        JSValue held = lk_get(ctx, manager, LKF_HELD);
        uint32_t i = 0;

        while (i < lk_len(ctx, held)) {
            JSValue lock = JS_GetPropertyUint32(ctx, held, i);
            JSValue ln = lk_get(ctx, lock, LKF_NAME);

            if (JS_IsSameValue(ctx, ln, name)) {
                JSValue rej = lk_get(ctx, lock, LKF_REJ);

                lk_remove(ctx, held, lock);
                JS_ThrowDOMException(ctx, "AbortError", "%s",
                                     "this lock was stolen by a request made with `steal`");
                {
                    JSValue exc = JS_GetException(ctx);

                    if (JS_CallAsFlow(ctx, rej, exc) < 0)
                        JS_FreeValue(ctx, JS_GetException(ctx));
                    JS_FreeValue(ctx, exc);
                }
                JS_FreeValue(ctx, rej);
            } else {
                i++;
            }
            JS_FreeValue(ctx, ln);
            JS_FreeValue(ctx, lock);
        }
        JS_FreeValue(ctx, held);
        /* "Prepend request in queue." */
        {
            uint32_t n = lk_len(ctx, queue), j;

            for (j = n; j > 0; j--)
                JS_SetPropertyUint32(ctx, queue, j, JS_GetPropertyUint32(ctx, queue, j - 1));
            JS_SetPropertyUint32(ctx, queue, 0, JS_DupValue(ctx, request));
        }
    } else if (if_available && !lk_grantable(ctx, manager, queue, name, mode, -1)) {
        /* "If ifAvailable is true and request is not grantable, then enqueue the following steps on callback's
           relevant settings object's responsible event loop: Let r be the result of invoking callback with null
           as the only argument. Resolve promise with r and abort these steps."
           NOT ENQUEUED IN THE QUEUE, which is what "abort these steps" means: the request never joins it, so
           nothing ever grants it and §4.4 never sees it. */
        lk_enqueue_grant(ctx, manager, JS_NULL, request, s->funcs[0]);
        JS_FreeValue(ctx, queue);
        JS_FreeValue(ctx, request);
        *presult = s->promise;
        s->promise = JS_UNDEFINED;
        return JS_STEP_DONE;
    } else {
        /* "Enqueue request in queue." */
        JS_SetPropertyUint32(ctx, queue, lk_len(ctx, queue), JS_DupValue(ctx, request));
    }
    /* "Process the lock request queue queue." */
    lk_process_queue(ctx, manager, queue);
    JS_FreeValue(ctx, queue);
    JS_FreeValue(ctx, request);
    /* STEP 12: "Return promise." */
    *presult = s->promise;
    s->promise = JS_UNDEFINED;
    return JS_STEP_DONE;
}

static const IdlStepDecl LK_REQUEST_DECL = {
    lk_request_step, sizeof(LkReqState), lk_req_visit, NULL,
    "Web Locks API §3.2.1 The request() method", LK_REQ_STEPS,
    /* catches_abrupt: every failure step of §3.2.1 REJECTS. A conversion that throws after suspending would
       otherwise tear this machine down and propagate synchronously, past the `.catch` the page wrote. */
    1
};

/* ---- §3.2.2 "The query() method", and §4.5 "Snapshot the lock state" -------------------------------------- */

#define LK_QUERY_STAGES(X)                                                                                     \
    X(LKQ_RUN, "Web Locks API §3.2.2 The query() method steps 2-6 and §4.5 Snapshot the lock state")
enum { IDL_STEP_STAGE_BASE(LK_QUERY_STAGES) LK_QUERY_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const LK_QUERY_STEPS[] = { LK_QUERY_STAGES(JS_STEP_STAGE_LABEL) NULL };

/* §4.5's «[ "name" → …, "mode" → …, "clientId" → … ]», as the `LockInfo` a page reads. It is a page-facing
   object and not an internal-slot record, so its members are DEFINED rather than assigned — which is what
   converting an IDL dictionary to an Object does, and what keeps a page's Object.prototype setter out of it. */
static JSValue lk_info(JSContext *ctx, JSValueConst rec)
{
    JSValue o = JS_NewObject(ctx);
    LkMode mode = (LkMode)lk_get_int(ctx, rec, LKF_MODE);

    CHECK(!JS_IsException(o), "§4.5's LockInfo could not be allocated");
    JS_DefinePropertyValueStr(ctx, o, "name", lk_get(ctx, rec, LKF_NAME), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, o, "mode", JS_NewString(ctx, LK_MODE_VALUES[mode]), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, o, "clientId", lk_get(ctx, rec, LKF_CLIENT), JS_PROP_C_W_E);
    return o;
}

static int lk_query_step(JSContext *ctx, JSStepHdr *hdr, void *st, int argc, JSValueConst *argv,
                         JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    LkReqState *s = st;
    JSValueConst manager;
    JSValue snapshot, held_out, pending_out, held, queues;
    uint32_t n, i;
    int rejected;

    (void)argc; (void)argv; (void)out_cb; (void)out_argc;
    DCHECK(hdr->stage == LKQ_RUN, "§3.2.2's query resumed into a stage it does not have");
    /* STATED IN ITS OWN TERMS AND NO LONGER BY DEFERRAL. It used to read `see §3.2.1's assert for why its one
       stage cannot be resumed into`, and §3.2.1's assert now admits a fork's re-entry — so a deferral would
       have inherited a reason that has stopped holding where it was written while still holding here. §3.2.2
       declares no `signal` and asks nothing that can fork, so `started` alone is the whole of it. */
    DCHECK(!s->started, "§3.2.2's query was re-entered after it had begun — its one stage makes no request and "
                        "§3.2.2 declares no member this engine can fork over, so it has no re-entry at all");
    JS_FreeValue(ctx, cb_result);
    manager = lk_prologue(ctx, s, hdr->this_val, "query", presult, &rejected);
    if (rejected)
        return lk_reject(ctx, s, presult);
    /* §4.5, in its own order: "Let pending be a new list. For each queue of manager's lock request queue map's
       values: For each request of queue: Append … to pending. Let held be a new list. For each lock of
       manager's held lock set: Append … to held." The ORDER of the two lists is a real guarantee for one
       resource and none across resources, which §4.5's own closing note says — so the queue map is walked in
       its own key order and each queue in its own. */
    pending_out = JS_NewArray(ctx);
    held_out = JS_NewArray(ctx);
    CHECK(!JS_IsException(pending_out) && !JS_IsException(held_out),
          "§4.5's two snapshot lists could not be allocated");
    queues = lk_get(ctx, manager, LKF_QUEUES);
    {
        JSPropertyEnum *tab = NULL;
        uint32_t nk = 0, k;
        int r = JS_GetOwnPropertyNames(ctx, &tab, &nk, queues, JS_GPN_STRING_MASK);

        CHECK(r == 0, "§4.5's walk of the lock request queue map could not enumerate it");
        for (k = 0; k < nk; k++) {
            JSValue q = JS_GetProperty(ctx, queues, tab[k].atom);
            uint32_t nq = JS_IsArray(q) ? lk_len(ctx, q) : 0, j;

            for (j = 0; j < nq; j++) {
                JSValue req = JS_GetPropertyUint32(ctx, q, j);

                JS_SetPropertyUint32(ctx, pending_out, lk_len(ctx, pending_out), lk_info(ctx, req));
                JS_FreeValue(ctx, req);
            }
            JS_FreeValue(ctx, q);
        }
        JS_FreePropertyEnum(ctx, tab, nk);
    }
    JS_FreeValue(ctx, queues);
    held = lk_get(ctx, manager, LKF_HELD);
    n = lk_len(ctx, held);
    for (i = 0; i < n; i++) {
        JSValue lock = JS_GetPropertyUint32(ctx, held, i);

        JS_SetPropertyUint32(ctx, held_out, lk_len(ctx, held_out), lk_info(ctx, lock));
        JS_FreeValue(ctx, lock);
    }
    JS_FreeValue(ctx, held);
    /* "Resolve promise with «[ 'held' → held, 'pending' → pending ]»" — the `LockManagerSnapshot` dictionary as
       an Object, so its two members are DEFINED. */
    snapshot = JS_NewObject(ctx);
    CHECK(!JS_IsException(snapshot), "§4.5's LockManagerSnapshot could not be allocated");
    JS_DefinePropertyValueStr(ctx, snapshot, "held", held_out, JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, snapshot, "pending", pending_out, JS_PROP_C_W_E);
    /* THE SETTLE IS A JOB, as every host settle in this engine is: a resolving function runs on a flow base. */
    {
        JSValueConst args[1] = { snapshot };

        JS_EnqueueCallJob(ctx, s->funcs[0], 1, args);
    }
    JS_FreeValue(ctx, snapshot);
    *presult = s->promise;
    s->promise = JS_UNDEFINED;
    return JS_STEP_DONE;
}

static const IdlStepDecl LK_QUERY_DECL = {
    lk_query_step, sizeof(LkReqState), lk_req_visit, NULL,
    "Web Locks API §3.2.2 The query() method", LK_QUERY_STEPS, 1
};

/* ---- §3.3's Lock ------------------------------------------------------------------------------------------ */

static bool lk_lock_brand(JSContext *ctx, JSValueConst this_val, const char *member)
{
    DCHECK(g_lock_class != 0, "a Lock member ran before lock_manager_init declared the class");
    if (JS_GetClassID(this_val) == g_lock_class)
        return true;
    JS_ThrowTypeError(ctx, "Lock.%s was read on something that is not a Lock object", member);
    return false;
}

enum { LK_LOCK_NAME = 0, LK_LOCK_MODE };

/* §3.3: "The name getter's steps are to return the associated lock's name." / "The mode getter's steps are to
   return the associated lock's mode." Two slot reads, so a plain C getter: they reach none of the page's code
   and take no decision. */
static JSValue js_lock_get(JSContext *ctx, JSValueConst this_val, int magic)
{
    JSValue slots, out;

    if (!lk_lock_brand(ctx, this_val, magic == LK_LOCK_NAME ? "name" : "mode"))
        return JS_EXCEPTION;
    slots = lk_slots_of(ctx, this_val);   /* §3.3's "associated lock" */
    if (magic == LK_LOCK_NAME) {
        out = lk_get(ctx, slots, LKF_NAME);
    } else {
        LkMode mode = (LkMode)lk_get_int(ctx, slots, LKF_MODE);

        DCHECK(mode == LK_SHARED || mode == LK_EXCLUSIVE,
               "a lock's mode is neither of §2.3's two — §3.2.1's conversion admits only `LockMode`'s values");
        out = JS_NewString(ctx, LK_MODE_VALUES[mode]);
    }
    JS_FreeValue(ctx, slots);
    return out;
}

/* ---- the component, per realm ----------------------------------------------------------------------------- */

JSValue lock_manager_object(JSContext *ctx)
{
    return realm_value_get(ctx, g_obj_slot);   /* OWNED — realm_value_get asserts the realm ran its install */
}

static void lock_manager_install_realm(JSContext *ctx)
{
    JSValue proto, prev, global, obj, slots, id;
    uint8_t u[16];
    char uuid[37];
    JSAtom k;

    /* §3.3's INTERFACE FIRST, because §4.4 asks the realm for its prototype and a grant can be enqueued by the
       first request() this realm ever takes. */
    prev = JS_GetClassProto(ctx, g_lock_class);
    DCHECK(JS_IsNull(prev), "lock_manager_install_realm ran twice in one realm — everything already holding the "
                            "first Lock.prototype would answer out of a discarded object");
    JS_FreeValue(ctx, prev);
    proto = JS_NewObject(ctx);
    CHECK(!JS_IsException(proto), "Lock.prototype could not be allocated");
    idl_interface_tag(ctx, proto, "Lock");
    idl_install_accessor_exposed(ctx, proto, "name", js_lock_get, LK_LOCK_NAME, -1, IDL_SECURE_CONTEXT);
    idl_install_accessor_exposed(ctx, proto, "mode", js_lock_get, LK_LOCK_MODE, -1, IDL_SECURE_CONTEXT);
    JS_SetClassProto(ctx, g_lock_class, JS_DupValue(ctx, proto));
    global = JS_GetGlobalObject(ctx);
    /* §3.7.1's INTERFACE OBJECT. `[SecureContext]` is on the INTERFACE, so in a non-secure realm there is no
       `Lock` property at all — absent, which is what a page's `"Lock" in window` is written to discover, and
       not an object whose members throw. */
    idl_install_interface_object_exposed(ctx, global, "Lock", proto, IDL_SECURE_CONTEXT);
    JS_FreeValue(ctx, proto);

    /* §3.2's INTERFACE. */
    prev = JS_GetClassProto(ctx, g_manager_class);
    DCHECK(JS_IsNull(prev), "lock_manager_install_realm ran twice in one realm");
    JS_FreeValue(ctx, prev);
    proto = JS_NewObject(ctx);
    CHECK(!JS_IsException(proto), "LockManager.prototype could not be allocated");
    idl_interface_tag(ctx, proto, "LockManager");
    DCHECK(g_id_request >= 0 && g_id_query >= 0,
           "§3.2's members were installed on a realm's prototype before lock_manager_init declared them");
    idl_install_method_exposed(ctx, proto, "request", g_id_request, IDL_SECURE_CONTEXT);
    idl_install_method_exposed(ctx, proto, "query", g_id_query, IDL_SECURE_CONTEXT);
    JS_SetClassProto(ctx, g_manager_class, JS_DupValue(ctx, proto));
    idl_install_interface_object_exposed(ctx, global, "LockManager", proto, IDL_SECURE_CONTEXT);
    JS_FreeValue(ctx, global);

    /* §3.1's "Each environment settings object has a LockManager object", minted WITH the realm — so
       `navigator.locks === navigator.locks` holds by construction and no flow's own first read can become every
       sibling's baseline. §2.2's manager is NOT obtained here: core/platform.c declares the storage shed AFTER
       the Navigator this component is declared from, so the shed's own intrinsic has not run yet — it is
       obtained on the first request() or query(), which is where §3.2.1 step 4 and §3.2.2 step 3 ask for it. */
    obj = JS_NewObjectProtoClass(ctx, proto, g_manager_class);
    JS_FreeValue(ctx, proto);
    CHECK(!JS_IsException(obj), "the realm's LockManager object could not be allocated");
    slots = idl_slots_new(ctx);
    CHECK(!JS_IsException(slots), "the realm's LockManager internal-slot record could not be allocated");
    /* §3.2.2's clientId — "a unique context (frame or worker)". A version 4 UUID, whose shape is what §3.2.2's
       own example shows; see the residual below for what it is NOT.
       THE DRAW IS AT INSTALL, AND THAT CONSTRAINS THIS COMPONENT'S ROW IN core/platform.c. It is from THIS
       realm's §10.1 stream (core/crypto/crypto.h's entry), so core/crypto's own per-realm install must already
       have run — core/realm.h runs the installs in DECLARATION order, and this whole component is declared
       under `navigator`, so the `crypto` row sits BEFORE the `navigator` row and states this as its reason.
       It did not, and the read answered NULL in every realm including the agent's first.
       IT IS NOT DEFERRED THE WAY §2.2's LOCK MANAGER RECORD IS, and the difference is what each would DO
       rather than when it would do it. That deferral LOOKS UP a storage shed another intrinsic already built
       at the pre-boot baseline; a deferred clientId would be MINTED inside whichever flow asked first,
       captured into that flow's COW delta — so two arms of one fork would answer §3.2.2 with two clientIds
       for ONE context, which is the hazard §3.1's LockManager object is built with the realm to avoid.
       RETIREMENT: this record goes when core/realm.h can refuse a declaration order in which an install reads
       a per-realm value whose writer is declared later, because this row's position is then not a thing a
       reader of this file has to know. */
    crypto_random_bytes(ctx, u, sizeof u);
    u[6] = (uint8_t)((u[6] & 0x0f) | 0x40);
    u[8] = (uint8_t)((u[8] & 0x3f) | 0x80);
    snprintf(uuid, sizeof uuid,
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             u[0], u[1], u[2], u[3], u[4], u[5], u[6], u[7], u[8], u[9], u[10], u[11], u[12], u[13], u[14],
             u[15]);
    id = JS_NewString(ctx, uuid);
    CHECK(!JS_IsException(id), "the realm's Web Locks client id could not be allocated");
    JS_SetPropertyStr(ctx, slots, LKF_CLIENT, id);
    k = JS_ValueToAtom(ctx, g_lock_key);
    CHECK(k != JS_ATOM_NULL, "the LockManager internal-slot key could not be interned");
    JS_SetProperty(ctx, obj, k, slots);
    JS_FreeAtom(ctx, k);
    realm_value_set(ctx, g_obj_slot, obj);
}

void lock_manager_init(JSContext *ctx)
{
    JSRuntime *rt = JS_GetRuntime(ctx);
    JSClassDef mdef = { "LockManager" };
    JSClassDef ldef = { "Lock" };

    DCHECK(!g_ready, "lock_manager_init ran twice — the classes, the slot key, the slot and the members' pool "
                     "ids are the AGENT's");
    g_rt = rt;
    /* ONE KEY FOR BOTH KINDS OF RECORD. A LockManager's record and a Lock's are read off objects of two
       different classes, so one symbol cannot confuse them: a brand check has already decided which interface
       the receiver implements before the slot is read. */
    g_lock_key = JS_NewSymbol(ctx, "webLocksInternal", false);
    CHECK(!JS_IsException(g_lock_key), "the Web Locks slot key allocation failed");
    JS_NewClassID(rt, &g_manager_class);
    CHECK(JS_NewClass(rt, g_manager_class, &mdef) == 0,
          "LockManager: the per-realm prototype slot could not be declared");
    JS_NewClassID(rt, &g_lock_class);
    CHECK(JS_NewClass(rt, g_lock_class, &ldef) == 0,
          "Lock: the per-realm prototype slot could not be declared");
    g_obj_slot = realm_value_declare(ctx, "Web Locks API §3.1's per-environment LockManager object");
    /* WHAT THIS COMPONENT HOLDS FOR THE AGENT, DECLARED — core/agent_state.h. The row is `navigator` and not
       this file, for core/permissions/permissions.c's reason: §3.1's mixin is included into Navigator, this
       component is declared from navigator_init, and navigator_free is the release that reaches it — so the
       name is the row whose RELEASE undoes these slots, which is what core/platform.c's first walk checks. */
    agent_state_class("navigator", &g_manager_class,
                      "Web Locks API §3.2's LockManager class — the per-realm prototype slot and the brand");
    agent_state_class("navigator", &g_lock_class,
                      "Web Locks API §3.3's Lock class — the per-realm prototype slot and the brand");
    agent_state_realm_slot("navigator", &g_obj_slot,
                           "Web Locks API §3.1's per-environment LockManager object slot");
    agent_state_value("navigator", &g_lock_key,
                      "Web Locks API §3.2/§3.3's internal-slot key — a runtime-lifetime Symbol");
    g_grant_stepid = JS_RegisterStepDef(rt, &lk_grant_def);
    g_settled_stepid = JS_RegisterStepDef(rt, &lk_settled_def);
    g_abort_stepid = JS_RegisterStepDef(rt, &lk_abort_def);
    g_id_request = idl_method_id_step(ctx, LK_REQUEST_ARGS, 3, LK_OPTIONS, LK_OPTIONS_N, &LK_REQUEST_DECL, 0);
    /* §3.7.7's PROMISE RETURN TYPE, which is what turns the prologue's arity TypeError — and every refusal
       above — into a rejection in the one place that rule belongs. */
    idl_returns_promise();
    /* THE SHORTER ENTRY `(DOMString, LockGrantedCallback)` DECLARES NEITHER POSITION OPTIONAL, so `nargs` past
       its own end is this member's "there are none" at that arity. */
    idl_optional_from(2);
    /* …AND THE LONGER ENTRY `(DOMString, LockOptions, LockGrantedCallback)` DECLARES NONE EITHER, which is a
       different list of optionality values for the same declaration and is why §3.6 step 15.3 needs both.
       Without it `locks.request('n', {}, undefined)` would read position 2 as an ABSENT optional and reach the
       algorithm with no callback, where the surviving entry owes it Web IDL §3.2.19's TypeError. */
    idl_overload_split_optional_from(3);
    g_id_query = idl_method_id_step(ctx, NULL, 0, NULL, 0, &LK_QUERY_DECL, 0);
    idl_returns_promise();
    realm_declare_intrinsic(lock_manager_install_realm);
    g_ready = 1;
}

void lock_manager_free(void)
{
    if (!g_ready)
        return;
    DCHECK(g_rt != NULL, "Web Locks was declared without recording the runtime its symbol belongs to");
    /* The prototypes and interface objects are the REALMS' — each is released with its context. What the agent
       holds is the slot key, and a component that mints a runtime-lifetime value owns it. */
    JS_FreeValueRT(g_rt, g_lock_key);
    g_lock_key = JS_UNDEFINED;
    g_obj_slot = JS_INVALID_CLASS_ID;
    g_manager_class = 0;
    g_lock_class = 0;
    g_id_request = g_id_query = -1;
    g_grant_stepid = g_settled_stepid = g_abort_stepid = -1;
    g_ready = 0;
    g_rt = NULL;
}

/* ---- NAMED RESIDUALS ------------------------------------------------------------------------------------- */

/* §2.6 "Termination of Locks" IS NOT PERFORMED, and the code here is correct and narrower rather than
 * unfinished. §2.6 says "Whenever the unloading document cleanup steps run with a document, terminate remaining
 * locks and requests with its agent" and "When an agent terminates, terminate remaining locks and requests with
 * the agent", and to terminate is to abort every request and release every lock whose AGENT is that agent.
 *   WHAT IS NOT COVERED: a lock or a request outliving the document that made it. §2.4 and §2.5 both give their
 * struct an `agent`, and no record built above carries one, because every agent this engine has is this runtime
 * — so there is no second agent for a per-agent sweep to distinguish and nothing this component could key one
 * on. The manager lives on the storage bottle, so a document that goes away leaves its locks held.
 *   WHAT THE NEXT DIFF BUILDS: an `agent` on the lock and lock-request records and a sweep over both keyed on
 * it, called from the unloading document cleanup steps — which arrives with the second agent that can reach one
 * of these managers, since that is the first moment the field can hold two values.
 *   HOW ITS ABSENCE WOULD SHOW: a manager whose held lock set is non-empty for a resource name no live document
 * ever requested, so a later request() for that name waits for a callback that will never run — observable as a
 * request whose promise never settles while §3.2.2's snapshot reports the name under `held` with a clientId no
 * live LockManager answers. */

/* THE clientId IS A UUID THIS COMPONENT MINTS AND NOT A Client's `id`, which is correct and narrower than
 * §3.2.2's sentence "The clientId field corresponds to a unique context (frame or worker), and is the same
 * value returned by Client's id attribute".
 *   WHAT IS NOT COVERED: the SAME value being readable two ways. There is no Service Worker Client component in
 * this engine, so there is no `id` attribute for this string to equal; what it is, is unique per environment,
 * which is every property §4.5's snapshot and §2.5's grantable test rest on.
 *   WHAT THE NEXT DIFF BUILDS: one environment id, minted where the environment settings object is, that both
 * this component and a Client read — so the equality is a fact about one value rather than about two that
 * happen to look alike.
 *   HOW ITS ABSENCE WOULD SHOW: a page that reads a client id from both surfaces and compares them sees two
 * different UUIDs for one frame, so a snapshot's `clientId` cannot be matched against anything the page holds. */
