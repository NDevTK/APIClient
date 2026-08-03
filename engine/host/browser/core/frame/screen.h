/* The Screen interface — Blink core/frame. Installed on the baseline as `screen`. */
#ifndef ENGINE_HOST_BROWSER_CORE_FRAME_SCREEN_H
#define ENGINE_HOST_BROWSER_CORE_FRAME_SCREEN_H
#include "quickjs.h"

void screen_install(JSContext *ctx, JSValueConst global);

#endif
