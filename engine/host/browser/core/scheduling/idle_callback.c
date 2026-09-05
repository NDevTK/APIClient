/* IDLE CALLBACKS — Cooperative Scheduling of Background Tasks §4 and §5. See idle_callback.h for the standard's
   identity, for why it has no committed index, for why the three concepts are heap objects, and for why §5 is a
   rung of the one event loop rather than a scheduler of its own. */
#include <stdbool.h>
#include <stdint.h>

#include "check.h"
#include "quickjs.h"
#include "core/agent_state.h"
#include "core/frame/navigable.h"
#include "core/frame/window_proxy.h"
#include "core/idl_args.h"
#include "core/realm.h"
#include "core/scheduling/idle_callback.h"
#include "core/scheduling/idle_deadline.h"
#include "solver/concolic.h"
#include "solver/engine.h"

/* §4's THREE ASSOCIATED CONCEPTS, as three fields of one per-realm record. The slot holds ONE object for the
   realm's whole life and is never replaced: what time-travels is that object's PROPERTIES, which the heap COW
   captures, and replacing the slot would put one flow's lists where every other flow looks. */
static int    g_slot = -1;
static JSAtom g_atom_pending = JS_ATOM_NULL;    /* §4's "list of idle request callbacks" */
static JSAtom g_atom_runnable = JS_ATOM_NULL;   /* §4's "list of runnable idle callbacks" */
static JSAtom g_atom_next = JS_ATOM_NULL;       /* §4's "idle callback identifier" */
static int    g_id_request = -1, g_id_cancel = -1;
static int    g_ready;

/* AN ENTRY IS [handle, callback] — §4's "each entry in this list is identified by a number". The handle rides
   the entry rather than being its POSITION, which is the whole of what makes cancellation and the two-list
   move sound: §5.1 step 4 appends every pending entry into the runnable list and §4.2 removes from the middle
   of either, so a position names a different entry after each of those and a handle names the same one for the
   lifetime of the Window, which is the standard's own guarantee. */
enum { IC_E_HANDLE = 0, IC_E_CALLBACK, IC_E_N };

/* ---- the per-realm record --------------------------------------------------------------------------------- */

static JSValue ic_store(JSContext *ctx)
{
    JSValue st;

    DCHECK(g_ready, "a Window's idle-callback concepts were reached before §4 was declared");
    st = realm_value_get(ctx, g_slot);
    DCHECK(JS_IsObject(st),
           "a realm answered §4's three associated concepts with no record — every Window is given one at "
           "creation, so this realm never ran idle_callback_install_store and its `requestIdleCallback` would "
           "register into nothing");
    return st;
}

/* One of §4's two lists, OWNED. `which` is g_atom_pending or g_atom_runnable. */
static JSValue ic_list(JSContext *ctx, JSAtom which)
{
    JSValue st = ic_store(ctx), l = JS_GetProperty(ctx, st, which);

    JS_FreeValue(ctx, st);
    DCHECK(JS_IsArray(l), "§4's list of idle callbacks is not a list — both are built at the realm install and "
                          "neither is ever replaced");
    return l;
}

static uint32_t ic_len(JSContext *ctx, JSValueConst l)
{
    JSValue len = JS_GetPropertyStr(ctx, l, "length");
    uint32_t n = 0;

    JS_ToUint32(ctx, &n, len);
    JS_FreeValue(ctx, len);
    return n;
}

static void ic_push(JSContext *ctx, JSValueConst l, JSValue entry)
{
    JS_SetPropertyUint32(ctx, (JSValue)l, ic_len(ctx, l), entry);
}

/* REMOVE THE ENTRY AT `at`, closing the gap. A real removal and NOT a tombstone, which is the difference
   between this list and HTML §8.12 Animation frames's map: that algorithm takes a KEY SNAPSHOT and re-checks
   each key against the live map, so compacting under it would renumber what the run is walking, while §5.2
   pops the top one entry at a time and snapshots nothing. Nothing here holds an index across a call. */
