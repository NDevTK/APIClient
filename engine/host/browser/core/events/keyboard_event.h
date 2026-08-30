/* KeyboardEvent — UI Events §3.5.1. See keyboard_event.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_EVENTS_KEYBOARD_EVENT_H
#define ENGINE_HOST_BROWSER_CORE_EVENTS_KEYBOARD_EVENT_H
#include <stdbool.h>

#include "quickjs.h"

void keyboard_event_init(JSContext *ctx);            /* the slot key + the IDL declarations (agent init) */
void keyboard_event_install_protos(JSContext *ctx);  /* §3.7: one prototype AND one interface object per REALM */
/* Undone ONCE PER AGENT. The RUNTIME, not a realm: what it gives back is the agent's — a private
   Symbol, a class id and this interface's member declarations — and every prototype it built is in
   some realm's class-proto slot and goes with that realm. Reached from core/events/event.c's
   event_free_subclasses, which is core/platform.c's `event` row. */
void keyboard_event_free(JSRuntime *rt);
/* `KeyboardEvent.prototype` for this realm. OWNED: the caller frees. */
JSValue keyboard_event_proto(JSContext *ctx);

/* DOM §2.5's CREATE AN EVENT using KeyboardEvent: every attribute at its un-initialized value. §4.5's
   createEvent is the caller. */
JSValue keyboard_event_new(JSContext *ctx);

/* Does this object carry KeyboardEvent's own slot record — this interface's brand. */
bool keyboard_event_is(JSContext *ctx, JSValueConst v);

#endif
