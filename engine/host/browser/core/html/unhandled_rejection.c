/* UNHANDLED PROMISE REJECTIONS — HTML §8.1.4.7 "Unhandled promise rejections".
 *
 * The section number was §8.1.7.5 throughout this file, which is a REAL section — "Dealing with the event loop
 * from other specifications" — and a different algorithm. A citation that resolves to the wrong text is worse
 * than none, because it reads as checked.
 *
 * WHAT WAS MISSING, AND WHY IT MATTERS HERE MORE THAN IN A BROWSER: a promise that rejects with nobody to catch
 * it was SILENT. Not softened, not swallowed by a fallback — never observed at all. A page error is this
 * engine's report of a capability the page needed and the engine does not have, and an async bundle delivers
 * most of its errors as rejections: an `await` of a missing global, a `.then` whose body touches an unbuilt
 * API. Every one of those looked exactly like a flow that ran and did nothing, which is the worst possible
 * failure mode for a tool whose whole job is to notice what code CAN do.
 *
 * THE TWO LISTS ARE THE SPEC, AND THEY ARE WHY THIS IS NOT JUST A PRINT AT THE THROW SITE. §8.1.4.7 keeps an
 * "about-to-be-notified" list and an "outstanding" list precisely because attaching a handler LATER is normal,
 * correct code — `var p = f(); p.catch(h)` rejects before the catch is attached. Reporting at rejection time
 * would call every one of those an error. The runtime's tracker reports both edges (rejected-with-no-handler,
 * and handled-after-the-fact), so an entry that gets handled is removed and never notified about.
 *
 * THE LIST IS A HEAP OBJECT, for the same reason the custom-element registry is: it is per-flow state. Flow A
 * rejecting a promise is not flow B's rejection, a parked flow must resume owed exactly what it was owed, and
 * a C-global list would report one flow's rejection against another's world. A baseline Array carries all of
 * that through the heap COW with no new primitive.
 *
 * THE CHECKPOINT IS THE END OF A MICROTASK CHECKPOINT, WHICH IS WHERE HTML PUTS IT. "Perform a microtask
 * checkpoint" — the algorithm HTML §8.1.7.3 "Processing model" defines — runs the queue to empty and then, as
 * its own next step, "For each environment settings object settingsObject whose responsible event loop is this
 * event loop, notify about rejected promises given settingsObject's global object". So the notify is a step of
 * that algorithm and not a thing that happens when a page is over, and the Cleanup-Indexed-Database-
 * transactions step is the one directly AFTER it in the same list. The scheduler calls both there, in that
 * order (solver/engine.c's slice).
 *
 * THE PARAGRAPH THAT STOOD HERE SAID THE OPPOSITE AND ITS PREMISE WAS FALSE ABOUT THIS TREE, which is why it
 * is written out rather than deleted: in its own words (this file's, not any standard's) the notify belonged
 * at the flow's end because this engine had no drain to hang a checkpoint on, the scheduler being the job
 * pump. The scheduler has had that seam — the point at which a step ends with no live frame, no parked
 * continuation and no microtask left, which IS the emptying HTML §8.1.4.4 "Calling scripts"' clean up after
 * running script step 3 triggers on — and the consumer registered there cites this very algorithm. A reader
 * who re-derives the retired reason will re-introduce it, so the reason is recorded as retired.
 *
 * WHAT IT COST, and it is not a rounding difference: the flow's end was an ARM at the BOTTOM of flow_step's
 * ladder, below the sequence, the jobs, the replies, the lifecycle, the rendering opportunity, the timer and
 * the host-owed return. A flow reaches it only by running out of every one of those, and CLAUDE.md §scheduler
 * says in terms that a justification resting on a drain is not a guarantee: on a frontier that grows it is the
 * same starvation with a reason attached. This file's own retired claim — that the flow's end is strictly the
 * same set minus the retractions — is therefore true only of a flow that ENDS; for one that does not it is the
 * EMPTY set, and every unhandled rejection of the run is silent — the exact failure the paragraph above this
 * one calls the worst possible for this tool. Measured on the native fixture, which stages rejecting chunks, over six runs
 * and 213 `@COLD` censuses: a member stood on that arm in nine of them, all inside the one deepest run and
 * none before its 126th census, while the seam this now rides carried one in 178 — and no flow FINISHED in
 * any census of any run, which is the drain the old position was waiting for.
 *
 * BOTH EDGES GO THROUGH ONE HOOK, WHICH IS WHY A REPORT THIS COMPONENT MADE CAN BE TAKEN BACK. §8.1.4.7 step
 * 4.1.3 reports a rejection; §8.1.6.4 step 7.4 is the standard's own admission that the report can turn out to
 * have been about a page that did nothing wrong, because attaching `.catch` in a LATER task is ordinary correct
 * code. The page hears that as `rejectionhandled`; this engine's console hears it as a RETRACTION on the same
 * `g_report` edge, carrying `RejectionReportEdge` (see the header for why one hook rather than two).
 * WHAT MADE IT EXPRESSIBLE IS ARITHMETIC ON THE OTHER SIDE OF THE SEAM AND NOT A LINE HERE: solver/result.c's
 * rows count their OCCURRENCES, so the retraction decrements one rather than deleting a row that may also
 * stand for a rejection nobody handled. That is also why nothing here decides what a retraction MEANS — a host
 * whose output is a document withdraws the row, a host whose output is a stream appends a correction line, and
 * both of those are result.h's to state.
 * WHAT THE SPEC ITSELF LEAVES UNRETRACTABLE, so it is not mistaken for the same gap: a handler attached by an
 * `unhandledrejection` listener DURING the fire. Step 4.1.3 has already reported by then and step 4.1.4
 * declines to append, so no `rejectionhandled` is ever owed. That is the standard's own accepted cost and not
 * a hole in this component.
 * AND THE MIRROR OF IT, WHICH IS A RETRACTION OWED FOR A REPORT THAT NEVER HAPPENED: step 4.1.3 is gated on
 * notCanceled and step 4.1.4's append is gated on [[PromiseIsHandled]] alone, so a page that calls
 * preventDefault() is never reported and IS appended, and step 7.4 will fire for it. The consumer sees a
 * retraction with nothing to take back — a legitimate no-op there, never a missed report.
 *
 * THE EVENT IS THE PAGE'S CHANCE TO ANSWER, and it is CANCELABLE, so the report is not this component's to
 * make: §8.1.4.7 fires `unhandledrejection` at the global and reports only if nothing called preventDefault.
 * A page that ships its own error reporter cancels it — and that reporter is code with a fetch in it, which is
 * exactly the surface this engine exists to reach, so firing the event is worth more than the report is.
 * Firing is a REQUEST (dispatch is synchronous and its listeners are the page's code), so each notification is
 * a step machine on the flow's queue rather than a call from C.
 *
 */
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "quickjs-step.h"
#include "core/agent_state.h"
#include "core/idl_args.h"
#include "core/realm.h"
#include "core/events/event.h"
#include "core/events/event_target.h"
#include "core/html/unhandled_rejection.h"

