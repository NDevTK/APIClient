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
 * THE ONE OBJECT PER REALM WEARS ITS OWN CLASS, and both halves of that matter. The class is Web IDL §3.7.7
 * Operations' BRAND — `Performance.prototype.now.call({})` is a TypeError and a page tells that apart from
 * `undefined` —
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
 * finds on `performance` in a browser comes from a PARTIAL in another standard, and those divide in two.
 *
 * USER TIMING §2.1.1's `mark()` IS BUILT AND LIVES IN core/timing/user_timing.c, which is where a partial's
 * member belongs — the interface is §7's and the member is that standard's, so folding it in here would make
 * this file a bag with five standards in it. It reaches this component through `performance_proto` and
 * `performance_now_value` below, and through nothing else.
 *
 * THE REST ARE ABSENT AND NEED A PERFORMANCE TIMELINE, WHICH THIS ENGINE DOES NOT HAVE — Performance
 * Timeline's getEntries/getEntriesByType/getEntriesByName and its PerformanceObserver, User Timing's
 * measure/clearMarks/clearMeasures, Navigation Timing's legacy `timing` and `navigation`, Resource Timing's
 * buffer members. core/timing/performance_entry.h is where that absence is now stated, beside the interface
 * every one of them would hand out, and it names what the next diff builds. They are ABSENT rather than
 * shaped, so `performance.getEntriesByType("navigation")` is a TypeError naming the operation, which is the
 * forcing function; a `[]` would be the plausible datum that makes a page conclude there was no navigation to
 * time. */
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

/* THIS REALM'S `Performance.prototype` — the target a PARTIAL interface in another standard installs its
   member onto. Web IDL §3.7.3 Interface prototype object is what makes it a PER-REALM object, which is why it
   is handed over rather than left to be reached through a class id: a partial that fetched one for itself
   would have to know which realm it was in. The caller is a realm intrinsic declared AFTER this component's,
   which core/realm.h states is what the declaration order guarantees. OWNED by the caller — free it. */
JSValue performance_proto(JSContext *ctx);

/* THE VALUE §7.1's now() WOULD RETURN, for an algorithm in another standard that says so in those words — USER
   TIMING §2.2.1 step 5.2 is "Otherwise, set it to the value that would be returned by the Performance object's
   now() method". It is this file's answer and not a second read of the clock, so a mark's startTime and a
   `performance.now()` in the same turn agree by construction rather than by two call sites happening to reach
   the same operation. */
JSValue performance_now_value(JSContext *ctx);

#endif