static void ic_remove_at(JSContext *ctx, JSValueConst l, uint32_t at)
{
    uint32_t n = ic_len(ctx, l), i;

    DCHECK(at < n, "an idle-callback list was asked to remove an entry past its end");
    for (i = at + 1; i < n; i++)
        JS_SetPropertyUint32(ctx, (JSValue)l, i - 1, JS_GetPropertyUint32(ctx, (JSValue)l, i));
    JS_SetPropertyStr(ctx, (JSValue)l, "length", JS_NewUint32(ctx, n - 1));
}

/* THE INDEX OF THE ENTRY `handle` NAMES IN `l`, or -1 — §4.2 step 2's "find the entry … that is associated
   with the value handle". By the handle the entry CARRIES, never by rank. */
static int ic_index_of(JSContext *ctx, JSValueConst l, uint32_t handle)
{
    uint32_t i, n = ic_len(ctx, l);

    for (i = 0; i < n; i++) {
        JSValue e = JS_GetPropertyUint32(ctx, (JSValue)l, i), h;
        uint32_t got = 0;

        DCHECK(JS_IsArray(e), "§4's list held something that is not an entry — every entry is written by "
                              "js_request_idle_callback and by §5.1's move, and both write the pair");
        h = JS_GetPropertyUint32(ctx, e, IC_E_HANDLE);
        JS_ToUint32(ctx, &got, h);
        JS_FreeValue(ctx, h);
        JS_FreeValue(ctx, e);
        if (got == handle) return (int)i;
    }
    return -1;
}

/* ---- §4.1 requestIdleCallback ------------------------------------------------------------------------------ */

/* Cooperative Scheduling of Background Tasks §4.1 The requestIdleCallback() method — SIX top-level steps, of
 * which step 6 holds one nested list of four:
 *   1. Let window be this Window object.
 *   2. Increment the window's idle callback identifier by one.
 *   3. Let handle be the current value of window's idle callback identifier.
 *   4. Push callback to the end of window's list of idle request callbacks, associated with handle.
 *   5. Return handle and then continue running this algorithm asynchronously.
 *   6. If the `timeout` property is present in options and has a positive value: whose one nested list is four
 *      substeps — wait for that many milliseconds; wait for every earlier-posted timeout of this algorithm;
 *      optionally pad further; and queue a task on the idle-task task source performing the invoke idle
 *      callback timeout algorithm.
 *
 * STEPS 1-5 ARE HERE IN FULL. Step 6 is the NAMED RESIDUAL below.
 *
 * NO PAGE CODE RUNS IN THIS BODY: the callback's Web IDL §3.2.19 brand check and the dictionary conversion are
 * the DECLARATION's (IDL_CALLBACK and IDL_DICT), so by the time this runs there is nothing left to coerce and
 * this is a plain body rather than a step machine. */
