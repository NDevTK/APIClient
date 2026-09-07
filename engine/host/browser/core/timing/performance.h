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
 * finds on `performance` in a browser comes from a PARTIAL in another standard. WHICH of them are absent is a
 * question with a command behind it — `node engine/idlgen.mjs`, and read the `Performance` row — so no count
 * is written here; what is written is that they DO NOT SHARE ONE REASON, and which reason governs which.
 *
 * USER TIMING §2.1.1's `mark()` IS BUILT AND LIVES IN core/timing/user_timing.c, which is where a partial's
 * member belongs — the interface is §7's and the member is that standard's, so folding it in here would make
 * this file a bag with five standards in it. It reaches this component through `performance_proto` and
 * `performance_now_value` below, and through nothing else.
 *
 * ONE REASON STOOD HERE FOR ALL OF THEM AND IT IS RETIRED — REWRITTEN RATHER THAN DELETED, because a reader
 * who re-derives it will re-introduce it. This paragraph used to say "THE REST ARE ABSENT AND NEED A
 * PERFORMANCE TIMELINE, WHICH THIS ENGINE DOES NOT HAVE", and it listed "its PerformanceObserver" among the
 * absences. THE DIRECTION OF THE ERROR IS THE DANGEROUS ONE: it named as ABSENT a thing this engine HAS.
 * PERFORMANCE TIMELINE §3's PerformanceEntry, §4's PerformanceObserver, §4.2.2's PerformanceObserverEntryList,
 * §5.1's observer half, §5.3 and §5.5 are all built, and the gap audit reads `complete` for both interfaces.
 * AN ANNOUNCED ABSENCE CLOSES THE QUESTION — a reader told a thing is missing does not grep for it — so this
 * clause went on being re-imported by everyone who trusted it, while the two sibling headers in this same
 * directory that rested on the same argument were rewritten by the commit that refuted it, and the two that
 * commit added state the narrowed reason in full. This file was the copy nobody edited, which is the ordinary
 * way a retired argument survives: the diff that retires one touches the files it changes and not the file
 * that merely agreed with them. A
 * decision paragraph is the most trusted prose in a file, which is exactly why a false clause inside one
 * survives: nobody audits the reasoning they came to the file for.
 *
 * AND IT DID NOT ONLY GO STALE: IT WAS NEVER ONE REASON. It reaches the members that want the PERFORMANCE
 * ENTRY BUFFER and no others, and it named that buffer as though it were a whole standard. What holds is four
 * decisions, each owned by the component that would build it.
 *
 *   PERFORMANCE TIMELINE §2.1.1 getEntries(), §2.1.2 getEntriesByType() and §2.1.3 getEntriesByName() — the
 *   three §2.1 readers the retired reason was written for. Still absent, and NOT for want of a timeline: what
 *   is missing is the per-global performance entry buffer map that §5.1 steps 9-12 append into, and the
 *   argument for leaving it missing is core/timing/performance_entry.h's, which is STRONGER than the one
 *   retired here. This build mints exactly one entry type, so these three would hand a page a timeline holding
 *   marks and NOTHING ELSE, which a page cannot tell from a page on which nothing else was ever timed.
 *
 *   USER TIMING §2.1.2 clearMarks(), §2.1.3 measure() and §2.1.4 clearMeasures() — the same buffer, and
 *   core/timing/user_timing.h is where that is stated. `measure()` additionally wants §2.3's PerformanceMeasure
 *   and §3.1 Convert a mark to a timestamp, which resolves a mark NAME out of that same buffer.
 *
 *   NAVIGATION TIMING §8.3's `timing` and `navigation` — THE RETIRED REASON NEVER REACHED THESE TWO, which was
 *   wrong when it was written rather than stale. §8 is that standard's "Obsolete" chapter: §8.1 "The
 *   PerformanceTiming interface" and §8.2 "The PerformanceNavigation interface" predate the timeline and hold
 *   their own attributes, so a performance entry buffer would hand a page neither object. What is actually
 *   missing is the MOMENTS they return — core/timing/event_loop.h records that every duration bracketing tree
 *   construction is 0 on this clock, and core/dom/document.c that this build has no PerformanceNavigationTiming
 *   for load timing info to be written onto. Installing them would mean CHOOSING numbers, which is the rule
 *   core/timing/performance_entry.h already states at PERFORMANCE TIMELINE §3's `id`.
 *
 *   RESOURCE TIMING §3.4 "Extensions to the Performance Interface"'s clearResourceTimings,
 *   setResourceTimingBufferSize and onresourcetimingbufferfull — also not the timeline's buffer. §3.4 gives the
 *   global its OWN state, a resource timing buffer size limit and a current size and a buffer-full event
 *   pending flag and a secondary buffer, and it is that state rather than the performance entry buffer map that
 *   decides when the event fires. The three are absent because this build mints no resource timing entry.
 *
 * AND SOME MEMBERS WERE NEVER NAMED AT ALL, WHICH IS THE HALF WITH NOTHING TO DO WITH STALENESS: an
 * enumeration written beside the words "THE REST" reads as a census, and this one was short by every member
 * whose standard the paragraph did not happen to mention. That is what writing a list where a derivation
 * belongs does, which is why the command above is the answer and why the groups here are named by STANDARD.
 * EVENT TIMING §2.3 "Extensions to the Performance interface"'s `eventCounts` and `interactionCount` are
 * absent for that standard's own processing model and reach nothing in this component.
 *
 * AND ONE OF THEM IS NOT A GAP AND MUST NOT BE BUILT. The Measure Memory API declares
 * `measureUserAgentSpecificMemory` `[CrossOriginIsolated]` — its harvested IDL is
 * node_modules/@webref/idl/performance-measure-memory.idl, the same artifact the gap audit reads — and
 * WEB IDL §3.3.4 "[CrossOriginIsolated]" says such a construct "is exposed only within an environment whose
 * cross-origin isolated capability is true". The renderer frame this engine runs in is not one, so a real
 * browser exposes the member here no more than this one does: installing it would flip a page's guard TRUE and
 * abandon a fallback branch that works, which is CLAUDE.md §NO STUBS' defect wearing an audit row. It charges
 * it because it walks WEB IDL §3.3.7 "[Exposed]" and does not walk §3.3.4; a finding about the audit, not
 * about this file, and the reason this member is grouped apart from the four decisions above.
 *
 * AND THE CITATIONS OF Resource Timing, Event Timing AND THE Measure Memory API ABOVE ARE READ, COUNTED AND
 * JUDGED BY NOTHING, because those are not standards this tree's citation audit holds a corpus for. It reports
 * that about itself rather than about them — run it on this file and read the line naming what stood in front
 * of a § that no list knows, beside the count of citations naming no standard. Said here so a reader does not
 * take silence at these lines for a clean bill: the repair is a corpus row over there and never an edit at
 * this site, and until there is one their numbers are checked the way any reader checks one, by fetching the
 * standard. The three standards beside them that the audit does index are judged on every run, which is the
 * whole of the difference and is not visible from any one line of this comment.
 *
 * NONE OF THE ABSENT MEMBERS IS SHAPED. `performance.getEntriesByType("navigation")` is a TypeError naming the
 * operation, which is the forcing function; a `[]` would be the plausible datum that makes a page conclude
 * there was no navigation to time. */
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
