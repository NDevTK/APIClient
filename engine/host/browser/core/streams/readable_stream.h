/* ReadableStream — the Streams Standard §4. See readable_stream.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_STREAMS_READABLE_STREAM_H
#define ENGINE_HOST_BROWSER_CORE_STREAMS_READABLE_STREAM_H
#include <stddef.h>

#include "quickjs.h"

void readable_stream_init(JSContext *ctx);
void readable_stream_install(JSContext *ctx, JSValueConst global);
void readable_stream_free(JSContext *ctx);

/* A STREAM OVER BYTES THE HOST ALREADY HAS — `blob.stream()`, and every other place a spec answers with a
   stream whose source is not the page's. The bytes are enqueued as one chunk and the stream is closed, which is
   what "a byte sequence" means when there is nothing left to arrive. */
JSValue readable_stream_from_bytes(JSContext *ctx, const char *bytes, size_t len);

#endif
