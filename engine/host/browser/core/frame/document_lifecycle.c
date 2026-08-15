/* HTML §7.5.9 unloading documents, §7.5.10 destroying documents, and the §7.3 close path — see
   document_lifecycle.h. */
#include <stdbool.h>

#include "check.h"
#include "quickjs.h"
#include "quickjs-step.h"
#include "core/dom/document.h"
#include "core/dom/page_visibility.h"
#include "core/events/before_unload_event.h"
#include "core/events/event.h"
#include "core/events/event_target.h"
#include "core/events/message_port.h"
#include "core/events/page_transition_event.h"
#include "core/frame/document_lifecycle.h"
#include "core/frame/session_history.h"
#include "core/frame/window_proxy.h"
#include "core/html/html_iframe.h"
#include "core/html/user_activation.h"
#include "core/timing/timer.h"

/* THE THREE PER-DOCUMENT OPERATIONS, and why they are ONE fan-out with three bodies.
 *
 * §7.5.9's "unload a document and its descendants", §7.5.10's "destroy a document and its descendants" and
 * §7.4.2.4's "check if unloading is canceled" are the SAME SHAPE: each queues a global task per navigable in a
 * subtree, waits until every one of them has finished, and then runs its own per-document body. Writing that
 * traversal three times would be three chances to get the wait wrong — and the wait is the part with a
 * termination argument behind it (see js_descend_step). So the traversal is one machine and the OPERATION is
 * its declaration: one `arg` per definition, which is also what lets each definition name its own section in
 * the stage labels a parked flow resumes by.
 *
 * §7.4.2.4's list is FLAT ("the active document of each item in navigablesThatNeedBeforeUnload", which
 * §7.3's definitely close passes as the traversable's INCLUSIVE DESCENDANT NAVIGABLES) where the other two
 * recurse — but the SET is the same set, and the standard fixes no order over it ("in what order?" is an open
 * issue on the unload fan-out itself). A post-order walk of the subtree visits exactly the inclusive
 * descendants, so the shared fan-out IS §7.4.2.4's loop, and its completion is §7.4.2.4's "wait for
 * completedTasks to be totalTasks". */
enum { LC_BEFOREUNLOAD, LC_UNLOAD, LC_DESTROY, LC_OP_N };

/* The operations index the WindowProxy's per-operation wait, so a fourth cannot be added without widening
   that record — C says so here rather than a DCHECK saying it at run time. */
typedef char lc_ops_fit_the_navigable_record[LC_OP_N <= WP_SUBTREE_OP_N ? 1 : -1];

/* WHAT HAPPENS WHEN THE WHOLE SUBTREE HAS FINISHED — §7.5.9's `afterAllUnloads` and §7.3's chaining of one
   operation into the next, as an ENUM rather than a callback because a work item's inputs must survive a park
   and a callback pointer does not. */
enum {
    LC_AFTER_NONE,
    /* §7.3 definitely close step 3, reached when §7.4.2.4 answered "continue" for the whole subtree. */
    LC_AFTER_UNLOAD,
    /* §7.3 definitely close step 3's afterAllUnloads: "an algorithm step which destroys traversable". */
    LC_AFTER_DESTROY_TRAVERSABLE
};

static void descend_enqueue(JSContext *ctx, JSValueConst proxy, int op, int after);
static void self_enqueue(JSContext *ctx, JSValueConst proxy, int op, int after);
static void destroy_a_top_level_traversable(JSContext *ctx, JSValueConst proxy);

/* THE OPERATION'S CONTINUATION, AND IT TRAVELS WITH THE JOB. §7.5.9's afterAllUnloads is an INPUT of "unload a
   document and its descendants", so it rides every job of that operation — down into each child, and back up
   in the report the last child makes — rather than being stored on the navigable and read back when the wait
   empties. A navigable holding it would answer for whichever operation touched it last, which is exactly the
   defect §scheduler names: an operation that becomes a work item takes its inputs with it. */
static int step_after(const JSStepHdr *h)
{
    JSValueConst v = step_arg(h, 1);

    DCHECK(JS_IsNumber(v), "a document-lifecycle job was queued without the continuation its operation carries");
    return JS_VALUE_GET_INT(v);
}

/* THE NAVIGABLE'S ACTIVE DOCUMENT'S REALM, or NULL when there is no document for a step to act on.
 *
 * AN UNMATERIALIZED NAVIGABLE ANSWERS NULL, and that is the computed answer rather than a skip: it holds the
 * initial about:blank Document §7.4 created it with, no script has ever run in it (script cannot run in a realm
 * that does not exist), so it has no event listener to fire at, its `page showing` is the false it was born
 * with, and it has no port, no queued task and no child navigable. Every step of §7.5.9 and §7.5.10 that acts
 * on a document is therefore empty for it. Asking for the realm anyway would BUILD one — materializing a
 * document for the sole purpose of tearing it down, which is navigable.h's deferral running backwards. */
static JSContext *active_realm(JSContext *ctx, JSValueConst proxy)
{
    return window_proxy_materialized(proxy) ? window_proxy_realm(ctx, proxy) : NULL;
}

/* §7.5.10's DESTROY A DOCUMENT — the eleven steps, over the state this engine has for them.
 *
 * STEP 2's ABORT A DOCUMENT (§7.5.11) CANCELS THE DOCUMENT'S FETCHES, AND THAT IS STEP 7's DROP. A fetch in
 * this engine is a JOB parked on a host-owed answer (navigable.c's §7.4 step 14 load), so "the fetches of this
 * document" and "the tasks of this document" are the same set and dropping the job IS what abandons the
 * request — one mechanism, asserted below to have emptied the queues rather than assumed to have. §7.5.11's
 * other half aborts an active parser, and a Document here is produced by a Lexbor parse that completes before
 * its realm is built (navigable.c's child_document), so there is no parser in flight to reach; a streaming
 * parser arrives with its own abort entry, as its own component.
 *
 * STEPS 10 AND 11 ITERATE SETS THAT ARE EMPTY BY CONSTRUCTION: this engine has no WorkerGlobalScope and no
 * worklet global scopes, so "for each" runs zero times. That is the correct implementation of those steps and
 * not an omission — a Worker subsystem arrives with its own owner set and its own entry here.
 *
 * STEP 9 IS SESSION HISTORY: "if document's node navigable is non-null, then set document's node navigable's
 * active session history entry's document state's document to null." §7.4.1's entries exist now
 * (core/frame/session_history.c), and this step is the half of them this file owes — but the field it nulls is
 * the document state's DOCUMENT, which is what a TRAVERSAL back to the entry would repopulate, and this build
 * performs no traversal. So the write has exactly one reader and that reader is unwritten, which is asserted at
 * the reader's own site (session_history.c's realm_awaits on PopStateEvent names the whole traversal) rather
 * than by nulling a field here that nothing consults. What step 8 records — the browsing context is null — is
 * the fact a page reads, through §7.2.5's `closed`. */
