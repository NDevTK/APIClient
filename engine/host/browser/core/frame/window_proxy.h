/* WindowProxy — HTML §7.2.5.1. See window_proxy.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_FRAME_WINDOW_PROXY_H
#define ENGINE_HOST_BROWSER_CORE_FRAME_WINDOW_PROXY_H
#include <stdbool.h>

#include "quickjs.h"

void window_proxy_init(JSContext *ctx);
void window_proxy_free(JSContext *ctx);

/* A proxy over a navigable whose active Window is `window` and whose active document's origin is `origin`.
   Both are the binding at this moment; a navigation replaces them, PER FLOW. */
JSValue window_proxy_new(JSContext *ctx, JSValueConst window, const char *origin);

/* IS THIS A WindowProxy? MessageEvent's `source` union names one, and §9.4.4's post takes one as its target. */
bool window_proxy_is(JSValueConst v);

/* The navigable's CURRENT active Window, as this flow sees it (owned). Crashes for a proxy whose navigable is
   in another WASM instance — that resolve is a host round trip and is not built; see window_proxy.c. */
JSValue window_proxy_window(JSContext *ctx, JSValueConst proxy);

/* The active document's origin, as this flow sees it — what §7.2.5.1's same-origin check reads. BORROWED. */
const char *window_proxy_origin(JSValueConst proxy);

/* NAVIGATE: the navigable's active Window is replaced while the proxy object stays the same, which is the whole
   reason a WindowProxy exists. The change is captured into the RUNNING FLOW's delta, so a sibling arm that did
   not navigate still resolves the proxy to the Window it knew. */
void window_proxy_navigate(JSContext *ctx, JSValueConst proxy, JSValueConst window, const char *origin);

#endif
