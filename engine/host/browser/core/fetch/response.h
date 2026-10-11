/* The Response interface — WHATWG Fetch §5.5 "Response class". See response.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_FETCH_RESPONSE_H
#define ENGINE_HOST_BROWSER_CORE_FETCH_RESPONSE_H
#include <stddef.h>

#include "quickjs.h"
#include "core/fetch/headers.h"

void    response_init(JSContext *ctx);   /* register the class, its prototype and its machines (install time) */
/* Fetch §5.5 "Response class"' prototype, its serializer AND its interface object with §5.5's two statics, for
   ONE realm — Web IDL §3.8 "Platform objects implementing interfaces" is given a realm and names no Document,
   and §5.5 is `[Exposed=(Window,Worker)]`, so the name is owed by a realm that reaches no per-document
   install. */
void    response_install_proto(JSContext *ctx);
/* THE AGENT'S half of §5.5, run from core/platform.c's RELEASE COLUMN — it takes no JSContext because it
   reads none: the prototypes and each realm's %JSON.stringify% are the REALMS' and go with their contexts. */
void    response_free(void);
/* §2.2.6's URL LIST, from the SERIALIZED URLs a host observed — "a list of zero or more URLs", of which only
   the FIRST and the LAST are ever exposed to script (the spec says so, and it is why atomic HTTP redirect
   handling holds). `n == 0` is « ». Every item must be an ABSOLUTE URL: `url` runs the URL parser back over
   the last one to answer §5.5's url getter — "otherwise this's response's URL, serialized with exclude
   fragment set to true". */
JSValue response_url_list(JSContext *ctx, const char *const *urls, int n);
/* A Response over the host's reply. The body is a BYTE SEQUENCE and carries its length: `arrayBuffer()` and
   `bytes()` hand those bytes back, and a strlen would have truncated the reply at its first interior NUL.
   `url_list` is §2.2.6's URL list — the whole of what `url` and `redirected` are computed from — and is
   COPIED, never adopted. */
/* `cors` is Fetch §2.2.6 "Responses"' CORS filtered response (type "cors"), whose header list the trusted zone
   has already narrowed to what that filter exposes; otherwise the type is "basic". */
JSValue response_new(JSContext *ctx, JSValueConst url_list, int status, const char *status_text,
                     const HeaderList *headers, const char *body, size_t body_len, int cors);
/* Fetch §2.2.6 "Responses"' OPAQUE FILTERED RESPONSE: "a filtered response whose type is "opaque", URL list
   is « », status is 0, status message is the empty byte sequence, header list is « », body is null, and body
   info is a new response body info." It is what a `no-cors` `fetch()` of another origin resolves with. */
JSValue response_new_opaque(JSContext *ctx);

#endif
