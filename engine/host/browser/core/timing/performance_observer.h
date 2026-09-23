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
 * BUILT SINCE — §5.1 STEPS 9-12, §5.6 AND §2'S PERFORMANCE ENTRY BUFFER MAP. The map is per-global, keyed by
 * entry type, built EAGERLY with the realm, and holds §2's three-field tuple per DECLARED type with the
 * registry's maxBufferSize as its producer stated it. §5.6 is performed rather than short-circuited even
 * though both declared rows read `Infinite`, so a later producer's real maxBufferSize needs no edit here. The
 * paragraph below is what is LEFT of the residual that named them, and its two halves are now unblocked
 * rather than blocked — which is the only thing that changed about them.
 *
 * NOT BUILT, AND NAMED RATHER THAN SHAPED — §4.2 STEP 7.5 AND §5.3 STEP 3.3.7, THE TWO OBSERVER-SIDE READERS:
 *   WHAT IS NOT COVERED. §4.2 step 7.5's `buffered: true` registers the observer and delivers it NOTHING that
 *     was queued before it observed, and its `droppedEntriesCount` is absent from §4.1's dictionary rather
 *     than a number. Both now have a map to read; neither reads it.
 *   WHY THEY ARE STILL HERE, WHICH IS A SCOPE AND NO LONGER AN ARGUMENT. The blocker was the absent map and
 *     it is gone. What stands in their way is only that the diff which built the map was scoped to the
 *     buffer and its first reader, so these two are the next diff rather than a decision.
 *   WHAT THE NEXT DIFF BUILDS. §4.2 step 7.5 reading the map back into a newly-registered observer's buffer,
 *     and §5.3 step 3.3.7's dropped-entries walk over the same tuples. performance_observer_buffer is the
 *     accessor both want and it already exists. §2.1.1-§2.1.3 remain a SEPARATE decision that stays with
 *     performance_entry.h's argument, which the buffer's arrival did not touch.
 *   HOW ITS ABSENCE WOULD SHOW. `performance.mark('a')` followed by
 *     `new PerformanceObserver(cb).observe({type: 'mark', buffered: true})` never calls `cb`, where a browser
 *     calls it with the earlier mark; and a callback that reads `options.droppedEntriesCount` finds the member
 *     absent on every call. Both are now observations about these two steps ALONE, because the mark really is
 *     on the timeline: `performance.measure('m', 'a')` resolves it.
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
/* A PRODUCER DECLARES ITS TYPE AND THE REGISTRY ROW THAT GOVERNS IT. `max_buffer_size` is the TIMING ENTRY
   TYPES REGISTRY's maxBufferSize column for this type and is a `double` because several rows read `Infinite`;
   §5.6 compares the buffer's size against it. It arrives WITH the name rather than from a table here for the
   reason the name does: a list somebody maintains beside the producers is the second copy of a generated fact,
   and the copy that drifts is the one nobody runs against reality. */
void performance_observer_declare_entry_type(const char *name, double max_buffer_size);

/* THIS REALM'S §2 PERFORMANCE ENTRY BUFFER for one entry type — the Array §5.1 step 12 appends into. OWNED.
   Its READER today is USER TIMING §3.1 "Convert a mark to a timestamp"; see core/timing/performance_entry.h
   for why that reader, and not PERFORMANCE TIMELINE §2.1.1-§2.1.3, is the one this build may install. The
   entry type must be one a producer DECLARED: an undeclared one is a caller reading an absence as an empty
   timeline, which is the plausible datum that argument is about, so it aborts rather than answering. */
JSValue performance_observer_buffer(JSContext *ctx, const char *entry_type);

/* §5.1 Queue a PerformanceEntry — the door every timing standard's mint calls. `entry` is BORROWED and `ctx`
   is the entry's relevant global object's realm, which for every producer in this build is the realm its own
   member ran in. See the header comment for which of §5.1's thirteen steps this build performs. */
void performance_observer_queue_entry(JSContext *ctx, JSValueConst entry);

#endif
