/* The Response interface — WHATWG Fetch §6. See response.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_FETCH_RESPONSE_H
#define ENGINE_HOST_BROWSER_CORE_FETCH_RESPONSE_H
#include "quickjs.h"

void    response_init(JSContext *ctx);                                        /* register the class (install time) */
JSValue response_new(JSContext *ctx, const char *url, const char *body);      /* a Response over the host's reply */

#endif
