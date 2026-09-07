/* A Window's CURRENT EVENT — DOM §2.3 "Legacy extensions to the Window interface". See current_event.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_EVENTS_CURRENT_EVENT_H
#define ENGINE_HOST_BROWSER_CORE_EVENTS_CURRENT_EVENT_H

#include "quickjs.h"

/* The per-AGENT half: the realm-value slot the per-realm record lives in. A slot is a class id and a class id
   is a registration in the one runtime, so this runs once per agent. */
void current_event_init(JSContext *ctx);

/* The per-REALM half: this Window's baseline record — DOM §2.3's "Unless stated otherwise it is undefined" —
   and §2.3's `event` attribute on the global. */
void current_event_install(JSContext *ctx, JSValueConst global);

/* DOM §2.3: "Each Window object has an associated current event (undefined or an Event object)." OWNED: the
   caller frees. `ctx` names the Window, because a per-realm fact is answered per realm. */
JSValue current_event_get(JSContext *ctx);

/* …and the write. DOM §2.9 "Dispatching events"' inner invoke performs it twice — step 2.8.2 sets it to the
   event being dispatched, step 2.13 puts back what step 2.8.1 saved — and NOTHING ELSE in this engine may:
   the value a page reads from `window.event` is a fact about a dispatch that is running, so a second writer
   would be a second answer to one question. `v` is BORROWED; the record takes its own reference. */
void current_event_set(JSContext *ctx, JSValueConst v);

/* The agent half undone — a row on core/platform.h's third column. */
void current_event_free(JSRuntime *rt);

#endif