/* §8.1.4.7's list, as a baseline heap object so it time-travels with the flow that filled it. Each live entry
   is a two-element array [promise, reason]; a handled entry becomes undefined in place, because the identity
   of a slot is what the handled edge finds it by and compacting would move the ones behind it. */
static JSValue g_list;
static int     g_ready;
/* §8.1.3.3's OTHER container — "a global object has an outstanding rejected promises WEAK SET" — and the word
   weak is the whole of why this one is not a second `g_list`. Its members are exactly the promises this global
   already REPORTED (step 4.1.4 appends only what survived the `unhandledrejection` fire still unhandled), and a
   page that rejects in a loop produces one member per iteration; a strong list of them would keep every
   rejected promise of every origin ever visited alive for the life of the agent, against a frontier §NO BOUNDS
   says is never reset.
   SO THE SET IS NOT A CONTAINER AT ALL — IT IS A STAMP ON THE PROMISE, under the Symbol below. Weakness is then
   structural rather than a policy: this component holds nothing, the promise holds a marker, and the marker
   dies with the promise. It costs nothing in fidelity because membership is only ever ASKED with the promise in
   hand: §8.1.6.4's handle arm is the sole reader, and "is p in global g's set" is exactly "does p carry g's
   marker". THE MARKER IS THE REALM'S OWN `rejectionhandled` DRIVER, so one read answers both halves of steps
   7.2-7.4 — whether this global holds it, and which global's task source to queue the fire on. A promise enters
   at most one set (it rejects once, so it is appended to one global's about-to-be-notified list), which is why
   a single-valued stamp is a set membership and not a lossy one.
   AND IT IS PAGE-VISIBLE, WHICH IS WHY NOTHING HERE ASSERTS ITS ABSENCE. `Object.getOwnPropertySymbols` lists
   an ordinary Symbol key, so a page can read the marker off one of its own outstanding promises and stamp it
   onto another. That buys it a spurious `rejectionhandled` at its own global — which it can already fire with
   `window.dispatchEvent(new PromiseRejectionEvent("rejectionhandled", {promise: p}))`, legal per this very
   section's IDL — so the forgery grants nothing, and an assert on the stamp's absence would be a DCHECK on
   attacker input, which is banned. The removal below CAN assert, because no page code runs between its
   membership read and its delete. */
static JSValue g_out_key = JS_UNDEFINED;
/* PromiseRejectionEvent: its prototype, and the Symbol its two slots live under — the same shape Event uses for
   its own. A claim that a page "cannot forge a slot bag it cannot name" stood here and is FALSE of an ordinary
   Symbol: `Object.getOwnPropertySymbols` on the event a listener is handed names it. What the slot buys is that
   the bag is not a property LOOKUP (see pre_slot) and is not writable; it is not a capability boundary. */
/* PER REALM, for the reason event.c states — a C member runs in the realm that DEFINED it. Held in quickjs's
   own per-context class-proto slot. */
/* THE CLASS ID IS AGENT STATE AND IS GIVEN BACK AT 0 — core/agent_state.h states the one policy and the
   reason: a class is registered in a RUNTIME, so a carried id names a class in a runtime that is gone, and
   `JS_NewClassID` returns the EXISTING value when the slot is non-zero, so the next agent's init would hand
   `JS_NewClass` an id its own runtime never allocated while a zeroing component draws that same number from a
   counter that restarted. There is no collector entry to pay the closing cost of that policy here: the
   JSClassDef below carries a NAME only, so PromiseRejectionEvent has neither a finalizer nor a gc_mark that
   could read this id after the release column has already put it back. */
static JSClassID g_pre_class;
/* AND THE CONSTRUCTOR'S POOL ENTRY, `= -1` because that is core/agent_state.h's pre-init value for an id.
   Bare, its pre-init value is `0` — a VALID entry of the next agent's member pool, which is the failure mode
   that answers instead of failing. */
static int g_id_pre_ctor = -1;
static JSValue g_pre_key = JS_UNDEFINED;
/* THE NOTIFICATION DRIVER IS PER REALM, and that is the standard's own requirement rather than tidiness. The
   global is fixed at the REJECTION and not at the notify: HTML §8.1.6.4 HostPromiseRejectionTracker(promise,
   operation) — which `rejection_tracker` below IS — lets "global be settingsObject's global object" for the
   running script's settings object and appends the promise to THAT global's about-to-be-notified rejected
   promises list; HTML §8.1.4.7 Unhandled promise rejections then fires `unhandledrejection` "at global". So
   this driver reads its global off the ctx it runs under — which, for one function object held for the agent,
   is whichever realm minted it. (A quoted phrase attributing the fire to the PROMISE's relevant global object
   stood here as though it were §8.1.4.7's; neither section contains it, and a promise answers no such
   question — the global is the running script's settings object's, fixed at the reject.) Every child
   document's unhandled rejection fired at the ROOT window, which is the same defect a shared
   EventTarget.prototype has one link up. A rejection RECORDS the driver of the realm it rejected in, so the
   job carries its realm with it rather than the notify pass guessing one. */