static void destroy_a_document(JSContext *ctx, JSValueConst proxy)
{
    JSContext *cctx = active_realm(ctx, proxy);

    if (cctx) {
        /* STEPS 4 AND 5 — the ports whose relevant global object's associated Document is this one, each
           disentangled. The SET is enumerable (message_port.c keeps the live ports and each one records its
           realm); the DISENTANGLE is not yet, and the difference is whose write it is. A disentangle is a
           write to the port record, so it belongs to the flow performing the destruction — and a list of
           borrowed pointers is agent-global, so it cannot tell this flow's port from a sibling arm's. Reaching
           into the wrong timeline would be a silent wrong answer; stopping here is a loud one. */
        DCHECK(message_port_count_in_realm(cctx) == 0,
               "a Document with live MessagePorts was destroyed — §7.5.10 steps 4-5 disentangle each of them. "
               "BUILD IT: the port list has to become PER FLOW before the disentangle can run, which is the "
               "same sentence as the port's own message queue being a JS Array — a list held as a JS value "
               "forks and parks with the flow that created the port, and then the walk is that flow's");
        /* STEP 7 — the tasks of this document, removed WITHOUT running. A task queued by a document that no
           longer exists must not run: it would script a destroyed Document, and every one of those is a
           use-after-destroy a page can trigger by removing a frame that queued work.
           THE SECOND CALL IS THE ASSERT, not a retry: a drop that left anything behind would leave exactly the
           task that runs against a destroyed document, which is silent until it is a crash somewhere else. */
        JS_DropJobsForContext(cctx);
        DCHECK(JS_DropJobsForContext(cctx) == 0,
               "§7.5.10 step 7 dropped a destroyed Document's queued tasks and there were still more — the "
               "queues this walks are not all of them, and whichever one it missed will run page code in a "
               "document whose browsing context is null");
    }
    /* STEP 8 — the browsing context is null, which is the half of §7.2.5's `closed` that destruction owns. */
    window_proxy_set_destroyed(ctx, proxy);
}

/* §7.5.9's SALVAGEABLE STATE, DECIDED HERE AND HELD NOWHERE.
 *
 * A Document's salvageable state starts true and step 8 sets it to false whenever `intendToKeepInBfcache` is
 * false — and intendToKeepInBfcache is "true if the user agent intends to keep oldDocument alive in a session
 * history entry, such that it can later be used for history traversal". This user agent holds session history
 * entries (core/frame/session_history.c) and NO BFCACHE to keep a document in: §7.4.1.2's document state may
 * have its document nulled out precisely so a later traversal rebuilds it, and this build performs no traversal
 * to rebuild one for (a parked FLOW resumes into the document it was suspended in, which is the opposite
 * arrangement: the snapshot carries the document rather than a cache holding it). So intendToKeepInBfcache is
 * FALSE for every document, and salvageable is FALSE by the time any step reads it.
 *
 * SO THERE IS NO FIELD. A `salvageable` byte on the Document would be written once and read three times with
 * the same answer every time, which is a field nothing can ever read differently — and its three readers say
 * something worth reading instead: `pagehide`'s `persisted` is FALSE (this document is not being kept), the
 * `unload` event DOES fire, and step 20 DOES destroy the document. The day a bfcache exists it is
 * intendToKeepInBfcache that has to be computed, and this is the one place that computes it. */
#define UNLOAD_SALVAGEABLE false

/* ---- §7.4.2.4's FIRE BEFOREUNLOAD, one document -------------------------------------------------------------
 *
 * The steps to fire beforeunload return (unloadPromptShown, unloadPromptCanceled), and everything between the
 * fire and that pair is ONE five-part conjunction guarding an unload PROMPT. Two of its conjuncts are facts
 * about the document and the agent rather than about the event, which is why before_unload_event.h answers only
 * the fourth. Of those two, the SANDBOXING one is decided here because a document's active sandboxing flag set
 * is state this engine's policy container does not carry; the STICKY ACTIVATION one is not decided here at all
 * — §6.4 is a component (user_activation.c) with per-Window state, and a conjunct that answered it locally
 * would be one algorithm's private copy of a fact §7.3.1.7's popup rules, §7.4.2.1's source snapshot params,
 * §7.1.5's sandboxing, §7.2.4's `location` history handling, §6.6.6's focus and §4.10.5.4's showPicker all read
 * as well — and each of those would then have grown its own copy of the same excuse. */

/* §7.4.2.4's second conjunct — "document's active sandboxing flag set does not have its sandboxed modals flag
   set". A document's sandboxing flags come from §7.6.2's navigable container through §7.2.6's policy container,
   and this engine's policy container carries a CSP and nothing else, so no document it builds has any flag set.
   The conjunct therefore HOLDS (nothing sandboxes modals) and it is evaluated rather than assumed away. */
static bool document_sandboxes_modals(JSContext *ctx)
{
    (void)ctx;
    return false;
}

#define BEFOREUNLOAD_STAGES(X) \
    X(BU_EVENT, "HTML §7.4.2.4 fire beforeunload steps 1-3 (the BeforeUnloadEvent, cancelable, whose " \
                "returnValue is the empty string)") \
    X(BU_FIRE,  "HTML §7.4.2.4 fire beforeunload steps 4-9 (fire it at the document's relevant global object " \
                "with the legacy target override flag, then the first two conjuncts of the unload prompt)") \
    X(BU_PROMPT, "HTML §7.4.2.4 step 6's third conjunct (the document's relevant global object has STICKY " \
                 "ACTIVATION — unknown external state, so this is where the flow forks and only one of the " \
                 "two worlds shows an unload prompt)")
