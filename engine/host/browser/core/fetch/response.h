/* The Response interface — WHATWG Fetch §6. See response.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_FETCH_RESPONSE_H
#define ENGINE_HOST_BROWSER_CORE_FETCH_RESPONSE_H
#include <stddef.h>

#include "quickjs.h"
#include "core/fetch/headers.h"

void    response_init(JSContext *ctx);   /* register the class, its prototype and its machines (install time) */
void    response_install_proto(JSContext *ctx);   /* §6.4's prototype and serializer, for ONE realm */
void    response_install(JSContext *ctx, JSValueConst global);   /* the Response interface object */
void    response_free(JSContext *ctx);   /* the prototype this component holds */
/* A Response over the host's reply. The body is a BYTE SEQUENCE and carries its length: `arrayBuffer()` and
   `bytes()` hand those bytes back, and a strlen would have truncated the reply at its first interior NUL. */
JSValue response_new(JSContext *ctx, const char *url, int status, const char *status_text,
                     const HeaderList *headers, const char *body, size_t body_len);

#endif
