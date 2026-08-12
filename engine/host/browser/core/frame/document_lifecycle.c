/* HTML §7.5.10 destroying documents — see document_lifecycle.h. */
#include <stdbool.h>

#include "check.h"
#include "quickjs.h"
#include "quickjs-step.h"
#include "core/events/message_port.h"
#include "core/frame/document_lifecycle.h"
#include "core/frame/window_proxy.h"
#include "core/html/html_iframe.h"

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
 * STEP 9 IS SESSION HISTORY, which this engine does not hold: there is no session history entry to null a
 * document out of, and the fact that step 8 records — the browsing context is null — is the one a page reads,
 * through §7.2.5's `closed`. */
static void destroy_a_document(JSContext *ctx, JSValueConst proxy)
{
    JSContext *cctx = window_proxy_materialized(proxy) ? window_proxy_realm(ctx, proxy) : NULL;

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

/* THE TWO QUEUED TASKS ARE TWO MACHINES, because §7.5.10 queues two and they run at different times. Step 4
 * queues one task PER CHILD; step 6 queues one for THIS document, and step 5 says the second may not begin
 * until every one of the first has finished.
 *
 * SO THE WAIT IS NOT A PARK, AND THAT IS NOT AN OPTIMISATION — IT IS THE ONLY THING THAT TERMINATES. A machine
 * that returned JS_STEP_YIELD until its children reported in would spin: YIELD is a PREEMPT POINT, re-entered
 * immediately when nothing outranks the flow, and the children's tasks are queued BEHIND the parent's job on
 * the same flow. The parent would re-check for ever and the children would never get to run. What the spec
 * actually describes is a COMPLETION: each child's task increments a count and the parent's task is the one
 * that runs when the count is full, so the LAST CHILD TO FINISH is what queues step 6 for its parent. That is
 * a notification going up, and it is why the count lives on the WindowProxy — the record that already holds
 * this navigable's per-flow state, so two forked arms destroying the same subtree count in their own worlds
 * instead of into each other's.
 *
 * THE PARENT LINK IS THE ONE THE PROXY ALREADY HAS. The child does not have to be told who is waiting for it:
 * the navigable it is nested in is exactly that. It is asked for as window_proxy_parent_navigable and NOT as
 * §7.2.5's `parent`, which answers the proxy ITSELF at the top of the tree — a report delivered back to its
 * own sender. And a navigable that is not itself being destroyed has a zero outstanding count, which is what
 * makes the notification a no-op there rather than a case the sender has to test for. */
#define DESCEND_STAGES(X) \
    X(DESCEND_TAKE, "HTML §7.5.10 destroy a document and its descendants steps 2-4 (bind childNavigables, " \
                    "then queue one destruction per child, one child per step)")
enum { DESCEND_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const DESCEND_STEPS[] = { DESCEND_STAGES(JS_STEP_STAGE_LABEL) NULL };

#define SELF_STAGES(X) \
    X(SELF_DESTROY, "HTML §7.5.10 destroy a document and its descendants step 6 → destroy a Document, steps " \
                    "1-11, then report this destruction to the navigable that is waiting on it (step 5)")
enum { SELF_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const SELF_STEPS[] = { SELF_STAGES(JS_STEP_STAGE_LABEL) NULL };

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
       `JS_IsUndefined(kids)` would answer NO on its very first entry and destroy every subtree without ever
       descending into it. The flag is the only field that can be read before anything has written one. */
    uint8_t  took;
} DescendState;

typedef struct {
    JSStepHdr hdr;   /* step 6 holds nothing: its whole input is the navigable its header carries */
} SelfState;

static void descend_enqueue(JSContext *ctx, JSValueConst proxy);
static void self_enqueue(JSContext *ctx, JSValueConst proxy);

static int js_descend_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    DescendState *s = st;
    JSValueConst proxy = step_arg(&s->hdr, 0);
    JSContext *cctx;

    (void)out_cb; (void)out_argc;
    JS_FreeValue(ctx, cb_result);
    DCHECK(s->hdr.stage == DESCEND_TAKE, "the destroy fan-out resumed at a stage §7.5.10 does not have");
    DCHECK(window_proxy_is(proxy), "the destroy fan-out was given something that is not a WindowProxy");

    if (!s->took) {
        int n = 0, k;

        s->took = 1;
        s->kids = JS_NewArray(ctx);
        CHECK(!JS_IsException(s->kids),
              "OOM taking §7.5.10 step 2's child navigables — a destruction that loses half its subtree leaves "
              "live documents under a destroyed one");
        /* AN UNMATERIALIZED NAVIGABLE HAS NO CHILD NAVIGABLES, and asking would BUILD its realm — a document
           materialized for the sole purpose of being destroyed, which is navigable.h's deferral running
           backwards. It holds the initial about:blank §7.4 created it with, which has no iframes. */
        if (window_proxy_materialized(proxy)) {
            cctx = window_proxy_realm(ctx, proxy);
            n = iframe_child_navigable_count(cctx);
            for (k = 0; k < n; k++)
                JS_SetPropertyUint32(ctx, s->kids, (uint32_t)k, iframe_child_navigable(cctx, k));
        }
        s->n = (uint32_t)n;
        s->i = 0;
        /* STEP 3's numberDestroyed, counted DOWN and recorded before a single child is queued — a child that
           finished while the count was still being raised would see it hit zero early and queue step 6 for a
           parent with siblings still live. */
        window_proxy_destroy_wait_set(ctx, proxy, s->n);
    }
    if (s->i < s->n) {
        JSValue kid = JS_GetPropertyUint32(ctx, s->kids, s->i++);   /* step 4, one child per rest point */

        /* A CHILD THAT IS ALREADY DESTROYED REPORTS AT ONCE INSTEAD OF BEING QUEUED, and that is not a
           shortcut — it is what keeps step 5 from waiting for ever. Queuing it would be queuing a destruction
           that returns immediately without reporting (descend_enqueue's own early return), so the count would
           sit one short of empty and this navigable's step 6 would never be queued at all. It is reachable
           whenever a subtree is removed in pieces: a frame removed first, then the frame containing it. */
        if (window_proxy_destroyed(kid)) {
            if (window_proxy_destroy_wait_finish(ctx, proxy))
                self_enqueue(ctx, proxy);
        } else {
            descend_enqueue(ctx, kid);
        }
        JS_FreeValue(ctx, kid);
        return JS_STEP_YIELD;
    }
    /* WITH NO CHILDREN THERE IS NOTHING TO WAIT FOR, so step 6 is queued here; with children, the last one to
       finish queues it (js_self_step). Both are step 5 — "wait until numberDestroyed equals size" is already
       true for a document that has no child navigables. */
    if (s->n == 0)
        self_enqueue(ctx, proxy);
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

/* STEP 6, AND THE REPORT THAT MAKES STEP 5 TERMINATE. The navigable waiting on this one is the navigable it is
   nested in, and one that is not itself being destroyed has nothing outstanding — so the report is a no-op
   there rather than a case to test for at the call site. */
static int js_self_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    SelfState *s = st;
    JSValueConst proxy = step_arg(&s->hdr, 0);
    JSValue parent;

    (void)out_cb; (void)out_argc;
    JS_FreeValue(ctx, cb_result);
    DCHECK(s->hdr.stage == SELF_DESTROY, "the destroy job resumed at a stage §7.5.10 does not have");
    DCHECK(window_proxy_is(proxy), "the destroy job was given something that is not a WindowProxy");

    destroy_a_document(ctx, proxy);
    parent = window_proxy_parent_navigable(ctx, proxy);
    if (window_proxy_is(parent) && window_proxy_destroy_wait_finish(ctx, parent))
        self_enqueue(ctx, parent);   /* the last child of a waiting parent: its step 6 may now run */
    JS_FreeValue(ctx, parent);
    return JS_STEP_DONE;
}

static void js_self_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    (void)ctx; (void)st; (void)v;   /* the navigable is the header's argument, and the flow owns those */
}

