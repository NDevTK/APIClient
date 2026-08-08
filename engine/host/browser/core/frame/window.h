/* WINDOW — the browsing-context half of the global object (HTML 7.2.2). */
#ifndef ENGINE_HOST_BROWSER_CORE_FRAME_WINDOW_H
#define ENGINE_HOST_BROWSER_CORE_FRAME_WINDOW_H
#include "quickjs.h"

/* Installs the Window members that answer "which browsing context am I?" — window/self/frames/parent/top,
   opener, closed, origin — plus `name`, which is attacker input. `origin` is the document address, the same
   one Location is built from. */
void window_install(JSContext *ctx, JSValueConst global, const char *url);

/* §7.2.5's INTERFACE PROTOTYPE OBJECT. Every Window member except the [LegacyUnforgeable] five (`window`,
   `self`, `location`, `top`, `document`) is declared HERE rather than on the global — a component that owns one
   (timers, `open`, `postMessage`, the event-handler attributes) installs it on this. */
JSValueConst window_proto(void);
/* Release what this component HOLDS across the document's lifecycle — the object the per-flow `closed` byte is
   keyed by, and the BarProp prototype. */
void window_free(JSContext *ctx);

#endif
