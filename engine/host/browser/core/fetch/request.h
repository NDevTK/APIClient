/* THE REQUEST INTERFACE — WHATWG Fetch §5.3. See request.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_FETCH_REQUEST_H
#define ENGINE_HOST_BROWSER_CORE_FETCH_REQUEST_H
#include "quickjs.h"

void request_init(JSContext *ctx);
void request_install(JSContext *ctx, JSValueConst global);
void request_free(JSContext *ctx);

/* The request's URL as `fetch(input)` reads it when `input` is a Request rather than a string, or NULL when
   the value is not one. */
const char *request_url_of(JSValueConst v);

/* §5.3's captured blob URL entry, or JS_UNDEFINED — the Blob a Request built from a `blob:` URL holds, so
   revoking the URL afterwards does not stop that request. Borrowed. */
JSValueConst request_blob_entry(JSValueConst v);

#endif