static JSValue js_request_idle_callback(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
                                        int magic)
{
    JSValue st, list, entry, nv;
    uint32_t handle = 0;

    (void)this_val; (void)magic;
    DCHECK(argc >= 1,
           "requestIdleCallback reached its body with no callback — Web IDL §3.6 step 5 throws for a call short "
           "of a member's REQUIRED arguments, and that throw is the declaration's");
    DCHECK(JS_IsFunction(ctx, argv[0]),
           "requestIdleCallback's callback reached the body uncoerced — Web IDL §3.2.19's brand check belongs "
           "to the declaration (IDL_CALLBACK), and a body that re-tests it is a second answer to one question");

    /* STEP 2 then STEP 3, in that order and not the other, because §4 makes the identifier "a number which
       MUST initially be zero" — so the first handle a page ever gets is 1 and `cancelIdleCallback(0)` names
       nothing. The counter is an ordinary property write, so the per-flow COW delta captures it: two arms of a
       fork mint the SAME handle for the same source line, which is what makes a replayed decision vector name
       the same callback. */
    st = ic_store(ctx);
    nv = JS_GetProperty(ctx, st, g_atom_next);
    JS_ToUint32(ctx, &handle, nv);
    JS_FreeValue(ctx, nv);
    handle += 1;
    JS_SetProperty(ctx, st, g_atom_next, JS_NewUint32(ctx, handle));
    JS_FreeValue(ctx, st);
    DCHECK(handle >= 1, "§4's idle callback identifier answered below 1 after being incremented — 0 is the "
                        "handle a page gets back for nothing and must name no entry");

    /* STEP 4 — "Push callback to the end of window's list of idle request callbacks, associated with handle." */
    entry = JS_NewArray(ctx);
    CHECK(!JS_IsException(entry),
          "idle callbacks: OOM recording a requestIdleCallback callback — a dropped one is work the page asked "
          "for and never gets, which is invisible from outside");
    JS_SetPropertyUint32(ctx, entry, IC_E_HANDLE, JS_NewUint32(ctx, handle));
    JS_SetPropertyUint32(ctx, entry, IC_E_CALLBACK, JS_DupValue(ctx, argv[0]));
    list = ic_list(ctx, g_atom_pending);
    ic_push(ctx, list, entry);
    JS_FreeValue(ctx, list);

    /* STEP 6's `timeout` IS READ so that the declaration's conversion is not decoration — a dictionary member
       left undeclared is one Web IDL never places, and `{timeout: {valueOf(){...}}}` runs the page's code
       during that conversion whether or not this build schedules anything from the result.
       NAMED RESIDUAL — §4.1 STEP 6 IS NOT SCHEDULED, AND THE CODE HERE IS NARROWER RATHER THAN WRONG.
       WHAT IS NOT COVERED: step 6's four substeps — the wait, the ordering against earlier-posted timeouts,
       the optional padding, and the last, which queues a task on the idle-task task source performing §5.3's
       invoke idle callback timeout algorithm. With them absent nothing ever sets §4.3's `timeout`, so `deadline.didTimeout` is
       false for every callback this engine invokes.
       WHY IT IS NOT WRONG HERE: step 6 exists to force a callback out when idleness never arrives, and in this
       engine idleness always arrives — the rung idle_callback_run sits on is reached whenever the running flow
       has nothing else to do, and the frontier is never empty, so every registered callback is invoked
       regardless of its timeout. Dropping the callback would be a cap; running it late is what a user agent
       that is generously idle does, and this one is.
       WHAT THE NEXT DIFF BUILDS: an EXPIRY on the entry (the same event-loop moment core/timing/event_loop.h
       hands §8.7 Timers, so the two clock-driven sources compare on one clock), plus §5.3 as a second arm of
       idle_callback_run that removes the entry from BOTH lists and invokes it with an IdleDeadline whose
       deadline is `now` and whose `timeout` is true — idle_deadline.c already holds both fields, so the arm
       needs a mint that sets them rather than a new record.
       HOW ITS ABSENCE WOULD SHOW: a page that writes `requestIdleCallback(f, {timeout: 50})` and branches on
       `deadline.didTimeout` takes only the false arm, for ever; with step 6 built, the true arm is reachable
       and the callback runs at the expiry rather than at the next idle rung. */
    if (argc >= 2 && JS_IsObject(argv[1])) {
        JSValue timeout = idl_dict_get(ctx, argv[1], "timeout");

        JS_FreeValue(ctx, timeout);
    }
    return JS_NewUint32(ctx, handle);   /* STEP 5 */
}

/* ---- §4.2 cancelIdleCallback ------------------------------------------------------------------------------- */

/* Cooperative Scheduling of Background Tasks §4.2 The cancelIdleCallback() method — THREE top-level steps, no
 * nesting:
 *   1. Let window be this Window object.
 *   2. Find the entry in either the window's list of idle request callbacks or list of runnable idle callbacks
 *      that is associated with the value handle.
 *   3. If there is such an entry, remove it from both window's list of idle request callbacks and the list of
 *      runnable idle callbacks.
 * A handle naming no entry does nothing, which is why there is no throw here: a page cancelling a callback that
 * has already run is ordinary code, and §4.2's own note says the entry may be in either list. */
