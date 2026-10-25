/* Which user act runs a function as an event listener — the act a PoC verified under an engine drive of that
   listener must state for a browser to reproduce it. See listener_act.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_EVENTS_LISTENER_ACT_H
#define ENGINE_HOST_BROWSER_CORE_EVENTS_LISTENER_ACT_H

#include "quickjs.h"

/* The act that runs `fn` as an event listener in this realm's document, as one step of a PoC envelope's
   `acts` list — `{"kind":"click","target":"#id","type":"click"}` — or NULL when no node of the document and not
   its Window holds `fn` as a listener of a type a page user can cause. `kind` is `click` for a click and
   `dispatch` for another user-interface event, `input` for a person typing into the text control whose
   `input`, `keyup` or `change` listener `fn` is, `message` for a postMessage to the Window; `target` is a
   locator a page script can resolve (`#id` for a unique identifier, otherwise a tag path from the document
   element, or `document` / `window`); `handler` is the listener's own `name` where it is a non-empty
   identifier, for a reader. Asked in the running flow's world, so it reads the registrations that world holds.
   malloc'd. */
char *listener_act_of(JSContext *ctx, JSValueConst fn);

/* The text control a person types into to run `fn` (the target of its `input` act), borrowed from the node's
   wrapper, or JS_UNDEFINED where `fn`'s act is not typing. */
JSValueConst listener_act_typed_control(JSContext *ctx, JSValueConst fn);

#endif
