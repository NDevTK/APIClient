/* RESIZE OBSERVER — the `ResizeObserver` interface (§2.1), its processing model (§3) and the five seams HTML's
 * update-the-rendering step 16 reaches it through. See resize_observer.c.
 *
 * WHY THIS MATTERS TO A SOLVER. `new ResizeObserver(cb).observe(el)` is how a real bundle wires responsive
 * layout, a chart that re-draws when its container changes, a virtualised list that re-measures its viewport, a
 * sticky header that recomputes its offsets — and the fetches behind all of those live INSIDE the callback.
 * Measured, by asking this engine which platform global names answer `false` to `in globalThis` and then
 * counting unambiguous construction sites across twelve real frozen bundles: `ResizeObserver` was TOP of that
 * ranking — six `new ResizeObserver(...)` sites across three of the twelve, and GUARDED in only two of the
 * three, so in the third the constructor's absence is a ReferenceError that kills the script and every endpoint
 * behind it. With the interface present but the entries never DELIVERED it is worse rather than better: nothing
 * throws, the callback is registered, and the whole branch is simply unreachable with no crash to say so.
 * Delivering is therefore the substance of this component and the geometry is the smaller half.
 *
 * DELIVERY IS SYNCHRONOUS AND THE ORDER IS THE POINT — this is the OPPOSITE of the interface next door, and
 * swapping them changes what a page observes. INTERSECTION OBSERVER's §3.2.10 runs from update-the-rendering
 * step 19, runs NO author code, and queues a TASK whose later run invokes the callback. §3.4.5 "Broadcast
 * active resize observations" INVOKES THE CALLBACK ITSELF, from update-the-rendering step 16, inside a loop
 * that re-enters style and layout after every callback and gathers again at a deeper depth. So the two halves
 * of this component are not "compute" and "deliver at a task": they are the four algorithms step 16's loop
 * calls in order, and the loop belongs to the caller because it is HTML's loop and not this standard's.
 *
 * EVERY PIECE OF STATE IS A JS VALUE, not malloc'd C:
 *   - §3.2.2's [[callback]], [[observationTargets]], [[activeTargets]] and [[skippedTargets]], own slots on
 *     the observer,
 *   - §3.1's ResizeObservation — its target, its observedBox and its lastReportedSizes — a member Array of the
 *     observation list,
 *   - §3.2.1's Document [[resizeObservers]], one Array per REALM.
 * Two forked arms see two documents, so they must see two observer lists and two observation lists, and an arm
 * whose element resized must not be observable from its sibling. An Array gives both for free — its mutations
 * are property writes the COW delta already captures and the snapshot machinery already carries it — while a C
 * list captured by its head pointer reverts the pointer on a context switch and leaves the observations
 * reachable from nothing (CLAUDE.md §State isolation). There is no C record in this component, so there is no
 * clone/finalizer/gc_mark triple that a new field could be added to two thirds of.
 *
 * §3.1's `lastReportedSizes` IS PER FLOW FOR THE SAME REASON A MediaQueryList's LATCH IS. `isActive()` returns
 * true when the current size differs from the FIRST entry of that list, so a value shared between two arms
 * would make one arm's frame report a resize the other one saw and swallow the resize this one is standing in.
 * It is a member of the observation Array, so the delta captures it with everything else.
 */
#ifndef ENGINE_HOST_BROWSER_CORE_RESIZE_OBSERVER_RESIZE_OBSERVER_H
#define ENGINE_HOST_BROWSER_CORE_RESIZE_OBSERVER_RESIZE_OBSERVER_H

#include <stdbool.h>
#include <stdint.h>

#include "quickjs.h"
#include "quickjs-step.h"
#include "core/events/event_target.h"   /* EventFireCb — the width of the fire request §3.4.6 parks on */