enum { BEFOREUNLOAD_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const BEFOREUNLOAD_STEPS[] = { BEFOREUNLOAD_STAGES(JS_STEP_STAGE_LABEL) NULL };

typedef struct {
    JSStepHdr hdr;
    uint8_t   fphase;   /* the fire request's own phase */
    uint8_t   ua_phase; /* the sticky-activation question's own phase (core/html/user_activation.h) */
    JSValue   ev;       /* the BeforeUnloadEvent, minted once and read back after the dispatch (owned) */
    EventFireCb   cb;    /* the fire request's buffer — event_target_fire_run needs four slots */
} BeforeUnloadState;

/* ---- §7.5.9's UNLOAD A DOCUMENT, one document ----------------------------------------------------------------
 *
 * WITH NO newDocument. §7.5.9's optional second argument feeds exactly one thing — `unloadTimingInfo`, the
 * document unload timing info the NEXT document reports as its previous document unload timing — and steps 2-4
 * say that with no newDocument that record is null, which makes steps 10, 12 and 21 no-ops. The one reach that
 * exists here is §7.3's definitely close, which passes null explicitly ("Unload a document and its descendants
 * given traversable's active document, NULL, and afterAllUnloads"), so this is the algorithm for that input
 * rather than the algorithm with a step left out.
 *
 * STEPS 5-7 AND 14 AND 21 count the event loop's TERMINATION NESTING LEVEL and the Document's UNLOAD COUNTER up
 * and back down. Both exist "to ignore certain operations while the below algorithms run" and both are
 * WRITE-ONLY here: the unload counter's readers are §4.5's `document.open()` and `document.close()`, and the
 * termination nesting level's are the modal dialogs (`alert`, `confirm`, `prompt`) and `window.print` — and this
 * engine has none of those interfaces, so nothing can observe either counter. A counter no algorithm reads is a
 * field that cannot be read differently; the first of those algorithms to arrive brings the counter with it.
 *
 * STEPS 15-17 AND 19 ARE BFCACHE STATE. The suspension time and the suspended timer handles are read by ONE
 * algorithm — §7.4.6's "update document for history step application", which resumes a document that came back
 * OUT of bfcache and adds the suspended duration to each timer it finds — and "not restored reasons" is the
 * report for a document that FAILED to be kept. Neither can happen where salvageable is false and step 20
 * destroys the document. "Has been scrolled by the user" is the same shape: a flag whose only reader is a
 * later scroll restoration of this same document. */
#define UNLOAD_STAGES(X) \
    X(UNLOAD_ENTER,    "HTML §7.5.9 unload a document steps 1-9.1 (salvageable becomes false; if page showing " \
                       "is true, page showing becomes false)") \
    X(UNLOAD_PAGEHIDE, "HTML §7.5.9 unload a document step 9.2 (fire a page transition event named pagehide at " \
                       "the relevant global object with the document's salvageable state)") \
    X(UNLOAD_UNLOAD,   "HTML §7.5.9 unload a document steps 9.3-13 (update the visibility state to \"hidden\"; " \
                       "fire unload at the relevant global object with the legacy target override flag)") \
    X(UNLOAD_DESTROY,  "HTML §7.5.9 unload a document steps 18-20 (the unloading document cleanup steps, then " \
                       "destroy the document)")
enum { UNLOAD_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const UNLOAD_STEPS[] = { UNLOAD_STAGES(JS_STEP_STAGE_LABEL) NULL };

typedef struct {
    JSStepHdr hdr;
    uint8_t   fphase;
    /* WAS STEP 9's BRANCH TAKEN — step 9.3's visibility update is INSIDE "if oldDocument's page showing is
       true", and it is a stage away from the test, so the answer has to survive the suspension the pagehide
       fire is. A document whose page showing was already false fires nothing and stays visible until it is
       destroyed, which is what the standard says about a document a page never revealed. */
    uint8_t   showing;
    JSValue   ev;
    EventFireCb   cb;
} UnloadState;

/* ---- the shared subtree fan-out ------------------------------------------------------------------------------
 *
 * THE TWO QUEUED TASKS ARE TWO MACHINES, because each algorithm queues two and they run at different times.
 * §7.5.10 step 4 (and §7.5.9's step 4) queues one task PER CHILD; step 6 queues one for THIS document, and
 * step 5 says the second may not begin until every one of the first has finished.
 *
 * SO THE WAIT IS NOT A PARK, AND THAT IS NOT AN OPTIMISATION — IT IS THE ONLY THING THAT TERMINATES. A machine
 * that returned JS_STEP_YIELD until its children reported in would spin: YIELD is a PREEMPT POINT, re-entered
 * immediately when nothing outranks the flow, and the children's tasks are queued BEHIND the parent's job on
 * the same flow. The parent would re-check for ever and the children would never get to run. What the spec
 * actually describes is a COMPLETION: each child's task increments a count and the parent's task is the one
 * that runs when the count is full, so the LAST CHILD TO FINISH is what queues step 6 for its parent. That is
 * a notification going up, and it is why the count lives on the WindowProxy — the record that already holds
 * this navigable's per-flow state, so two forked arms unloading the same subtree count in their own worlds
 * instead of into each other's.
 *
 * THE COUNT IS PER OPERATION, and that is not symmetry for its own sake: a page's `unload` listener may remove
 * an `<iframe>`, which is §7.3.1's destroy-a-child-navigable, which starts a DESTROY over a subtree while an
 * UNLOAD is still counting children in the same tree. One count would have let that destroy's report empty the
 * unload's wait and run a parent's unload before its children had finished theirs.
 *
 * THE PARENT LINK IS THE ONE THE PROXY ALREADY HAS. The child does not have to be told who is waiting for it:
 * the navigable it is nested in is exactly that. It is asked for as window_proxy_parent_navigable and NOT as
 * §7.2.5's `parent`, which answers the proxy ITSELF at the top of the tree — a report delivered back to its
 * own sender. And a navigable that is not itself running this operation is not waiting, which is what makes
 * the report identify the ROOT of the operation rather than being a case the sender has to test for. */
/* ONE STAGE LIST, EXPANDED ONCE PER ALGORITHM with that algorithm's own step text — the three walks rest at
   the SAME point (one child queued, then the flow is offered up) and the standards number that point
   differently, which is exactly what the list's operands are for. A stage cannot move in one of them without
   moving in all three. */
#define DESCEND_STAGES(X, SECTION, WHAT) \
    X(DESCEND_TAKE, SECTION " (bind " WHAT ", then queue one task per child, one child per rest point)")
enum { DESCEND_STAGES(JS_STEP_STAGE_ENUM, "", "") };
static const char *const BU_DESCEND_STEPS[] = {
    DESCEND_STAGES(JS_STEP_STAGE_LABEL, "HTML §7.4.2.4 check if unloading is canceled steps 1-6",
                   "documentsToFireBeforeunload") NULL };
static const char *const UNLOAD_DESCEND_STEPS[] = {
    DESCEND_STAGES(JS_STEP_STAGE_LABEL, "HTML §7.5.9 unload a document and its descendants steps 2-5",
                   "childNavigables") NULL };
static const char *const DESTROY_DESCEND_STEPS[] = {
    DESCEND_STAGES(JS_STEP_STAGE_LABEL, "HTML §7.5.10 destroy a document and its descendants steps 2-5",
                   "childNavigables") NULL };

typedef struct {
    JSStepHdr hdr;
    /* THE CHILD NAVIGABLES, TAKEN ONCE — step 2 binds `childNavigables` before step 4 queues anything, and it
       has to: a walk re-taken at each step would be a different set each time step 5's "size" was read. It is
       an Array because a step machine's state is forked and parked with its flow, and a JS value is what both
       of those already carry. */
    JSValue  kids;
    uint32_t n;      /* childNavigables's size */
    uint32_t i;      /* step 4's cursor */
    /* HAS STEP 2 RUN — a flag rather than a test on `kids`, because a zeroed state's JSValue is the INTEGER 0
       and not JS_UNDEFINED (JS_TAG_INT is 0). A machine that decided "I have not taken the list yet" by asking
       `JS_IsUndefined(kids)` would answer NO on its very first entry and walk every subtree without ever
       descending into it. The flag is the only field that can be read before anything has written one. */
    uint8_t  took;
} DescendState;

/* A SUBTREE OPERATION HAS FINISHED FOR ONE NAVIGABLE — report it upwards, and run the operation's continuation
   when there is nothing above waiting for it. The three answers are the three cases and none of them is a
   fallback: LAST is step 5's wait being over, MORE is a sibling still outstanding, and NONE means no navigable
   above is running this operation, which makes this one the root it was started at. */
static void subtree_report(JSContext *ctx, JSValueConst proxy, int op, int after)
{
    JSValue parent = window_proxy_parent_navigable(ctx, proxy);
    int r = window_proxy_is(parent) ? window_proxy_child_wait_report(ctx, parent, op) : WP_WAIT_NONE;

    if (r == WP_WAIT_LAST) {
        self_enqueue(ctx, parent, op, after);   /* the last child of a waiting parent: its own body may run */
    } else if (r == WP_WAIT_NONE) {
        switch (after) {
        case LC_AFTER_NONE:
            break;
        case LC_AFTER_UNLOAD:
            /* §7.3 definitely close step 3, reached because §7.4.2.4 answered "continue" for every document in
               the subtree — which is the only answer it can give here; the one place that could change it is
               the prompt conjunction in js_beforeunload_step, and that is where the accumulator it would need
               is named. */
            descend_enqueue(ctx, proxy, LC_UNLOAD, LC_AFTER_DESTROY_TRAVERSABLE);
            break;
        case LC_AFTER_DESTROY_TRAVERSABLE:
            destroy_a_top_level_traversable(ctx, proxy);
            break;
        default:
            DFAIL("a subtree operation finished with a continuation §7.3 and §7.5.9 do not define");
        }
    }
    JS_FreeValue(ctx, parent);
}

static int js_descend_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    DescendState *s = st;
    JSValueConst proxy = step_arg(&s->hdr, 0);
    int op = s->hdr.arg, after = step_after(&s->hdr);
    JSContext *cctx;

