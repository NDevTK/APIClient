/* USER TIMING §2 User Timing — §2.1.1 mark(), §2.2 The PerformanceMark Interface and §2.2.1 The PerformanceMark
 * Constructor.
 *
 * WHAT A PAGE GOT BEFORE THIS COMPONENT EXISTED, WHICH IS WHY IT IS WORTH ITS OWN FILE. `mark` is the FIRST
 * line of a great many real bundles — an application that measures its own boot writes
 * `performance.mark('...')` at the top of its first inline script, before it has done anything else — and this
 * engine answered that call with a TypeError saying `mark` of the Performance object is not a function (it is
 * undefined), which is this engine's own message and not any standard's. A member that does not
 * exist is a TypeError, an uncaught TypeError at the top of an inline script ends that script, and everything
 * the script would have gone on to do is code no run has ever reached. It was measured on a real application
 * whose first inline script is `performance.mark(...)` at line 2, against a browser reporting ZERO page errors
 * on the same document.
 *
 * WHY IT IS NOT core/timing/performance.c. `mark()` is a PARTIAL interface member: the Performance object is
 * HIGH RESOLUTION TIME §7's and every member a page finds on it beyond §7's three comes from some other
 * standard's partial. Putting them in the file that owns §7 would make that file a bag with five standards in
 * it, and the seam this component installs across is exactly the seam the standards themselves draw.
 *
 * ---- WHAT IS BUILT AND WHAT IS NOT --------------------------------------------------------------------------
 *
 * BUILT: §2.2's PerformanceMark interface (its class, its per-realm prototype chained to PERFORMANCE TIMELINE
 * §3's, its interface object, its `detail` attribute and the five attributes it states over §3's), §2.2.1's
 * constructor in full including both of its throws, and §2.1.1's mark() steps 1, 2 and 4.
 *
 * BUILT ALSO — §2.1.1's STEP 2. "Queue a PerformanceEntry entry" is PERFORMANCE TIMELINE §5.1, and
 * core/timing/performance_observer.c performs its observer half, so a `performance.mark('a')` now reaches a
 * PerformanceObserver observing `mark`. This component's part of it is one call between steps 1 and 4, plus
 * the §4.5 declaration in user_timing_init that says this build has a producer for the `mark` entry type.
 * THIS PARAGRAPH USED TO SAY STEPS 2 AND 3 TOGETHER, and it rested on an argument that has since stopped being
 * true — that §4's PerformanceObserver was absent, so a queue here would be a write with no reader. It has a
 * reader now, which is why the residual below is step 3 alone.
 *
 * NOT BUILT — §2.1.1's STEP 3, and this is a NAMED RESIDUAL rather than a partial member:
 *   WHAT IS NOT COVERED. USER TIMING §2.1.1's "Add entry to the performance entry buffer" — so a mark this
 *     engine mints is delivered to an observer that asked for one and is not RETAINED on any timeline.
 *   WHY THE CODE IS CORRECT AND NOT MERELY UNFINISHED. The buffer's remaining readers are PERFORMANCE TIMELINE
 *     §2.1.1 getEntries(), §2.1.2 getEntriesByType() and §2.1.3 getEntriesByName() on the Performance
 *     interface, and all three are ABSENT from this build — so a buffer written here would still be a write
 *     with no reader, and readers installed over a timeline holding nothing but marks would answer a page that
 *     nothing else on the page was ever timed. See core/timing/performance_entry.h, which states that argument
 *     where the buffer would live, and core/timing/performance_observer.h, which states why an OBSERVER is not
 *     under it: an observer names the entry types it wants and §4.5 states which of them exist.
 *   WHAT THE NEXT DIFF BUILDS. The per-global performance entry buffer map and §5.1 steps 9-12 that append
 *     into it, together with a non-mark entry type, so that the first reader installed answers about a
 *     timeline rather than about marks. This file then changes by nothing at all: step 3 is inside §5.1's
 *     steps 9-12 rather than a second call from here.
 *   HOW ITS ABSENCE WOULD SHOW. `performance.mark('a')` returns a real PerformanceMark whose four §3
 *     attributes and whose `detail` are all correct and delivers it to an observer, and
 *     `performance.getEntriesByName('a')` is a TypeError naming an operation this engine does not have —
 *     which is the forcing function. An observer registered with `buffered: true` AFTER the mark receives
 *     nothing, where a browser hands it the earlier mark.
 *
 * NOT BUILT — §2.1.2 clearMarks(), §2.1.3 measure(), §2.1.4 clearMeasures() and §2.3's PerformanceMeasure.
 * All four are operations over the same buffer: `clearMarks` empties it, `measure` reads two of its entries
 * through §3.1 Convert a mark to a timestamp, `clearMeasures` empties it again. They arrive with the buffer
 * above and not before it, because every one of them would otherwise be a member with nothing to operate on.
 * They are ABSENT rather than shaped, so each is a TypeError naming itself.
 */
#ifndef ENGINE_HOST_BROWSER_CORE_TIMING_USER_TIMING_H
#define ENGINE_HOST_BROWSER_CORE_TIMING_USER_TIMING_H

#include <stdbool.h>

#include "quickjs.h"

/* Declared ONCE PER AGENT — §2.2's class, §2.2.1's constructor machine and §2.1.1's member. */
void user_timing_init(JSContext *ctx);
void user_timing_free(void);

/* Web IDL §3.7 Interfaces' implementation-check, for §2.2's `detail`. */
bool performance_mark_is(JSValueConst v);

#endif
