/* The Response interface — WHATWG Fetch §5.5 "Response class". See response.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_FETCH_RESPONSE_H
#define ENGINE_HOST_BROWSER_CORE_FETCH_RESPONSE_H
#include <stddef.h>

#include "quickjs.h"
#include "core/fetch/headers.h"

void    response_init(JSContext *ctx);   /* register the class, its prototype and its machines (install time) */
void    response_install_proto(JSContext *ctx);   /* §5.5's prototype and serializer, for ONE realm */
void    response_install(JSContext *ctx, JSValueConst global);   /* the Response interface object */
void    response_free(JSContext *ctx);   /* the prototype this component holds */
/* §2.2.6's URL LIST, from the SERIALIZED URLs a host observed — "a list of zero or more URLs", of which only
   the FIRST and the LAST are ever exposed to script (the spec says so, and it is why atomic HTTP redirect
   handling holds). `n == 0` is « ». Every item must be an ABSOLUTE URL: `url` runs the URL parser back over
   the last one to answer "serialized with exclude fragment set to true". */
JSValue response_url_list(JSContext *ctx, const char *const *urls, int n);
/* A Response over the host's reply. The body is a BYTE SEQUENCE and carries its length: `arrayBuffer()` and
   `bytes()` hand those bytes back, and a strlen would have truncated the reply at its first interior NUL.
   `url_list` is §2.2.6's URL list — the whole of what `url` and `redirected` are computed from — and is
   COPIED, never adopted. */
JSValue response_new(JSContext *ctx, JSValueConst url_list, int status, const char *status_text,
                     const HeaderList *headers, const char *body, size_t body_len);

#endif