/* §2.1's `enum ResizeObserverBoxOptions`, IN THE ORDER THE IDL LISTS ITS KEYWORDS — the enumerator IS the
   index the keyword table and the entry's matching-sizes table are both written over, so a keyword added to
   one and not the other cannot compile. `RO_BOX_CONTENT` is the dictionary's `= "content-box"` default and is
   named rather than assumed at the two sites that read it. */
typedef enum {
    RO_BOX_BORDER = 0,
    RO_BOX_CONTENT,
    RO_BOX_DEVICE_PIXEL_CONTENT,
    RO_BOX_COUNT
} RoBoxOption;

/* The AGENT's half: the class, the member ids, §2.1's declarations. It registers the per-realm installs, so no
   host has a line to remember, and it declares §2.3's two record interfaces with it. */
void resize_observer_init(JSContext *ctx);
/* §3.7: THIS realm's ResizeObserver.prototype and this realm's §3.2.1 [[resizeObservers]] list. A list held in
   one static would make every document's observers one document's, because js_call_c_function does
   `ctx = p->u.cfunc.realm` and a member installed once answers every realm's question out of the realm that
   defined it. */
void resize_observer_install_proto(JSContext *ctx);
/* The interface objects on one realm's global — §2.1's and §2.3's two. */
void resize_observer_install(JSContext *ctx, JSValueConst global);
void resize_observer_free(JSRuntime *rt);

/* ---- §3.4's five algorithms, as the seam HTML's update-the-rendering step 16 drives ----------------------- */

/* §3.4.1 "GATHER ACTIVE RESIZE OBSERVATIONS AT DEPTH" for `docctx`'s document — for each observer, clear its
 * [[activeTargets]] and [[skippedTargets]], then put each observation whose `isActive()` is true into one of
 * the two by comparing §3.4.7's depth for its target against `depth`.
 *
 * IT RUNS NO AUTHOR CODE, which is why it is a call and not a machine: every value it reads is a used length
 * core/layout computed, and its walk is over the page's observers rather than over the page's script. The
 * caller's loop is where the rest point is — step 16 re-enters style and layout between gathers, and a rest
 * inside one gather would leave half a frame's observations classified against a depth the next half no longer
 * agrees with.
 *
 * `depth` IS STEP 16's LOOP VARIABLE and is a signed count of nodes: the loop begins at 0 and each turn passes
 * the depth §3.4.5 returned, so an observation is active exactly while its target is DEEPER than everything
 * already broadcast this frame. That is the whole of the standard's resize-loop protection and it is a
 * NARROWING rather than a bound — an observation the loop skips is reported as a skipped observation and
 * §3.4.6 says so out loud, which is the opposite of a cap silently dropping work. */
void resize_observer_gather(JSContext *docctx, int32_t depth);

/* §3.4.2 "HAS ACTIVE RESIZE OBSERVATIONS" and §3.4.3 "HAS SKIPPED RESIZE OBSERVATIONS" — step 16's two loop
 * conditions, asked of `docctx`'s document. They are TWO ENTRIES over one walk and not one predicate with a
 * flag, because they answer two questions about two different lists: the first decides whether the loop turns
 * again, the second decides whether §3.4.6 delivers an error after it stops. One bit for both is the shape
 * CLAUDE.md §A-PREDICATE-THAT-ANSWERS-TWO-QUESTIONS names. */
bool resize_observer_has_active(JSContext *docctx);
bool resize_observer_has_skipped(JSContext *docctx);

