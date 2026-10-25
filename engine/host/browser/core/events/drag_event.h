/* DragEvent — HTML §6.11.4 The DragEvent interface. See drag_event.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_EVENTS_DRAG_EVENT_H
#define ENGINE_HOST_BROWSER_CORE_EVENTS_DRAG_EVENT_H

#include "quickjs.h"

void drag_event_init(JSContext *ctx);            /* the slot key + the IDL declarations (agent init) */
void drag_event_install_protos(JSContext *ctx);  /* Web IDL §3.7: one prototype and interface object per realm */
/* Undone once per agent, from core/events/event.c's event_free_subclasses (core/platform.c's `event` row). */
void drag_event_free(JSRuntime *rt);

/* DOM §2.5's create an event using DragEvent: every attribute at its un-initialized value. §4.5's createEvent
   is the caller. */
JSValue drag_event_new(JSContext *ctx);

/* HTML §6.11.4's fire a DND event, steps 7-9: a trusted DragEvent named `type`, bubbling, cancelable unless
   `type` is dragleave or dragend, with `view` (a Window, borrowed) and `data_transfer` (borrowed), and every
   mouse attribute 0 since no pointing device is modelled. Owned. */
JSValue drag_event_new_for_dnd(JSContext *ctx, const char *type, JSValueConst view, JSValueConst data_transfer);

#endif
