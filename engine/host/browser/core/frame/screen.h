/* The Screen interface — Blink core/frame. Installed on the baseline as `screen`. */
#ifndef ENGINE_HOST_BROWSER_CORE_FRAME_SCREEN_H
#define ENGINE_HOST_BROWSER_CORE_FRAME_SCREEN_H
#include "quickjs.h"

void screen_install(JSContext *ctx, JSValueConst global);

/* THE MODELLED DISPLAY'S BIT DEPTH — the EXAMPLE `screen.colorDepth` carries, as a plain number. MEDIA QUERIES
   §4.5's `color` feature is the second reader of that one fact (bits per RGB component is this divided by
   three), and a media-feature table with its own constant would be a second answer to it. */
int screen_color_depth(void);

/* THE MODELLED DISPLAY'S GEOMETRY, in CSS pixels — the EXAMPLES §4.3's four size members carry, as plain
   numbers, and the numbers themselves rather than the concolics wrapping them.
   THREE STANDARDS READ THIS ONE FACT and two of them had already written their own copy of it. §4.3 exposes it;
   MEDIA QUERIES §12's `device-width` and `device-height` report it, and media_query.c held a second literal
   1920 and 1080 for exactly that until this existed; and CSSOM VIEW §2.3's WEB-EXPOSED AVAILABLE SCREEN AREA is
   the area a CLIENT WINDOW is positioned inside, which is what viewport.c derives `screenX`/`screenY` from.
   `avail` is that available area — §2.3's separate term, and a separate fact: `availHeight < height` is the
   "does the OS reserve a taskbar" question, and one shared source would tie it to the size question. */
double screen_width(void);
double screen_height(void);
double screen_avail_width(void);
double screen_avail_height(void);

#endif