/* §3.4.5 "BROADCAST ACTIVE RESIZE OBSERVATIONS", SPLIT AT THE ONE POINT THE PAGE'S CODE RUNS.
 *
 * §3.4.5's step 2 loop invokes an author callback per observer, so it cannot be one C call: the callback may
 * loop, await, mutate the DOM, fork, or park to the cold tier, and a C activation holding a walk across it is
 * the drive-to-completion this engine aborts on. It is split the way core/rendering/rendering.c already splits
 * every step of §8.1.7.3 that reaches the page — a COUNT, a per-index PROLOGUE that produces the call, and a
 * per-index EPILOGUE that runs when the call has answered — so the CALLER owns the rest point and this
 * component owns every step of the algorithm.
 *
 *   `_broadcast_begin`   step 1 ("let shallowestTargetDepth be ∞") and step 2's list, snapshotted; returns how
 *                        many observers the loop will visit.
 *   `_broadcast_prepare` step 2.1 through step 2.4 for observer `i`: continue past an empty [[activeTargets]],
 *                        build the entries (§3.4.4 per observation), set each observation's lastReportedSizes,
 *                        and lower shallowestTargetDepth. Returns JS_UNDEFINED for step 2.1's "continue", and
 *                        otherwise a three-member Array [callback, observer, entries] — the callback to
 *                        invoke, the `this` value and second argument step 2.4 names, and the first argument.
 *                        OWNED.
 *   `_broadcast_finish`  step 2.5, "Clear observer.[[activeTargets]]" — AFTER the callback has returned, which
 *                        is what makes an observation the callback itself re-activated survive into the loop's
 *                        next turn.
 *   `_broadcast_depth`   step 3's return value. ∞ (HUGE_VAL) when no observer had an active target, which is
 *                        the value step 16's loop compares its next gather against.
 *
 * THE SNAPSHOT IS A SNAPSHOT IN THE SENSE THAT MATTERS: an observer constructed by one of these callbacks
 * belongs to a later turn of step 16's loop, and it cannot broadcast on this one anyway because it has no
 * active targets yet. */
uint32_t resize_observer_broadcast_begin(JSContext *docctx);
JSValue  resize_observer_broadcast_prepare(JSContext *docctx, uint32_t i);
void     resize_observer_broadcast_finish(JSContext *docctx, uint32_t i);
double   resize_observer_broadcast_depth(JSContext *docctx);

/* §3.4.6 "DELIVER RESIZE LOOP ERROR" AS A REQUEST, because its step 3 REPORTS the event and reporting one runs
 * the page's `error` listeners.
 *
 * Its three steps are: create a new ErrorEvent, initialize the event's message slot to "ResizeObserver loop
 * completed with undelivered notifications.", and report the exception event. This engine performs that third
 * step as HTML §8.1.4.6 "Runtime script errors"' own step 6.2 does — fire `error` at the global, using
 * ErrorEvent, cancelable — rather than through core/events/report_exception.c's entry, because that entry
 * takes an EXCEPTION and derives the event from it while §3.4.6 has no exception at all and states the whole
 * event itself. The one thing both spellings must agree on is the FIRE, and they do: the same §2.9 dispatch,
 * the same cancelability, so an `onerror` that returns true cancels this the way it cancels any other.
 *
 * The work record is the CALLING machine's, exactly as core/events/report_exception.h's is: the caller visits
 * it from its own declaration and releases it on teardown, so a report abandoned mid-dispatch frees the event
 * it was holding. `_run` returns JS_STEP_CALL or JS_STEP_YIELD (return it — the fire has parked on the page's
 * listeners or on the scheduler's answer) or 0 when §3.4.6 has finished. */
typedef struct {
    uint8_t stage;      /* which step of §3.4.6 this delivery is at — RO_ERR_STAGES in resize_observer.c */
    uint8_t phase;      /* event_target_fire_run's own */
    JSValue ev;         /* step 1's ErrorEvent, minted once and held across the dispatch (owned) */
    EventFireCb cb;
} ResizeObserverLoopError;

void resize_observer_loop_error_start(ResizeObserverLoopError *w);
void resize_observer_loop_error_visit(JSContext *ctx, ResizeObserverLoopError *w, JSStepVisit *v);
void resize_observer_loop_error_release(JSContext *ctx, ResizeObserverLoopError *w);
int  resize_observer_loop_error_run(JSContext *ctx, ResizeObserverLoopError *w, JSValue in,
                                    JSValue **out_cb, int *out_argc);

#endif
