/* MouseEvent — Pointer Events 4 "Interface MouseEvent" (UI Events hands the interface to it). See
   mouse_event.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_EVENTS_MOUSE_EVENT_H
#define ENGINE_HOST_BROWSER_CORE_EVENTS_MOUSE_EVENT_H
#include <stdbool.h>

#include "quickjs.h"

void mouse_event_init(JSContext *ctx);             /* the slot key + the IDL declarations (agent init) */
void mouse_event_install_protos(JSContext *ctx);   /* §3.7: one prototype AND one interface object per REALM */
/* Undone ONCE PER AGENT. The RUNTIME, not a realm: what it gives back is the agent's — a private
   Symbol, a class id and this interface's member declarations — and every prototype it built is in
   some realm's class-proto slot and goes with that realm. Reached from core/events/event.c's
   event_free_subclasses, which is core/platform.c's `event` row. */
void mouse_event_free(JSRuntime *rt);
/* `MouseEvent.prototype` for this realm — what a derived interface (PointerEvent, WheelEvent, DragEvent)
   chains to. OWNED: the caller frees. */
JSValue mouse_event_proto(JSContext *ctx);

/* DOM §2.5's CREATE AN EVENT using MouseEvent: every attribute at its un-initialized value. §4.5's createEvent
   is the caller. */
JSValue mouse_event_new(JSContext *ctx);

/* IS THIS OBJECT A MouseEvent — DOM §2.9 step 6.4's own question ("event is a MouseEvent object and event's
   type attribute is \"click\""), and the brand this interface's members check. It is the slot record and not the class,
   so it stays true across an interface that inherits this one. */
bool mouse_event_is(JSContext *ctx, JSValueConst v);

/* HTML §8.1.8.3 Event firing's FIRE A SYNTHETIC POINTER EVENT, minus the dispatch — steps 1-8 build the event
 * and step 9 dispatches it, and the caller is the dispatch. `view` is step 7's ("target's node document's
 * Window object, if any, and null otherwise"), so JS_NULL is a POSITIVE answer and not an unknown. The not
 * trusted flag is always set here because the only caller is HTML §6.5 Activation behavior of elements'
 * `click()`, whose step 4 is "Fire a synthetic pointer event named click at this element, with the not
 * trusted flag set".
 *
 * A NAMED RESIDUAL, AND THE CODE IS RIGHT RATHER THAN UNFINISHED. Step 1 is "Let event be the result of
 * creating an event using PointerEvent" and this engine has no PointerEvent interface, so the object it
 * creates is a MouseEvent — the interface PointerEvent INHERITS, which is why every step below it and DOM
 * §2.9 step 6.4's brand test are answered exactly as a browser answers them.
 *   NOT COVERED: PointerEvent's own identity and its own members — `pointerId`, `width`, `height`,
 *     `pressure`, `tangentialPressure`, `tiltX`, `tiltY`, `twist`, `altitudeAngle`, `azimuthAngle`,
 *     `pointerType`, `isPrimary`, `getCoalescedEvents()` and `getPredictedEvents()`.
 *   THE NEXT DIFF BUILDS core/events/pointer_event.c — a PointerEvent interface deriving through
 *     mouse_event_new_derived exactly as DragEvent and WheelEvent will, its global installed per realm beside
 *     this one, and this function creating one instead. (idl_inheritance.h already carries the row
 *     `{ "PointerEvent", "MouseEvent", IDL_PROTO_INHERITS }`; nothing installs it. GREP THAT ENTRY BEFORE
 *     ACTING ON THIS SENTENCE — it is a claim about the tree and the tree moves.)
 *   ITS ABSENCE SHOWS as a `click` listener reading `event.pointerType` and getting `undefined` where a
 *     browser answers "mouse" for a synthetic click, and as `PointerEvent` being absent from the global so
 *     `event instanceof PointerEvent` throws a ReferenceError rather than answering true.
 * Returns a new owned MouseEvent, or JS_EXCEPTION with the throw live. */
JSValue mouse_event_new_synthetic(JSContext *ctx, const char *type, JSValueConst view);

#endif