static int g_notify_slot = -1, g_notify_stepid = -1;
/* §8.1.6.4 step 7.4's fire, which is a DIFFERENT ALGORITHM from step 4.1's and gets its own machine rather than
   a mode flag on that one: it fires a NON-cancelable event, reports nothing, and appends to nothing. Sharing a
   driver would mean one `.algorithm` string naming two sections, which is the citation defect this component's
   own header is about. It is per realm for the same reason the notify driver is, and it is ALSO the marker
   §8.1.3.3's weak set is spelled with — see g_out_key. */
static int g_handled_slot = -1, g_handled_stepid = -1;
static void  (*g_report)(JSContext *ctx, JSValueConst reason, RejectionReportEdge edge);

void unhandled_rejection_set_report_hook(void (*fn)(JSContext *ctx, JSValueConst reason,
                                                    RejectionReportEdge edge)) { g_report = fn; }

static uint32_t list_len(JSContext *ctx)
{
    JSValue v = JS_GetPropertyStr(ctx, g_list, "length");
    uint32_t n = 0;
    JS_ToUint32(ctx, &n, v);
    JS_FreeValue(ctx, v);
    return n;
}

/* ---- §8.1.3.3's outstanding rejected promises weak set — see g_out_key for the structure --------------- */
static JSAtom outstanding_atom(JSContext *ctx)
{
    JSAtom k = JS_ValueToAtom(ctx, g_out_key);
    CHECK(k != JS_ATOM_NULL, "the outstanding-rejected-promises marker key could not be reached");
    return k;
}

/* WHICH global's set holds this promise — its `rejectionhandled` driver, or undefined for none. OWNED.
   AN OWN SLOT, never a property LOOKUP, for pre_slot's reason: a lookup walks the prototype chain into the
   solver's absent-state seam and would mint a concolic marker for a promise nobody stamped. */
static JSValue outstanding_holder(JSContext *ctx, JSValueConst promise)
{
    JSAtom k = outstanding_atom(ctx);
    JSValue v;
    if (JS_GetOwnSlot(ctx, &v, promise, k) <= 0) v = JS_UNDEFINED;
    JS_FreeAtom(ctx, k);
    return v;
}

/* §8.1.4.7 step 4.1.4's append. CONSUMES `holder`. Configurable, because §8.1.6.4 step 7.3 REMOVES — a
   non-configurable slot would make the set one you can only join. */
static void outstanding_add(JSContext *ctx, JSValueConst promise, JSValue holder)
{
    JSAtom k = outstanding_atom(ctx);
    int r = JS_DefinePropertyValue(ctx, promise, k, holder, JS_PROP_CONFIGURABLE);
    JS_FreeAtom(ctx, k);
    CHECK(r >= 0, "unhandled rejections: OOM appending to the outstanding rejected promises weak set — a lost "
                  "member is a report this engine can never retract");
}

/* §8.1.6.4 step 7.3's removal. */
static void outstanding_remove(JSContext *ctx, JSValueConst promise)
{
    JSAtom k = outstanding_atom(ctx);
    int r = JS_DeleteProperty(ctx, promise, k, 0);
    JS_FreeAtom(ctx, k);
    /* SAFE TO ASSERT, unlike the append: the caller read the membership one line up and no page code runs in
       between, so a delete that finds nothing is this component disagreeing with itself rather than a page
       having deleted a marker it can name. */
    DCHECK(r > 0, "§8.1.6.4 step 7.3 removed a promise the membership test one line above had just found in "
                  "this global's outstanding rejected promises weak set, and the slot was not there — the read "
                  "and the write are naming different keys");
}

static void rejection_tracker(JSContext *ctx, JSValueConst promise, JSValueConst reason,
                              bool is_handled, void *opaque)
{
    uint32_t n, i;

    (void)opaque;
    DCHECK(g_ready, "the rejection tracker fired after its list was freed");
    n = list_len(ctx);
    if (!is_handled) {
        /* "add promise to the about-to-be-notified rejected promises list" */
        JSValue e = JS_NewArray(ctx);
        CHECK(!JS_IsException(e), "unhandled rejections: OOM recording a rejection — a dropped one is an error "
                                  "the page reported and this engine never saw");
        JS_SetPropertyUint32(ctx, e, 0, JS_DupValue(ctx, promise));
        JS_SetPropertyUint32(ctx, e, 1, JS_DupValue(ctx, reason));
        /* THE REJECTING REALM'S driver — quickjs hands this hook the ctx of the realm the promise belongs to,
           which is the one place that fact is known. */
        JS_SetPropertyUint32(ctx, e, 2, realm_value_get(ctx, g_notify_slot));
        JS_SetPropertyUint32(ctx, g_list, n, e);
        return;
    }
    /* §8.1.6.4 step 7.1: "If global's about-to-be-notified rejected promises list contains promise, then remove
       promise from that list and RETURN" — a handler attached after the rejection but before the checkpoint,
       which is ordinary code, not an error, and which is never reported and never announced. */
    for (i = 0; i < n; i++) {
        JSValue e = JS_GetPropertyUint32(ctx, g_list, i), p;
        bool same;
        if (!JS_IsObject(e)) { JS_FreeValue(ctx, e); continue; }
        p = JS_GetPropertyUint32(ctx, e, 0);
        same = JS_VALUE_GET_PTR(p) == JS_VALUE_GET_PTR(promise);
        JS_FreeValue(ctx, p);
        JS_FreeValue(ctx, e);
        if (same) { JS_SetPropertyUint32(ctx, g_list, i, JS_UNDEFINED); return; }
    }
    /* §8.1.6.4 steps 7.2-7.4. The promise is not awaiting notification, so the only remaining question is
       whether it was already NOTIFIED — i.e. whether this global's outstanding rejected promises weak set holds
       it — and if so the page is owed the retraction event.
       STEP 7.2 IS A NORMATIVE EARLY RETURN AND IS NOT AN IMPOSSIBLE STATE. It is the ORDINARY case, three ways
       over, and asserting here would abort on correct pages: the runtime reports the handled edge for EVERY
       rejected promise, including one handled in the same turn it rejected (never listed at all), one handled
       between the checkpoint and its queued task (the list was emptied at step 3 and step 4.1.4 has not run),
       and one handled by an `unhandledrejection` listener during its own fire (step 4.1.4 then declines to
       append it, which is how the spec spends its one un-retractable report). */
    {
        JSValue holder = outstanding_holder(ctx, promise), mine;
        bool contains;

        /* Step 7.2, first half — in NO global's set, which is every promise that was never notified about, and
           is the overwhelmingly common shape. Answered without asking the realm anything, so the ordinary path
           costs one own-slot read. */
        if (!JS_IsFunction(ctx, holder)) { JS_FreeValue(ctx, holder); return; }
        /* Step 7.2, second half — "THIS global's" set. The marker is the realm's own driver object, so identity
           of the two IS the membership test. A promise rejected in another realm carries that realm's marker
           and is not in this one's set, which is §8.1.6.4's own answer: global is the running script's settings
           object's global, never the promise's. */
        mine = realm_value_get(ctx, g_handled_slot);
        contains = JS_VALUE_GET_PTR(holder) == JS_VALUE_GET_PTR(mine);
        JS_FreeValue(ctx, holder);
        if (!contains) { JS_FreeValue(ctx, mine); return; }
        outstanding_remove(ctx, promise);                     /* step 7.3 */
        {
            /* Step 7.4: "queue a global task on the DOM manipulation task source given global to fire an event
               named rejectionhandled at global, with the promise attribute initialized to promise and the
               reason attribute initialized to promise.[[PromiseResult]]" — which is the `reason` this hook was
               handed, read off the promise by call_promise_rejection_tracker. */
            JSValueConst argv[2];
            argv[0] = promise; argv[1] = reason;
            JS_EnqueueCallTask(ctx, mine, 2, argv);
        }
        JS_FreeValue(ctx, mine);
    }
}

