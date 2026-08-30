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
   type is `click`"), and the brand this interface's members check. It is the slot record and not the class,
   so it stays true across an interface that inherits this one. */
bool mouse_event_is(JSContext *ctx, JSValueConst v);

#endif
