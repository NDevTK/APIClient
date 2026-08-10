/* DOM §4.5 createEvent — the legacy event factory. See create_event.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_EVENTS_CREATE_EVENT_H
#define ENGINE_HOST_BROWSER_CORE_EVENTS_CREATE_EVENT_H

#include "quickjs.h"

/* §4.5 "createEvent(interface)", steps 1-9. `iface` is the already-converted DOMString the member was given.
   Returns a new owned event, or JS_EXCEPTION with a "NotSupportedError" DOMException pending — which is the
   answer for a name outside the table AND for a name in it whose interface this engine does not expose. */
JSValue create_event(JSContext *ctx, JSValueConst global, const char *iface);

#endif
