/* solver/reply_body.h — a streamed response body's pieces, from the trusted zone to every flow reading them.
 *
 * Fetch §5.6 "Fetch methods" resolves `fetch()`'s promise with a Response once the response's head is
 * available; the body then arrives in pieces, and Fetch §4.6 "HTTP-network fetch" enqueues each piece into the
 * response body's stream as a fetch task (HTML's networking task source), closing the stream at the end or
 * erroring it with a TypeError on a network failure.
 *
 * The trusted zone's head record (`qjs_provide` with a `stream` token) opens a body SOURCE: the append-only
 * list of the reply's pieces, a fact about the server shared by every flow that took the head, written outside
 * any flow's delta. Each flow reads it through its own FLOW_PENDING_BODY parks, one per piece, so the stream a
 * flow enqueues into and how far it has consumed ride that flow's COW delta and a fork mid-body keeps per-arm
 * stream state. */
#ifndef ENGINE_HOST_SOLVER_REPLY_BODY_H
#define ENGINE_HOST_SOLVER_REPLY_BODY_H

#include <stddef.h>
#include <stdint.h>
#include "quickjs.h"

/* What a piece asks the stream to do: Streams §4.9.4's ReadableStreamDefaultControllerEnqueue, Close or
   Error. */
typedef enum { REPLY_BODY_ENQUEUE = 0, REPLY_BODY_CLOSE = 1, REPLY_BODY_ERROR = 2 } ReplyBodyOp;

/* Opens the source of the streamed head `reply` (a reply record carrying `stream`), whose `body` is the first
   read's bytes, and attaches it to the record as `source`. `method`/`url` name the request for the learning
   done at the body's end. The token must name no source still open. */
void reply_body_open(JSContext *ctx, JSValueConst reply, const char *token, const char *method, const char *url);
/* Whether `reply` is a streamed head (it carries the source reply_body_open attached). */
int reply_body_streamed(JSContext *ctx, JSValueConst reply);
/* The grade the replies the head answered were folded to, for the learning at the body's end. */
void reply_body_set_grade(JSContext *ctx, JSValueConst reply, int prov, int driven);
/* The zone's next record for `token`: `bytes` (may be empty) and then `state`, one of `more`, `end`, `network`
   or `decline`; `meta` is the end's `computedType` or a failure's `reason`, and empty for `more`. Wakes every flow
   parked on a piece this record adds. Returns 0 when no source with that token is open. */
int reply_body_append(JSContext *ctx, const char *token, const char *state, const uint8_t *bytes, size_t n,
                      const char *meta);
/* Parks the running flow on piece `item` of the source `reply` carries, to be handed to `deliver` (a
   fetch_body_delivery). Answered at once when the source already holds that piece. `doc` is the asking
   document, for the program the body's end may queue. */
void reply_body_park(JSContext *ctx, JSValueConst reply, JSValueConst deliver, int item, uint32_t doc);
/* A delivered FLOW_PENDING_BODY entry `e` holding `piece`: the op it asks of the stream, and the next park
   when the piece is not the stream's last and `open` (the stream still takes pieces). */
ReplyBodyOp reply_body_piece_op(JSContext *ctx, JSValueConst piece);
void reply_body_next(JSContext *ctx, JSValueConst e, JSValueConst piece, int open);
/* The whole reply record the body's end assembled (status, headers, URL list, type, `computedType` and the
   whole body), for the program a JavaScript-typed body queues; JS_UNDEFINED before the end. The URL is the
   request's. */
JSValue reply_body_whole(JSContext *ctx, JSValueConst e);
JSValue reply_body_url(JSContext *ctx, JSValueConst e);
/* Whether the FLOW_PENDING_BODY entry `e` waits on a source whose body has not ended: the zone owes it a record,
   which is how the host is told of such a park. */
int reply_body_entry_open(JSContext *ctx, JSValueConst e);
/* How many streamed bodies the zone has opened and not yet ended: records it owes this instance. */
int reply_body_open_count(void);
/* Releases the open sources at the session's end. */
void reply_body_reset(JSContext *ctx);

#endif