static JSValue js_cancel_idle_callback(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
                                       int magic)
{
    JSValue pending, runnable;
    uint32_t want = 0;
    int at;

    (void)this_val; (void)magic;
    DCHECK(argc >= 1, "cancelIdleCallback reached its body with no handle — Web IDL §3.6 step 5's TypeError is "
                      "the declaration's, not this body's");
    /* AN UNKNOWN HANDLE IS A FORK THIS MEMBER CANNOT YET ASK, so it crashes here rather than reaching past it.
       Web IDL §3.2's conversion is a BOUNDARY unknown external input crosses AS ITSELF (core/idl_args.h's
       idl_concolic_rule answers IDL_CONCOLIC_CROSSES for IDL_UNSIGNED_LONG), so
       `cancelIdleCallback(location.hash.length)` reaches this body still holding the unknown, and both the tag
       assert below and the JS_ToUint32 under it are FALSE for it — the coercion aborting inside ToNumber, one
       frame below this file, where there is no return to check. */
    if (concolic_is(argv[0]))
        DFAIL("cancelIdleCallback's `handle` is UNKNOWN EXTERNAL INPUT and this member has no fork to ask over "
              "it. §4.2 step 2 finds the entry associated with `handle` in either of this Window's two lists, "
              "whose keys are the handles §4.1 minted, so the arm set is GIVEN — one world per handle the two "
              "lists hold, plus one remainder that does nothing — and it is a set of platform-assigned NAMES "
              "rather than a range of positions, so core/idl_index_arg.h's ascending `index == k` chain does "
              "NOT serve it: the lists are sparse (every invocation and every cancel removes a key) and the "
              "identifier only grows, so that chain would ask about handles naming nothing. The LINK such a "
              "chain is built out of is written and shared — core/idl_name_chain.h, which composes the "
              "constraint key from the member's own NAME and refuses a truncated one — and "
              "core/timing/timer.c's js_clear_timer already builds §8.7 Timers's chain out of it, keyed \"is "
              "`id` the timer with identifier H\" one monotone identifier at a time; §4's handles are monotone "
              "and start at 1 exactly as §8.7's do (see js_request_idle_callback). WHAT IS MISSING IS THIS "
              "MEMBER'S OWN HALF: make it an IdlStepBody (idl_method_id_step, so it can park at a link) "
              "holding an IdlNameChainKey and a cursor, and enumerate THIS Window's two lists — the "
              "enumeration is deliberately the caller's, because the askers of that link enumerate different "
              "sets. HTML §8.12's cancelAnimationFrame carries the identical crash "
              "(core/rendering/animation_frame.c) and the two are one absence at two members");
    DCHECK(JS_VALUE_GET_TAG(argv[0]) == JS_TAG_INT || JS_TAG_IS_FLOAT64(JS_VALUE_GET_TAG(argv[0])),
           "cancelIdleCallback's `handle` reached the body neither converted nor unknown — the IDL declaration "
           "is what converts an `unsigned long`, and the one value it does NOT produce a Number for is unknown "
           "external input, which the ask above answers");
    JS_ToUint32(ctx, &want, argv[0]);

    /* STEPS 2 AND 3 TOGETHER, over both lists, because §4.2 step 3 says "remove it from BOTH". An entry can
       only ever be in one of them — §5.1 step 5 clears the pending list as it appends — so the second removal
       is the standard's belt-and-braces and is written as the standard writes it rather than as an else. */
    pending = ic_list(ctx, g_atom_pending);
    runnable = ic_list(ctx, g_atom_runnable);
    if ((at = ic_index_of(ctx, pending, want)) >= 0)
        ic_remove_at(ctx, pending, (uint32_t)at);
    if ((at = ic_index_of(ctx, runnable, want)) >= 0)
        ic_remove_at(ctx, runnable, (uint32_t)at);
    JS_FreeValue(ctx, pending);
    JS_FreeValue(ctx, runnable);
    return JS_UNDEFINED;
}

/* ---- §5 Processing ----------------------------------------------------------------------------------------- */

/* §5.1 START AN IDLE PERIOD, for ONE Window — SIX top-level steps:
 *   1. Optionally, if the user agent determines the idle period should be delayed, return from this algorithm.
 *   2. Let pending_list be window's list of idle request callbacks.
 *   3. Let run_list be window's list of runnable idle callbacks.
 *   4. Append all entries from pending_list into run_list preserving order.
 *   5. Clear pending_list.
 *   6. Queue a task on the queue associated with the idle-task task source, which performs the steps defined
 *      in the invoke idle callbacks algorithm with window and getDeadline as parameters.
 *
 * STEP 1 IS DECLINED, AND DECLINING IT IS A CHOICE THIS ENGINE IS ENTITLED TO MAKE. The standard's own note
 * says the delay is for power ("if the Document's visibility state is 'hidden' then the user agent can
 * throttle idle period generation"), which is a property of a display this engine does not have; and a
 * throttle here would be a bound on when work happens rather than on how it is ordered.
 * STEP 6's QUEUE IS THE RUNG ITSELF. Answering 1 hands the flow back to the scheduler, which reaches this same
 * rung again when it next has nothing else to do, and finds the run list non-empty. Returning 1 here is
 * PROGRESS and cannot repeat: step 5 empties the pending list, so this arm is unreachable until a page
 * requests again. */
