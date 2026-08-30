/* FocusEvent — UI Events §3.3.1, the interface HTML §6.6.4's focus model fires with. See focus_event.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_EVENTS_FOCUS_EVENT_H
#define ENGINE_HOST_BROWSER_CORE_EVENTS_FOCUS_EVENT_H
#include <stdbool.h>

#include "quickjs.h"

void focus_event_init(JSContext *ctx);             /* the slot key + the IDL declarations (agent init) */
void focus_event_install_protos(JSContext *ctx);   /* §3.7: one prototype AND one interface object per REALM */
/* Undone ONCE PER AGENT. The RUNTIME, not a realm: what it gives back is the agent's — a private
   Symbol, a class id and this interface's member declarations — and every prototype it built is in
   some realm's class-proto slot and goes with that realm. Reached from core/events/event.c's
   event_free_subclasses, which is core/platform.c's `event` row. */
void focus_event_free(JSRuntime *rt);
/* `FocusEvent.prototype` for this realm — what a derived interface would chain to. OWNED: the caller frees. */
JSValue focus_event_proto(JSContext *ctx);

/* DOM §2.5's CREATE AN EVENT using FocusEvent: every attribute at its un-initialized value. §4.5's createEvent
   is the caller. */
JSValue focus_event_new(JSContext *ctx);

/* IS THIS OBJECT A FocusEvent — Web IDL §3.7.5's brand check, which `relatedTarget` makes before reading the
   Event value it is an attribute over. It is the slot record and not the class, so it stays true across an
   interface that inherits this one. */
bool focus_event_is(JSContext *ctx, JSValueConst v);

/* HTML §6.6.4's "FIRE A FOCUS EVENT named e at t with a given related target r" — the EVENT half of it: "fire
   an event named e at t, USING FocusEvent, with the relatedTarget attribute initialized to r, the view
   attribute initialized to t's node document's relevant global object, and the composed flag set". The
   DISPATCH is the caller's, because §2.9 is a request the caller's machine parks on, and because the same
   event object is fired at one target and then released.
   `ctx` IS THE REALM THE EVENT BELONGS TO — the relevant realm of the target, which is also the realm whose
   global `view` is. Minting one document's focus event out of another document's realm would chain it to the
   wrong FocusEvent.prototype and would then reject that document's own Window as a `Window?`.
   `bubbles` is the caller's, because §3.3.4 gives `blur`/`focus` no bubbling and `focusout`/`focusin` the same
   event with it. isTrusted is TRUE: the engine fired it, which is the difference a page reads. */
JSValue focus_event_new_to_fire(JSContext *ctx, const char *type, bool bubbles,
                                JSValueConst related, JSValueConst view);

#endif