    (void)out_cb; (void)out_argc;
    JS_FreeValue(ctx, cb_result);
    DCHECK(s->hdr.stage == DESCEND_TAKE, "a subtree fan-out resumed at a stage its algorithm does not have");
    DCHECK(window_proxy_is(proxy), "a subtree fan-out was given something that is not a WindowProxy");
    DCHECK(op >= 0 && op < LC_OP_N, "a subtree fan-out was declared with an operation this file does not name");

    if (!s->took) {
        int n = 0, k;

        s->took = 1;
        s->kids = JS_NewArray(ctx);
        CHECK(!JS_IsException(s->kids),
              "OOM taking a subtree operation's child navigables — a walk that loses half its subtree leaves "
              "live documents under a document that has finished");
        /* AN UNMATERIALIZED NAVIGABLE HAS NO CHILD NAVIGABLES — see active_realm. */
        if ((cctx = active_realm(ctx, proxy)) != NULL) {
            n = iframe_child_navigable_count(cctx);
            for (k = 0; k < n; k++)
                JS_SetPropertyUint32(ctx, s->kids, (uint32_t)k, iframe_child_navigable(cctx, k));
        }
        s->n = (uint32_t)n;
        s->i = 0;
        /* STEP 3's numberDestroyed (§7.4.2.4's totalTasks, §7.5.9's numberUnloaded), counted DOWN and recorded
           before a single child is queued — a child that finished while the count was still being raised would
           see it hit zero early and run its parent's body with siblings still live. */
        window_proxy_child_wait_set(ctx, proxy, op, s->n);
    }
    if (s->i < s->n) {
        JSValue kid = JS_GetPropertyUint32(ctx, s->kids, s->i++);   /* step 4, one child per rest point */

        /* A CHILD WITH NO ACTIVE DOCUMENT REPORTS AT ONCE INSTEAD OF BEING QUEUED, and that is not a shortcut —
           it is what keeps step 5 from waiting for ever. Queuing it would be queuing work that returns
           immediately without reporting (descend_enqueue's own early return), so the count would sit one short
           of empty and this navigable's own body would never run at all. It is reachable whenever a subtree is
           taken apart in pieces: a frame removed first, then the frame containing it. */
        if (window_proxy_destroyed(kid)) {
            if (window_proxy_child_wait_report(ctx, proxy, op) == WP_WAIT_LAST)
                self_enqueue(ctx, proxy, op, after);
        } else {
            descend_enqueue(ctx, kid, op, after);
        }
        JS_FreeValue(ctx, kid);
        return JS_STEP_YIELD;
    }
    /* WITH NO CHILDREN THERE IS NOTHING TO WAIT FOR, so this navigable's own body is queued here; with children,
       the last one to finish queues it. Both are step 5 — "wait until numberUnloaded equals size" is already
       true for a document that has no child navigables. */
    if (s->n == 0)
        self_enqueue(ctx, proxy, op, after);
    return JS_STEP_DONE;
}

static void js_descend_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    DescendState *s = st;

    if (s->took) v->val(ctx, &s->kids);   /* before step 2 the slot holds no value to visit — see `took` */
}

static JSValue js_descend_fini(JSContext *ctx, void *st, bool take_result)
{
    DescendState *s = st;

    (void)take_result;
    if (s->took) {
        JS_FreeValue(ctx, s->kids);
        s->took = 0;
    }
    return JS_UNDEFINED;
}

/* ---- the three per-document bodies ---------------------------------------------------------------------------
 *
 * Each is the task §7.4.2.4 step 6 / §7.5.9 step 5 / §7.5.10 step 6 queues for ONE document, and each ends in
 * the report that makes the wait above it terminate. */

