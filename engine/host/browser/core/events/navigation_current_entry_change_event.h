/* NavigationCurrentEntryChangeEvent — HTML §7.2.7.1. See navigation_current_entry_change_event.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_EVENTS_NAVIGATION_CURRENT_ENTRY_CHANGE_EVENT_H
#define ENGINE_HOST_BROWSER_CORE_EVENTS_NAVIGATION_CURRENT_ENTRY_CHANGE_EVENT_H

#include "quickjs.h"

void navigation_current_entry_change_event_init(JSContext *ctx);
/* §3.7: THIS REALM's prototype and interface object — declared into core/realm.h's one list. */
void navigation_current_entry_change_event_install_protos(JSContext *ctx);
/* Undone ONCE PER AGENT. The RUNTIME, not a realm: what it gives back is the agent's — a private
   Symbol, a class id and this interface's member declarations — and every prototype it built is in
   some realm's class-proto slot and goes with that realm. Reached from core/events/event.c's
   event_free_subclasses, which is core/platform.c's `event` row. */
void navigation_current_entry_change_event_free(JSRuntime *rt);

/* HTML §7.2.6.4's and §7.2.6.6's "fire an event named currententrychange at navigation USING
   NavigationCurrentEntryChangeEvent, with its navigationType attribute initialized to navigationType and its
   from initialized to oldCurrentNHE" — the EVENT half. The DISPATCH is the caller's, because §2.9 runs the
   page's listeners and is therefore a request the calling machine parks on.
   `navigation_type` is the NavigationType string, or NULL for §7.2.6.6's updateCurrentEntry, whose whole
   distinguishing signal is that the attribute is null: "Returns the type of navigation which caused the
   current entry to change, or null if the change is due to navigation.updateCurrentEntry()."
   `from` is BORROWED — the NavigationHistoryEntry that was current BEFORE the change, dupped into the event.
   The event neither bubbles nor is cancelable (§7.2.6.4 sets none of DOM's three flags) and isTrusted is TRUE.
   Returns a new owned NavigationCurrentEntryChangeEvent, or JS_EXCEPTION. */
JSValue navigation_current_entry_change_event_new_to_fire(JSContext *ctx, const char *navigation_type,
                                                          JSValueConst from);

#endif
