/* PERFORMANCE TIMELINE §4 The PerformanceObserver interface, and the half of §5 Processing that serves it.
 *
 * WHAT IT IS. The subscription side of the performance timeline: a page hands this a callback and a set of
 * entry types, and every entry of one of those types that the platform queues is delivered to it in a task.
 * §4 is the interface and its four members; §5.1 Queue a PerformanceEntry is what every timing standard in the
 * platform calls when it mints one, and §5.3 Queue the PerformanceObserver task is the delivery.
 *
 * WHY IT MATTERS HERE MORE THAN ITS SIZE SUGGESTS. `PerformanceObserver.supportedEntryTypes` is a GATE that
 * real bundles branch on before doing anything else with the timeline, and an ABSENT interface object makes
 * that first read a ReferenceError — so the whole arm behind it, including the code that has nothing to do
 * with timing, is never reached. §4.5's frozen array is therefore not decoration: it is the value that decides
 * which arm a forced run takes, which is why it is DERIVED from the components that actually mint entries
 * (performance_observer_declare_entry_type) rather than typed here. A list somebody maintains would be a
 * second copy of "which standards in this build queue entries", and the copy that drifts is the one nobody
 * runs against reality.
 *
 * ---- WHAT IS BUILT AND WHAT IS NOT ------------------------------------------------------------------------
 *
 * BUILT: §4's interface, class, per-realm prototype and interface object; the constructor; §4.2 observe() in
 * both of its arms; §4.3 takeRecords(); §4.4 disconnect(); §4.5 supportedEntryTypes; §4.1's
 * PerformanceObserverCallbackOptions; §5.1's observer half (steps 2, 3, 4, 7, 8 and 13); §5.3 whole. §4.2.2
 * and §5.5 are core/timing/performance_observer_entry_list.h.
 *
 * NOT BUILT, AND NAMED RATHER THAN SHAPED — §5.1 STEPS 9-12 AND §4.2 STEP 7.5, THE PERFORMANCE ENTRY BUFFER:
 *   WHAT IS NOT COVERED. There is no per-global performance entry buffer map in this build, so §5.1 steps 9-12
 *     do not run and §4.2 step 7.5's `buffered: true` registers the observer and delivers it NOTHING that was
 *     queued before it observed. Its `droppedEntriesCount` is consequently always absent from §4.1's
 *     dictionary rather than a number: §5.3 step 3.3.7.2.1.3 reads a tuple's dropped entries count off that
 *     same absent map, and inventing a 0 there would be a datum a page could not tell from a measurement.
 *   WHY IT IS NOT HERE YET, which is a rule and not a shortage of effort. core/timing/performance_entry.h
 *     declines to build that buffer because its other readers — PERFORMANCE TIMELINE §2.1.1 getEntries(),
 *     §2.1.2 getEntriesByType() and §2.1.3 getEntriesByName() on the Performance interface — would then hand a
 *     page a whole timeline containing marks and NOTHING ELSE, which a page cannot tell from a page on which
 *     nothing else happened. THAT ARGUMENT DOES NOT REACH THE OBSERVER, and the difference is what makes this
 *     component landable ahead of the buffer: an observer NAMES the entry types it wants and §4.5 states
 *     exactly which of them exist, so a page asking for `mark` gets marks and a page asking for anything else
 *     is told so rather than being handed an empty answer to a question this build cannot answer.
 *   WHAT THE NEXT DIFF BUILDS. A per-global performance entry buffer map keyed by entry type, holding for each
 *     a performance entry buffer, a maxBufferSize and a dropped entries count taken from the Timing Entry
 *     Types Registry's row for that type (for `mark` the row reads maxBufferSize Infinite and should add entry
 *     "Return true", so §5.6 Determine if a performance entry buffer is full answers false for it and §5.1
 *     step 12 always appends); §5.1 steps 9-12 appending into it; §4.2 step 7.5 reading it back; and §5.3 step
 *     3.3.7's dropped-entries walk over it. §2.1.1-§2.1.3 are a SEPARATE decision that stays with
 *     performance_entry.h's argument above.
 *   HOW ITS ABSENCE WOULD SHOW. `performance.mark('a')` followed by
 *     `new PerformanceObserver(cb).observe({type: 'mark', buffered: true})` never calls `cb`, where a browser
 *     calls it with the earlier mark; and a callback that reads `options.droppedEntriesCount` finds the member
 *     absent on every call.
 */
#ifndef ENGINE_HOST_BROWSER_CORE_TIMING_PERFORMANCE_OBSERVER_H
#define ENGINE_HOST_BROWSER_CORE_TIMING_PERFORMANCE_OBSERVER_H

#include "quickjs.h"

/* Declared ONCE PER AGENT — the class, the member declarations and §5.3's task machine. It also declares
   §4.2.2's component, which shares this row on core/platform.h's three columns. */
void performance_observer_init(JSContext *ctx);
/* Web IDL §3.7's per-realm objects for PERFORMANCE TIMELINE §4 and §4.2.2 — the two interface prototype
   objects, the two Web IDL §3.7.1 interface objects, Web IDL §3.8's property references for both names, and
   this realm's §4/§5 state; declared into core/realm.h's intrinsic list.
   IT IS ONE FUNCTION AND NOT TWO because Web IDL §3.8's `define the global property references` is "To define
   the global property references on target, given realm realm" and names no Document, and §4 is
   `[Exposed=(Window,Worker)]`: an interface object placed from core/platform.c's per-document column reaches
   no realm that has no Document over it. §4.2.2's name was already placed here; §4's was not. */
void performance_observer_install_realm(JSContext *ctx);
void performance_observer_free(JSRuntime *rt);

/* WHICH ENTRY TYPES THIS BUILD SUPPORTS — §4.5's "the sequence of strings among the registry that are
 * supported for the global object".
 *
 * DECLARED BY THE COMPONENT THAT MINTS THE ENTRIES, from its own `_init`, and never listed here: the fact is
 * "does this build have a producer for this entry type", which only that producer knows. A list in this file
 * would be the second copy of it, and the copy that drifts is the one nobody runs against reality — the same
 * rule core/realm.h states about the intrinsic list and engine/idlgen.mjs states about member names.
 *
 * `name` is the registry's own key for the type and must OUTLIVE THE AGENT: every caller passes a string
 * literal, which is what lets this keep the pointer rather than a copy. Declaring one twice is a DCHECK — two
 * producers for one entry type is two answers to §4.5's question. The ORDER of declaration does not matter;
 * §4.5's "in alphabetical order" is applied where the frozen array is built. */
void performance_observer_declare_entry_type(const char *name);

/* §5.1 Queue a PerformanceEntry — the door every timing standard's mint calls. `entry` is BORROWED and `ctx`
   is the entry's relevant global object's realm, which for every producer in this build is the realm its own
   member ran in. See the header comment for which of §5.1's thirteen steps this build performs. */
void performance_observer_queue_entry(JSContext *ctx, JSValueConst entry);

#endif
