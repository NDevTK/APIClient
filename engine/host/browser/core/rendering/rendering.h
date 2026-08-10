/* "UPDATE THE RENDERING" — HTML §8.1.7.3 "Processing model", the IN-PARALLEL half of the window event loop.
 *
 * NOT §14.3, AND NOT §15 EITHER. HTML §14 is "The XML syntax" and §15 is "Rendering", which is a CSS
 * requirements section (the UA stylesheet and presentational hints) containing no algorithm of this name.
 * "Update the rendering" is defined in §8.1.7.3 as the steps of a task queued on the RENDERING TASK SOURCE,
 * and its definition id is `#update-the-rendering`. SPEC_STEPS.md §6 numbers all twenty-three of its steps and
 * marks the seventeen at which the page's own code runs; this component is those steps.
 *
 * WHAT IS DOWNSTREAM OF IT, and why an engine without it is missing much more than a paint: there is no
 * rendering opportunity, so `requestAnimationFrame` never fires, ResizeObserver never delivers,
 * IntersectionObserver's entries are never queued, `resize`/`scroll`/`pagereveal` never dispatch, animation
 * events never send, and the MICROTASK CHECKPOINT that Web Animations §4.4 step 3 performs INSIDE step 11
 * never happens. A large fraction of real page code hangs off exactly those, so an engine that cannot reach
 * them cannot report what that code does.
 *
 * IT IS DRIVEN BY THE ENGINE'S OWN SCHEDULER — there is no loop here. §CLAUDE.md's scheduler is one BFS with
 * one WFQ and every runtime job is a flow in it; a rendering opportunity is therefore a WORK ITEM on that one
 * frontier, and "update the rendering" is a STEP MACHINE that rests at declared spec steps, preemptible and
 * parkable at every one of them like any other program. A `while (rendering_opportunity())` here would be the
 * second scheduler CLAUDE.md calls the cardinal violation, and it would also be a drive-to-completion: step 16
 * is a `while (true)` whose body runs the page's ResizeObserver callbacks, and step 14 runs an animation
 * callback that may itself hold a loop.
 *
 * THE REFRESH RATE IS A MODELLED UA CHOICE, NOT AN UNKNOWN. HTML says outright that a rendering opportunity is
 * the user agent's to decide ("the user agent might decide to drop that page to a much slower 4 rendering
 * opportunities per second, or even less") and that "the refresh rate can be hardware- or
 * implementation-specific". So a fixed 60 Hz on the virtual clock is a UA answering the question the spec
 * asks it, which is what §Headless requires: the missing piece is a display, and the spec still defines the
 * behaviour without one. What a display would change is step 22 and nothing else.
 *
 * WHEN THERE IS AN OPPORTUNITY AT ALL is the in-parallel loop's step 1 ("wait until at least one navigable
 * MIGHT have a rendering opportunity"), and this engine answers it with exactly step 4's own test hoisted:
 * a navigable might have one when its active document has something for these steps to do — it has not been
 * revealed, or its map of animation frame callbacks is non-empty. That is the spec's own reason redundant
 * queuing is harmless, read from the other end, and it is what keeps a page that is not animating from
 * queueing a rendering task forever. It is not a bound: it is the condition for the work item to EXIST, the
 * same shape as the timer source having no timer due. */
#ifndef ENGINE_HOST_BROWSER_CORE_RENDERING_RENDERING_H
#define ENGINE_HOST_BROWSER_CORE_RENDERING_RENDERING_H

#include "quickjs.h"

/* THE AGENT'S HALF — the machine, and the per-realm slot its driver lives in. */
void rendering_init(JSContext *ctx);
/* §3.7's per-realm half: this realm's driver function object, so a task queued for THIS navigable's window
   runs its steps in THAT realm. Declared into realm.h's one list. */
void rendering_install_driver(JSContext *ctx);
void rendering_free(JSContext *ctx);

/* THE IN-PARALLEL LOOP'S ONE STEP, asked by whoever drives the event loop and only when it has nothing else
   to run — the same moment, and for the same reason, that the timer source is asked (timer.h). Two task
   sources become due at moments on the ONE virtual clock, so this defers to the timer source whenever a timer
   expires before the next frame: the clock is one clock, and a rendering opportunity that stepped over a due
   timer would run the page's frame before the work the page scheduled ahead of it.
   Returns 1 when a rendering task was queued (the driver has work again), 0 when no navigable might have a
   rendering opportunity or a timer is due first. */
int rendering_run_opportunity(JSContext *ctx);

#endif
