/* The Location interface — Blink core/frame. Installed on the baseline from the document's own URL. */
#ifndef ENGINE_HOST_BROWSER_CORE_FRAME_LOCATION_H
#define ENGINE_HOST_BROWSER_CORE_FRAME_LOCATION_H
#include "quickjs.h"

/* Install `location` (and `document.URL`'s eventual source) built from `url` — the document's address, which
   the host captured. A NULL or empty url installs nothing: a document with no address has no Location, and the
   page's own throw on reading it is the honest answer. */
void location_install(JSContext *ctx, JSValueConst global, const char *url);

#endif
