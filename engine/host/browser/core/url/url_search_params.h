/* THE URLSearchParams INTERFACE — WHATWG URL §6, and the urlencoded list behind it. See url_search_params.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_URL_URL_SEARCH_PARAMS_H
#define ENGINE_HOST_BROWSER_CORE_URL_URL_SEARCH_PARAMS_H
#include <stddef.h>

#include "quickjs.h"

void usp_init(JSContext *ctx);
void usp_install(JSContext *ctx, JSValueConst global);
void usp_free(JSContext *ctx);

/* §6.1's "a URLSearchParams object has an associated URL object". A URL's `searchParams` is [SameObject] and
   every mutation of it runs §6.1's UPDATE STEPS on that URL — so the two are built as a pair, and neither can
   be reached without the other. `owner` is the URL wrapper (JS_UNDEFINED for a standalone one). */
JSValue usp_new(JSContext *ctx, JSValueConst owner, const char *query, size_t query_len);
/* §5.1's `search` setter re-initialises the object's query, which §6.1 says re-initialises the list. */
void    usp_reset(JSContext *ctx, JSValueConst usp, const char *query, size_t query_len);

#endif