static int js_beforeunload_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    BeforeUnloadState *s = st;
    JSValueConst proxy = step_arg(&s->hdr, 0);
    JSContext *cctx = active_realm(ctx, proxy);
    int after = step_after(&s->hdr), k;

    DCHECK(window_proxy_is(proxy), "§7.4.2.4's beforeunload task was given something that is not a WindowProxy");
    if (s->hdr.stage == BU_EVENT) {
        /* EVERY OWNED FIELD IS ON THE STATE BEFORE ANYTHING THAT CAN FAIL — the failure path tears the machine
           down through fini, which frees exactly what the state holds and nothing else. */
        s->ev = JS_UNDEFINED;
        STEP_CB_FOREACH(s->cb, k) s->cb[k] = JS_UNDEFINED;
        s->fphase = 0;
        s->ua_phase = 0;
        s->hdr.stage = BU_FIRE;
        JS_FreeValue(ctx, cb_result);
        cb_result = JS_UNDEFINED;
        /* A document nothing has ever run in has no listener for this event and no returnValue to set, so the
           whole of the steps to fire beforeunload is empty for it — see active_realm. */
        if (cctx)
            s->ev = before_unload_event_new(cctx);   /* steps 1-3, and §7.2.7.7's empty returnValue */
    }
    if (s->hdr.stage == BU_FIRE && cctx && !JS_IsUndefined(s->ev)) {
        JSValue g = JS_GetGlobalObject(cctx);
        int r;

        /* STEP 4: `beforeunload`, CANCELABLE, at the document's relevant global object — and with the LEGACY
           TARGET OVERRIDE FLAG, so the event a listener sees is targeted at the DOCUMENT. */
        r = event_target_fire_run(cctx, &s->fphase, STEP_CB(s->cb), g, s->ev, document_object(cctx), cb_result,
                                  NULL, out_cb, out_argc);
        JS_FreeValue(cctx, g);
        if (r > 0) return r;                          /* parked on the page's beforeunload listeners */
        cb_result = JS_UNDEFINED;                     /* the fire consumed it */
        /* STEP 6's CONJUNCTION — the unload prompt. Four of its five parts are decided here and the fifth is
           the user agent's own judgement ("showing an unload prompt is unlikely to be annoying, deceptive, or
           pointless"), which is not reached while an earlier part answers false.
           THE FIRST TWO ARE DECIDED HERE AND THE THIRD IS A QUESTION, which is why it is a stage below rather
           than a third `&&`. §6.4.1's sticky activation is a comparison against this Window's own last
           activation timestamp, and that timestamp is UNKNOWN EXTERNAL STATE for a Window this engine has
           dispatched no trusted input event into (core/html/user_activation.h): both answers are feasible, so
           the question FORKS and only one of the two worlds reaches the prompt. Written as one conjunction it
           would have been a C `if` over a value the solver had not decided — the arm-picking this whole
           mechanism exists to prevent — and the fire above would have been re-run on every re-entry, because a
           machine may have exactly one request outstanding and this stage already spends it on the dispatch. */
        if (before_unload_event_asks_to_cancel(cctx, s->ev)          /* the event's own two-part answer */
            && !document_sandboxes_modals(cctx))
            s->hdr.stage = BU_PROMPT;
    }
    if (s->hdr.stage == BU_PROMPT) {
        bool sticky = false;
        int r;

        DCHECK(cctx != NULL, "§7.4.2.4 step 6 resumed at the sticky-activation conjunct with no realm to ask "
                             "it of — the stage is only ever set inside the branch that has one");
        r = user_activation_sticky_run(cctx, &s->hdr, &s->ua_phase, &sticky);
        if (r) return r;
        if (sticky) {
            DFAIL("§7.4.2.4 reached the unload prompt — BUILD the two things it needs: an unload prompt (the "
                  "user agent asks the user to confirm and PAUSES, so it is a suspension of this flow and not "
                  "a call), and the accumulator its answer feeds — unloadPromptShown and finalStatus are "
                  "shared by every task of one check-if-unloading-canceled, so they belong to the operation "
                  "the fan-out is running and have to be reported up with each child, exactly as the wait is. "
                  "Until then subtree_report's LC_AFTER_UNLOAD is unconditional because \"continue\" is the "
                  "only status this can produce");
        }
    }
    DCHECK(s->hdr.stage == BU_FIRE || s->hdr.stage == BU_PROMPT,
           "§7.4.2.4's beforeunload task resumed at a stage it does not have");
    JS_FreeValue(ctx, cb_result);
    subtree_report(ctx, proxy, LC_BEFOREUNLOAD, after);
    return JS_STEP_DONE;
}

static void js_beforeunload_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    BeforeUnloadState *s = st;
    int k;

    v->val(ctx, &s->ev);
    STEP_CB_FOREACH(s->cb, k) v->val(ctx, &s->cb[k]);
}

static JSValue js_beforeunload_fini(JSContext *ctx, void *st, bool take_result)
{
    BeforeUnloadState *s = st;
    int k;

    (void)take_result;
    JS_FreeValue(ctx, s->ev);
    s->ev = JS_UNDEFINED;
    STEP_CB_FOREACH(s->cb, k) {
        JS_FreeValue(ctx, s->cb[k]);
        s->cb[k] = JS_UNDEFINED;
    }
    return JS_UNDEFINED;
}

