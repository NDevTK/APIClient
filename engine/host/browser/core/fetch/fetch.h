/* The Fetch API — Blink core/fetch. One global, installed on the baseline before any flow runs. */
#ifndef ENGINE_HOST_BROWSER_CORE_FETCH_H
#define ENGINE_HOST_BROWSER_CORE_FETCH_H
#include "quickjs.h"

/* Install `fetch` on `global`. Every request forced execution reaches funnels one endpoint into the @H
   surface; the network itself is the trusted bridge's, never this sandbox's. */
void fetch_install(JSContext *ctx, JSValueConst global);

/* THE PENDING REGISTER — the requests flows are parked on, and the bodies the host delivers back.
   The sandbox cannot fetch (SECURITY.md), so a fetch returns a PENDING promise and its URL is reported here;
   the host fetches through the trusted chokepoint and provides the body, which resolves the flow's
   continuation. This is what makes a consumed reply the richest source of real example values, and what makes
   a lazy JS chunk more code the same flow runs. */

/* The URLs with an unresolved promise, newline-joined, or "" — the exact shape qjs_pending returns. The buffer
   is the register's and is valid until the next call. */
const char *fetch_pending_urls(JSContext *ctx);

/* Deliver `body` for `url`: resolve every promise parked on it. `is_script` runs the body as more code in the
   CURRENT flow rather than handing it back as data. Returns the number of promises resolved — 0 means the host
   provided something nothing was waiting for, which is a bug in the host's pairing, not here. */
int fetch_provide(JSContext *ctx, const char *url, const char *body, int is_script);

#endif
