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
void before_unload_event_free(JSContext *ctx);

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

#endif
