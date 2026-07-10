#ifndef ENGINE_HOST_BROWSER_CORE_INTERSECTION_OBSERVER_H
#define ENGINE_HOST_BROWSER_CORE_INTERSECTION_OBSERVER_H
#include "quickjs.h"
/* IntersectionObserver — Blink core/intersection_observer. Its callback fires on intersection changes that never
 * happen headless, so the ctor registers the callback as a driven scheduler flow; the instance SHAPE comes from
 * canonical IntersectionObserver IDL (root/rootMargin/thresholds concolic; observe/disconnect deliberate noop),
 * replacing the shared generic {observer} opaque that drifted from every observer's real interface. */
JSValue js_intersection_observer_ctor(JSContext *ctx, JSValueConst nt, int argc, JSValueConst *argv);
#endif
