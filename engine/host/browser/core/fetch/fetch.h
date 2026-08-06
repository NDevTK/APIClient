/* The Fetch API — Blink core/fetch. One global, installed on the baseline before any flow runs. */
#ifndef ENGINE_HOST_BROWSER_CORE_FETCH_H
#define ENGINE_HOST_BROWSER_CORE_FETCH_H
#include "quickjs.h"

/* Install `fetch` on `global`. Every request forced execution reaches funnels one endpoint into the @H
   surface; the network itself is the trusted bridge's, never this sandbox's. */
void fetch_install(JSContext *ctx, JSValueConst global);

/* THE HOST'S NETWORK, as a seam the browser half takes rather than names.
 *
 * §5.5's `fetch()` is the browser's; WHO actually goes to the network is the host's, and SECURITY.md puts
 * every byte of it behind a trusted chokepoint the sandbox cannot reach. `owe` is the whole contract: the
 * component has built a Promise<Response> and a `deliver` closure, and it hands the host the URL it must
 * satisfy; the host calls `deliver` with the body when it has one, and the flow cannot finish until it does —
 * which is what keeps reply-gated code reachable.
 * It is a PARAMETER because the two hosts differ and neither is a special case of the other: the extension's
 * host parks the request on the flow's pending register and lets the trusted zone fetch it, while the wpt
 * runner serves the checked-out corpus off disk. Naming the solver's register here made the browser half
 * depend on the scheduler, and through it on the whole DOM, so nothing could take `fetch` without taking the
 * solver too. */
typedef struct {
    void (*owe)(JSContext *ctx, JSValueConst deliver, JSValueConst value, const char *url);
} FetchProvider;
void fetch_set_provider(const FetchProvider *p);

#endif
