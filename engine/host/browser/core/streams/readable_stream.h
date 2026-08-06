/* ReadableStream — the Streams Standard §4. See readable_stream.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_STREAMS_READABLE_STREAM_H
#define ENGINE_HOST_BROWSER_CORE_STREAMS_READABLE_STREAM_H
#include <stdbool.h>
#include <stddef.h>

#include "quickjs.h"

void readable_stream_init(JSContext *ctx);
void readable_stream_install(JSContext *ctx, JSValueConst global);
void readable_stream_free(JSContext *ctx);

/* A STREAM OVER BYTES THE HOST ALREADY HAS — `blob.stream()`, and every other place a spec answers with a
   stream whose source is not the page's. The bytes are enqueued as one chunk and the stream is closed, which is
   what "a byte sequence" means when there is nothing left to arrive. */
JSValue readable_stream_from_bytes(JSContext *ctx, const char *bytes, size_t len);

/* §4.2's `disturbed` flag, which Fetch §5.2 defines `bodyUsed` over: a body is used when its stream has been
   READ FROM, not when the stream was merely handed out. Returns false for a value that is not a stream. */
bool readable_stream_disturbed(JSValueConst v);

/* IS THIS A ReadableStream? Fetch §5.1's BodyInit union names one as an arm, and a union's arms are brand
   tests — `new Response(stream)` took the USVString arm and stringified to "[object ReadableStream]" while
   this was missing, which is the same bug the Blob arm had before it existed. */
bool readable_stream_is(JSValueConst v);

/* THE OPERATIONS A HOST PERFORMS ON A STREAM, as the function objects this component installed. Fetch §5.2's
   "fully read" acquires a reader and reads to the end, and it performs the ABSTRACT operations — a page that
   rebinds ReadableStreamDefaultReader.prototype.read must not thereby change what `response.text()` does.
   BORROWED; the caller calls them as it would any function, which is what keeps them suspendable. */
typedef enum { RS_OP_GET_READER = 0, RS_OP_READ, RS_OP_RELEASE, RS_OP_N } ReadableStreamOp;
JSValueConst readable_stream_op(ReadableStreamOp which);

/* FULLY READ a stream the caller has already acquired a reader on and issued the FIRST read against, and
   answer a promise for what `make` builds from the whole byte sequence.
 *
 * WHY THE CALLER STARTS IT. Acquiring the reader and issuing a read are CALLS of code that can suspend, and a
 * host component reaching this is already a machine, so it makes those two calls itself; what it cannot do is
 * LOOP — each read answers a promise, and continuing means reacting to it. That is what this owns: the
 * accumulating byte buffer and the reactions that drive the loop to the end.
 * `read_promise` is BORROWED. Returns the promise to hand the page, or JS_EXCEPTION. */
JSValue readable_stream_drain(JSContext *ctx, JSValueConst reader, JSValueConst read_promise,
                              JSValueConst recv,
                              JSValue (*make)(JSContext *ctx, JSValueConst recv, const char *bytes,
                                              size_t len));

#endif
