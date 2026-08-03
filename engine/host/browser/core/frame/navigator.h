/* The Navigator interface — Blink core/frame. Installed on the baseline as `navigator` (and its MSIE-era
   alias `clientInformation`, which HTML still specifies as the same object). */
#ifndef ENGINE_HOST_BROWSER_CORE_FRAME_NAVIGATOR_H
#define ENGINE_HOST_BROWSER_CORE_FRAME_NAVIGATOR_H
#include "quickjs.h"

/* Install `navigator` and `clientInformation`. Both name ONE Navigator object, because HTML says they do and a
   bundle that feature-detects through the alias must see the same identity. */
void navigator_install(JSContext *ctx, JSValueConst global);

#endif