/* ---- PromiseRejectionEvent — §8.1.4.7's own interface ------------------------------------------------------ */
/* The two slots. Own properties under a private Symbol, read back by the getters — `promise` and `reason` are
   readonly attributes on the interface, never properties the page can assign. */
static JSValue pre_slot(JSContext *ctx, JSValueConst ev, const char *name)
{
    JSAtom k;
    JSValue slots, v;

    if (!JS_IsObject(ev)) return JS_UNDEFINED;
    k = JS_ValueToAtom(ctx, g_pre_key);
    if (k == JS_ATOM_NULL) return JS_UNDEFINED;
    /* AN OWN SLOT, never a property LOOKUP: a lookup walks the prototype chain and reaches the solver's
       absent-state seam, which would mint a concolic for a name nobody defined. */
    if (JS_GetOwnSlot(ctx, &slots, ev, k) <= 0) slots = JS_UNDEFINED;
    JS_FreeAtom(ctx, k);
    if (!JS_IsObject(slots)) { JS_FreeValue(ctx, slots); return JS_UNDEFINED; }
    v = JS_GetPropertyStr(ctx, slots, name);
    JS_FreeValue(ctx, slots);
    return v;
}

static JSValue js_pre_get(JSContext *ctx, JSValueConst this_val, int magic)
{
    return pre_slot(ctx, this_val, magic ? "reason" : "promise");
}

/* Mint one. `ev` is an Event this component then RE-POINTS at PromiseRejectionEvent.prototype, because the
   derived interface is a prototype chain and not a second kind of event object. */
static JSValue pre_new(JSContext *ctx, JSValue ev, JSValueConst promise, JSValueConst reason)
{
    JSValue slots;

    if (JS_IsException(ev)) return ev;
    {
        JSValue pre_proto = unhandled_rejection_proto(ctx);
        JS_SetPrototype(ctx, ev, pre_proto);
        JS_FreeValue(ctx, pre_proto);
    }
    slots = JS_NewObjectProto(ctx, JS_NULL);
    CHECK(!JS_IsException(slots), "PromiseRejectionEvent: OOM allocating its slots");
    JS_SetPropertyStr(ctx, slots, "promise", JS_DupValue(ctx, promise));
    JS_SetPropertyStr(ctx, slots, "reason", JS_DupValue(ctx, reason));
    {
        JSAtom k = JS_ValueToAtom(ctx, g_pre_key);
        CHECK(k != JS_ATOM_NULL, "the PromiseRejectionEvent slot key could not be reached");
        JS_DefinePropertyValue(ctx, ev, k, slots, 0);   /* not enumerable, not writable: an internal slot */
        JS_FreeAtom(ctx, k);
    }
    return ev;
}

/* `new PromiseRejectionEvent(type, init)`. Every conversion is DECLARED — the type is a DOMString, the init is
   a dictionary whose `promise` member the IDL marks `required` — so by the time this runs there is no page
   code left to reach and it reads the engine-built dictionary with an ordinary get. */
static const IdlArgType PRE_CTOR_ARGS[2] = { IDL_DOMSTRING, IDL_DICT };
/* `dictionary PromiseRejectionEventInit : EventInit { required object promise; any reason; }`, in §3.2.17's
   READ order and not in IDL declaration order — the two coincide here, which is exactly why the LEVEL is
   stated rather than left to the coincidence. `promise` and `reason` are declared on PromiseRejectionEventInit
   itself and the three above are EventInit's, so they are one level apart; with both at zero this table
   asserted that all five belong to ONE dictionary, an untrue statement idl_dict_order_check cannot see while
   `promise` happens to sort after `composed`. */
static const IdlDictMember PRE_INIT[] = {
    { "bubbles", IDL_BOOLEAN }, { "cancelable", IDL_BOOLEAN }, { "composed", IDL_BOOLEAN },
    { "promise", IDL_ANY, true, NULL, 1 }, { "reason", IDL_ANY, false, NULL, 1 },
};

