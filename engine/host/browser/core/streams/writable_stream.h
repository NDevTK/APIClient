/* WritableStream — the Streams Standard §5. See writable_stream.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_STREAMS_WRITABLE_STREAM_H
#define ENGINE_HOST_BROWSER_CORE_STREAMS_WRITABLE_STREAM_H
#include <stdbool.h>

#include "quickjs.h"

void writable_stream_init(JSContext *ctx);
void writable_stream_install(JSContext *ctx, JSValueConst global);
void writable_stream_free(JSContext *ctx);

/* IS THIS A WritableStream? §4.2's `pipeTo` takes one, and a union arm is a brand test. */
bool writable_stream_is(JSValueConst v);

#endif
