/* WindowProxy — HTML §7.2.5.1. See window_proxy.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_FRAME_WINDOW_PROXY_H
#define ENGINE_HOST_BROWSER_CORE_FRAME_WINDOW_PROXY_H
#include <stdbool.h>
#include <stdint.h>

#include "quickjs.h"

void window_proxy_init(JSContext *ctx);
void window_proxy_free(JSContext *ctx);

/* A proxy over a navigable whose active Window is `window` and whose active document's origin is `origin`.
   Both are the binding at this moment; a navigation replaces them, PER FLOW. */
JSValue window_proxy_new(JSContext *ctx, JSValueConst window, const char *origin);

/* A proxy over a navigable whose active document lives in ANOTHER WASM instance. It carries no Window — there
   is no local object to hold — so every read through it is a cross-document operation the flow suspends on. */
/* `name` is the BROWSING CONTEXT's name (the iframe element's `name` attribute, or §7.4's target), NULL for
   none. `parent` is the parent navigable — this instance's own Window for a child navigable, JS_UNDEFINED for a
   top-level one — and `opener` is §7.2.5's, JS_NULL when the navigable was not opened by a script. */
JSValue window_proxy_new_remote(JSContext *ctx, uint32_t doc, const char *origin, const char *name,
                                JSValueConst parent, JSValueConst opener);

/* §4.8.5's DESTROY A CHILD NAVIGABLE, from the side that owns the navigable: the proxy a page is holding stays
   the object it was — the spec files check that it does — and reports `closed`, an empty `name`, and no active
   document from that point on. Captured into the RUNNING FLOW's delta, so a sibling arm that never removed the
   element still sees the frame it knew. */
void window_proxy_close(JSContext *ctx, JSValueConst proxy);

/* §7.2.5.1's shared member surface, so a component that owns one of the cross-origin-accessible members
   (postMessage) installs it where every proxy sees it, and `a.postMessage === b.postMessage` holds. */
JSValueConst window_proxy_proto(void);

/* §7.2.5.1's members on that prototype. A LOCAL proxy answers every one by reading its own Window in this turn.
   A REMOTE one answers the NAVIGABLE's own state — window/self/frames/globalThis/parent/top/opener/closed/name,
   plus close() — in this turn too, because the navigable belongs to the instance that created it; only a member
   that reads through to the ACTIVE DOCUMENT SUSPENDS the flow on a host request carrying (document, world,
   member). Installed separately from init because it needs the IDL declaration machinery, which is not up when
   the class is registered. */
void window_proxy_install_members(JSContext *ctx);

/* IS THIS A WindowProxy? MessageEvent's `source` union names one, and §9.4.4's post takes one as its target. */
bool window_proxy_is(JSValueConst v);

/* IS THE NAVIGABLE'S ACTIVE DOCUMENT IN ANOTHER INSTANCE? One comparison against the one document identity the
   world registry also names worlds by — never a second scheme. Asserts that the proxy's document id and its
   Window agree, because a proxy where they disagree answers a cross-document read with the wrong document. */
bool window_proxy_is_remote(JSValueConst proxy);

/* WHICH DOCUMENT the navigable's active document is — what the host routes a cross-document request by. */
uint32_t window_proxy_doc(JSValueConst proxy);

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