static JSValue js_pre_ctor(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    JSValueConst init = argc > 1 ? argv[1] : JS_UNDEFINED;
    JSValue promise, reason, ev;
    const char *type;
    bool bubbles, cancelable;

    (void)magic;
    if (JS_IsUndefined(this_val))
        return JS_ThrowTypeError(ctx, "constructor PromiseRejectionEvent requires 'new'");
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "PromiseRejectionEvent constructor requires a type");
    bubbles = idl_dict_bool(ctx, init, "bubbles");
    cancelable = idl_dict_bool(ctx, init, "cancelable");
    type = JS_ToCString(ctx, argv[0]);   /* a real string by now: this cannot reach the page */
    if (!type) return JS_EXCEPTION;
    ev = event_new_untrusted(ctx, type, bubbles, cancelable);   /* §2.2: what the PAGE constructs is untrusted */
    JS_FreeCString(ctx, type);
    promise = idl_dict_get(ctx, init, "promise");
    reason = idl_dict_get(ctx, init, "reason");
    ev = pre_new(ctx, ev, promise, reason);
    JS_FreeValue(ctx, promise);
    JS_FreeValue(ctx, reason);
    return ev;
}

/* ---- the notification ------------------------------------------------------------------------------------- */
/* ONE rejection's notification: fire `unhandledrejection` at the global and, unless a listener cancelled it,
   hand the reason to whoever decides what an unreported rejection means. A machine because §2.9 dispatch is
   SYNCHRONOUS and its listeners are the page's code — the fire is a request, not a call from C. */
/* WHERE THIS MACHINE RESTS. §8.1.4.7's steps 1-3 clone and empty the list and step 4 QUEUES a global task; this
   machine is that task's body for ONE promise, which is step 4.1's loop. Its two halves are the two stages:
   4.1.2 fires the event (the page's listeners), 4.1.3 decides whether the rejection is reported. Minting the
   PromiseRejectionEvent is part of 4.1.2 and cannot suspend, so it shares that stage's entry. */
#define REJECT_NOTIFY_STAGES(X) \
    X(REJECT_EVENT, "HTML §8.1.4.7 step 4.1.2 (the PromiseRejectionEvent, cancelable, for this promise)") \
    X(REJECT_FIRE,  "HTML §8.1.4.7 steps 4.1.2-4.1.3 (fire unhandledrejection at the global; report unless it " \
                    "was canceled)")
enum { REJECT_NOTIFY_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const REJECT_NOTIFY_STEPS[] = { REJECT_NOTIFY_STAGES(JS_STEP_STAGE_LABEL) NULL };

typedef struct JSRejectNotify {
    JSStepHdr hdr;      /* FIRST — the driver writes the def and the operand bounds through it */
    uint8_t   fphase;   /* the fire request's own phase */
    bool      not_canceled;
    JSValue   ev;       /* the event, minted once and re-read after the dispatch (owned) */
    EventFireCb   cb;    /* the fire request's buffer — event_target_fire_run needs four slots */
} JSRejectNotify;

static void js_reject_notify_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSRejectNotify *s = st;
    int k;
    v->val(ctx, &s->ev);
    STEP_CB_FOREACH(s->cb, k)
        v->val(ctx, &s->cb[k]);
}

static int js_reject_notify_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    JSRejectNotify *s = st;
    JSValueConst promise = step_arg(&s->hdr, 0), reason = step_arg(&s->hdr, 1);
    JSValue global;
    int r, k;

    if (s->hdr.stage == REJECT_EVENT) {
        s->ev = JS_UNDEFINED;
        STEP_CB_FOREACH(s->cb, k) s->cb[k] = JS_UNDEFINED;
        /* §8.1.4.7 step 4.1.1: "If p.[[PromiseIsHandled]] is true, then CONTINUE." The check is inside the
           QUEUED TASK, not at the checkpoint that queued it, and that gap is real work: `var p = f();` followed
           by a `.catch` in a later task is ordinary correct code, and this engine widens the gap further by
           queueing one task PER PROMISE — so a listener on the first rejection can attach a handler to the
           second before the second's task runs. Without this line every one of those is reported, and a
           reported rejection is a `pageErrors` entry about a page that did nothing wrong. */
        if (JS_IsPromiseHandled(ctx, promise)) { JS_FreeValue(ctx, cb_result); return JS_STEP_DONE; }
        STEP_GOTO(s->hdr.stage, REJECT_FIRE, &s->fphase, NULL);
        /* §8.1.4.7: `unhandledrejection` does not bubble and IS cancelable — the cancel is the whole point. */
        s->ev = pre_new(ctx, event_new(ctx, "unhandledrejection", false, true), promise, reason);
        if (JS_IsException(s->ev)) { s->ev = JS_UNDEFINED; JS_FreeValue(ctx, cb_result); return JS_STEP_ABRUPT; }
    }
    DCHECK(s->hdr.stage == REJECT_FIRE, "the rejection notification resumed into a stage §8.1.4.7 does not have");
    global = JS_GetGlobalObject(ctx);
    r = event_target_fire_run(ctx, &s->fphase, STEP_CB(s->cb), global, s->ev, JS_UNDEFINED, cb_result, &s->not_canceled,
                              out_cb, out_argc);
    JS_FreeValue(ctx, global);
    if (r > 0) return r;
    /* Step 4.1.3: "If notCanceled is true, then the user agent MAY report p.[[PromiseResult]] to a developer
       console." A page that cancels is doing its own reporting, which is code with a fetch in it — the endpoint
       that reporter posts to is learned either way. Gated on the CANCEL and not on handled-ness, which is why
       step 4.1.4 below can still decline to append a promise this line has already reported. */
    if (s->not_canceled && g_report)
        g_report(ctx, reason, REJECTION_REPORTED);
    /* Step 4.1.4: "If p.[[PromiseIsHandled]] is FALSE, then append p to global's outstanding rejected promises
       weak set." Checked AGAIN, after the listeners ran: one of them may have attached a handler, and a promise
       that leaves this task handled is not owed a `rejectionhandled` — it is the one case where the spec
       reports and never retracts. The global is THIS ctx's, which is the rejecting realm's: the driver object
       this machine runs under was minted per realm, and js_call_c_function enters a C function in the realm
       that defined it. */
    if (!JS_IsPromiseHandled(ctx, promise))
        outstanding_add(ctx, promise, realm_value_get(ctx, g_handled_slot));
    return JS_STEP_DONE;
}

