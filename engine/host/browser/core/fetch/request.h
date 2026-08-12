/* THE REQUEST INTERFACE — WHATWG Fetch §5.3. See request.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_FETCH_REQUEST_H
#define ENGINE_HOST_BROWSER_CORE_FETCH_REQUEST_H
#include "quickjs.h"

void request_init(JSContext *ctx);
void request_install_proto(JSContext *ctx);   /* §5.3's prototype, for ONE realm */
void request_install(JSContext *ctx, JSValueConst global);
void request_free(JSContext *ctx);

/* The request's URL as `fetch(input)` reads it when `input` is a Request rather than a string, or NULL when
   the value is not one. */
const char *request_url_of(JSValueConst v);

/* §5.3's captured blob URL entry, or JS_UNDEFINED — the Blob a Request built from a `blob:` URL holds, so
   revoking the URL afterwards does not stop that request. Borrowed. */
JSValueConst request_blob_entry(JSValueConst v);

/* §5.3 step 25 — "if method is not a method or is a forbidden method, throw a TypeError", then "normalize
   method" — as ONE operation, because `fetch(input, init)` performs §5.3 inline and needs the same answer.
   Returns the normalized method, which the caller releases with js_free, or NULL with a TypeError live. */
char *request_method_check(JSContext *ctx, const char *m);

/* §5.1's "method": an RFC 7230 token. Public because the answer to "is this a method" and the answer to "is
   this a FORBIDDEN method" carry DIFFERENT errors in a second standard: XHR §3.5.1 open() throws a
   "SyntaxError" for the first and a "SecurityError" for the second, where §5.3 throws one TypeError for both.
   So the grammar is shared and the error stays each standard's own. */
bool request_method_is_token(const char *m);

#endif
