/* WheelEvent — Pointer Events 4 §12.1 "WheelEvent interface" (UI Events hands the interface to it, exactly as
   it hands over MouseEvent — see wheel_event.c). */
#ifndef ENGINE_HOST_BROWSER_CORE_EVENTS_WHEEL_EVENT_H
#define ENGINE_HOST_BROWSER_CORE_EVENTS_WHEEL_EVENT_H

#include "quickjs.h"

void wheel_event_init(JSContext *ctx);             /* the slot key + the IDL declarations (agent init) */
/* Web IDL §3.7 "Interfaces": one prototype AND one interface object per REALM. */
void wheel_event_install_protos(JSContext *ctx);
/* Undone ONCE PER AGENT. The RUNTIME, not a realm: what it gives back is the agent's — a private Symbol, a
   class id and this interface's member declarations — and every prototype it built is in some realm's
   class-proto slot and goes with that realm. Reached from core/events/event.c's event_free_subclasses, which
   is core/platform.c's `event` row. */
void wheel_event_free(JSRuntime *rt);

/* THERE IS NO `wheel_event_proto` AND NO BRAND PREDICATE HERE, AND THAT IS A STATEMENT RATHER THAN AN
   OMISSION. mouse_event.h exports its prototype because THREE interfaces chain to it, and pointer_event.h
   exports its brand because PointerEventInit's two `sequence<PointerEvent>` members need Web IDL §3.2.15
   "Interface types"' `I` as a predicate. Nothing inherits WheelEvent and no dictionary in the platform
   declares a member of its type, so either would be a symbol with no reader — the mirror of the
   defaulted-field defect, and the thing to check before adding one is that a caller exists. A brand predicate
   was written here and DELETED in the same diff when `-Wunused-function` reported it: the file's own accessor
   reads the slot record directly, exactly as pointer_event.c's does, so the predicate had no second caller to
   justify it. */

#endif