static const JSTrampStepDef js_reject_notify_def = {
    sizeof(JSRejectNotify), js_reject_notify_step, NULL, 0, .visit = js_reject_notify_visit,
    .algorithm = "HTML §8.1.4.7 notify about rejected promises — step 4's queued task, one promise",
    .steps = REJECT_NOTIFY_STEPS
};

/* ---- the RETRACTION — §8.1.6.4 step 7.4's queued task ------------------------------------------------------ */
/* The other edge, and the one this component reported and never announced: a promise this global already told
   the page about has since been handled. A separate machine rather than a mode on the one above, because it is
   a separate algorithm in a separate section — non-cancelable (there is nothing to prevent; the report already
   happened), with no report step and no set to append to. Its two stages are 7.4's single sentence split at its
   one suspension point, which is the dispatch: §2.9 is synchronous and its listeners are the page's code. */
#define REJECT_HANDLED_STAGES(X) \
    X(HANDLED_EVENT, "HTML §8.1.6.4 step 7.4 (the PromiseRejectionEvent for a promise that has since been " \
                     "handled)") \
    X(HANDLED_FIRE,  "HTML §8.1.6.4 step 7.4 (fire rejectionhandled at the global)")
enum { REJECT_HANDLED_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const REJECT_HANDLED_STEPS[] = { REJECT_HANDLED_STAGES(JS_STEP_STAGE_LABEL) NULL };

typedef struct JSRejectHandled {
    JSStepHdr hdr;      /* FIRST — the driver writes the def and the operand bounds through it */
    uint8_t   fphase;
    /* §8.1.6.4 fires a NON-cancelable event, so this is written by the fire and read by nobody. It exists
       because event_target_fire_run's contract requires somewhere to put it, which is a different fact from
       this algorithm having a use for it — stating that here is what stops the next reader adding a branch on
       a value the spec gives no meaning. */
    bool      not_canceled;
    JSValue   ev;
    EventFireCb   cb;
} JSRejectHandled;

static void js_reject_handled_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSRejectHandled *s = st;
    int k;
    v->val(ctx, &s->ev);
    STEP_CB_FOREACH(s->cb, k)
        v->val(ctx, &s->cb[k]);
}

static int js_reject_handled_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    JSRejectHandled *s = st;
    JSValueConst promise = step_arg(&s->hdr, 0), reason = step_arg(&s->hdr, 1);
    JSValue global;
    int r, k;

    if (s->hdr.stage == HANDLED_EVENT) {
        s->ev = JS_UNDEFINED;
        STEP_CB_FOREACH(s->cb, k) s->cb[k] = JS_UNDEFINED;
        /* THE CONSOLE'S OWN WITHDRAWAL, AND IT IS NOT A STEP OF §8.1.6.4. The standard says only that this
           global tells the PAGE, because a browser's console is a scrollback a person reads and a line already
           printed there is a line already read. This engine's console is a DOCUMENT that is composed after the
           fact and reported as a finding, so a report it never takes back is a fabricated finding about a page
           that did nothing wrong — which is worth more than the fidelity of leaving it standing.
           BEFORE THE FIRE, NOT AFTER IT, and the difference is not stylistic. The fact this withdraws on was
           established by step 7.3 — the promise was REMOVED from the outstanding rejected promises weak set,
           which is what committed this task — so it does not depend on the page's `rejectionhandled`
           listeners running, or on the event minting below succeeding. Putting it after the dispatch would
           make an abrupt completion in a listener leave the false entry standing, which is the one outcome
           this exists to prevent. §8.1.4.7 step 4.1.3's report is after ITS fire for the opposite reason: that
           event is cancelable and the report is gated on the cancel, so it has something to wait for. */
        if (g_report)
            g_report(ctx, reason, REJECTION_RETRACTED);
        STEP_GOTO(s->hdr.stage, HANDLED_FIRE, &s->fphase, NULL);
        /* NO re-check of [[PromiseIsHandled]] here, and its absence is the spec's: §8.1.6.4 step 7.3 REMOVED
           the promise from the set before queueing this, so the fire is already committed. The flag is a
           one-way latch besides — nothing un-handles a promise — so a re-check could only ever be true. */
        s->ev = pre_new(ctx, event_new(ctx, "rejectionhandled", false, false), promise, reason);
        if (JS_IsException(s->ev)) { s->ev = JS_UNDEFINED; JS_FreeValue(ctx, cb_result); return JS_STEP_ABRUPT; }
    }
    DCHECK(s->hdr.stage == HANDLED_FIRE, "the rejection-handled fire resumed into a stage §8.1.6.4 does not have");
    global = JS_GetGlobalObject(ctx);
    r = event_target_fire_run(ctx, &s->fphase, STEP_CB(s->cb), global, s->ev, JS_UNDEFINED, cb_result,
                              &s->not_canceled, out_cb, out_argc);
    JS_FreeValue(ctx, global);
    if (r > 0) return r;
    return JS_STEP_DONE;
}

static const JSTrampStepDef js_reject_handled_def = {
    sizeof(JSRejectHandled), js_reject_handled_step, NULL, 0, .visit = js_reject_handled_visit,
    .algorithm = "HTML §8.1.6.4 HostPromiseRejectionTracker — step 7.4's queued rejectionhandled fire",
    .steps = REJECT_HANDLED_STEPS
};

