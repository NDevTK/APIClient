#ifndef ENGINE_HOST_WINNAME_H
#define ENGINE_HOST_WINNAME_H
#include "quickjs.h"
/* window.name — an ATTACKER-CONTROLLED, RAW string source: the opener sets it and it PERSISTS across a
 * navigation, and unlike location.hash/search the browser does NOT percent-encode it, so a breakout via
 * `el.innerHTML = window.name` / `eval(window.name)` needs no decode (a very common real DOM-XSS + JSONP
 * vector). A page WRITE overrides it (the value becomes page-owned concrete), so a page that overwrites name
 * before the sink blocks the attacker path — no false PoC. Read = attacker opaque (or the @S replay candidate,
 * delivered RAW). */
JSValue js_winname_get(JSContext *ctx, JSValueConst this_val);
JSValue js_winname_set(JSContext *ctx, JSValueConst this_val, JSValueConst val);
void winname_free(JSContext *ctx);
#endif
