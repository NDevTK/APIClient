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
/* §4.7's and §4.8's prototypes and INTERFACE OBJECTS, per realm. IT IS ITS OWN REALM INTRINSIC and is NOT
   called from readable_stream_install_protos, which is what the comment here used to say: readable_stream.c's
   init registers the two side by side with realm_declare_intrinsic, in that order, so this one runs SECOND and
   over a realm the first has already finished with. The distinction is load-bearing rather than pedantic — a
   reader who believes the two are one function has every reason to mint these two names in the other one, and
   there the byte controller's class prototype does not exist yet. Web IDL §3.8 "Platform objects implementing
   interfaces" is given a realm, so there is no per-document half to declare here. */
void readable_byte_stream_install_protos(JSContext *ctx);
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

/* ---- what §4.9.1's ReadableByteStreamTee performs on a BRANCH -------------------------------------------------
 *
 * A tee branch is a stream this component set up, and the tee drives its controller the way §4.9.1 says: with
 * the ABSTRACT OPERATIONS, never with whatever a page has since put on the prototypes. So the three §4.7 members
 * are captured at each realm's build, exactly as readable_stream.h's ReadableControllerOp captures §4.6's.
 *
 * THE TWO RESPONDS ARE §4.9.5 OPERATIONS AND NOT §4.8 MEMBERS, and the difference is observable. §4.8's
 * `respond(bytesWritten)` step 2 is "If ! IsDetachedBuffer(this.[[view]].[[ArrayBuffer]]) is true, throw a
 * TypeError exception" — and a tee HANDS ITS BRANCH'S BYOB VIEW TO THE SOURCE READ, which transfers that
 * buffer, so `this.[[view]]` is detached for as long as the read is outstanding. Routing the tee's
 * "Perform ! ReadableByteStreamControllerRespond(branch1.[[controller]], 0)" through the member would throw
 * where the algorithm asserts. The operation forms take the CONTROLLER as their receiver and run §4.9.5's steps
 * only. */
typedef enum { RBC_ENQUEUE = 0,       /* §4.7 enqueue(chunk) */
               RBC_CLOSE,             /* §4.7 close() */
               RBC_ERROR,             /* §4.7 error(e) */
               RBC_RESPOND,           /* §4.9.5 ReadableByteStreamControllerRespond(controller, bytesWritten) */
               RBC_RESPOND_VIEW,      /* §4.9.5 ReadableByteStreamControllerRespondWithNewView(controller, view) */
               RBC_N } ReadableByteCtrlOp;
/* THIS REALM'S copy. OWNED: the caller frees. */
JSValue readable_byte_ctrl_op(JSContext *ctx, ReadableByteCtrlOp which);

/* §4.9.1's ReadableByteStreamTee steps 17.3-17.5 and 18.3-18.5 in ONE answer:
   ReadableByteStreamControllerGetBYOBRequest, and the [[view]] the tee reads off whatever it answers. One entry
   because that slot is the tee's only use of the request object, and because reaching it through §4.8's `view`
   accessor would run a property read off an object a page can reach — a host may not do that from C.
   The request is MINTED AND CACHED as GetBYOBRequest requires, so a later `controller.byobRequest` is the same
   object. Answers JS_NULL exactly when GetBYOBRequest answers null, and JS_EXCEPTION when the view could not be
   constructed. */
JSValue readable_byte_byob_view(JSContext *ctx, JSValueConst ctrl);

/* "[[pendingPullIntos]] is not empty" — §4.9.1's ReadableByteStreamTee asks it of a BRANCH's controller at the
   close steps of both of its request kinds, to decide whether a zero-byte respond is owed. */
bool readable_byte_has_pending(JSContext *ctx, JSValueConst ctrl);

/* §8.3 Miscellaneous' CloneAsUint8Array(O) — the copy §4.9.1's ReadableByteStreamTee gives its second branch.
   It is not §8.3's StructuredClone, which is what §4.9.1's ReadableStreamDefaultTee uses under cloneForBranch2:
   a byte stream's chunks are views, and a tee branch must receive its own memory whatever the flag says.
   Answers JS_EXCEPTION for the standard's ABRUPT COMPLETION, which the tee errors both branches with. */
JSValue readable_byte_clone_as_uint8(JSContext *ctx, JSValueConst view);

#endif
