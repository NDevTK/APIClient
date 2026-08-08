/* The Fetch API — Blink core/fetch. One global, installed on the baseline before any flow runs. */
#ifndef ENGINE_HOST_BROWSER_CORE_FETCH_H
#define ENGINE_HOST_BROWSER_CORE_FETCH_H
#include "quickjs.h"
#include "core/url/url.h"
#include "core/fetch/headers.h"

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
/* WHAT THE HOST IS OWED, as the REQUEST and not as a URL. It was a URL string, which is the half of a request
   that names WHERE — and a host that can actually answer needs the rest: a POST's method and body decide what
   comes back, and a request's headers are what a handler echoes. The wpt runner reached the point of asking
   (its corpus's `echo-content.py` and `inspect-headers.py` answer exactly those), and the extension's host has
   always needed them to satisfy a real request through the trusted zone. `headers` is the request's own list,
   borrowed; `body`/`body_len` are its bytes, NULL for a request that has none. */
typedef struct {
    const char   *method;
    const char   *url;
    const HeaderList *headers;
    const char   *body;
    size_t        body_len;
} FetchRequest;

typedef struct {
    void (*owe)(JSContext *ctx, JSValueConst deliver, JSValueConst value, const FetchRequest *req);
} FetchProvider;
void fetch_set_provider(const FetchProvider *p);

/* PARSE A URL A PAGE WROTE, against HTML's API base URL — the one operation every Fetch entry point performs
   on a URL string, so `new Request("/api/users")` and `Response.redirect("/there")` resolve the same address by
   the same rule rather than each reaching for url_parse with whatever base it remembered. Fills `*rec` and
   returns true; on failure `*rec` is already freed and the caller throws whichever error its spec names — a
   TypeError for both of today's two, but the spec says so at each site rather than here. */
bool fetch_parse_url(JSContext *ctx, UrlRecord *rec, const char *url, size_t len);

/* THE REPLY, as the value a host DELIVERS. It was the body's bytes and nothing else, so every reply built from
   it had no status but 200 and NO HEADERS AT ALL — `response.headers.get(...)` was null for everything a page
   fetched, and with it went the Content-Type that decides whether `.formData()` parses a body and the
   `Location` an endpoint's redirect is made of. A host builds one of these with whatever it knows; `headers`
   may be NULL for a host that knows none, which is a different statement from a reply that HAD none. */
JSValue fetch_reply_new(JSContext *ctx, int status, const char *status_text, const HeaderList *headers,
                        const char *body, size_t body_len);

#endif
