/* EventSource — HTML §9.2.2 "The EventSource interface" and HTML §9.2.3 "Processing model". See
   event_source.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_EVENTSOURCE_EVENT_SOURCE_H
#define ENGINE_HOST_BROWSER_CORE_EVENTSOURCE_EVENT_SOURCE_H

#include <stdbool.h>
#include "quickjs.h"

/* Declares the interface for the agent: the class, the member pool entries, the remote event task machine and
   the per-realm slot holding a realm's EventSource objects that are not CLOSED. Once per agent, from
   core/platform.c's list. */
void event_source_init(JSContext *ctx);
/* This realm's prototype, interface object and object set; declared into realm.h's list by the init. */
void event_source_install_realm(JSContext *ctx);
void event_source_free(JSRuntime *rt);

/* HTML §7.5.9 "Unloading documents"' unloading document cleanup steps: "For each EventSource object
   eventSource whose relevant global object is equal to window, forcibly close eventSource". Runs inside a
   flow; queues nothing. */
void event_source_forcibly_close_all(JSContext *ctx);

/* Whether `deliver` is an EventSource's processResponse (the steps HTML §9.2.2 step 15 hands Fetch), the one
   subresource park whose reply may arrive as a head before its body (solver/reply_body.h). */
bool event_source_is_response_delivery(JSContext *ctx, JSValueConst deliver);
/* Whether `deliver` takes the pieces of an EventSource's response body, and whether it still takes them: an
   object that close() moved to CLOSED aborted its fetch, so it reads no further piece. */
bool event_source_is_body_delivery(JSContext *ctx, JSValueConst deliver);
bool event_source_body_delivery_open(JSContext *ctx, JSValueConst deliver);

#endif