static int idle_start_period(JSContext *ctx)
{
    JSValue pending = ic_list(ctx, g_atom_pending), run = ic_list(ctx, g_atom_runnable);
    uint32_t i, n = ic_len(ctx, pending);

    DCHECK(n > 0, "§5.1 was run for a Window with no pending idle request callbacks — the rung asks first and "
                  "a period started over an empty list queues a task with nothing to do");
    for (i = 0; i < n; i++)                                       /* step 4, preserving order */
        ic_push(ctx, run, JS_GetPropertyUint32(ctx, pending, i));
    JS_SetPropertyStr(ctx, pending, "length", JS_NewUint32(ctx, 0));   /* step 5 */
    /* THE RUNG ANSWERS 1, SO IT MUST HAVE CONSUMED SOMETHING. A source that reports progress it did not make
       is a live-lock the scheduler cannot tell from progress — core/dom/document.c's lifecycle step says so in
       those words — and this rung is asked whenever the running flow has nothing else to do, so a repeat costs
       every other member of the frontier its turn. Step 5 emptying the pending list IS this arm's progress:
       until a page requests again, idle_step_window cannot reach it a second time. */
    DCHECK(ic_len(ctx, pending) == 0,
           "§5.1 step 5 left entries in the list of idle request callbacks — the arm answers 1, and an arm that "
           "reports progress without emptying that list is asked again at the very next dispatch with the same "
           "state in front of it");
    JS_FreeValue(ctx, pending);
    JS_FreeValue(ctx, run);
    return 1;
}

/* §5.2 INVOKE IDLE CALLBACKS, for ONE Window — THREE top-level steps, of which step 3 holds one nested list of
 * four:
 *   1. If the user-agent believes it should end the idle period early due to newly scheduled high-priority
 *      work, return from the algorithm.
 *   2. Let now be the current time.
 *   3. If now is less than the result of calling getDeadline and the window's list of runnable idle callbacks
 *      is not empty: 3.1 pop the top callback; 3.2 let deadlineArg be a new IdleDeadline whose get deadline
 *      time algorithm is getDeadline; 3.3 invoke callback with « deadlineArg » and "report"; 3.4 if the list is
 *      not empty, queue a task performing this algorithm again and return.
 *
 * STEP 1 IS DECLINED for the same reason §5.1's step 1 is: this engine never has "newly scheduled
 * high-priority work" that a period is in the way of, because the rung is only reached when the running flow
 * has nothing else to do at all, and anything that becomes runnable outranks this rung on the next dispatch
 * rather than needing the period cut short.
 *
 * STEPS 2 AND 3's GUARD ARE THE EVENT LOOP'S QUESTION AND NOT THE PAGE'S, WHICH IS WHY THEY DO NOT FORK.
 * idle_deadline.c holds the deadline as an UNKNOWN, because how much time a callback has is a number this
 * engine cannot state honestly — and there are TWO QUESTIONS asked of that one fact. The PAGE asks "how much
 * time do I have", which is unknowable and forks (timeRemaining). The EVENT LOOP asks "is this idle period
 * still open", which is a question about this engine's own bookkeeping and which it ANSWERS: §5.1 opened the
 * period, nothing since has closed it (step 1 above is declined), and the period ends when the run list
 * empties. Forking the loop's question instead would mint an arm per visit that pops nothing and leaves the
 * list unchanged — worlds no observation separates, asked again at every dispatch, for ever.
 *
 * STEP 3.4's RE-QUEUE IS THE RUNG AGAIN, exactly as §5.1 step 6's queue is: one callback per visit, and the
 * next visit finds the rest. That is also what makes it preemptible — the enqueued callback is a flow, and the
 * rung is not re-entered until the scheduler has nothing else to run, so a callback that parks does not hold
 * the period open against anything. */
