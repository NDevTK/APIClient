/* WINDOW — the browsing-context half of the global object (HTML 7.2.2). */
#ifndef ENGINE_HOST_BROWSER_CORE_FRAME_WINDOW_H
#define ENGINE_HOST_BROWSER_CORE_FRAME_WINDOW_H
#include "quickjs.h"

/* Installs the Window members that answer "which browsing context am I?" — window/self/frames/parent/top,
   opener, closed, origin — plus `name`, which is attacker input. `origin` is the document address, the same
   one Location is built from. */
void window_install(JSContext *ctx, JSValueConst global, const char *url);
/* Release what this component HOLDS across the document's lifecycle — the object the per-flow `closed` byte is
   keyed by, and the BarProp prototype. */
void window_free(JSContext *ctx);

#endif