static int js_unload_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    UnloadState *s = st;
    JSValueConst proxy = step_arg(&s->hdr, 0);
    JSContext *cctx = active_realm(ctx, proxy);
    int after = step_after(&s->hdr), k, r;

    DCHECK(window_proxy_is(proxy), "§7.5.9's unload task was given something that is not a WindowProxy");
    if (s->hdr.stage == UNLOAD_ENTER) {
        s->ev = JS_UNDEFINED;
        STEP_CB_FOREACH(s->cb, k) s->cb[k] = JS_UNDEFINED;
        s->fphase = 0;
        s->showing = 0;
        s->hdr.stage = UNLOAD_PAGEHIDE;
        JS_FreeValue(ctx, cb_result);
        cb_result = JS_UNDEFINED;
        /* STEPS 8 AND 9: salvageable becomes false (see UNLOAD_SALVAGEABLE), and then — only if the document
           was ever shown — page showing becomes false and a pagehide is fired with that salvageable state.
           The two writes are here, before the fire, because step 9.1 precedes step 9.2 and a listener that
           closes another window must not find this document still showing. */
        if (cctx && document_page_showing(cctx)) {
            s->showing = 1;
            document_page_showing_set(cctx, false);                                    /* step 9.1 */
            s->ev = page_transition_event_new(cctx, "pagehide", UNLOAD_SALVAGEABLE);   /* step 9.2's event */
        }
    }
    if (s->hdr.stage == UNLOAD_PAGEHIDE) {
        if (!JS_IsUndefined(s->ev)) {
            JSValue g = JS_GetGlobalObject(cctx);
            /* §7.2.7.6 fires a page transition event with the LEGACY TARGET OVERRIDE FLAG set, so a handler's
               `e.target` is the DOCUMENT — which is what every `e.target.location` in a pagehide handler
               reads. */
            r = event_target_fire_run(cctx, &s->fphase, STEP_CB(s->cb), g, s->ev, document_object(cctx),
                                      cb_result, NULL, out_cb, out_argc);
            JS_FreeValue(cctx, g);
            if (r > 0) return r;                      /* parked on the page's pagehide listeners */
            cb_result = JS_UNDEFINED;
            JS_FreeValue(cctx, s->ev);
            s->ev = JS_UNDEFINED;
        }
        s->fphase = 0;
        s->hdr.stage = UNLOAD_UNLOAD;
        /* STEP 9.3, INSIDE step 9's branch: update the visibility state of oldDocument to "hidden", which fires
           `visibilitychange` at the Document when it was not hidden already. A document that was never showing
           never had a visibility state a page could have observed changing. */
        if (s->showing && cctx)
            page_visibility_update(cctx, true);
        /* STEP 13: salvageable is false, so `unload` fires — at the relevant global object, with the legacy
           target override flag set. */
        if (cctx)
            s->ev = event_new(cctx, "unload", /*bubbles*/ false, /*cancelable*/ false);
    }
    if (s->hdr.stage == UNLOAD_UNLOAD) {
        if (!JS_IsUndefined(s->ev)) {
            JSValue g = JS_GetGlobalObject(cctx);
            r = event_target_fire_run(cctx, &s->fphase, STEP_CB(s->cb), g, s->ev, document_object(cctx),
                                      cb_result, NULL, out_cb, out_argc);
            JS_FreeValue(cctx, g);
            if (r > 0) return r;                      /* parked on the page's unload listeners */
            cb_result = JS_UNDEFINED;
            JS_FreeValue(cctx, s->ev);
            s->ev = JS_UNDEFINED;
        }
        s->hdr.stage = UNLOAD_DESTROY;
        /* The cleanup and the destruction run no page code, so the flow is offered to the scheduler here rather
           than carried through them: the listeners that just ran are exactly when a sibling flow is most likely
           to have overtaken this one. */
        return JS_STEP_YIELD;
    }
    DCHECK(s->hdr.stage == UNLOAD_DESTROY, "§7.5.9's unload task resumed at a stage it does not have");
    JS_FreeValue(ctx, cb_result);
    if (cctx) {
        /* STEP 18's UNLOADING DOCUMENT CLEANUP STEPS. The WebSocket, WebTransport and EventSource loops iterate
           sets that are EMPTY BY CONSTRUCTION — this engine has none of those three interfaces, so "for each"
           runs zero times, exactly as §7.5.10's worker loops do. What remains is the salvageable-false branch's
           second half: "clear window's map of active timers".
           §8.6 GIVES EVERY GLOBAL ITS OWN MAP AND THIS ENGINE HAS ONE. core/timing/timer.c keeps a single
           agent-wide list with no Window key, so there is no map of THIS window's to clear: clearing what
           exists would take a sibling navigable's timers with it, and a same-origin popup this document opened
           is such a sibling. The assert fires exactly when there is something to clear. */
        DCHECK(timer_next_due() < 0,
               "§7.5.9 step 18 must clear the unloading window's map of active timers, and this agent's timers "
               "are ONE list shared by every document in it — BUILD the per-Window map: §8.6's map of active "
               "timers belongs to a global, so core/timing/timer.c has to key each entry by the realm that set "
               "it before an unload can clear one document's without taking the whole agent's");
    }
    /* STEP 20: salvageable is false, so the document is destroyed. The DESCENDANTS are already gone — each one
       ran this same body and destroyed itself at its own step 20 — so this is §7.5.10's single-document form
       and not its descendant form. */
    destroy_a_document(ctx, proxy);
    subtree_report(ctx, proxy, LC_UNLOAD, after);
    return JS_STEP_DONE;
}

static void js_unload_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    UnloadState *s = st;
    int k;

    v->val(ctx, &s->ev);
    STEP_CB_FOREACH(s->cb, k) v->val(ctx, &s->cb[k]);
}

static JSValue js_unload_fini(JSContext *ctx, void *st, bool take_result)
{
    UnloadState *s = st;
    int k;

    (void)take_result;
    JS_FreeValue(ctx, s->ev);
    s->ev = JS_UNDEFINED;
    STEP_CB_FOREACH(s->cb, k) {
        JS_FreeValue(ctx, s->cb[k]);
        s->cb[k] = JS_UNDEFINED;
    }
    return JS_UNDEFINED;
}

#define SELF_DESTROY_STAGES(X) \
    X(SELF_DESTROY, "HTML §7.5.10 destroy a document and its descendants step 6 → destroy a Document, steps " \
                    "1-11, then report this destruction to the navigable that is waiting on it (step 5)")
enum { SELF_DESTROY_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const SELF_DESTROY_STEPS[] = { SELF_DESTROY_STAGES(JS_STEP_STAGE_LABEL) NULL };

typedef struct {
    JSStepHdr hdr;   /* step 6 holds nothing: its whole input is the navigable and the continuation it carries */
} SelfDestroyState;

static int js_self_destroy_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    SelfDestroyState *s = st;
    JSValueConst proxy = step_arg(&s->hdr, 0);

    (void)out_cb; (void)out_argc;
    JS_FreeValue(ctx, cb_result);
    DCHECK(s->hdr.stage == SELF_DESTROY, "the destroy job resumed at a stage §7.5.10 does not have");
    DCHECK(window_proxy_is(proxy), "the destroy job was given something that is not a WindowProxy");

    destroy_a_document(ctx, proxy);
    subtree_report(ctx, proxy, LC_DESTROY, step_after(&s->hdr));
    return JS_STEP_DONE;
}

static void js_self_destroy_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    (void)ctx; (void)st; (void)v;   /* the navigable is the header's argument, and the flow owns those */
}

static JSValue js_self_destroy_fini(JSContext *ctx, void *st, bool take_result)
{
    (void)ctx; (void)st; (void)take_result;
    return JS_UNDEFINED;
}

/* ---- §7.3's DEFINITELY CLOSE, the task §7.2.5.2 step 6 queues -------------------------------------------------
 *
 * It is its OWN task and not a call, because §7.2.5.2 step 6 queues one — a page that calls `close()` and then
 * reads `closed` on the next line sees true (is closing was set synchronously) while its beforeunload listener
 * has not run yet, and collapsing the two would fire that listener one task early. */
#define CLOSE_STAGES(X) \
    X(CLOSE_DEFINITELY, "HTML §7.3 definitely close a top-level traversable steps 1-3 (the active document's " \
                        "inclusive descendant navigables, then check if unloading is canceled over them)")
enum { CLOSE_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const CLOSE_STEPS[] = { CLOSE_STAGES(JS_STEP_STAGE_LABEL) NULL };

typedef struct {
    JSStepHdr hdr;
} CloseState;

static int js_close_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    CloseState *s = st;
    JSValueConst proxy = step_arg(&s->hdr, 0);

    (void)out_cb; (void)out_argc;
    JS_FreeValue(ctx, cb_result);
    DCHECK(s->hdr.stage == CLOSE_DEFINITELY, "definitely close resumed at a stage §7.3 does not have");
    DCHECK(window_proxy_is_top_level(proxy),
           "§7.3's definitely close was given a navigable that is not a TOP-LEVEL traversable — §7.2.5.2 step "
           "2 returns for one, and a frame is closed by removing its container");

