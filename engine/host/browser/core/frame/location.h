/* The Location interface — Blink core/frame. Installed on the baseline from the document's own URL. */
#ifndef ENGINE_HOST_BROWSER_CORE_FRAME_LOCATION_H
#define ENGINE_HOST_BROWSER_CORE_FRAME_LOCATION_H
#include "quickjs.h"

/* Install `location` (and `document.URL`'s eventual source) built from `url` — the document's address, which
   the host captured. A NULL or empty url installs nothing: a document with no address has no Location, and the
   page's own throw on reading it is the honest answer. */
void location_install(JSContext *ctx, JSValueConst global, const char *url);

/* RECORD THE DOCUMENT'S ADDRESS, without installing the interface. The two are different facts with different
   audiences: the API BASE URL is what every spec that parses a page-written URL resolves against, and the
   Location INTERFACE additionally declares `search` and `hash` as concolic attacker sources. A host that has an
   address but is not exploring — a conformance runner, whose document genuinely has no query — needs the first
   and must not have the second, because a concolic `search` is a value the harness's own coercion of it
   refuses. location_install calls this first; a host that installs its own Location calls only this. */
void location_set_document_url(const char *url);

/* THE API BASE URL — HTML's "current settings object's API base URL", which is the document's own address.
   Every spec that parses a URL a page wrote parses it against this: `new Request("/api/users")`, `new URL(x)`'s
   implicit base, `Response.redirect("/there")`. Without it those all took a NULL base and a RELATIVE URL — the
   ordinary way a bundle names its own endpoints — was a TypeError, which is not a shortcoming of any one of
   them but of the base never being plumbed anywhere. It lives here because this is where the address arrives.
   NULL when the document has no address, which is what a platform-less test build looks like; a caller passing
   NULL to url_parse gets exactly the absolute-only behaviour it had. */
const char *location_api_base_url(void);

/* Release it — the string is this component's for the runtime's life. */
void location_free(void);

#endif
