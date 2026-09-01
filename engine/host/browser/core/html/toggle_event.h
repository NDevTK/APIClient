/* The ToggleEvent interface — HTML §6.5.1 The ToggleEvent interface. See toggle_event.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_HTML_TOGGLE_EVENT_H
#define ENGINE_HOST_BROWSER_CORE_HTML_TOGGLE_EVENT_H

#include <stdbool.h>

#include "quickjs.h"

/* Declared once per AGENT; the prototype and the interface object are per REALM, through realm.h's one list. */
void toggle_event_init(JSContext *ctx);
void toggle_event_free(JSRuntime *rt);

/* THE ENGINE'S OWN FIRE, minted with the three initialisers the standard's callers name and nothing else.
   `old_state` and `new_state` are the spec's strings ("open", "closed"); `source` is the `Element?` — JS_NULL
   when the algorithm has none. `bubbles` and `cancelable` are the FIRE's, not this interface's: §4.11.4's
   close-the-dialog fires `beforetoggle` with neither, and the popover algorithms fire it cancelable, so the
   caller states what its own step says rather than this file guessing from the type. TRUSTED — the user agent
   fired it. Returns the event (owned) or JS_EXCEPTION with the throw live. */
JSValue toggle_event_new(JSContext *ctx, const char *type, const char *old_state, const char *new_state,
                         JSValueConst source, bool bubbles, bool cancelable);

#endif
