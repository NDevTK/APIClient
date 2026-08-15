/* PopStateEvent — HTML §7.2.7.2. See pop_state_event.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_EVENTS_POP_STATE_EVENT_H
#define ENGINE_HOST_BROWSER_CORE_EVENTS_POP_STATE_EVENT_H
#include <stdbool.h>

#include "quickjs.h"

void pop_state_event_init(JSContext *ctx);             /* the slot key + the IDL declarations (agent init) */
/* §3.7: THIS REALM's prototype AND interface object. Declared into realm.h's one list by the init above, so a
   host adds the `_init` line and nothing else. */
void pop_state_event_install_protos(JSContext *ctx);
void pop_state_event_free(JSContext *ctx);

/* HTML §7.4.6.2's "fire an event named popstate at document's relevant global object, USING PopStateEvent, with
   the state attribute initialized to document's history object's state and hasUAVisualTransition initialized to
   true if a visual transition, to display a cached rendered state of the latest entry, was done by the user
   agent" — the EVENT half of it. The DISPATCH is the caller's, because §2.9 is a request the caller's machine
   parks on and because the caller is the one holding the target.
   `state` is BORROWED and is the value §7.4.6.2 just deserialized into THIS realm; `ctx` is therefore the realm
   the event belongs to, and minting one document's popstate out of another's would chain it to the wrong
   PopStateEvent.prototype.
   `has_ua_visual_transition` is the spec's own condition, asked of the user agent — the caller answers it.
   `popstate` neither bubbles nor is cancelable: DOM's fire-an-event gives an event none of the three flags
   unless the algorithm firing it says otherwise, and §7.4.6.2 says nothing. isTrusted is TRUE.
   Returns a new owned PopStateEvent. */
JSValue pop_state_event_new_to_fire(JSContext *ctx, JSValueConst state, bool has_ua_visual_transition);

#endif