    /* STEPS 1-2: toUnload is the active document's INCLUSIVE DESCENDANT NAVIGABLES, and checking if unloading
       is canceled over that list is the beforeunload fan-out — the subtree rooted here, one task per document.
       STEP 3 is its continuation: when the whole subtree has answered, unload the document and its descendants
       with afterAllUnloads = destroy this traversable. */
    descend_enqueue(ctx, proxy, LC_BEFOREUNLOAD, LC_AFTER_UNLOAD);
    return JS_STEP_DONE;
}

static void js_close_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    (void)ctx; (void)st; (void)v;
}

static JSValue js_close_fini(JSContext *ctx, void *st, bool take_result)
{
    (void)ctx; (void)st; (void)take_result;
    return JS_UNDEFINED;
}

/* ---- the declarations ---------------------------------------------------------------------------------------
 *
 * THREE DEFINITIONS OVER ONE FAN-OUT: the `arg` is the operation, and the labels are the section each one is
 * performing — so a flow parked in the middle of a subtree walk says which STANDARD's step it is parked at,
 * which one shared definition could not. */
static const JSTrampStepDef js_bu_descend_def = { sizeof(DescendState), js_descend_step, js_descend_fini,
                                                  LC_BEFOREUNLOAD, .visit = js_descend_visit,
                                                  .algorithm = "HTML §7.4.2.4 check if unloading is canceled",
                                                  .steps = BU_DESCEND_STEPS };
static const JSTrampStepDef js_unload_descend_def = { sizeof(DescendState), js_descend_step, js_descend_fini,
                                                      LC_UNLOAD, .visit = js_descend_visit,
                                                      .algorithm = "HTML §7.5.9 unload a document and its "
                                                                   "descendants, steps 2-5",
                                                      .steps = UNLOAD_DESCEND_STEPS };
static const JSTrampStepDef js_destroy_descend_def = { sizeof(DescendState), js_descend_step, js_descend_fini,
                                                       LC_DESTROY, .visit = js_descend_visit,
                                                       .algorithm = "HTML §7.5.10 destroy a document and its "
                                                                    "descendants, steps 2-5",
                                                       .steps = DESTROY_DESCEND_STEPS };
static const JSTrampStepDef js_beforeunload_def = { sizeof(BeforeUnloadState), js_beforeunload_step,
                                                    js_beforeunload_fini, 0, .visit = js_beforeunload_visit,
                                                    .algorithm = "HTML §7.4.2.4 the steps to fire beforeunload",
                                                    .steps = BEFOREUNLOAD_STEPS };
static const JSTrampStepDef js_unload_def = { sizeof(UnloadState), js_unload_step, js_unload_fini, 0,
                                              .visit = js_unload_visit,
                                              .algorithm = "HTML §7.5.9 unload a document",
                                              .steps = UNLOAD_STEPS };
static const JSTrampStepDef js_self_destroy_def = { sizeof(SelfDestroyState), js_self_destroy_step,
                                                    js_self_destroy_fini, 0, .visit = js_self_destroy_visit,
                                                    .algorithm = "HTML §7.5.10 destroy a document and its "
                                                                 "descendants, step 6",
                                                    .steps = SELF_DESTROY_STEPS };
static const JSTrampStepDef js_close_def = { sizeof(CloseState), js_close_step, js_close_fini, 0,
                                             .visit = js_close_visit,
                                             .algorithm = "HTML §7.3 definitely close a top-level traversable",
                                             .steps = CLOSE_STEPS };

/* The fan-out and the body for each operation, in the operation's own order — one table so that adding an
   operation is one row rather than three switches that can disagree about which body belongs to which walk. */
static const JSTrampStepDef *const LC_DESCEND_DEF[LC_OP_N] = {
    &js_bu_descend_def, &js_unload_descend_def, &js_destroy_descend_def
};
static const JSTrampStepDef *const LC_SELF_DEF[LC_OP_N] = {
    &js_beforeunload_def, &js_unload_def, &js_self_destroy_def
};
static const char *const LC_DESCEND_NAME[LC_OP_N] = {
    "checkIfUnloadingIsCanceled", "unloadDescendants", "destroyDescendants"
};
static const char *const LC_SELF_NAME[LC_OP_N] = { "fireBeforeunload", "unloadDocument", "destroyDocument" };
static int g_descend_stepid[LC_OP_N] = { -1, -1, -1 };
static int g_self_stepid[LC_OP_N] = { -1, -1, -1 };
static int g_close_stepid = -1;

/* THEY ARE QUEUED ON THE TASK QUEUE, NOT THE MICROTASK QUEUE, because every one of these algorithms queues a
   GLOBAL TASK — §7.4.2.4, §7.5.9 and §7.5.10 on the navigation and traversal task source, §7.2.5.2 step 6 on
   the DOM manipulation task source. This engine has one task queue, so what the source decides (the order two
   sources are drained in) is not modelled; what the MICROTASK/task split decides is, and it is the whole reason
   `closed` needed splitting. What a removal changes SYNCHRONOUSLY is the container's slot, so
   `frame.contentWindow` is null on the next line; what it changes LATER is the destroyed document's browsing
   context, which is the other half of the getter. A single byte written at the removal collapsed those two
   times into one and reported a destruction that had not happened — while the document it claimed to have
   destroyed was still there, still scheduled, and still entangled. */
static void enqueue_job(JSContext *ctx, JSValueConst proxy, const JSTrampStepDef *def, int *pid, const char *nm,
                        int after)
{
    JSValueConst argv[2];
    JSValue fn, cont;

    if (*pid < 0)
        *pid = JS_RegisterStepDef(JS_GetRuntime(ctx), def);
    fn = JS_NewCFunction2(ctx, NULL, nm, 2, JS_CFUNC_step, *pid);
    CHECK(!JS_IsException(fn), "a document-lifecycle job's callee could not be allocated");
    cont = JS_NewInt32(ctx, after);
    argv[0] = proxy;
    argv[1] = cont;
    JS_EnqueueCallTask(ctx, fn, 2, argv);
    JS_FreeValue(ctx, cont);
    JS_FreeValue(ctx, fn);
}

static void descend_enqueue(JSContext *ctx, JSValueConst proxy, int op, int after)
{
    DCHECK(op >= 0 && op < LC_OP_N, "a subtree operation this file does not name was started");
    /* A NAVIGABLE WHOSE DOCUMENT IS ALREADY DESTROYED IS NOT OPERATED ON AGAIN — §7.5.10's descendant form
       opens by asking whether the document is fully active, and a destroyed one is not; §7.4.2.4 and §7.5.9
       have no active document to fire at or unload. Without this, removing an ancestor of an already-removed
       frame would queue a second walk of the same subtree, and the second one's report would decrement a count
       the first had already emptied. */
    if (window_proxy_destroyed(proxy)) return;
    /* A NAVIGABLE WHOSE ACTIVE DOCUMENT LIVES IN ANOTHER INSTANCE cannot be unloaded or destroyed from here:
       its listeners, its ports and its child navigables are all in the peer's heap, and this side would walk an
       empty tree and call it a subtree. */
    DCHECK(!window_proxy_is_remote(proxy),
           "a subtree operation reached a navigable whose ACTIVE DOCUMENT is a PEER instance's — BUILD the "
           "cross-instance route: the beforeunload/pagehide/unload listeners are the peer's page code and run "
           "in the peer's own scheduled turn, so this job has to be POSTED to the instance holding that "
           "document (window_proxy_doc names which one) and its completion reported back the way a "
           "cross-origin read's answer is");
    enqueue_job(ctx, proxy, LC_DESCEND_DEF[op], &g_descend_stepid[op], LC_DESCEND_NAME[op], after);
}

