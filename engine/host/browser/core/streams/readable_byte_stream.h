/* ReadableByteStreamController — the Streams Standard §4.7, §4.8 and §4.9.5. See readable_byte_stream.c.
 *
 * These are the operations §4.2's stream and §4.4's reader perform on a controller that is a BYTE one: §4.9.2
 * says the ReadableStream algorithms call [[CancelSteps]], [[PullSteps]] and [[ReleaseSteps]]
 * POLYMORPHICALLY, and this header is that polymorphism written out. Each answers for a §4.7 controller and
 * asserts it was handed one, so a stream with a §4.6 controller never reaches any of them. */
#ifndef ENGINE_HOST_BROWSER_CORE_STREAMS_READABLE_BYTE_STREAM_H
#define ENGINE_HOST_BROWSER_CORE_STREAMS_READABLE_BYTE_STREAM_H
#include <stdbool.h>
#include <stdint.h>

#include "quickjs.h"
#include "quickjs-step.h"
#include "core/streams/stream_work.h"

void readable_byte_stream_init(JSContext *ctx);            /* from readable_stream_init */
void readable_byte_stream_install_protos(JSContext *ctx);  /* from readable_stream_install_protos */
void readable_byte_stream_install(JSContext *ctx, JSValueConst global);
void readable_byte_stream_free(void);

/* IS THIS A §4.7 CONTROLLER? The one question §4.9.2's polymorphic call is: `getReader({mode:"byob"})` is a
   TypeError for a stream whose controller is not one, and every operation below asserts the same thing. */
bool readable_byte_ctrl_is(JSValueConst ctrl);

/* §4.9.5's SetUpReadableByteStreamController, minus its start algorithm (which the §4.2 constructor runs, as
   it does for §4.6). `pull_fn`, `cancel_fn` and `source` are BORROWED; `auto_alloc` is
   [[autoAllocateChunkSize]], 0 meaning undefined. Attaches the controller to the stream. 0, or -1 throwing. */
int readable_byte_ctrl_setup(JSContext *ctx, JSValueConst stream, JSValueConst pull_fn, JSValueConst cancel_fn,
                             JSValueConst source, double hwm, uint64_t auto_alloc);

/* §4.9.5's SetUpReadableByteStreamController steps 15-16: the start promise's two reactions. */
int readable_byte_start(JSContext *ctx, JSValueConst stream, JSValueConst start_promise);

/* §4.9.5's ReadableByteStreamControllerCallPullIfNeeded, as a sub-sequence — the shape §4.6's has, because it
   is the same three page-code points (the pull call, the PromiseResolve over what it answered, the reactions
   attached to that). The caller sets `w->pull = P_TEST` and calls until it returns 0. */
int readable_byte_pull_run(JSContext *ctx, StreamWork *w, JSValueConst ctrl, JSValue in,
                           JSValue **out_cb, int *out_argc);

/* §4.7's [[PullSteps]], decomposed into the parts that differ from §4.6's — the rest of that algorithm (park
   the request, ask whether to pull, settle it) is the READ machine's and is already shared.
   `…has_chunk` is "[[queueTotalSize]] > 0"; `…take_chunk` is FillReadRequestFromQueue up to its chunk steps
   (the entry leaves the queue and becomes a Uint8Array over exactly its bytes); `…drained` is
   HandleQueueDrain's own question, asked after a take. */
bool    readable_byte_has_chunk(JSValueConst ctrl);
JSValue readable_byte_take_chunk(JSContext *ctx, JSValueConst ctrl);
bool    readable_byte_drained(JSValueConst ctrl);
/* §4.7 [[PullSteps]] steps 4-5: the automatic buffer allocation, performed BEFORE the read request is parked.
   Answers JS_UNDEFINED when the controller declared no autoAllocateChunkSize, or JS_EXCEPTION when the buffer
   could not be constructed — which the spec answers with the read request's ERROR steps rather than a throw. */
JSValue readable_byte_auto_allocate(JSContext *ctx, JSValueConst ctrl);

/* §4.7's [[ReleaseSteps]]: the pull-into a released reader was filling is kept, with its reader type set to
   "none" so whatever arrives for it goes to the queue instead of to a reader that no longer exists. */
void readable_byte_release_steps(JSContext *ctx, JSValueConst ctrl);

/* §4.7's [[CancelSteps]] steps 1-2 and §4.9.5's ReadableByteStreamControllerError steps 2-3 — the same pair,
   which is why it is one operation: the pending pull-intos are dropped and the byte queue is reset. */
void readable_byte_clear(JSContext *ctx, JSValueConst ctrl);

/* §4.9.5's ReadableByteStreamControllerClearAlgorithms, and the two algorithms ReadableStreamCancel calls
   BEFORE it. Both answers are BORROWED. */
JSValueConst readable_byte_cancel_fn(JSValueConst ctrl);
JSValueConst readable_byte_source(JSValueConst ctrl);
void readable_byte_clear_algorithms(JSContext *ctx, JSValueConst ctrl);

/* §4.5's `read(view, options)` — the machine, so §4.4's prototype can be built beside §4.3's mixin where the
   reader class lives. */
int readable_byob_read_stepid(void);

#endif
