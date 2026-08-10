/* The Screen interface — Blink core/frame. Installed on the baseline as `screen`. */
#ifndef ENGINE_HOST_BROWSER_CORE_FRAME_SCREEN_H
#define ENGINE_HOST_BROWSER_CORE_FRAME_SCREEN_H
#include "quickjs.h"

void screen_install(JSContext *ctx, JSValueConst global);

/* THE MODELLED DISPLAY'S BIT DEPTH — the EXAMPLE `screen.colorDepth` carries, as a plain number. MEDIA QUERIES
   §4.5's `color` feature is the second reader of that one fact (bits per RGB component is this divided by
   three), and a media-feature table with its own constant would be a second answer to it. */
int screen_color_depth(void);

#endif
