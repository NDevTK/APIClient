/* TextEncoder AND TextDecoder — the Encoding Standard §7. See encoding.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_ENCODING_ENCODING_H
#define ENGINE_HOST_BROWSER_CORE_ENCODING_ENCODING_H
#include "quickjs.h"

void encoding_init(JSContext *ctx);
void encoding_install(JSContext *ctx, JSValueConst global);
void encoding_free(JSContext *ctx);

#endif
