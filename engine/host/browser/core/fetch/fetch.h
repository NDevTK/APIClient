/* The Fetch API — Blink core/fetch. One global, installed on the baseline before any flow runs. */
#ifndef ENGINE_HOST_BROWSER_CORE_FETCH_H
#define ENGINE_HOST_BROWSER_CORE_FETCH_H
#include "quickjs.h"

/* Install `fetch` on `global`. Every request forced execution reaches funnels one endpoint into the @H
   surface; the network itself is the trusted bridge's, never this sandbox's. */
void fetch_install(JSContext *ctx, JSValueConst global);

#endif
