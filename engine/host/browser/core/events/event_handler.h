/* HTML §8.1.8.1 Event handlers — THE EVENT HANDLER PROCESSING ALGORITHM. See event_handler.c.
 *
 * AN EVENT HANDLER IS NOT A LISTENER, AND THIS FILE IS THE WHOLE OF THE DIFFERENCE. §8.1.7 registers ONE
 * ordinary listener the first time `el.onfoo` is set, and DOM §2.9 "inner invoke" step 2.11 invokes it — but
 * what it invokes is not the page's function. It is "a callback that TYPE-CHECKS and RUNS the steps of the event
 * handler processing algorithm", and those steps differ from a listener's in two observable ways:
 *
 *   (1) THE ARGUMENTS. A listener always gets one argument, the event. A handler gets one too — EXCEPT for
 *       §8.1.8.1's `special error event handling`, where the handler is invoked with FIVE: « event's message,
 *       event's filename, event's lineno, event's colno, event's error ». So `window.onerror = (m,f,l,c,e)=>…`
 *       and `window.addEventListener("error", ev=>…)` receive DIFFERENT things from the same dispatch, and a
 *       page that reads `arguments[0].message` off the first one is reading a character out of a string.
 *   (2) THE RETURN VALUE. DOM §2.9 discards a listener's — correctly, and that discard must not be touched.
 *       §8.1.8.1 step 6 READS a handler's, and it reads it THREE different ways: `false` cancels in the general
 *       case, `true` cancels under special error event handling, and any non-null DOMString cancels (and fills
 *       `returnValue`) for a BeforeUnloadEvent. Three cases, one for historical reasons apiece, and the general
 *       one is most of the web: `<form onsubmit="return validate()">` is how forms have been cancelled since
 *       before preventDefault existed.
 *
 * THE JOIN WITH §8.1.4.6. Report-an-exception's step 7 skips its own reporting when the `error` event it fired
 * was cancelled. Until step 6 ran, the ONLY way to cancel was preventDefault(), so `window.onerror = () => true`
 * — which is how the overwhelming majority of shipped error handlers say "handled" — silently did nothing and
 * every such page was reported as having an unhandled exception. This file is what makes the two spellings
 * agree, and it agrees through the SAME primitive: DOM §2.2's set the canceled flag, never a second write.
 *
 * IT IS NOT A STEP MACHINE, IT IS A SUB-ALGORITHM OF ONE, for the reason report_exception.h gives: its one
 * caller is already a machine (DOM §2.9's dispatch walk), so the work record is the CALLER's — visited by the
 * caller's visit, torn down by the caller's teardown — and this file borrows the caller's request buffer and
 * call phase rather than growing a second copy of both. It can park TWICE per invocation, on the handler's own
 * body and on the Web IDL return-type coercion, which is why the record carries a stage at all. */
#ifndef ENGINE_HOST_BROWSER_CORE_EVENTS_EVENT_HANDLER_H
#define ENGINE_HOST_BROWSER_CORE_EVENTS_EVENT_HANDLER_H
#include <stdbool.h>
#include <stdint.h>

#include "quickjs.h"
#include "quickjs-step.h"

/* WHAT ONE INVOCATION HOLDS ACROSS A SUSPENSION. Two fields, and both are here because a handler's body is the
   page's code: `stage` says which of §8.1.8.1's steps the resume belongs to, and `rv` is step 5's `return value`
   waiting for step 6 — across the ToString the beforeunload arm's IDL return type performs, which is itself the
   page's code and parks. `stage` is ZERO exactly when no invocation is in flight, which is what the caller's
   own resume routing reads. */
typedef struct {
    uint8_t stage;
    JSValue rv;
} EventHandlerWork;

/* The record, before anything can throw. Every entry point that owns one calls this at its own init. */
void event_handler_work_start(EventHandlerWork *w);
/* The record's ONE reference, named for the caller's visit — a fork mid-handler must not hand two arms one
   return value. The caller's declaration is what discharges it. */
void event_handler_work_visit(JSContext *ctx, EventHandlerWork *w, JSStepVisit *v);

/* §8.1.8.1's EVENT HANDLER PROCESSING ALGORITHM, steps 4-6, for `event` at its own currentTarget.
 *
 * Steps 2-3 are the CALLER's and that split is the spec's own shape, not a shortcut: DOM §2.9 "inner invoke"
 * has to get the current value of the handler anyway — a handler slot whose handler is null is not a listener
 * at all and the walk skips it without invoking anything — so asking twice would be two answers to one
 * question. What arrives here is step 3's already-non-null `callback`.
 *
 * STEP 1 IS NOT PERFORMED, AND ITS SUBPROBLEM IS NAMED RATHER THAN APPROXIMATED. "If scripting is disabled for
 * eventTarget, then return" is HTML §8.1.3.4 Enabling and disabling scripting's predicate for a PLATFORM
 * OBJECT, and it is a disjunction of three: scripting disabled for the object's RELEVANT SETTINGS OBJECT (which
 * bottoms out in that settings object's global's associated Document's active sandboxing flag set carrying the
 * sandboxed scripts browsing context flag), the object implementing Node with its NODE DOCUMENT's browsing
 * context null, and the object implementing Window with its associated Document's browsing context null. The
 * middle term is what makes half a predicate worse than none here: a same-origin script can set
 * `otherFrame.contentDocument.body.onclick` in a frame whose own scripting is disabled, so the answer is about
 * the TARGET's settings object and not about the realm the dispatch is running in — and answering it from the
 * running realm's Document would be a plausible datum rather than a measurement. The ordered subproblem is
 * therefore §8.1.3.4's platform-object predicate as a component, over a relevant-settings-object lookup this
 * engine does not have; build that, and step 1 is one call at the top of the algorithm below.
 *
 * `name` is §8.1.8.1's own argument, "a string representing the name of an event handler", and it is what
 * selects the Web IDL callback function type §8.1.8.2.1 declares for the attribute — which is the whole of what
 * decides whether step 5's `return value` is coerced. It is NOT derivable from the event: `onbeforeunload` is
 * the one attribute whose type is not `any`-returning, and the standard's note in step 6 is precisely about the
 * case where the event and the attribute disagree.
 *
 * `cphase` and `cb`/`cb_cap` are the CALLER's call-request phase and buffer, forwarded. The buffer must hold
 * 2 + 5 slots, because the five-argument invocation is what it is; a caller whose buffer is smaller aborts on
 * the first special error event rather than scribbling over the field next door.
 *
 *   > 0  — parked; the caller returns the code as it stands (a call, a coercion, a fork, an unknown).
 *   0    — the invocation is complete and step 6 has been performed. `w->stage` is back to zero.
 *   -1   — ABRUPT: the handler threw, or the return-type coercion did. §8.1.8.1 step 5 invokes with "rethrow",
 *          so the completion propagates to DOM §2.9's inner invoke, which reports it and carries on down the
 *          listener list — the caller's `catches_abrupt` arm, exactly as a plain listener's throw arrives. */
int event_handler_run(JSContext *ctx, EventHandlerWork *w, JSStepHdr *hdr, uint8_t *cphase,
                      JSValue *cb, int cb_cap, JSValueConst callback, JSValueConst event,
                      const char *name, JSValue in, JSValue **out_cb, int *out_argc);

#endif
