/* The Response interface — WHATWG Fetch §6. See response.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_FETCH_RESPONSE_H
#define ENGINE_HOST_BROWSER_CORE_FETCH_RESPONSE_H
#include <stddef.h>

#include "quickjs.h"

void    response_init(JSContext *ctx);   /* register the class + its body readers (install time) */
/* A Response over the host's reply. The body is a BYTE SEQUENCE and carries its length: `arrayBuffer()` and
   `bytes()` hand those bytes back, and a strlen would have truncated the reply at its first interior NUL. */
JSValue response_new(JSContext *ctx, const char *url, const char *body, size_t body_len);

#endif