static JSValue js_self_fini(JSContext *ctx, void *st, bool take_result)
{
    (void)ctx; (void)st; (void)take_result;
    return JS_UNDEFINED;
}

static const JSTrampStepDef js_descend_def = { sizeof(DescendState), js_descend_step, js_descend_fini, 0,
                                               .visit = js_descend_visit,
                                               .algorithm = "HTML §7.5.10 destroy a document and its "
                                                            "descendants, steps 2-5",
                                               .steps = DESCEND_STEPS };
static const JSTrampStepDef js_self_def = { sizeof(SelfState), js_self_step, js_self_fini, 0,
                                            .visit = js_self_visit,
                                            .algorithm = "HTML §7.5.10 destroy a document and its descendants, "
                                                         "step 6",
                                            .steps = SELF_STEPS };
static int g_descend_stepid = -1, g_self_stepid = -1;

/* THEY ARE QUEUED ON THE TASK QUEUE, NOT THE MICROTASK QUEUE, because §7.5.10 queues GLOBAL TASKS on the
   navigation and traversal task source — and that is the whole reason `closed` needed splitting. What a
   removal changes SYNCHRONOUSLY is the container's slot, so `frame.contentWindow` is null on the next line;
   what it changes LATER is the destroyed document's browsing context, which is the other half of the getter.
   A single byte written at the removal collapsed those two times into one and reported a destruction that had
   not happened — while the document it claimed to have destroyed was still there, still scheduled, and still
   entangled. */
static void enqueue_job(JSContext *ctx, JSValueConst proxy, const JSTrampStepDef *def, int *pid, const char *nm)
{
    JSValueConst argv[1];
    JSValue fn;

    if (*pid < 0)
        *pid = JS_RegisterStepDef(JS_GetRuntime(ctx), def);
    fn = JS_NewCFunction2(ctx, NULL, nm, 1, JS_CFUNC_step, *pid);
    CHECK(!JS_IsException(fn), "a document-destroy job's callee could not be allocated");
    argv[0] = proxy;
    JS_EnqueueTaskJob(ctx, fn, 1, argv);
    JS_FreeValue(ctx, fn);
}

static void descend_enqueue(JSContext *ctx, JSValueConst proxy)
{
    /* A NAVIGABLE WHOSE DOCUMENT IS ALREADY DESTROYED IS NOT DESTROYED AGAIN — §7.5.10's descendant form opens
       by asking whether the document is fully active, and a destroyed one is not. Without this, removing an
       ancestor of an already-removed frame would queue a second destruction of the same subtree, and the
       second one's report would decrement a count the first had already emptied. */
    if (window_proxy_destroyed(proxy)) return;
    enqueue_job(ctx, proxy, &js_descend_def, &g_descend_stepid, "destroyDescendants");
}

static void self_enqueue(JSContext *ctx, JSValueConst proxy)
{
    enqueue_job(ctx, proxy, &js_self_def, &g_self_stepid, "destroyDocument");
}

void document_lifecycle_destroy_child(JSContext *ctx, JSValueConst proxy)
{
    DCHECK(window_proxy_is(proxy),
           "§7.3.1 was asked to destroy something that is not a navigable's WindowProxy");
    descend_enqueue(ctx, proxy);
}
