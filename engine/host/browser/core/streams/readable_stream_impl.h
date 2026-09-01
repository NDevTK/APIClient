/* THE §4 INTERNALS §4.6's CONTROLLER AND §4.7's BOTH REACH — see readable_stream.c and readable_byte_stream.c.
 *
 * §4.9.1's operations are the STREAM'S and are performed by whichever controller it has: ReadableStreamClose
 * settles the reader's `closed` promise and answers its parked read requests, ReadableStreamError rejects them,
 * ReadableStreamAddReadRequest parks one. §4.9.3's are the READER'S and are shared by the default reader and
 * the BYOB one, because ReadableStreamGenericReader is a mixin both interfaces include. So the stream's record,
 * the reader's record and those operations are declared HERE rather than being reachable from one translation
 * unit: the alternative is a byte controller that cannot answer a read request, which is to say one that is not
 * a controller at all.
 *
 * WHAT IS NOT HERE is either controller's own state. §4.6's queue is a list of (value, size) pairs weighed by
 * the strategy the page supplied; §4.7's is a list of BYTE RANGES and there is no size algorithm anywhere near
 * it. That difference is the whole of why the standard has two controller sections, and a shared queue would be
 * the wrong implementation wearing the right name. */
#ifndef ENGINE_HOST_BROWSER_CORE_STREAMS_READABLE_STREAM_IMPL_H
#define ENGINE_HOST_BROWSER_CORE_STREAMS_READABLE_STREAM_IMPL_H
#include <stdbool.h>
#include <stdint.h>

#include "quickjs.h"
#include "quickjs-step.h"
#include "core/streams/stream_work.h"
#include "core/streams/readable_stream.h"

/* §4.2's stream. The state byte is readable_stream.h's ReadableStreamState. */
typedef struct {
    uint8_t state;
    uint8_t disturbed;    /* §4.2: set by the first read, and what `bodyUsed` is defined over */
    JSValue stored_error;
    JSValue reader;       /* the reader holding the lock, or JS_UNDEFINED */
    JSValue queue;        /* §4.6's Array of chunks not yet read — EMPTY for a byte stream, whose queue is its
                             controller's byte ranges */
    /* §4.6's queue is a list of (value, SIZE) pairs and its [[queueTotalSize]] is their sum — the strategy's
       size algorithm decides what a chunk weighs, and desiredSize is the mark minus that total, not minus a
       COUNT. A parallel array rather than a pair object per chunk: the collector already traces this one. */
    JSValue queue_size;
    double  queue_total;
    uint32_t head;        /* how many of them have been read */
    /* §4.2's READ REQUESTS — and §4.5's READ-INTO REQUESTS, which are the same two arrays because a stream has
       at most ONE reader and a reader has exactly one of the two lists. Both are answered by CALLING a
       resolving function, which is what a request IS at this level; what differs is only the value the caller
       builds (a chunk for a default reader, the filled view for a BYOB one), and that is the caller's. */
    JSValue read_resolve, read_reject;
    uint32_t rhead;
    JSValue controller;   /* §4.6's or §4.7's, or JS_UNDEFINED for a host-byte stream that needs none */
} StreamData;

/* §4.4's and §4.5's readers, which are ONE record because §4.3's mixin is most of both. `byob` is which
   interface this object implements — the brand every member of the two prototypes tests, and what
   ReadableStreamHasBYOBReader asks. */
typedef struct {
    JSValue stream;       /* the stream this reader locks, or JS_UNDEFINED once released */
    JSValue closed;       /* §4.3's `closed` promise */
    JSValue closed_funcs[2];
    /* §4.3 settles `closed` EXACTLY ONCE — a stream that closes and is then released must not settle it twice,
       and a resolving function is not idempotent. A flag rather than a look at the slots, because a zeroed
       JSValue reads as the integer 0 and not as undefined. */
    uint8_t closed_settled;
    uint8_t byob;
} ReaderData;

/* The settle sequence's states — §4.9.2's ReadableStreamClose, ReadableStreamError and §4.9.3's
   GenericRelease, which are one sequence with several entry points. S_INTO_LOOP is ReadableStreamCancel's own
   step 6: a BYOB reader's read-into requests are NOT answered by Close (which is why the close loop skips
   them) and ARE answered by a cancel, with the backing memory deliberately dropped. */
enum { S_IDLE = 0,
       S_CLOSE_SET, S_CLOSE_CLOSED, S_CLOSE_LOOP,   /* §4.9.2's ReadableStreamClose */
       S_ERR_SET,   S_ERR_CLOSED,   S_ERR_LOOP,     /* §4.9.2's ReadableStreamError */
       S_REL_CLOSED, S_REL_LOOP,                    /* §4.9.3's release: the reader loses, the stream survives */
       S_INTO_LOOP };                               /* §4.9.2's ReadableStreamCancel step 6 */
enum { P_IDLE = 0, P_TEST, P_CALL, P_RESOLVE, P_REJECT, P_THEN };

/* The records behind the class opaques, CAPTURED INTO THE RUNNING FLOW'S DELTA on the way — see the note in
   readable_stream.c. NULL when the value is not one. */
StreamData *rs_stream_data(JSValueConst v);
ReaderData *rs_reader_data(JSValueConst v);

/* WRITE ONE OF A STREAM'S OWNED SLOTS — PUBLISH BEFORE RELEASE, see cow.h. It is EXPORTED because §4.6's
   controller is attached from the other translation unit, and the layout that says what a StreamData owns
   lives with the record: a write spelled by hand over there is a write no layout governs, which is exactly the
   slot a later field addition would be missed at. Passing a slot that is not this record's crashes at the
   assert inside — AND THE ABORT NAMES THE WRITE, which is the whole reason this is a macro over an `_at`
   function rather than a plain one: the caller is in another translation unit, so a check that stamped its own
   line would name readable_stream.c for a write made anywhere. See cow.h's THE SITE TRAVELS WITH THE
   OPERATION. */
void rs_stream_set_at(JSContext *ctx, StreamData *d, JSValue *slot, JSValue v,
                      const char *file, int line);
#define rs_stream_set(ctx_, d_, slot_, v_) \
    rs_stream_set_at((ctx_), (d_), (slot_), (v_), __FILE__, __LINE__)

/* 7.4.14's CreateIterResultObject — `{ value, done }`, own properties DEFINED and not assigned. `value` is
   CONSUMED. */
JSValue rs_read_result(JSContext *ctx, JSValue value, bool done);

/* §4.9.2's ReadableStreamGetNumReadRequests / ReadableStreamGetNumReadIntoRequests — one operation, because
   the two lists are one pair of arrays. */
uint32_t rs_read_pending(JSContext *ctx, StreamData *d);
/* TAKE the next parked request's resolve (or reject), or JS_UNDEFINED when none is parked. */
JSValue rs_take_read(JSContext *ctx, StreamData *d, int reject);
/* §4.9.2's ReadableStreamAddReadRequest / …AddReadIntoRequest: park a capability's resolving functions, which
   are CONSUMED. */
void rs_park_read(JSContext *ctx, StreamData *d, JSValue *funcs);

/* The settle sequence itself. The caller sets `w->settle` (and `w->err` for the arms that carry a reason) and
   calls until it returns 0; >0 is the step code to return, <0 is a throw. */
int rs_settle_run(JSContext *ctx, StreamWork *w, StreamData *d, JSValue in, JSValue **out_cb, int *out_argc);

#endif
