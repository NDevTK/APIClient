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
typedef enum { RS_OP_GET_READER = 0, RS_OP_READ, RS_OP_RELEASE, RS_OP_CANCEL, RS_OP_N } ReadableStreamOp;
JSValueConst readable_stream_op(ReadableStreamOp which);

/* §4.9.4's CreateReadableStream, and the START that is deliberately not part of it — see readable_stream.c.
   The algorithms are the CALLER's function objects, called with the arguments the matching underlying-source
   member takes and with `this` = undefined; §6's TransformStream is built out of exactly this, because a
   transform stream's readable half has no source object at all. All are BORROWED; the answer is the stream
   (owned) with its controller already attached. */
JSValue readable_stream_create(JSContext *ctx, JSValueConst pull_fn, JSValueConst cancel_fn,
                               double hwm, JSValueConst size_fn);

/* React to the start algorithm's promise: on fulfilment the controller is STARTED and may pull, on rejection
   the stream errors. Returns 0, or -1 with the exception live. */
int readable_stream_start(JSContext *ctx, JSValueConst stream, JSValueConst start_promise);

/* §4.2's [[state]], and whether a reader holds the lock. §4.2.4's pipeTo branches on both at half a dozen
   points — "if source.[[state]] is 'readable', cancel it", "if source is locked, reject" — and each of those
   is an INTERNAL slot read, so it cannot go through the page-visible `locked` accessor a page may have
   patched. Answers false for a value that is not a stream, which is the brand test the union arms use. */
typedef enum { RS_READABLE = 0, RS_CLOSED, RS_ERRORED } ReadableStreamState;
bool readable_stream_query(JSValueConst v, ReadableStreamState *pstate, bool *plocked);

/* §4.2's [[storedError]] — what an errored stream's reader rejects with, and what pipeTo propagates onward.
   DUP'D; undefined when the stream has not errored. */
JSValue readable_stream_stored_error(JSContext *ctx, JSValueConst v);

/* §4.3's [[closedPromise]] on a reader THIS COMPONENT handed out. pipeTo reacts to it to learn that the source
   closed or errored, and the spec names the slot rather than the `closed` getter — a page that patches
   ReadableStreamDefaultReader.prototype must not thereby change what a pipe observes. DUP'D. */
JSValue readable_reader_closed(JSContext *ctx, JSValueConst reader);

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
