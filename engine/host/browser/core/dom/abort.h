/* AbortController / AbortSignal — DOM §3.2. See abort.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_DOM_ABORT_H
#define ENGINE_HOST_BROWSER_CORE_DOM_ABORT_H
#include <stdbool.h>

#include "quickjs.h"
#include "quickjs-step.h"
#include "core/events/event_target.h"   /* EventFireCb — the width of the fire request this machine parks on */

void abort_init(JSContext *ctx);
/* §3.2's two prototypes for ONE realm — declared into core/realm.h's list. */
void abort_install_protos(JSContext *ctx);
/* PER REALM. OWNED: the caller frees. */
JSValue abort_signal_proto(JSContext *ctx);                                 /* the private slot key (install time) */
void abort_install(JSContext *ctx, JSValueConst global);         /* AbortController + AbortSignal */
void abort_free(JSContext *ctx);                                 /* release the slot key this component owns */

/* A FRESH, UNABORTED SIGNAL.
 *
 * Streams §5.4 gives every WritableStreamDefaultController an AbortController whose signal the page reads as
 * `controller.signal`, and §5.2's abort() signals it. That is the DOM's AbortSignal and not a second one — a
 * page passes `controller.signal` straight to `fetch`, so anything else would be an object that looks like one. */
JSValue abort_signal_new(JSContext *ctx);

/* §3.2 "SIGNAL ABORT" — THE WHOLE OPERATION, AS A REQUEST, and there is only this one.
 *
 * The spec's steps are: return if already aborted; set the flag and the reason (an undefined one becoming an
 * "AbortError" DOMException); RUN EACH OF THE SIGNAL'S ABORT ALGORITHMS; empty that list; then fire `abort` at
 * the signal. Two of those five steps run code — the algorithms and the listeners — so the operation cannot be
 * a plain C call, and it was previously split into a `bool` that did the first two steps and left each caller
 * to fire for itself. That split had no place to put the algorithms at all, which is exactly why §4.2.4's
 * pipeTo could not register one: a caller cannot run a list the operation does not know it has.
 *
 * The work record is the CALLING machine's, like every other request here, so a fork copies it and a suspension
 * inside a listener resumes in the same stage. The caller visits and releases it.
 *   JS_STEP_CALL = return it, 0 = the operation has finished, -1 = it threw.
 *
 * THE OPERATION RUNS "THE ABORT STEPS" FOR MORE THAN ONE SIGNAL. §3.2 splits signal abort into a REASON pass
 * (steps 3-4: every non-aborted DEPENDENT signal takes this signal's abort reason, and the ones that did are
 * collected) and an EFFECT pass (steps 5-6: "run the abort steps" — the algorithms, then the `abort` event —
 * for this signal first and then for each collected dependent). The split is OBSERVABLE: a listener on the
 * source already sees every dependent reading `aborted === true`, because the reason pass finished before the
 * first listener ran. So the record carries a LIST of targets rather than one signal, and `j` says which of
 * them the walk is inside. */
typedef struct {
    uint8_t  stage;
    uint8_t  phase;    /* step_call_run's / event_target_fire_run's, for whichever call is in flight */
    uint32_t i;        /* how far through the CURRENT target's algorithm list */
    uint32_t j;        /* which target's abort steps are running — 0 is the signal itself */
    JSValue  targets;  /* §3.2 steps 5-6: the signal, then each dependentSignal that took its reason (owned) */
    JSValue  algos;    /* the current target's algorithm snapshot (owned) */
    JSValue  ev;       /* the current target's `abort` event, held across the dispatch (owned) */
    EventFireCb  cb;    /* the request buffer: four slots, because the fire needs [this, fn, target, event] */
} AbortSignalWork;

void abort_signal_work_start(AbortSignalWork *w);
void abort_signal_work_visit(JSContext *ctx, AbortSignalWork *w, JSStepVisit *v);
void abort_signal_work_release(JSContext *ctx, AbortSignalWork *w);
int  abort_signal_run(JSContext *ctx, AbortSignalWork *w, JSValueConst sig, JSValueConst reason,
                      JSValue in, JSValue **out_cb, int *out_argc);

/* §3.2's "add an algorithm to an AbortSignal" and its removal. An ALGORITHM is not a listener: it runs BEFORE
   the `abort` event, it is not visible to the page, and it is dropped once it has run. Streams §4.2.4 adds one
   so a pipe can shut down when the signal fires, and removes it when the pipe finalizes — which is why the
   removal exists at all. Adding to an ALREADY-ABORTED signal does nothing, as the spec says. */
void abort_signal_add_algorithm(JSContext *ctx, JSValueConst sig, JSValueConst fn);
void abort_signal_remove_algorithm(JSContext *ctx, JSValueConst sig, JSValueConst fn);

/* IS THIS SIGNAL ABORTED, and WITH WHAT — §4.2.4 step 5 tests the flag before it registers, and its abort
   algorithm reads the reason. Both go through the concolic-safe test, so a signal whose flag is unknown forks
   rather than silently taking one arm. Returns 0/1, and never -1: a caller that must branch has already been
   given a definite answer by solver_decide. */
bool    abort_signal_aborted(JSContext *ctx, JSValueConst sig);

/* IS THIS AN AbortSignal? Web IDL's `AbortSignal signal` member is an interface type, so a value that is not
   one is a TypeError — Streams §4.2.4's options carry one, and a union arm is a brand test. */
bool    abort_signal_is(JSContext *ctx, JSValueConst v);
JSValue abort_signal_reason(JSContext *ctx, JSValueConst sig);   /* dup'd */

/* §3.2 "CREATE A DEPENDENT ABORT SIGNAL" — one signal that aborts when ANY of `signals` does, with that one's
 * reason. It is a real §3.2 concept and not a convenience: it is the WHOLE of `AbortSignal.any(signals)`, whose
 * method steps are "return the result of creating a dependent abort signal from signals using AbortSignal and
 * the current realm" and nothing else. The Observable standard's promise-returning operators reach the same
 * algorithm from C — each holds an AbortController of its own AND accepts the caller's `options.signal`, and the
 * subscription they open has to end when EITHER fires: `every()` aborts its own controller the moment the
 * predicate answers false, while the caller's signal may abort it first.
 *
 * THIS ENTRY IS THE C-VECTOR ADAPTER, for those callers whose list is two locals. `any()` converts a page's
 * ITERABLE, suspending at every element, so the algorithm itself works over the list as a JS Array — the form a
 * parked flow's snapshot carries and its COW delta captures. One algorithm, reached two ways; see abort.c.
 *
 * WHY IT CANNOT BE "add an algorithm to each that aborts the result". §3.2 states the propagation as STATE, not
 * as a listener: step 4 of signal abort sets each dependent's abort reason BEFORE any of the abort steps run,
 * so by the time the source's own algorithms and `abort` listeners execute, every dependent already reads
 * `aborted === true`. An algorithm-based imitation aborts the dependent DURING the source's algorithm walk,
 * which is one turn late and visible to the page. It is also why a dependent signal FLATTENS: a dependent built
 * over a dependent takes the SOURCE signals of the inner one, so a chain of operators is one hop deep however
 * many of them there are, and step 4.2's assert says the flattening is total.
 *
 * `signals` is BORROWED, the answer is OWNED. */
JSValue abort_signal_dependent_new(JSContext *ctx, JSValueConst *signals, int n);

#endif
