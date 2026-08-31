/* HIGH RESOLUTION TIME Level 3 §7 The Performance interface — THE OBJECT §4's ALGORITHMS WERE ALWAYS FOR.
 *
 * WHAT IT IS AND WHY IT IS NOT core/timing/hr_time.c. §4 Time Origin holds the OPERATIONS (coarsen time,
 * relative high resolution time, current high resolution time, get time origin timestamp) and the per-realm
 * field they read; §7 holds the INTERFACE that exposes two of them to a page. They are separate for the reason
 * every other component in this tree is separate from the algorithm it presents: §4's operations have callers
 * all over this engine — DOM §2.5's `timeStamp`, HTML §6.4.1's activation questions, §8.1.7.3's animation frame
 * callbacks — and not one of those goes near a Performance object. An interface that owned them would make
 * every one of those sites reach through a page-visible object to ask a question about the environment.
 *
 * WHAT A PAGE GOT BEFORE THIS COMPONENT EXISTED, WHICH IS THE THING WORTH WRITING DOWN. `performance` is a name
 * browser/platform_names.h carries, so solver/absent.c's read hook LEFT IT ALONE: a bare `performance.now()`
 * threw a ReferenceError naming the component this engine owed, which is the honest absence §NO STUBS wants.
 * `window.performance`, though, read a CONCRETE `undefined` — not unknown input, because the platform owns the
 * name — so `if (window.performance && performance.now)` was DECIDED, with no fork, to the arm where the API is
 * missing. Every telemetry, instrumentation, profiling and lazy-hydration path a bundle puts behind that guard
 * was therefore code the forced execution could not reach, in a browser where the guard is always true. That is
 * not a wrong value; it is a whole arm of the program that no run has ever taken, and it is why this is worth
 * more than the reference count that ranked it.
 *
 * THE ONE OBJECT PER REALM WEARS ITS OWN CLASS, and both halves of that matter. The class is Web IDL §3.7.5's
 * BRAND — `Performance.prototype.now.call({})` is a TypeError and a page tells that apart from `undefined` —
 * and it is quickjs's per-context prototype slot, which is what makes §3.7.3's interface prototype object a
 * per-REALM object rather than a module static answering every document from whichever realm ran first.
 *
 * IT ANSWERS FOR `this`'s REALM AND THAT IS ASSERTED. §7.1 and §7.2 both say "this's relevant global object",
 * and a C member runs in the realm that DEFINED it (js_call_c_function sets `ctx = p->u.cfunc.realm`), so an
 * ordinary `performance.now()` arrives with the right realm and `topWindow.Performance.prototype.now.call(
 * iframeWindow.performance)` does not — it would answer the TOP document's clock and origin for the FRAME's
 * Performance. The same shape and the same assert as core/frame/visual_viewport.c's, and the same repair named
 * in it: give the instance a record as its class opaque so the member reads its environment off `this`.
 *
 * WHAT IS HONESTLY ABSENT HERE. §7's IDL is three members and all three are built. Every other member a page
 * finds on `performance` in a browser comes from a PARTIAL in another standard — Performance Timeline's
 * getEntries/getEntriesByType/getEntriesByName, User Timing's mark/measure/clearMarks/clearMeasures, Navigation
 * Timing's legacy `timing` and `navigation`, Resource Timing's buffer members — and every one of them needs a
 * PERFORMANCE TIMELINE, which this engine does not have (core/rendering/rendering.c's realm_awaits over
 * `PerformanceObserver` is the assertion that says so, and it names the two update-the-rendering steps that
 * would queue entries onto one). They are ABSENT rather than shaped, so `performance.getEntriesByType("navigation")`
 * is a TypeError naming the operation, which is the forcing function; a `[]` would be the plausible datum that
 * makes a page conclude there was no navigation to time. */
#ifndef ENGINE_HOST_BROWSER_CORE_TIMING_PERFORMANCE_H
#define ENGINE_HOST_BROWSER_CORE_TIMING_PERFORMANCE_H

#include <stdbool.h>

#include "quickjs.h"

/* Declared ONCE PER AGENT — the class, the per-realm slot and the two operation declarations. Released through
   core/platform.c's third column, so no host has a line to remember. */
void performance_init(JSContext *ctx);
void performance_free(void);

/* Web IDL §3.7 Interfaces' implementation-check an object, step 3 — "If object does not implement interface,
   then throw a TypeError" — for §7's two OPERATIONS, which state it at their declaration
   (idl_args.h: idl_this_iface) so it is asked before §3.6 Overload resolution converts anything. */
bool performance_is(JSValueConst v);

#endif
