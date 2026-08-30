/* BeforeUnloadEvent — HTML §7.2.7.7. See before_unload_event.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_EVENTS_BEFORE_UNLOAD_EVENT_H
#define ENGINE_HOST_BROWSER_CORE_EVENTS_BEFORE_UNLOAD_EVENT_H
#include <stdbool.h>

#include "quickjs.h"

void before_unload_event_init(JSContext *ctx);            /* the slot key + the IDL declarations (agent init) */
/* §3.7: THIS REALM's prototype AND interface object. Declared into realm.h's one list by the init above, so a
   host adds the `_init` line and nothing else. The interface object is NOT a constructor: §7.2.7.7's IDL
   declares none, so `new BeforeUnloadEvent()` is a TypeError and `document.createEvent('BeforeUnloadEvent')`
   is the only way a page makes one — which is why DOM §4.5's table has a row for it. */
void before_unload_event_install_protos(JSContext *ctx);
/* Undone ONCE PER AGENT. The RUNTIME, not a realm: what it gives back is the agent's — a private
   Symbol, a class id and this interface's member declarations — and every prototype it built is in
   some realm's class-proto slot and goes with that realm. Reached from core/events/event.c's
   event_free_subclasses, which is core/platform.c's `event` row. */
void before_unload_event_free(JSRuntime *rt);

/* HTML §7.5.9's "fire beforeunload" step 4: `beforeunload`, CANCELABLE, at the document's relevant global
   object, using this interface. TRUSTED — the engine fired it. `returnValue` starts as the empty string, which
   §7.2.7.7 states as "when the event is created". Returns a new owned BeforeUnloadEvent. */
JSValue before_unload_event_new(JSContext *ctx);

/* DOES THIS FIRED EVENT ASK TO CANCEL THE UNLOAD — read AFTER the dispatch, and it is the WHOLE of §7.5.9's
   fourth bullet: "eventFiringResult is false, OR the returnValue attribute of event is not the empty string".
   `eventFiringResult` is what DOM §2.10's "fire an event" returns, which is false exactly when the event's
   CANCELED FLAG is set — so the two disjuncts are the canceled flag and a non-empty returnValue, and either
   one alone is the answer. That is why this is one function and not two reads at the call site: the legacy
   half is the half that gets forgotten, and forgetting it silently unloads a document the page asked to keep.
   It answers ONLY that bullet. §7.5.9's other three (the prompt has not already been shown, the sandboxed
   modals flag, and sticky activation) are facts about the DOCUMENT and the AGENT, not about the event, and
   they belong to the unload machine that knows them. */
bool before_unload_event_asks_to_cancel(JSContext *ctx, JSValueConst ev);

/* IS THIS A BeforeUnloadEvent — the slot record is the brand, exactly as error_event.h's test is. HTML
   §8.1.8.1's event handler processing algorithm step 6 asks it: its first arm is "If event IS A
   BeforeUnloadEvent OBJECT and event's type is `beforeunload`", and the standard writes both conjuncts because
   they can come apart — `document.createEvent('BeforeUnloadEvent')` then `initEvent('x')` is one of these with
   another type, and a plain `new Event('beforeunload')` is the other type without the interface. That second
   case is the one the standard's own note is about, and it lands in step 6's LAST arm. */
bool before_unload_event_is(JSContext *ctx, JSValueConst ev);

/* §7.2.7.7's `returnValue`, as the two primitives HTML §8.1.8.1 step 6's first arm is written out of: "If
   event's returnValue attribute's value IS THE EMPTY STRING, then set event's returnValue attribute's value to
   return value." The `if` is §8.1.8.1's and lives there; the attribute is this interface's and lives here.
   `v` MUST already be a DOMString — the conversion is OnBeforeUnloadEventHandler's return type and has already
   run by the time step 6 reads the value, and a body that coerced one here would run the page's toString from
   C, which is the drive-to-completion this engine aborts on. */
bool before_unload_event_return_value_is_empty(JSContext *ctx, JSValueConst ev);
void before_unload_event_set_return_value(JSContext *ctx, JSValueConst ev, JSValueConst v);

#endif