static int idle_invoke_one(JSContext *ctx)
{
    JSValue run = ic_list(ctx, g_atom_runnable), entry, cb, deadline;
    uint32_t n_before = ic_len(ctx, run);

    DCHECK(n_before > 0,
           "§5.2 was run for a Window whose list of runnable idle callbacks is empty — step 3's guard is asked "
           "by the rung above, so reaching here means the two disagree about one list");
    entry = JS_GetPropertyUint32(ctx, run, 0);                    /* step 3.1: "pop the top callback" */
    DCHECK(JS_IsArray(entry), "§4's list of runnable idle callbacks held something that is not an entry");
    cb = JS_GetPropertyUint32(ctx, entry, IC_E_CALLBACK);
    DCHECK(JS_IsFunction(ctx, cb),
           "§4's list held an entry whose callback is not callable — the IDL's IdleRequestCallback brand check "
           "is what puts one there, so nothing else can have");
    /* POPPED BEFORE THE INVOKE, which is what makes a callback that re-requests get a NEW handle rather than
       resurrecting the one being run, and what makes §4.2 from inside a callback name the remaining entries
       rather than this one. */
    ic_remove_at(ctx, run, 0);
    /* AND THIS ARM'S PROGRESS IS THE POP, for the reason step 5's is the clear — see idle_start_period. It is
       what makes a callback that re-requests from inside itself ORDINARY rather than a live-lock: the entry it
       adds goes to the PENDING list (§4.1 step 4), this entry is already gone, and the next visit finds a
       shorter run list. A self-rescheduling idle callback then runs once per turn of the loop, for ever, which
       is what the page asked for and is the same thing HTML §8.12's map does for a self-rescheduling
       animation callback. */
    DCHECK(ic_len(ctx, run) == n_before - 1,
           "§5.2 step 3.1's pop did not shorten the list of runnable idle callbacks — the arm answers 1, so an "
           "arm that leaves the list as it found it is re-entered with the same top entry at every dispatch");
    JS_FreeValue(ctx, entry);
    JS_FreeValue(ctx, run);

    deadline = idle_deadline_new_for_idle_period(ctx);            /* step 3.2 */
    CHECK(!JS_IsException(deadline),
          "idle callbacks: §5.2 step 3.2's IdleDeadline could not be allocated — a dropped one is a callback "
          "the page asked for and never gets");
    /* STEP 3.3 — "[=Invoke=] callback with « deadlineArg » and \"report\"". THROUGH THE ONE TASK DOOR: this is
       the same JS_EnqueueCallTask every timer expiry goes through, so the callback runs as a call-root flow in
       the one WFQ — preemptible per opcode, forkable, parkable to the cold tier at any depth — and an
       exception inside it is REPORTED rather than propagated, which is what "report" means. Calling it here
       instead would be a C activation hosting the page's loops, which is the drive-to-completion this engine
       aborts on. */
    {
        JSValueConst args[1] = { deadline };

        JS_EnqueueCallTask(ctx, cb, 1, args);
    }
    JS_FreeValue(ctx, deadline);
    JS_FreeValue(ctx, cb);
    return 1;
}

/* ONE Window's idle step: start a period if one is owed, otherwise run one callback of the open period.
   RUNNABLE FIRST, because a period that is open must drain before another is started — §5.1 step 4 appends
   into the run list, so starting one over a non-empty run list would interleave two periods' entries and
   §5.2's "top" would stop naming the oldest. */
static int idle_step_window(JSContext *ctx)
{
    JSValue run = ic_list(ctx, g_atom_runnable), pending;
    uint32_t n_run = ic_len(ctx, run), n_pending;

    JS_FreeValue(ctx, run);
    if (n_run > 0)
        return idle_invoke_one(ctx);
    pending = ic_list(ctx, g_atom_pending);
    n_pending = ic_len(ctx, pending);
    JS_FreeValue(ctx, pending);
    return n_pending > 0 ? idle_start_period(ctx) : 0;
}

