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

/* STREAMS §9.3 — WHAT ANOTHER SPECIFICATION REACHES §6 THROUGH. A spec that layers its own interface over a
   transform stream (Encoding's TextDecoderStream, Compression's CompressionStream) does not construct one with
   a transformer object: it creates one and SETS IT UP with algorithms of its own, then enqueues into it from
   inside those algorithms. Both halves of that are here, and both are the ORIGINAL function objects for the
   reason readable_stream_op gives — a page that replaces `TransformStreamDefaultController.prototype.enqueue`
   must not thereby change what a TextDecoderStream does.

   TS_OP_SETUP is §9.3.1's "set up a TransformStream": called with « transformAlgorithm, flushAlgorithm,
   cancelAlgorithm » (a function, or undefined for the two optional ones) and `this` = undefined, it answers a
   TransformStream already initialized with the fixed marks §9.3.1 states — writable 1, readable 0, both size
   algorithms 1. The algorithms are called the way §6 calls a transformer's: « chunk, controller », so the one
   that needs to enqueue has the controller in hand.

   TS_OP_ENQUEUE / _TERMINATE / _ERROR are §9.3.1's "enqueue into", "terminate" and "error", which ARE §6.4's
   three controller operations — called with `this` = the controller the algorithm was handed.

   They are STEP METHODS, so a caller reaches them the way it reaches any of the page's code: as a request. */
typedef enum { TS_OP_SETUP = 0, TS_OP_ENQUEUE, TS_OP_TERMINATE, TS_OP_ERROR, TS_OP_N } TransformStreamOp;
JSValueConst transform_stream_op(TransformStreamOp which);

/* The two halves of a set-up stream, for the GenericTransformStream mixin's two getters. BORROWED. */
JSValueConst transform_stream_readable(JSValueConst stream);
JSValueConst transform_stream_writable(JSValueConst stream);

#endif
