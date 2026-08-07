/* THE MessageEvent INTERFACE — HTML §9.4.1. See message_event.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_EVENTS_MESSAGE_EVENT_H
#define ENGINE_HOST_BROWSER_CORE_EVENTS_MESSAGE_EVENT_H
#include <stdbool.h>

#include "quickjs.h"

void message_event_init(JSContext *ctx);
void message_event_install(JSContext *ctx, JSValueConst global);
void message_event_free(JSContext *ctx);

/* `MessageEvent.prototype`, for an interface derived from THIS one to chain from. */
JSValue message_event_proto(void);

/* IS THIS A MessageEvent? The brand every 9.4 algorithm that hands one on performs — a private-symbol slot, so
   a page cannot forge it. */
bool message_event_is(JSContext *ctx, JSValueConst v);

/* Mint one the ENGINE fires: isTrusted TRUE, neither bubbling nor cancelable, `lastEventId` empty and `ports`
   `lastEventId` empty — which is what 9.4.2's "port message queue" and 9.4.4's window post message steps both
   dispatch. `origin` is the sender's, `source` is null or the sending WindowProxy/MessagePort, and `ports` is
   the [[TransferredValues]] that arrived with the message (JS_UNDEFINED for none), which this FREEZES.
   All three values are BORROWED; the answer is a new owned MessageEvent. */
JSValue message_event_new(JSContext *ctx, const char *type, JSValueConst data, const char *origin,
                          JSValueConst source, JSValueConst ports);

#endif