static void self_enqueue(JSContext *ctx, JSValueConst proxy, int op, int after)
{
    DCHECK(op >= 0 && op < LC_OP_N, "a subtree operation this file does not name reached its per-document body");
    enqueue_job(ctx, proxy, LC_SELF_DEF[op], &g_self_stepid[op], LC_SELF_NAME[op], after);
}

/* ---- §7.3's DESTROY A TOP-LEVEL TRAVERSABLE ------------------------------------------------------------------
 *
 * STEP 2 IS A WALK OF THE SESSION HISTORY ENTRIES, and this engine holds exactly one per navigable — its
 * active document's. window_proxy_navigate REPLACES the active document rather than appending an entry, so
 * there is no second entry holding a second document to destroy, and the walk over the entries this engine has
 * IS the destruction of the active document and its descendants.
 *
 * STEPS 1 AND 3 are §7.3.2.3's remove-a-browsing-context: take the browsing context out of its group's set,
 * and drop the group when its set empties. This engine has no browsing context group OBJECT — an instance is
 * an origin-keyed agent cluster, which is the heap boundary rather than the group — and the one consequence a
 * page can read is that the navigable's browsing context becomes null, which is §7.5.10 step 8 and is written
 * by the destruction step 2 just queued.
 *
 * STEPS 4 AND 5 remove the traversable from the user interface and from the user agent's top-level traversable
 * set. There is no user interface, and the set is the browser's own list of tabs, which one WASM instance per
 * document does not hold. STEP 6 is WebDriver BiDi. */
static void destroy_a_top_level_traversable(JSContext *ctx, JSValueConst proxy)
{
    DCHECK(window_proxy_is_top_level(proxy),
           "§7.3's destroy a top-level traversable was given a navigable that is not one");
    descend_enqueue(ctx, proxy, LC_DESTROY, LC_AFTER_NONE);   /* step 2 */
}

/* ---- §7.2.5.2's close() --------------------------------------------------------------------------------------
 *
 * §7.2.5.2 step 6's THREE CONJUNCTS, each decided from what this engine models. */

/* "thisTraversable is SCRIPT-CLOSABLE": a top-level traversable, and either its "is created by web content" is
   true or its session history entries's size is 1. BOTH DISJUNCTS ARE NOW REAL, and the second one stopped
   being universally true the moment §7.4.1's entries existed: this used to answer `true` unconditionally
   because "this engine keeps none — window_proxy_navigate replaces the active document rather than appending an
   entry — so every navigable has exactly the one entry its active document is in", and one `history.pushState`
   makes that two. A traversable that has routed once would then have been un-closable, which is a page that
   calls `w.close()` and watches nothing happen.
   THE ORDER IS THE STANDARD'S DISJUNCTION AND IT MATTERS HERE: `is created by web content` is answered from the
   navigable itself, so a popup a page opened stays closable however long its history grows, and only the
   navigable the instance STARTED in — which the host loaded, not web content — falls through to counting. */
static bool traversable_is_script_closable(JSContext *ctx, JSValueConst proxy)
{
    JSContext *tctx;

    DCHECK(window_proxy_is_top_level(proxy),
           "script-closable was asked of a navigable that is not a top-level traversable — §7.2.5.2 step 2 has "
           "already returned for one");
    if (window_proxy_created_by_web_content(proxy)) return true;
    /* AN UNMATERIALIZED NAVIGABLE HAS EXACTLY ONE ENTRY, and that is the computed answer rather than a skip:
       it holds the initial about:blank Document §7.4 created it with, nothing has ever run in it, so nothing
       has pushed. Asking for its realm would BUILD one — materializing a document in order to close it, which
       is active_realm's deferral running backwards. */
    tctx = active_realm(ctx, proxy);
    if (!tctx) return true;
    return session_history_length(tctx) == 1;
}

/* "the incumbent global object's navigable is ALLOWED BY SANDBOXING TO NAVIGATE thisTraversable, given
   sourceSnapshotParams". §7.4.5's algorithm is a walk over the SOURCE's sandboxing flags, and a document's
   flags come from §7.6.2's navigable container through §7.2.6's policy container, which in this engine carries
   a CSP and nothing else — so every one of that algorithm's "if the … flag is set, return false" tests fails
   and its answer is true. Evaluated at the step that asks it, in the same shape §4.10.22.3's sandboxed-forms
   condition has. */
static bool allowed_by_sandboxing_to_navigate(JSContext *ctx, JSValueConst proxy)
{
    (void)ctx; (void)proxy;
    return true;
}

void document_lifecycle_window_close(JSContext *ctx, JSValueConst proxy)
{
    /* STEP 1: thisTraversable is this's navigable — the caller holds the proxy, which IS that navigable, so
       both spellings of close() arrive here having resolved it the same way. */
    DCHECK(window_proxy_is(proxy), "§7.2.5.2's close() was called on something that is not a WindowProxy");
    if (!window_proxy_is_top_level(proxy)) return;                        /* step 2 */
    if (window_proxy_closing(proxy)) return;                              /* step 3 */
    /* STEPS 4-5 bind the browsing context and snapshot the source snapshot params; both are operands of the
       conjunction below, which reads the state they name directly. */
    if (!traversable_is_script_closable(ctx, proxy)) return;               /* step 6, first conjunct */
    if (!window_proxy_familiar_with(ctx, proxy)) return;                  /* step 6, second conjunct */
    if (!allowed_by_sandboxing_to_navigate(ctx, proxy)) return;           /* step 6, third conjunct */
    window_proxy_set_closing(ctx, proxy);                                 /* step 6.1 */
    /* STEP 6.2: queue a task to DEFINITELY CLOSE — never a call, so the page's own line after `close()` runs
       first and reads the `closed` step 6.1 just made true. */
    enqueue_job(ctx, proxy, &js_close_def, &g_close_stepid, "definitelyClose", LC_AFTER_NONE);
}

void document_lifecycle_destroy_child(JSContext *ctx, JSValueConst proxy)
{
    DCHECK(window_proxy_is(proxy),
           "§7.3.1 was asked to destroy something that is not a navigable's WindowProxy");
    descend_enqueue(ctx, proxy, LC_DESTROY, LC_AFTER_NONE);
}
