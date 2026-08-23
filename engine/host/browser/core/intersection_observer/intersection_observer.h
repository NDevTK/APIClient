/* INTERSECTION OBSERVER — the `IntersectionObserver` interface (§2.2), its processing model (§3.2) and the
 * two seams the HTML event loop reaches it through. See intersection_observer.c.
 *
 * WHY THIS MATTERS TO A SOLVER. `new IntersectionObserver(cb).observe(el)` is how a real bundle wires lazy
 * image loading, infinite scroll, "load this route when it scrolls into view", impression analytics and every
 * visibility-gated fetch — and the fetches behind all of those live INSIDE the callback. With the interface
 * absent the page's own `new IntersectionObserver(...)` throws a ReferenceError at that line, so every
 * statement AFTER it in that script has never been executed by this engine at all; with the interface present
 * but the entries never DELIVERED, the callback is registered and silently never called, which is worse —
 * nothing throws and the whole branch is simply unreachable. Delivering is therefore the substance of this
 * component and the geometry is the smaller half.
 *
 * DELIVERY IS NOT SYNCHRONOUS AND THE ORDER IS THE POINT. §3.2.10 runs from HTML §8.1.7.3's update-the-
 * rendering STEP 19 and runs NO author code: it computes, QUEUES an IntersectionObserverEntry (§3.2.6) and
 * QUEUES A TASK on the IntersectionObserver task source (§3.2.4). The callback is invoked from THAT task, by
 * §3.2.5. Getting that backwards — invoking from step 19, the way ResizeObserver's step 16 does — changes what
 * the page observes, so the two halves are two entry points here and the second is a task.
 *
 * EVERY PIECE OF STATE IS A JS VALUE, not malloc'd C:
 *   - an observer's [[ObservationTargets]] and [[QueuedEntries]] (§3.1.3), own slots on the observer,
 *   - an Element's [[RegisteredIntersectionObservers]] (§3.1.2), an own slot on its wrapper,
 *   - the document's observers and its IntersectionObserverTaskQueued flag (§3.1.1), one Array per REALM.
 * Two forked arms see two documents, so they must see two registration lists and two entry queues, and an arm
 * that queues an entry must not be observable from its sibling. An Array gives both for free — its mutations
 * are property writes the COW delta already captures and the snapshot machinery already carries it — while a C
 * list captured by its head pointer reverts the pointer on a context switch and leaves the entries reachable
 * from nothing (CLAUDE.md §State isolation). There is no C record in this component, so there is no
 * clone/finalizer/gc_mark triple that a new field could be added to two thirds of.
 *
 * THE REGISTRATION RECORD IS PER FLOW FOR THE SAME REASON A MediaQueryList's LATCH IS. §3.2.10 step 3.18 fires
 * only when the threshold index, `isIntersecting` or `isVisible` has changed since the LAST time these steps
 * ran, so a `previousThresholdIndex` shared between two arms would make one arm's frame report a change the
 * other one saw. It is a member of the registration Array, so the delta captures it with everything else.
 */
#ifndef ENGINE_HOST_BROWSER_CORE_INTERSECTION_OBSERVER_INTERSECTION_OBSERVER_H
#define ENGINE_HOST_BROWSER_CORE_INTERSECTION_OBSERVER_INTERSECTION_OBSERVER_H

#include <stdbool.h>
#include <stdint.h>

#include "quickjs.h"

/* The AGENT's half: the class, the member ids, the notify machine's step definition. It registers the
   per-realm installs, so no host has a line to remember. */
void intersection_observer_init(JSContext *ctx);
/* §3.7: THIS realm's IntersectionObserver.prototype, this realm's document-observer list and this realm's
   notification driver. A driver held in one static would invoke every document's callbacks out of whichever
   realm happened to build it first, because js_call_c_function does `ctx = p->u.cfunc.realm`. */
void intersection_observer_install_proto(JSContext *ctx);
/* The interface object on one realm's global. */
void intersection_observer_install(JSContext *ctx, JSValueConst global);
void intersection_observer_free(JSRuntime *rt);

/* §3.4.2 "PENDING INITIAL IntersectionObserver TARGETS", for HTML §8.1.7.3 update-the-rendering STEP 4.
 *
 * §3.4.2 is explicit that step 4 — the "Unnecessary rendering" removal — must gain an additional requirement
 * for skipping: "the document does not have pending initial IntersectionObserver targets". Without it the frame
 * that would run step 19 is never queued for a document whose only pending work is an observation, so §3.2.10
 * is written and UNREACHABLE — the shape CLAUDE.md §Consumer-defaults names, and the one CSSOM VIEW §4.2's own
 * `media_query_list_pending` exists to avoid one step further up the same list.
 *
 * A document HAS them when some observer whose root is in its tree has a target for which NO entry has yet
 * been queued. That second condition is DERIVED and is not a fourth field on the registration: §3.2.2 sets
 * `previousThresholdIndex` to −1, §3.2.10 step 3.13 can only produce an index in [0, thresholds.length], and
 * step 3.18 queues whenever the two differ — so the first update for a target ALWAYS queues, and "no entry has
 * been queued yet" is exactly "previousThresholdIndex is still −1".
 *
 * READS ONLY. The latch is step 19's to move, and moving it here would consume the very change the frame is
 * being queued to deliver. */
bool intersection_observer_pending(JSContext *ctx);

/* §3.2.10 "RUN THE UPDATE INTERSECTION OBSERVATIONS STEPS", for HTML §8.1.7.3 update-the-rendering STEP 19 —
 * split into a COUNT and a per-observer step.
 *
 * The split is the same one CSSOM VIEW §4.2's is, for a different reason: these steps run no author code, so
 * the walk cannot be interrupted by the page, but it is O(the page's observers × their targets) and every one
 * of those is geometry. A machine that walks a structure of the PAGE'S SIZE has to be able to rest, which is
 * what JS_STEP_YIELD is for, and step 2's "for each observer in observer list" is the granularity the algorithm
 * itself iterates at.
 *
 * `intersection_observer_count` is step 1's list — every observer whose root is in this document's tree,
 * including the implicit-root observers when this realm is the top-level traversable's. It is a SNAPSHOT in the
 * sense that matters: an observer constructed while the walk rests belongs to a later frame, and it cannot fire
 * on this one anyway because it has no targets yet.
 *
 * `intersection_observer_update` performs the whole of step 2 for observer `i`. `frame_ts` is the UNSAFE moment
 * HTML §8.1.7.3 step 1 bound as frameTimestamp; the entry's `time` is that moment made relative to the
 * observer's own global (§2.3), which is a question about a realm and so is asked here. */
uint32_t intersection_observer_count(JSContext *ctx);
/* `frame_ts` IS A MOMENT AND NOT A `double` — HTML §8.1.7.3 step 1's last render opportunity time, read off
   the event loop's one virtual clock, which is unknown external input once a timer set with an unknown
   `timeout` has fired (core/timing/event_loop.h). §3.2.10 step 3.2's throttle over one is a fork this walk
   has no seam to ask at, and it says so at the line where it would ask. */
void     intersection_observer_update(JSContext *ctx, uint32_t i, JSValueConst frame_ts);

#endif