int unhandled_rejection_notify(JSContext *ctx)
{
    uint32_t n, i;
    int queued = 0;

    DCHECK(g_ready, "an unhandled-rejection checkpoint ran before the tracker was installed");
    n = list_len(ctx);
    for (i = 0; i < n; i++) {
        JSValue e = JS_GetPropertyUint32(ctx, g_list, i);
        JSValueConst argv[2];
        JSValue promise, reason, notify;

        if (!JS_IsObject(e)) { JS_FreeValue(ctx, e); continue; }
        promise = JS_GetPropertyUint32(ctx, e, 0);
        reason = JS_GetPropertyUint32(ctx, e, 1);
        notify = JS_GetPropertyUint32(ctx, e, 2);   /* the REJECTING realm's driver, recorded with the entry */
        DCHECK(JS_IsFunction(ctx, notify), "a recorded rejection lost the driver of the realm it rejected in");
        argv[0] = promise; argv[1] = reason;
        /* §8.1.4.7 step 4: "QUEUE A GLOBAL TASK ON THE DOM MANIPULATION TASK SOURCE given global to run the
           following step". It was a microtask, which is a different position in HTML §8.1.7's event loop and
           not a smaller one — a microtask runs inside the enqueuing flow's own checkpoint, so a
           `unhandledrejection` handler's own promise reaction was queued BEHIND the remaining notifications
           instead of running before the next one, which is what the spec's one-task-then-checkpoint shape
           gives. */
        JS_EnqueueCallTask(ctx, notify, 2, argv);
        JS_FreeValue(ctx, notify);
        JS_FreeValue(ctx, promise);
        JS_FreeValue(ctx, reason);
        JS_FreeValue(ctx, e);
        queued++;
    }
    /* CLEARED HERE, not when the fires complete: the list is "about to be notified about", and these now are. */
    JS_SetPropertyStr(ctx, g_list, "length", JS_NewInt32(ctx, 0));
    return queued;
}

int unhandled_rejection_pending(JSContext *ctx)
{
    uint32_t n, i;
    int live = 0;

    DCHECK(g_ready, "the rejected-promise list was counted before the tracker was installed");
    n = list_len(ctx);
    /* THE SAME LIVE-ENTRY TEST `notify` USES, and it is the whole content of this function: a slot the handled
       edge emptied is `undefined` IN PLACE (the identity of a slot is what that edge finds it by, so the list
       is never compacted), and counting those would report a rejection somebody already caught. One predicate,
       two readers, so a caller asserting "nothing is owed" and the loop that owes it cannot drift. */
    for (i = 0; i < n; i++) {
        JSValue e = JS_GetPropertyUint32(ctx, g_list, i);
        if (JS_IsObject(e)) live++;
        JS_FreeValue(ctx, e);
    }
    return live;
}

void unhandled_rejection_init(JSContext *ctx)
{
    DCHECK(!g_ready, "unhandled_rejection_init ran twice — one instance is one document");
    /* Built at init so it belongs to the pre-boot BASELINE: a write during a flow is captured by the heap COW.
       A list allocated lazily inside a flow would be that flow's private object and no sibling would see it. */
    g_list = JS_NewArray(ctx);
    CHECK(!JS_IsException(g_list), "the rejected-promise list could not be allocated");
    g_pre_key = JS_NewSymbol(ctx, "promiseRejectionSlots", false);
    CHECK(!JS_IsException(g_pre_key), "the PromiseRejectionEvent slot key allocation failed");
    /* §8.1.3.3's weak set has no allocation of its own — see g_out_key. This is the whole of it. */
    g_out_key = JS_NewSymbol(ctx, "outstandingRejectedPromise", false);
    CHECK(!JS_IsException(g_out_key), "the outstanding-rejected-promises marker key allocation failed");
    {
        JSClassDef d = { "PromiseRejectionEvent" };
        JS_NewClassID(JS_GetRuntime(ctx), &g_pre_class);
        JS_NewClass(JS_GetRuntime(ctx), g_pre_class, &d);
    }
    /* The notification driver is a step function nobody installs, so a page can neither see it nor replace it. */
    g_notify_stepid = JS_RegisterStepDef(JS_GetRuntime(ctx), &js_reject_notify_def);
    CHECK(g_notify_stepid >= 0, "no step id for the rejection-notification driver");
    g_notify_slot = realm_value_declare(ctx, "§8.1.4.7 notifyRejected");
    g_handled_stepid = JS_RegisterStepDef(JS_GetRuntime(ctx), &js_reject_handled_def);
    CHECK(g_handled_stepid >= 0, "no step id for the rejection-handled driver");
    g_handled_slot = realm_value_declare(ctx, "§8.1.6.4 fireRejectionHandled");
    g_ready = 1;
    JS_SetHostPromiseRejectionTracker(JS_GetRuntime(ctx), rejection_tracker, NULL);
    /* THE CONSTRUCTOR'S DECLARATION IS THE AGENT'S — one pool entry per member; every realm's interface object
       is built from that same one. */
    g_id_pre_ctor = idl_method_id_dict(ctx, PRE_CTOR_ARGS, 2, PRE_INIT,
                                       (int)(sizeof(PRE_INIT) / sizeof(PRE_INIT[0])), js_pre_ctor, 0);
    agent_state_flag("unhandled_rejection", &g_ready, "the declaration latch");
    agent_state_value("unhandled_rejection", &g_list, "§8.1.3.3's about-to-be-notified rejected promises list");
    agent_state_value("unhandled_rejection", &g_pre_key, "PromiseRejectionEvent's internal-slot key");
    agent_state_value("unhandled_rejection", &g_out_key,
                      "§8.1.3.3's outstanding rejected promises weak set — its marker key IS the set");
    agent_state_id("unhandled_rejection", &g_notify_slot, "§8.1.4.7's notifyRejected realm slot");
    agent_state_id("unhandled_rejection", &g_notify_stepid, "§8.1.4.7's notification driver machine");
    agent_state_id("unhandled_rejection", &g_handled_slot, "§8.1.6.4's fireRejectionHandled realm slot");
    agent_state_id("unhandled_rejection", &g_handled_stepid, "§8.1.6.4's rejectionhandled driver machine");
    agent_state_class("unhandled_rejection", &g_pre_class, "§8.1.4.7's PromiseRejectionEvent class");
    agent_state_id("unhandled_rejection", &g_id_pre_ctor, "§8.1.4.7's PromiseRejectionEvent constructor declaration");
    /* THE REPORT HOOK WAS GIVEN BACK BY THE RELEASE AND DECLARED BY NOBODY — a slot on the released side of a
       pairing that has only one side is a slot agent_state_check_released holds nothing to assert, which is
       the same silence core/agent_state.h found on the other arm. It is a pointer INTO another component
       (solver/engine.c installs it, and the solver is released after the platform), so it is exactly the kind
       agent_state_ptr exists for. */
    agent_state_ptr("unhandled_rejection", &g_report, "the host edge an unreported rejection is reported through");
    realm_declare_intrinsic(unhandled_rejection_install_proto);
}

