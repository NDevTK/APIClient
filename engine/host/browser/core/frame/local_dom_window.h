/* LocalDOMWindow global-scope API install — Blink core/frame. The window global exposes the web-platform
 * interface constructors (XMLHttpRequest, the observers, WebSocket/Worker, Notification, Intl, ...); Blink's
 * global scope references every [Exposed=Window] interface, so this installer legitimately depends on all of
 * them. Extracted from main.c so the window-globals install is a browser COMPONENT, not scheduler code. */
#ifndef ENGINE_HOST_BROWSER_LOCAL_DOM_WINDOW_H
#define ENGINE_HOST_BROWSER_LOCAL_DOM_WINDOW_H
#include "quickjs.h"
void install_window_apis(JSContext *ctx, JSValue g, JSValueConst el_proto);   /* interface constructors (observers/XHR/WebSocket/...) */
void install_window_objects(JSContext *ctx, JSValue g);   /* Math/Date opaque + timers/storage/URL/fetch-objects */
#endif
