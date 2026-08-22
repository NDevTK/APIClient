/* WINDOW — the browsing-context half of the global object (HTML 7.2.2). */
#ifndef ENGINE_HOST_BROWSER_CORE_FRAME_WINDOW_H
#define ENGINE_HOST_BROWSER_CORE_FRAME_WINDOW_H
#include <stdbool.h>

#include "quickjs.h"

/* Installs the Window members that answer "which browsing context am I?" — window/self/frames/parent/top,
   opener, closed, origin — plus `name`, which is attacker input. `origin` is the document address, the same
   one Location is built from. */
/* THE AGENT'S HALF: the Window and WindowProperties CLASS ids, registered once per JSRuntime. A class is a
   runtime registration and a prototype is a realm's object — §3.7 gives every realm its own, which is why
   `frames[0].Window.prototype !== Window.prototype` in a browser. */
void window_init(JSContext *ctx);

void window_install(JSContext *ctx, JSValueConst global, const char *url);

/* IS THIS A `Window` OBJECT — the class brand, which the global carries from window_install. DOM §2.9's event
   path walk (step 6.9.5) is the caller: a Window is the one entry of a propagation path that is not a node, and
   the branch it takes decides whether the event's target is retargeted there. No `ctx`, because a class id is
   the AGENT's registration and not a realm's — a child navigable's Window answers this in its parent's realm. */
bool window_is(JSValueConst v);

/* Release what this component HOLDS across the document's lifecycle — the object the per-flow `closed` byte is
   keyed by, and the BarProp prototype. */
void window_free(JSContext *ctx);

#endif
