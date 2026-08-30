/* PageTransitionEvent — HTML §7.2.7.6. See page_transition_event.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_EVENTS_PAGE_TRANSITION_EVENT_H
#define ENGINE_HOST_BROWSER_CORE_EVENTS_PAGE_TRANSITION_EVENT_H
#include <stdbool.h>

#include "quickjs.h"

void page_transition_event_init(JSContext *ctx);            /* the slot key + the IDL declarations (agent init) */
/* §3.7: THIS REALM's prototype AND interface object. Declared into realm.h's one list by the init above, so a
   host adds the `_init` line and nothing else — a per-realm install written into each host's realm builder is
   the hand-copied list realm.h exists to delete. */
void page_transition_event_install_protos(JSContext *ctx);
/* Undone ONCE PER AGENT. The RUNTIME, not a realm: what it gives back is the agent's — a private
   Symbol, a class id and this interface's member declarations — and every prototype it built is in
   some realm's class-proto slot and goes with that realm. Reached from core/events/event.c's
   event_free_subclasses, which is core/platform.c's `event` row. */
void page_transition_event_free(JSRuntime *rt);

/* HTML §7.2.7.6's "FIRE A PAGE TRANSITION EVENT named eventName at a Window window with a boolean persisted" —
   the EVENT half of it: a trusted PageTransitionEvent whose `persisted` is `persisted` and whose `cancelable`
   and `bubbles` are BOTH true ("the values don't make any sense, since canceling the event does nothing and
   it's not possible to bubble past the Window object. They are set to true for historical reasons").
   §7.5.9 fires one named `pagehide` with the old document's SALVAGEABLE state, and §7.4.6's reactivation fires
   one named `pageshow` with true.
   THE FIRING HALF THE CALLER STILL OWES IS §2.9's LEGACY TARGET OVERRIDE FLAG, which this algorithm sets: with
   it, the dispatched event's `target` is the Window's associated DOCUMENT and not the Window, which is what
   every `e.target.location` in a pagehide handler reads. It is a parameter of DISPATCH and not a field of the
   event, so it cannot be carried here — and `event_target_fire` has no way to pass it yet.
   Returns a new owned PageTransitionEvent. */
JSValue page_transition_event_new(JSContext *ctx, const char *type, bool persisted);

#endif