/* §8.1.7.2's PROTOTYPE FOR ONE REALM. `interface PromiseRejectionEvent : Event` — a real chain, so
   `e instanceof Event` and every Event member hold on one of these, and it chains to THIS realm's
   Event.prototype because a chain to another document's is the same defect one link up. */
void unhandled_rejection_install_proto(JSContext *ctx)
{
    JSValue proto, prev, base;

    DCHECK(g_ready, "a realm asked for PromiseRejectionEvent.prototype before the interface was declared");
    prev = JS_GetClassProto(ctx, g_pre_class);
    DCHECK(JS_IsNull(prev), "unhandled_rejection_install_proto ran twice in one realm");
    JS_FreeValue(ctx, prev);
    base = event_proto(ctx);
    proto = JS_NewObjectProto(ctx, base);
    JS_FreeValue(ctx, base);
    CHECK(!JS_IsException(proto), "PromiseRejectionEvent.prototype could not be allocated");
    idl_interface_tag(ctx, proto, "PromiseRejectionEvent");
    idl_install_accessor(ctx, proto, "promise", js_pre_get, 0, -1);
    idl_install_accessor(ctx, proto, "reason", js_pre_get, 1, -1);
    JS_SetClassProto(ctx, g_pre_class, proto);
    {
        JSValue fn = JS_NewCFunction2(ctx, NULL, "notifyRejected", 2, JS_CFUNC_step, g_notify_stepid);
        CHECK(!JS_IsException(fn), "the rejection-notification driver could not be allocated");
        realm_value_set(ctx, g_notify_slot, fn);
    }
    {
        /* AND THE RETRACTION'S, which is this realm's IDENTITY as well as its driver: §8.1.3.3's weak set is
           spelled as this object stamped on the promises this global has notified about, so a realm without it
           is a realm whose set can never be joined and whose reports can never be taken back. */
        JSValue fn = JS_NewCFunction2(ctx, NULL, "fireRejectionHandled", 2, JS_CFUNC_step, g_handled_stepid);
        CHECK(!JS_IsException(fn), "the rejection-handled driver could not be allocated");
        realm_value_set(ctx, g_handled_slot, fn);
    }
}

JSValue unhandled_rejection_proto(JSContext *ctx)
{
    JSValue proto = JS_GetClassProto(ctx, g_pre_class);
    DCHECK(!JS_IsNull(proto),
           "PromiseRejectionEvent.prototype was asked for in a realm that never ran its install");
    return proto;   /* OWNED */
}

void unhandled_rejection_install(JSContext *ctx, JSValueConst global)
{
    JSValue ctor;

    DCHECK(g_ready, "PromiseRejectionEvent was installed before its prototype was built");
    ctor = idl_step_constructor(ctx, "PromiseRejectionEvent", g_id_pre_ctor);
    CHECK(!JS_IsException(ctor), "the PromiseRejectionEvent interface object could not be allocated");
    {
        JSValue proto = unhandled_rejection_proto(ctx);
        JS_SetConstructor(ctx, ctor, proto);
        JS_FreeValue(ctx, proto);
    }
    JS_SetPropertyStr(ctx, (JSValue)global, "PromiseRejectionEvent", ctor);
}

/* THE RUNTIME, NOT A REALM, and it is the platform's release column that calls it — see core/platform.h. What
   this holds is AGENT state (§8.1.3.3's about-to-be-notified rejected promises list and the slot key beside
   it), so
   the thing it is released against is the agent, which is a JSRuntime; taking a JSContext is what made it a
   line each host had to remember, and the WPT runner did not. That cost every file in that gate its result: the
   list is a live Array held by a C static, so JS_FreeRuntime's gc_obj_list walk found it and aborted the run
   after the test had already passed. */
void unhandled_rejection_free(JSRuntime *rt)
{
    /* NOT `if (!g_ready) return;`. The release is the inverse of the DECLARATION and rides the same row of the
       same list, so a release without a declaration is not a state to tolerate — it is a host that reached
       teardown without having built this browser, which core/platform.c asserts from its end too. */
    DCHECK(g_ready, "§8.1.3.3's rejected-promise list was released in an agent that never declared it — the release "
                    "column is the inverse of the declare column, so reaching here without an init means this "
                    "component was torn down by something that is not the platform's one list");
    /* The tracker goes FIRST: teardown frees promises, and a rejected one still on the runtime's list would
       fire the callback into a list this call is about to release. */
    JS_SetHostPromiseRejectionTracker(rt, NULL, NULL);
    g_ready = 0;
    JS_FreeValueRT(rt, g_list);   /* the prototypes are the REALMS' — each is released with its context */
    JS_FreeValueRT(rt, g_pre_key);
    /* THE WEAK SET NEEDS NOTHING RELEASED BUT ITS KEY, which is the property that made it weak: every member is
       a marker on a promise, so the members go when the promises do and there is no container to walk. */
    JS_FreeValueRT(rt, g_out_key);
    g_list = g_pre_key = g_out_key = JS_UNDEFINED;   /* the per-realm drivers are released with their contexts */
    /* THE TWO REGISTRATIONS, GIVEN BACK. They name a realm slot and a step machine in a runtime that is going
       away — but they are also what the init above would find set, and this file's own paragraph about the
       early-return is the argument for why leaving them is not harmless. */
    g_notify_slot = g_notify_stepid = -1;
    g_handled_slot = g_handled_stepid = -1;
    /* AND THE CLASS WITH ITS CONSTRUCTOR'S POOL ENTRY. The class is registered in `rt`, which is going away;
       the pool entry names a member declaration of an agent that is going away. Neither is freed by anything —
       what makes leaving them wrong is that both are READ by the next agent, the class id by `JS_NewClassID`
       (which hands back a non-zero slot unchanged) and the pool entry by `idl_step_constructor` at every
       realm. */
    g_pre_class = 0;
    g_id_pre_ctor = -1;
    g_report = NULL;
}
