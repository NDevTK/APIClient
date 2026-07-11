/* ReadableStream (WHATWG Streams) — a real, shared Web-platform component. Response.body, Blob.stream, and any
 * streaming source build one via readable_stream_new; a subagent can own/extend it (tee/pipeTo/pipeThrough) in
 * isolation behind this one contract. The solver-relevant behavior: a reader delivers the underlying value as
 * ONE taint-carrying chunk then done, so streaming code (`reader.read()`, `for await (const c of stream)`) runs
 * with the real value and its `while(!done)` loop forks continue/exit. See readable_stream.c. */
#ifndef ENGINE_HOST_BROWSER_READABLE_STREAM_H
#define ENGINE_HOST_BROWSER_READABLE_STREAM_H
#include "quickjs.h"

/* A ReadableStream over `chunk`: getReader().read() yields { value: chunk, done: false } once (taint intact),
   then { value: undefined, done: true }. CONSUMES `chunk`. */
JSValue readable_stream_new(JSContext *ctx, JSValue chunk);

#endif
