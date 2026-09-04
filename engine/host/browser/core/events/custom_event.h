/* CustomEvent — DOM §2.4 Interface CustomEvent. See custom_event.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_EVENTS_CUSTOM_EVENT_H
#define ENGINE_HOST_BROWSER_CORE_EVENTS_CUSTOM_EVENT_H

#include "quickjs.h"

void custom_event_init(JSContext *ctx);              /* the slot key + the IDL declarations (agent init) */
/* Web IDL §3.7.3 Interface prototype object and §3.7.1 Interface object: THIS REALM's prototype AND interface
   object. Declared into realm.h's one list by the init above. */
void custom_event_install_protos(JSContext *ctx);
/* Undone ONCE PER AGENT. The RUNTIME, not a realm: what it gives back is the agent's — a private Symbol, a
   class id and this interface's member declarations — and every prototype it built is in some realm's
   class-proto slot and goes with that realm. Reached from core/events/event.c's event_free_subclasses, which
   is core/platform.c's `event` row. */
void custom_event_free(JSRuntime *rt);

/* DOM §2.5 Constructing events' CREATE AN EVENT using CustomEvent: every attribute at its un-initialized
   value, which for `any detail` is the dictionary's own `= null`. DOM §4.5 Interface Document's createEvent is
   the caller — its table names CustomEvent, so this row exists the moment the interface does, and
   core/events/create_event.c asserts the pairing from both sides. */
JSValue custom_event_new(JSContext *ctx);

#endif
