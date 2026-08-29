/* THE URLSearchParams INTERFACE — WHATWG URL §6, and the urlencoded list behind it. See url_search_params.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_URL_URL_SEARCH_PARAMS_H
#define ENGINE_HOST_BROWSER_CORE_URL_URL_SEARCH_PARAMS_H
#include <stddef.h>

#include "quickjs.h"
#include "core/url/url.h"

void usp_init(JSContext *ctx);
void usp_install_proto(JSContext *ctx);   /* §6.2's prototype, for ONE realm */
void usp_install(JSContext *ctx, JSValueConst global);
void usp_free(JSContext *ctx);

/* §6.2 URLSearchParams class's "a URLSearchParams object has an associated URL object". A URL's
   `searchParams` is [SameObject] and every mutation of it runs those same UPDATE STEPS on that URL — so the
   two are built as a pair, and neither can
   be reached without the other. `owner` is the URL wrapper (JS_UNDEFINED for a standalone one). */
JSValue usp_new(JSContext *ctx, JSValueConst owner, const char *query, size_t query_len);
/* §6.1 URL class's `search` setter re-initialises the object's query, which §6.2 says re-initialises
   the list. */
void    usp_reset(JSContext *ctx, JSValueConst usp, const char *query, size_t query_len);

/* THE LIST one holds, or NULL when the value is not a URLSearchParams. The brand test Fetch §5.1's BodyInit
   union performs, and what it serialises the body from. */
const UrlEncodedList *usp_list_of(JSValueConst v);

#endif
