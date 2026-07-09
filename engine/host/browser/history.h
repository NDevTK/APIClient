/* History — Blink core/frame/History. A real state machine (see history.c): pushState/replaceState set
 * history.state, so a page that stashes SSR/route state via pushState and later reads history.state gets its
 * REAL value, not a noop write + opaque read. */
#ifndef ENGINE_HOST_BROWSER_HISTORY_H
#define ENGINE_HOST_BROWSER_HISTORY_H
#include "quickjs.h"
JSValue js_history_make(JSContext *ctx);   /* the window.history object */
#endif
