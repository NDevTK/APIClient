/* document.cookie — a per-CODE-FLOW cookie jar (Blink core/dom/Document cookie), NOT a headless unknown:
 * cookies are per-flow STATE, so a value the bundle sets round-trips (document.cookie='session='+t; …
 * document.cookie -> the session). See cookie.c. The getter/setter are installed on the document accessor. */
#ifndef ENGINE_HOST_BROWSER_COOKIE_H
#define ENGINE_HOST_BROWSER_COOKIE_H
#include "quickjs.h"
JSValue js_cookie_get(JSContext *ctx, JSValueConst this_val);                 /* document.cookie getter */
JSValue js_cookie_set(JSContext *ctx, JSValueConst this_val, JSValueConst v); /* document.cookie setter */
void cookie_seed(JSContext *ctx, const char *raw);                           /* seed REAL same-origin cookies (content-script edge) */
void cookie_free(JSContext *ctx);
#endif
