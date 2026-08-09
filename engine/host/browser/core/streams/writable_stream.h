/* WritableStream — the Streams Standard §5. See writable_stream.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_STREAMS_WRITABLE_STREAM_H
#define ENGINE_HOST_BROWSER_CORE_STREAMS_WRITABLE_STREAM_H
#include <stdbool.h>

#include "quickjs.h"

void writable_stream_init(JSContext *ctx);
void writable_stream_install_protos(JSContext *ctx);   /* §5's three prototypes, for ONE realm */
void writable_stream_install(JSContext *ctx, JSValueConst global);
void writable_stream_free(JSContext *ctx);

/* IS THIS A WritableStream? §4.2's `pipeTo` takes one, and a union arm is a brand test. */
bool writable_stream_is(JSValueConst v);

/* THE OPERATIONS A HOST PERFORMS ON A WRITABLE STREAM, as the function objects this component installed — the
   §5 half of what readable_stream.h already exposes, and for the same reason: §4.2.4's pipeTo performs the
   ABSTRACT operations, so a page that rebinds WritableStreamDefaultWriter.prototype.write must not thereby
   change what a pipe does. BORROWED; the caller calls them, which is what keeps them suspendable. */
typedef enum {
    WS_OP_GET_WRITER = 0,   /* on the STREAM */
    WS_OP_WRITE, WS_OP_CLOSE, WS_OP_ABORT, WS_OP_RELEASE,   /* on the WRITER */
    WS_OP_STREAM_ABORT,     /* on the STREAM — §4.2.4's abort algorithm aborts the stream, not through a writer */
    WS_OP_CTRL_ERROR,       /* on the CONTROLLER — and it IS §5.4's ErrorIfNeeded: `error` already returns
                               without doing anything unless the stream is still writable */
    WS_OP_N
} WritableStreamOp;
/* THIS REALM'S copy of a §5 abstract operation. OWNED: the caller frees. */
JSValue writable_stream_op(JSContext *ctx, WritableStreamOp which);

/* §5.4's CreateWritableStream, and the START that is deliberately not part of it — see writable_stream.c.
   The three algorithms are the CALLER's function objects, called with the arguments the matching
   underlying-sink member takes and with `this` = undefined; §6's TransformStream is built out of exactly this,
   because a transform stream's writable half has no sink object at all. All four values are BORROWED, and the
   answer is the stream (owned) with its controller already attached. */
JSValue writable_stream_create(JSContext *ctx, JSValueConst write_fn, JSValueConst close_fn,
                               JSValueConst abort_fn, double hwm, JSValueConst size_fn);

/* React to the start algorithm's promise: on fulfilment the controller is STARTED and the queue advances, on
   rejection the stream errors. §5.2's constructor hands the promise its `start` produced; a host that built the
   stream itself hands its own. Returns 0, or -1 with the exception live. */
int writable_stream_start(JSContext *ctx, JSValueConst stream, JSValueConst start_promise);

/* §5.2's four states, and the two flags pipeTo reads beside them. `close_queued` is
   WritableStreamCloseQueuedOrInFlight, which decides both whether the pipe may still wait for writes to finish
   and whether the destination has already begun closing under it. Answers false for a non-stream. */
typedef enum { WS_WRITABLE = 0, WS_CLOSED, WS_ERRORING, WS_ERRORED } WritableStreamState;
bool writable_stream_query(JSValueConst v, WritableStreamState *pstate, bool *plocked, bool *pclose_queued);

/* §5.4's controller for a stream this component built. BORROWED. */
JSValueConst writable_stream_controller(JSValueConst stream);

/* §5.2's [[storedError]]. DUP'D; undefined when the stream has not failed. */
JSValue writable_stream_stored_error(JSContext *ctx, JSValueConst v);

/* §5.3's [[readyPromise]] and [[closedPromise]] on a writer THIS COMPONENT handed out — the internal slots,
   not the accessors, for the reason the reader's `closed` is read that way. Both DUP'D. */
JSValue writable_writer_ready(JSContext *ctx, JSValueConst writer);
JSValue writable_writer_closed(JSContext *ctx, JSValueConst writer);

#endif
