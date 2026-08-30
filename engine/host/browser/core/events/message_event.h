/* THE MessageEvent INTERFACE — HTML §9.1 "The MessageEvent interface". See message_event.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_EVENTS_MESSAGE_EVENT_H
#define ENGINE_HOST_BROWSER_CORE_EVENTS_MESSAGE_EVENT_H
#include <stdbool.h>

#include "quickjs.h"

void message_event_init(JSContext *ctx);
void message_event_install(JSContext *ctx, JSValueConst global);
/* Undone ONCE PER AGENT. The RUNTIME, not a realm: what it gives back is the agent's — a private
   Symbol, a class id and this interface's member declarations — and every prototype it built is in
   some realm's class-proto slot and goes with that realm. Reached from core/events/event.c's
   event_free_subclasses, which is core/platform.c's `event` row. */
void message_event_free(JSRuntime *rt);

/* `MessageEvent.prototype`, for an interface derived from THIS one to chain from. */
/* PER REALM — see event.h. OWNED: the caller frees. */
JSValue message_event_proto(JSContext *ctx);
/* §9.1's prototype for ONE realm; run beside the realm's other intrinsics, once per realm. */
void message_event_install_proto(JSContext *ctx);

/* IS THIS A MessageEvent? The brand every 9.4 algorithm that hands one on performs — a private-symbol slot, so
   a page cannot forge it. */
bool message_event_is(JSContext *ctx, JSValueConst v);

/* Mint one the ENGINE fires: isTrusted TRUE, neither bubbling nor cancelable, `lastEventId` empty and `ports`
   `lastEventId` empty — which is what 9.4.2's "port message queue" and 9.4.4's window post message steps both
   dispatch. `origin` is the sender's, `source` is null or the sending WindowProxy/MessagePort, and `ports` is
   the [[TransferredValues]] that arrived with the message (JS_UNDEFINED for none), which this FREEZES.
   All four values are BORROWED; the answer is a new owned MessageEvent.
   `origin` IS A VALUE AND NOT A C STRING, and that is the difference between a message this agent sent and one
   that came from outside it. Within an agent the origin is always this agent's own serialization and a `char *`
   said so; a message from a CROSS-ORIGIN document is unknown external input by §9.3.2.2 "User agents" — the
   attacker chooses which origin posts by owning the domain, and the engine may not decide it — so the value the
   caller hands over is a concolic carrying the stamped origin as its example. A `char *` could not express one,
   and coercing it here is the one thing an unknown refuses. The three callers whose origin IS a serialization
   spell it as a string at their own call, which is one JS_NewString each and no second entry point. */
JSValue message_event_new(JSContext *ctx, const char *type, JSValueConst data, JSValueConst origin,
                          JSValueConst source, JSValueConst ports);

/* §9.3.3's and §9.4.4's `newPorts` — the MessagePorts among a StructuredDeserializeWithTransfer's
   [[TransferredValues]], in order. A FILTER and not a conversion: a transferred ArrayBuffer is delivered and is
   simply not a port, where `sequence<MessagePort>`'s conversion would make it a TypeError. Answers a plain
   owned Array to hand straight to message_event_new, which is what freezes it. */
JSValue message_event_ports_of(JSContext *ctx, JSValueConst transferred);

#endif
