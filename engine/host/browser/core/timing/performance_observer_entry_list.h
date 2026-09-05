/* PERFORMANCE TIMELINE §4.2.2 The PerformanceObserverEntryList interface — the object a
 * PerformanceObserverCallback is handed, and the only reader of §5.5 Filter buffer by name and type in this
 * build.
 *
 * WHY IT IS ITS OWN COMPONENT AND NOT A STRUCT INSIDE THE OBSERVER. §4.2.2 is a separate interface with its own
 * class, its own per-realm prototype and its own interface object on the global, and its three members are the
 * whole of one algorithm (§5.5) over a list it does not own. The observer's file owns §4's state machine and
 * §5.1/§5.3's delivery; this one owns a filtered read. Folding them would give the observer's file a second
 * class and a second prototype to keep straight, which is the "one problem per file" this tree is built on.
 *
 * THE ENTRY LIST IS A JS ARRAY IN AN OWN SLOT, for the reason core/events/message_port.c gives about a port's
 * queue: an Array's mutations are property writes the per-flow COW delta already captures, and the snapshot
 * machinery already carries one across a park and a resume. A malloc'd list would revert its POINTER on a
 * context switch and leave the entries reachable from nothing.
 *
 * §4.2.2 says the list "is initialized upon construction" and declares NO constructor, so a page cannot build
 * one: the only mint is performance_observer_entry_list_new, called by §5.3 step 3.3.5. The interface OBJECT is
 * still on the global, because §3.7.1 puts one there for every exposed interface and because
 * `entries instanceof PerformanceObserverEntryList` is a thing a bundle writes.
 */
#ifndef ENGINE_HOST_BROWSER_CORE_TIMING_PERFORMANCE_OBSERVER_ENTRY_LIST_H
#define ENGINE_HOST_BROWSER_CORE_TIMING_PERFORMANCE_OBSERVER_ENTRY_LIST_H

#include "quickjs.h"

/* Declared ONCE PER AGENT — the class and the three member declarations. */
void performance_observer_entry_list_init(JSContext *ctx);
/* §3.7's per-realm interface prototype object and interface object; declared into core/realm.h's list. */
void performance_observer_entry_list_install(JSContext *ctx);
/* Agent teardown. It takes the RUNTIME because what it gives back is the agent's: the class id, the
   three pool entries and the slot key. The per-realm prototypes go with their contexts. */
void performance_observer_entry_list_free(JSRuntime *rt);

/* §4.2.2's ONE MINT — "a new PerformanceObserverEntryList, with its entry list set to entries" (§5.3 step
   3.3.5). `entries` is a JS Array of PerformanceEntry objects and is BORROWED; the new object holds its own
   reference to that same array, which is what makes the list the callback reads a snapshot the observer's
   buffer can no longer reach. Minted in `ctx`, which must be the realm whose page will read it. */
JSValue performance_observer_entry_list_new(JSContext *ctx, JSValueConst entries);

#endif
