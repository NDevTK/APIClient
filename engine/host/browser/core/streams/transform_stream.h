/* TransformStream — the Streams Standard §6. See transform_stream.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_STREAMS_TRANSFORM_STREAM_H
#define ENGINE_HOST_BROWSER_CORE_STREAMS_TRANSFORM_STREAM_H
#include <stdbool.h>

#include "quickjs.h"

void transform_stream_init(JSContext *ctx);
void transform_stream_install(JSContext *ctx, JSValueConst global);
void transform_stream_free(JSContext *ctx);

/* IS THIS A TransformStream? A brand test, for the same reason §4's and §5's exist. */
bool transform_stream_is(JSValueConst v);

#endif