int idle_callback_run(JSContext *ctx)
{
    JSValue all;
    uint32_t i, n;
    int did = 0;

    DCHECK(g_ready, "the event loop reached its idle rung in an agent that never declared §4");
    /* EVERY FULLY ACTIVE DOCUMENT OF THIS AGENT, in tree order — §4 gives the three concepts to a WINDOW, and
       one event loop serves every document of an agent, so a same-origin popup's idle callback is this loop's
       work as much as this document's. The walk is core/frame/navigable.c's because the tree is, and the realm
       a callback is queued on is the one whose lists hold it and not whichever realm happened to drive the
       loop — the same rule core/timing/timer.c's timer_earliest states for §8.7 Timers. */
    all = navigable_tree_order(ctx);
    n = ic_len(ctx, all);
    for (i = 0; i < n && !did; i++) {
        JSValue proxy = JS_GetPropertyUint32(ctx, all, i);
        JSContext *docctx = window_proxy_realm(ctx, proxy);

        DCHECK(docctx != NULL, "a navigable in this agent's tree order answered with no realm — the walk "
                               "reports only materialized ones, and a materialized navigable has a realm");
        did = idle_step_window(docctx);
        JS_FreeValue(ctx, proxy);
    }
    JS_FreeValue(ctx, all);
    return did;
}

/* ---- declaration, install, release -------------------------------------------------------------------------- */

void idle_callback_init(JSContext *ctx)
{
    /* §4's IDL: `unsigned long requestIdleCallback(IdleRequestCallback callback,
                                                    optional IdleRequestOptions options = {})`
                 `undefined cancelIdleCallback(unsigned long handle)`
                 `dictionary IdleRequestOptions { unsigned long timeout; }` — one member, NOT required and with
       no default, which is what makes §4.1 step 6's "if the timeout property is PRESENT" a question at all. */
    static const IdlDictMember OPTIONS[1] = {
        { "timeout", IDL_UNSIGNED_LONG, false, NULL, 0, NULL, IDL_DEFAULT_NONE, NULL },
    };
    static const IdlArgType REQUEST_ARGS[2] = { IDL_CALLBACK, IDL_DICT };
    static const IdlArgType CANCEL_ARGS[1] = { IDL_UNSIGNED_LONG };

    DCHECK(!g_ready, "idle_callback_init ran twice — §4's members are declared once per agent");
    idle_deadline_init(ctx);

    g_id_request = idl_method_id_dict(ctx, REQUEST_ARGS, 2, OPTIONS, 1, js_request_idle_callback, 0);
    idl_optional_from(1);                       /* `optional IdleRequestOptions options = {}` */
    g_id_cancel = idl_method_id(ctx, CANCEL_ARGS, 1, js_cancel_idle_callback, 0);

    g_atom_pending = JS_NewAtom(ctx, "idleRequestCallbacks");
    g_atom_runnable = JS_NewAtom(ctx, "runnableIdleCallbacks");
    g_atom_next = JS_NewAtom(ctx, "idleCallbackIdentifier");
    CHECK(g_atom_pending != JS_ATOM_NULL && g_atom_runnable != JS_ATOM_NULL && g_atom_next != JS_ATOM_NULL,
          "idle callbacks: §4's own field names could not be interned");
    g_slot = realm_value_declare(ctx, "§4's list of idle request callbacks, list of runnable idle callbacks "
                                      "and idle callback identifier");
    g_ready = 1;
    realm_declare_intrinsic(idle_callback_install_store);
    /* §5.1's OWN NOTE PUTS THIS ALGORITHM ON THE EVENT LOOP — "called by the event loop processing model when
       it determines that the event loop is otherwise idle" — so it is registered here, beside the timer and
       rendering steps, for the reason solver/engine.c gives for both: naming it there would make the scheduler
       depend on the browser half. */
    engine_set_idle_hook(idle_callback_run);

    agent_state_flag("idle_callback", &g_ready, "the declaration latch");
    agent_state_id("idle_callback", &g_id_request, "§4.1's requestIdleCallback declaration");
    agent_state_id("idle_callback", &g_id_cancel, "§4.2's cancelIdleCallback declaration");
    agent_state_atom("idle_callback", &g_atom_pending,
                     "§4's list-of-idle-request-callbacks key on a Window's record");
    agent_state_atom("idle_callback", &g_atom_runnable,
                     "§4's list-of-runnable-idle-callbacks key on that record");
    agent_state_atom("idle_callback", &g_atom_next, "§4's idle-callback-identifier key on that record");
    agent_state_id("idle_callback", &g_slot, "the per-realm slot §4's three concepts are held in");
}

