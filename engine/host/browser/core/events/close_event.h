/* CloseEvent — WebSockets §6 "The CloseEvent interface". See close_event.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_EVENTS_CLOSE_EVENT_H
#define ENGINE_HOST_BROWSER_CORE_EVENTS_CLOSE_EVENT_H
#include <stdbool.h>
#include <stdint.h>

#include "quickjs.h"

/* Declares the interface for the agent: the slot key, the class and the constructor's IDL declaration.
   Called by event_init, as every Event subclass is. */
void close_event_init(JSContext *ctx);
/* This realm's prototype and interface object, declared into realm.h's list by the init above. */
void close_event_install_protos(JSContext *ctx);
/* Releases what the init took, once per agent; called by event_free. */
void close_event_free(JSRuntime *rt);

/* A trusted `close` event for WebSockets §4 "Feedback from the protocol"'s closed task: not bubbling, not
   cancelable, with the three attributes initialized as given. `reason` is borrowed UTF-8. Returns an owned
   event or JS_EXCEPTION. */
JSValue close_event_new(JSContext *ctx, bool was_clean, uint16_t code, const char *reason);

#endif
