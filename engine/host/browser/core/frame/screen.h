/* Screen — Blink core/frame/Screen. window.screen with concolic dimensions (a real browser HAS a screen even
 * headless — the default-viewport size as example, forkable so a responsive `if(screen.width<768)` explores
 * both worlds). Also installs the window viewport properties (innerWidth/innerHeight/devicePixelRatio). See
 * screen.c. Being defined at all also fixes the crash where `screen.width` accessed `.width` on undefined. */
#ifndef ENGINE_HOST_BROWSER_SCREEN_H
#define ENGINE_HOST_BROWSER_SCREEN_H
#include "quickjs.h"
JSValue js_screen_make(JSContext *ctx);            /* window.screen */
void screen_install_viewport(JSContext *ctx, JSValueConst g);   /* innerWidth/innerHeight/... on the window */
#endif
