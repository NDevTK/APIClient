/* CSSOM environment reads — Blink core/css/cssom + core/css/MediaQueryList. getComputedStyle and matchMedia
 * need LAYOUT / VIEWPORT the headless engine genuinely cannot compute, so each returns the OPAQUE concolic
 * value (any property read or method call stays opaque -> a gate on it FORKS, a callback arg is driven) rather
 * than a hand-rolled fixed-shape stub object the page's JS wouldn't expect (its `.width`/`.matches` would read
 * undefined instead of a value). Not a stub — the honest unknown the solver handles soundly. See cssom.c. */
#ifndef ENGINE_HOST_BROWSER_CSSOM_H
#define ENGINE_HOST_BROWSER_CSSOM_H
#include "quickjs.h"
JSValue js_get_computed_style(JSContext *ctx, JSValueConst t, int c, JSValueConst *v);   /* getComputedStyle(el) */
JSValue js_match_media(JSContext *ctx, JSValueConst t, int c, JSValueConst *v);          /* matchMedia(query) */
JSValue js_el_inline_style(JSContext *ctx, JSValueConst el_obj);                         /* el.style — per-flow inline CSSStyleDeclaration */
void cssom_init(JSContext *ctx);   /* register the native CSSStyleDeclaration class + prototype (call once at setup) */
#endif