/* THE THREE CONCEPTS ARE BUILT AT REALM INSTALL, which puts them in the pre-boot BASELINE. Built lazily on the
   first `requestIdleCallback` instead they would be whichever FLOW touched them first that owned them, and
   every sibling would then be registering into an object created inside another flow's delta. */
void idle_callback_install_store(JSContext *ctx)
{
    JSValue st, l;

    DCHECK(g_ready, "a realm asked for §4's three concepts before they were declared");
    /* Running twice in one realm is asserted by realm_value_set, which is where the first value is standing. */
    st = JS_NewObjectProto(ctx, JS_NULL);
    CHECK(!JS_IsException(st), "idle callbacks: this Window's record could not be allocated");
    l = JS_NewArray(ctx);
    CHECK(!JS_IsException(l), "idle callbacks: this Window's list of idle request callbacks could not be "
                              "allocated");
    JS_SetProperty(ctx, st, g_atom_pending, l);
    l = JS_NewArray(ctx);
    CHECK(!JS_IsException(l), "idle callbacks: this Window's list of runnable idle callbacks could not be "
                              "allocated");
    JS_SetProperty(ctx, st, g_atom_runnable, l);
    /* §4: "An idle callback identifier, which is a number which MUST initially be zero." */
    JS_SetProperty(ctx, st, g_atom_next, JS_NewUint32(ctx, 0));
    realm_value_set(ctx, g_slot, st);
}

void idle_callback_install(JSContext *ctx, JSValueConst global)
{
    DCHECK(g_ready, "§4's members were installed before they were declared");
    idl_install_method(ctx, (JSValue)global, "requestIdleCallback", g_id_request);
    idl_install_method(ctx, (JSValue)global, "cancelIdleCallback", g_id_cancel);
    idle_deadline_install(ctx, global);
}

/* THE RUNTIME, NOT A REALM, and it is core/platform.c's release column that calls it — see core/platform.h.
   IT RELEASES BOTH INTERFACES, because this row declares both: §4.3's own references go back through
   idle_deadline_free, and every HANDLE either file declared goes back through the one line at the end. */
void idle_callback_free(JSRuntime *rt)
{
    /* NOT `if (!g_ready) return;`. The release is the inverse of the DECLARATION and rides the same row of
       core/platform.c's one list, whose declare pass is unconditional — so reaching here undeclared is a host
       that tore this component down with something that is not the platform's one list. */
    DCHECK(g_ready, "§4 was released in an agent that never declared it");
    /* THE HOOK IS GIVEN BACK, because solver/engine.c asserts a single claimant and a second agent in one
       process would otherwise find the slot still held by a runtime that is gone. */
    engine_set_idle_hook(NULL);
    idle_deadline_free(rt);
    /* The RECORDS are the realms' — each is released with its context, which is what the per-realm slot array
       is for, and each flow's own entries go with the delta that holds them. What this owns is the three
       interned keys. */
    JS_FreeAtomRT(rt, g_atom_pending);
    JS_FreeAtomRT(rt, g_atom_runnable);
    JS_FreeAtomRT(rt, g_atom_next);
    /* EVERY HANDLE THIS ROW DECLARED, GIVEN BACK FROM THE ONE LIST THAT ALREADY NAMES THEM — this file's three
       interned keys, its realm slot, its two member declarations and its latch, AND idle_deadline.c's seven,
       which are declared under this row's name for the reason stated there.
       LAST, AND THAT ORDER IS THE CONTRACT (core/agent_state.h): the frees above and the assert at the top are
       reading slots this nulls, so a release that undid first would be answering its own checks. It is called
       BY this component rather than from core/platform.c's release column deliberately — a column that undid
       every component automatically would leave agent_state_check_released nothing to catch. */
    agent_state_undo("idle_callback");
}
