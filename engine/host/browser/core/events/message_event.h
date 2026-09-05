/* THE MessageEvent INTERFACE — HTML §9.1 "The MessageEvent interface". See message_event.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_EVENTS_MESSAGE_EVENT_H
#define ENGINE_HOST_BROWSER_CORE_EVENTS_MESSAGE_EVENT_H
#include <stdbool.h>

#include "quickjs.h"

void message_event_init(JSContext *ctx);
/* Undone ONCE PER AGENT. The RUNTIME, not a realm: what it gives back is the agent's — a private
   Symbol, a class id and this interface's member declarations — and every prototype it built is in
   some realm's class-proto slot and goes with that realm. Reached from core/events/event.c's
   event_free_subclasses, which is core/platform.c's `event` row. */
void message_event_free(JSRuntime *rt);

/* `MessageEvent.prototype`, for an interface derived from THIS one to chain from. */
/* PER REALM — see event.h. OWNED: the caller frees. */
JSValue message_event_proto(JSContext *ctx);
/* HTML §9.1 The MessageEvent interface's prototype, its Web IDL §3.7.1 Interface object's interface object and
   the Web IDL §3.8 property reference for it — for ONE realm, declared into core/realm.h's list. ONE entry
   because Web IDL §3.8 Platform objects implementing interfaces' `define the global property references` is
   given "target" and "a realm realm" and its step 1 population is "every interface that is exposed in realm":
   no Document appears in it. html.idl declares the interface `[Exposed=(Window,Worker,AudioWorklet)]`, so a
   WORKER realm owes the name too — and while the interface object was installed from core/platform.c's
   per-document column, a worker realm, which reaches no platform_document_install, received neither. */
void message_event_install_realm(JSContext *ctx);

/* IS THIS A MessageEvent? The brand every 9.4 algorithm that hands one on performs — a private-symbol slot, so
   a page cannot forge it. */
bool message_event_is(JSContext *ctx, JSValueConst v);

/* Mint one the ENGINE fires: isTrusted TRUE, neither bubbling nor cancelable, and `ports` FROZEN.
   `origin` is the sender's, `source` is null or the sending WindowProxy/MessagePort, `lastEventId` is §9.1's
   fifth member spelled below, and `ports` is the [[TransferredValues]] that arrived with the message
   (JS_UNDEFINED for none). All five values are BORROWED; the answer is a new owned MessageEvent.

   THE PARAMETER LIST IS §9.1'S MEMBER LIST, IN §9.1'S ORDER — `data, origin, lastEventId, source, ports`,
   which is the order this file's own `ME_NAMES` and `initMessageEvent` already state. That is why
   `lastEventId` sits third rather than last: a mint whose arguments are the interface's members in the
   interface's order is one a reader checks against the IDL instead of against this comment.

   `lastEventId` USED TO BE HARDCODED TO THE EMPTY STRING HERE, AND THAT SENTENCE WAS TRUE OF EVERY CALLER
   THERE WAS AND IS RETIRED RATHER THAN DELETED, because a reader who re-derives it will re-introduce it. It
   said the mint mints `lastEventId` empty, "which is what 9.4.2's port message queue and 9.4.4's window post
   message steps both dispatch", and the SUBSTANCE of that is still exactly right — a port delivery, a window
   post and §9.5's bus each have no last event ID, so all three pass the empty string at their own call, in one
   line each. Two things about it were wrong. Its CITATIONS were shifted by one slot each, which is worth
   recording because a shifted PAIR reads as consistent: §9.4.2 is Message channels and §9.4.4 is Message
   ports, so the PORT MESSAGE QUEUE is §9.4.4's and not §9.4.2's, and the WINDOW POST MESSAGE STEPS are
   §9.3.3 Posting messages', which is a different section of a different subsection — checked against the
   standard's own heading list rather than corrected from memory. And its CLAIM was that those callers are all
   the callers there can be. HTML §9.2.6 Interpreting an event stream's dispatch the event step 5 initializes
   the event's "lastEventId attribute to the last event ID string of the event source", and a mint that fixes
   the member at "" cannot express that step at all — so a contract that read as a description of §9.1 was a
   description of three of its four callers, and the fourth could not be written. A comment that names a
   hazard and offers no exit from it does not warn callers away, it guarantees them into it; the exit is this
   parameter.

   IT IS A VALUE AND NOT A `const char *`, FOR EXACTLY THE ARGUMENT `origin` MAKES ONE LINE DOWN, and the
   argument is stronger here rather than weaker. §9.2.6's last event ID buffer is filled from an `id` field of
   an EVENT STREAM — bytes a server the page addressed chose — so it is unknown external input by
   construction, in the way a cross-origin `origin` is by §9.3.2.2 "User agents". A `char *` cannot express one
   and coercing it is the one thing an unknown refuses. The three callers whose last event ID is genuinely the
   empty string spell it as a string at their own call, which is one JS_NewString each and no second entry
   point — the same shape, and the same reason, as `origin`.

   `origin` IS A VALUE AND NOT A C STRING, and that is the difference between a message this agent sent and one
   that came from outside it. Within an agent the origin is always this agent's own serialization and a `char *`
   said so; a message from a CROSS-ORIGIN document is unknown external input by §9.3.2.2 "User agents" — the
   attacker chooses which origin posts by owning the domain, and the engine may not decide it — so the value the
   caller hands over is a concolic carrying the stamped origin as its example. A `char *` could not express one,
   and coercing it here is the one thing an unknown refuses. The three callers whose origin IS a serialization
   spell it as a string at their own call, which is one JS_NewString each and no second entry point. */
JSValue message_event_new(JSContext *ctx, const char *type, JSValueConst data, JSValueConst origin,
                          JSValueConst last_event_id, JSValueConst source, JSValueConst ports);

/* §9.3.3's and §9.4.4's `newPorts` — the MessagePorts among a StructuredDeserializeWithTransfer's
   [[TransferredValues]], in order. A FILTER and not a conversion: a transferred ArrayBuffer is delivered and is
   simply not a port, where `sequence<MessagePort>`'s conversion would make it a TypeError. Answers a plain
   owned Array to hand straight to message_event_new, which is what freezes it. */
JSValue message_event_ports_of(JSContext *ctx, JSValueConst transferred);

#endif
