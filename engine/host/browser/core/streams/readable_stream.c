/* ReadableStream — the Streams Standard §4.2, §4.3 and §4.5.
 *
 * WHY IT IS BUILT NOW. It is the largest absent component by a wide margin: 74 corpus failures name it
 * directly, 19 more name TextDecoderStream, and every `response.body.getReader()` is another. It is also the
 * one place this engine's own architecture is most on display — a reader's `read()` settles a promise, and
 * settling is the page's code, so it is a machine like every other member that touches one.
 *
 * WHAT IS HERE. The stream, the default reader, §4.5's default controller with its PULL loop, and a source that
 * is the HOST'S BYTES. §4.5's ReadableStreamDefaultControllerCallPullIfNeeded reacts to the promise the page's
 * `pull` returned by calling `pull` AGAIN and, on rejection, by erroring the controller — so its reactions are
 * MACHINES (rejecting a parked read request settles a promise) that know their controller only by CAPTURE, and
 * they are attached with PerformPromiseThen rather than a `.then` read because that is what the spec performs.
 * Both of those primitives are quickjs's (JS_NewStepClosure, JS_PerformPromiseThen), added for this.
 *
 * WHAT IS NOT. The page's `cancel` algorithm is not yet invoked, and `tee`/`pipeTo`/`pipeThrough`/`from` and the
 * BYTE stream are absent. Each is its own piece of work over the same two primitives.
 *
 * THE QUEUE IS A JS ARRAY. Its chunks are the page's values and the collector must see them; an array is what
 * this component already has that the collector traces, and `gc_mark` on the class opaque is what makes the
 * stream itself a root for it. */
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "quickjs-step.h"
#include "core/idl_args.h"
#include "core/streams/stream_work.h"
#include "core/streams/readable_stream.h"

/* §4.2's states. A stream is readable until it is closed or errored, and those two are terminal. */
enum { RS_READABLE = 0, RS_CLOSED, RS_ERRORED };

typedef struct {
    uint8_t state;
    uint8_t disturbed;    /* §4.2: set by the first read, and what `bodyUsed` is defined over */
    JSValue stored_error;
    JSValue reader;       /* the ReadableStreamDefaultReader holding the lock, or JS_UNDEFINED */
    JSValue queue;        /* an Array of chunks not yet read */
    /* §4.5's queue is a list of (value, SIZE) pairs and its [[queueTotalSize]] is their sum — the strategy's
       size algorithm decides what a chunk weighs, and desiredSize is the mark minus that total, not minus a
       COUNT. A parallel array rather than a pair object per chunk: the collector already traces this one. */
    JSValue queue_size;
    double  queue_total;
    uint32_t head;        /* how many of them have been */
    /* §4.2's READ REQUESTS: a read on a readable stream with an empty queue PARKS, and is answered when the
       controller enqueues or closes. Two parallel Arrays of the capabilities' resolving functions, because a
       promise is settled by CALLING one of them and both must survive until it is. */
    JSValue read_resolve, read_reject;
    uint32_t rhead;
    JSValue controller;   /* §4.5's controller, or JS_UNDEFINED for a host-byte stream that needs none */
} StreamData;

typedef struct {
    JSValue stream;       /* the stream this reader locks, or JS_UNDEFINED once released */
    JSValue closed;       /* §4.3's `closed` promise */
    JSValue closed_funcs[2];
    /* §4.3 settles `closed` EXACTLY ONCE — a stream that closes and is then released must not settle it twice,
       and a resolving function is not idempotent. A flag rather than a look at the slots, because a zeroed
       JSValue reads as the integer 0 and not as undefined. */
    uint8_t closed_settled;
} ReaderData;

/* §4.5's controller. The three flags are the spec's own [[started]], [[pulling]] and [[pullAgain]], and they
   exist because `pull` is ASYNCHRONOUS: nothing may pull before start's promise fulfils, only one pull may be in
   flight, and a pull requested while one is in flight is remembered rather than dropped. */
typedef struct {
    JSValue stream;
    JSValue pull_fn;          /* the page's `pull`, or JS_UNDEFINED */
    JSValue cancel_fn;        /* the page's `cancel`, or JS_UNDEFINED */
    /* §4.9.4 builds each algorithm with CreateAlgorithmFromUnderlyingMethod, which INVOKES the method on the
       underlying source — so `start`, `pull` and `cancel` see it as their receiver, and a source written with
       methods that use `this` works. (The strategy's `size` is the exception: §4.2's ExtractSizeAlgorithm
       Calls it with undefined, which is why it is not held here.) */
    JSValue source;
    JSValue size_fn;          /* the strategy's `size`, or JS_UNDEFINED for the implicit one-per-chunk */
    double  hwm;              /* §4.2's high-water mark, 1 for a default stream with no strategy */
    uint8_t started;
    uint8_t pulling;
    uint8_t pull_again;
    uint8_t close_requested;  /* §4.5's [[closeRequested]]: closed once the queue drains, not at once */
} ControllerData;

static JSClassID g_stream_class, g_reader_class, g_ctrl_class;
static JSValue   g_stream_proto = JS_UNDEFINED, g_reader_proto = JS_UNDEFINED, g_ctrl_proto = JS_UNDEFINED;
static JSRuntime *g_rs_rt;
static int       g_ctor_stepid = -1, g_reader_ctor_stepid = -1, g_read_stepid = -1, g_cancel_stepid = -1;
static int       g_release_stepid = -1, g_from_ctor_stepid = -1;
static int       g_ctrl_stepids[3] = { -1, -1, -1 };
static int       g_rxn_stepids[4] = { -1, -1, -1, -1 };

static void stream_finalizer(JSRuntime *rt, JSValue val)
{
    StreamData *d = JS_GetOpaque(val, g_stream_class);
    if (!d) return;
    JS_FreeValueRT(rt, d->stored_error);
    JS_FreeValueRT(rt, d->reader);
    JS_FreeValueRT(rt, d->queue);
    JS_FreeValueRT(rt, d->queue_size);
    JS_FreeValueRT(rt, d->read_resolve);
    JS_FreeValueRT(rt, d->read_reject);
    JS_FreeValueRT(rt, d->controller);
    js_free_rt(rt, d);
}

/* The stream holds the page's chunks and its reader; the reader holds the stream. A real cycle, which is what
   gc_mark is for. */
static void stream_gc_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func)
{
    StreamData *d = JS_GetOpaque(val, g_stream_class);
    if (!d) return;
    JS_MarkValue(rt, d->stored_error, mark_func);
    JS_MarkValue(rt, d->reader, mark_func);
    JS_MarkValue(rt, d->queue, mark_func);
    JS_MarkValue(rt, d->queue_size, mark_func);
    JS_MarkValue(rt, d->read_resolve, mark_func);
    JS_MarkValue(rt, d->read_reject, mark_func);
    JS_MarkValue(rt, d->controller, mark_func);
}

static void reader_finalizer(JSRuntime *rt, JSValue val)
{
    ReaderData *r = JS_GetOpaque(val, g_reader_class);
    if (!r) return;
    JS_FreeValueRT(rt, r->stream);
    JS_FreeValueRT(rt, r->closed);
    JS_FreeValueRT(rt, r->closed_funcs[0]);
    JS_FreeValueRT(rt, r->closed_funcs[1]);
    js_free_rt(rt, r);
}

static void reader_gc_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func)
{
    ReaderData *r = JS_GetOpaque(val, g_reader_class);
    if (!r) return;
    JS_MarkValue(rt, r->stream, mark_func);
    JS_MarkValue(rt, r->closed, mark_func);
    JS_MarkValue(rt, r->closed_funcs[0], mark_func);
    JS_MarkValue(rt, r->closed_funcs[1], mark_func);
}

static void ctrl_finalizer(JSRuntime *rt, JSValue val)
{
    ControllerData *c = JS_GetOpaque(val, g_ctrl_class);
    if (!c) return;
    JS_FreeValueRT(rt, c->stream);
    JS_FreeValueRT(rt, c->pull_fn);
    JS_FreeValueRT(rt, c->cancel_fn);
    JS_FreeValueRT(rt, c->source);
    JS_FreeValueRT(rt, c->size_fn);
    js_free_rt(rt, c);
}

static void ctrl_gc_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func)
{
    ControllerData *c = JS_GetOpaque(val, g_ctrl_class);
    if (!c) return;
    JS_MarkValue(rt, c->stream, mark_func);
    JS_MarkValue(rt, c->pull_fn, mark_func);
    JS_MarkValue(rt, c->cancel_fn, mark_func);
    JS_MarkValue(rt, c->source, mark_func);
    JS_MarkValue(rt, c->size_fn, mark_func);
}

static StreamData *stream_of(JSValueConst v) { return JS_GetOpaque(v, g_stream_class); }
static ReaderData *reader_of(JSValueConst v) { return JS_GetOpaque(v, g_reader_class); }
static ControllerData *ctrl_of(JSValueConst v) { return JS_GetOpaque(v, g_ctrl_class); }

/* How many chunks are still unread. §4.2 has no length to expose; this is the queue's own bookkeeping. */
static uint32_t stream_queued(JSContext *ctx, StreamData *d)
{
    JSValue len_v = JS_GetPropertyStr(ctx, d->queue, "length");
    uint32_t n = 0;
    JS_ToUint32(ctx, &n, len_v);
    JS_FreeValue(ctx, len_v);
    return n - d->head;
}

/* §4.5's ResetQueue: the chunks, their sizes and the total go together, so they are dropped together. Every
   site that empties the queue calls this — an error, a cancel, a fresh stream — because a total left behind
   makes desiredSize answer for chunks that are gone. Returns -1 with an exception live. */
static int stream_queue_reset(JSContext *ctx, StreamData *d)
{
    JS_FreeValue(ctx, d->queue);
    JS_FreeValue(ctx, d->queue_size);
    d->queue = JS_NewArray(ctx);
    d->queue_size = JS_NewArray(ctx);
    d->head = 0;
    d->queue_total = 0;
    return JS_IsException(d->queue) || JS_IsException(d->queue_size) ? -1 : 0;
}

/* §4.5's EnqueueValueWithSize, minus its own RangeError — the caller checks that, because the check's failure
   is what errors the stream. */
static void stream_enqueue(JSContext *ctx, StreamData *d, JSValueConst chunk, double size)
{
    JSValue len_v = JS_GetPropertyStr(ctx, d->queue, "length");
    uint32_t n = 0;
    JS_ToUint32(ctx, &n, len_v);
    JS_FreeValue(ctx, len_v);
    JS_SetPropertyUint32(ctx, d->queue, n, JS_DupValue(ctx, chunk));
    JS_SetPropertyUint32(ctx, d->queue_size, n, JS_NewFloat64(ctx, size));
    d->queue_total += size;
}

/* §4.5's DequeueValue: the chunk leaves and the total loses its size. The clamp at zero is the spec's own,
   for the floating-point arithmetic a page can arrange with fractional sizes. */
static JSValue stream_dequeue(JSContext *ctx, StreamData *d)
{
    JSValue chunk = JS_GetPropertyUint32(ctx, d->queue, d->head);
    JSValue sz = JS_GetPropertyUint32(ctx, d->queue_size, d->head);
    double x = 0;
    JS_ToFloat64(ctx, &x, sz);
    JS_FreeValue(ctx, sz);
    d->head++;
    d->queue_total -= x;
    if (d->queue_total < 0) d->queue_total = 0;
    return chunk;
}

/* How many read requests are parked. */
static uint32_t stream_read_pending(JSContext *ctx, StreamData *d)
{
    JSValue len_v = JS_GetPropertyStr(ctx, d->read_resolve, "length");
    uint32_t n = 0;
    JS_ToUint32(ctx, &n, len_v);
    JS_FreeValue(ctx, len_v);
    return n - d->rhead;
}

/* TAKE the next parked read request's resolve (or reject), or JS_UNDEFINED when none is parked. */
static JSValue stream_take_read(JSContext *ctx, StreamData *d, int reject)
{
    JSValue f;
    if (stream_read_pending(ctx, d) == 0) return JS_UNDEFINED;
    f = JS_GetPropertyUint32(ctx, reject ? d->read_reject : d->read_resolve, d->rhead);
    /* the sibling capability of the pair is dropped with it: one read is answered once */
    JS_SetPropertyUint32(ctx, d->read_resolve, d->rhead, JS_UNDEFINED);
    JS_SetPropertyUint32(ctx, d->read_reject, d->rhead, JS_UNDEFINED);
    d->rhead++;
    return f;
}

/* §4.3's read result: `{ value, done }`, a plain object this component builds — so nothing of the page's is on
   it until the settle hands it over. One per read request: §4.2 says CreateIterResultObject per request, and a
   shared object would be observable as identity. */
static JSValue read_result(JSContext *ctx, JSValue value, bool done)
{
    JSValue o = JS_NewObject(ctx);
    if (JS_IsException(o)) { JS_FreeValue(ctx, value); return o; }
    JS_SetPropertyStr(ctx, o, "value", value);
    JS_SetPropertyStr(ctx, o, "done", JS_NewBool(ctx, done));
    return o;
}

/* §4.5's controller, EMPTY and already attached — the same discipline readable_stream_empty follows, and for
   the reason its own comment gives: a record whose fields are placed before it is attached to its object is a
   record that leaks on any failure in between. */
static JSValue controller_new(JSContext *ctx, JSValueConst stream)
{
    ControllerData *c;
    JSValue obj;

    obj = JS_NewObjectProtoClass(ctx, g_ctrl_proto, g_ctrl_class);
    if (JS_IsException(obj)) return obj;
    c = js_mallocz(ctx, sizeof *c);
    if (!c) { JS_FreeValue(ctx, obj); return JS_EXCEPTION; }
    c->pull_fn = c->cancel_fn = c->size_fn = c->source = JS_UNDEFINED;
    c->hwm = 1;                                  /* §4.2's default mark for a default-controller stream */
    c->stream = JS_DupValue(ctx, stream);
    JS_SetOpaque(obj, c);
    return obj;
}

/* AN EMPTY READABLE STREAM. Both entry points build one: the host-byte source fills and closes it, the
   constructor hands it to `start`. One allocation, so a field added to the record cannot be initialised by one
   and forgotten by the other — the same obligation the flow's fork taught. */
static JSValue readable_stream_empty(JSContext *ctx)
{
    StreamData *d;
    JSValue obj;

    DCHECK(g_stream_class != 0, "a ReadableStream was built before the class existed");
    obj = JS_NewObjectProtoClass(ctx, g_stream_proto, g_stream_class);
    if (JS_IsException(obj)) return obj;
    d = js_mallocz(ctx, sizeof *d);
    if (!d) { JS_FreeValue(ctx, obj); return JS_EXCEPTION; }
    d->stored_error = JS_UNDEFINED;
    d->reader = JS_UNDEFINED;
    d->controller = JS_UNDEFINED;
    d->queue = JS_NewArray(ctx);
    d->queue_size = JS_NewArray(ctx);
    d->read_resolve = JS_NewArray(ctx);
    d->read_reject = JS_NewArray(ctx);
    JS_SetOpaque(obj, d);
    if (JS_IsException(d->queue) || JS_IsException(d->queue_size) ||
        JS_IsException(d->read_resolve) || JS_IsException(d->read_reject)) {
        JS_FreeValue(ctx, obj);
        return JS_EXCEPTION;
    }
    /* THE CONTROLLER COMES WITH IT. §4.2 has no stream without one — the queue and the mark it is read
       against belong to the controller — and the three callers that built one separately each had to remember
       to, which is what left `readable_stream_from_bytes` with a stream that had none and a read path with a
       guard for it. */
    d->controller = controller_new(ctx, obj);
    if (JS_IsException(d->controller)) {
        d->controller = JS_UNDEFINED;
        JS_FreeValue(ctx, obj);
        return JS_EXCEPTION;
    }
    return obj;
}

JSValue readable_stream_from_bytes(JSContext *ctx, const char *bytes, size_t len)
{
    StreamData *d;
    JSValue obj, chunk;

    obj = readable_stream_empty(ctx);
    if (JS_IsException(obj)) return obj;
    d = stream_of(obj);
    /* ONE CHUNK, then closed. A byte sequence the host already holds has nothing left to arrive, and §4.2's
       "close" is exactly that statement — not an empty queue, which is a stream that may still be fed. */
    chunk = JS_NewUint8ArrayCopy(ctx, (const uint8_t *)(bytes ? bytes : ""), len);
    if (JS_IsException(chunk)) { JS_FreeValue(ctx, obj); return JS_EXCEPTION; }
    stream_enqueue(ctx, d, chunk, 1);
    JS_FreeValue(ctx, chunk);
    /* ONE CHUNK, then CLOSE REQUESTED — which is `enqueue` followed by `close`, and not the same thing as
       the CLOSED state: §4.2 answers a read on a closed stream `done` whatever is still queued, so marking
       this one closed outright would hand back an empty body. The drain is what closes it, exactly as it is
       for a page's own stream. */
    ctrl_of(d->controller)->close_requested = 1;
    ctrl_of(d->controller)->started = 1;
    return obj;
}

bool readable_stream_is(JSValueConst v)
{
    return g_stream_class != 0 && JS_GetOpaque(v, g_stream_class) != NULL;
}

bool readable_stream_disturbed(JSValueConst v)
{
    StreamData *d = g_stream_class ? JS_GetOpaque(v, g_stream_class) : NULL;
    return d != NULL && d->disturbed != 0;
}

/* ---- THE WORK A STREAM OPERATION DOES AFTER ITS OWN STEP ----------------------------------------------------
 *
 * Three sequences — settling every parked read request, PromiseResolve, and §4.5's CallPullIfNeeded — are run by
 * four different machines (`read`, the three controller members, the four reactions, the constructor). Each is a
 * chain of CALLS of the page's code, so each suspends, and the state it suspends in is this record, embedded in
 * every machine that runs one. Written per machine it would be five copies of the same five slots and the same
 * `visit`, which is the drift JSTrampStepDef.visit exists to stop.
 *
 * Each sequence owns its own state byte, because they NEST: a `pull` that throws runs ControllerError, which is
 * the settle sequence, from inside the pull sequence. `phase` is step_call_run's and is shared, which is sound
 * because exactly one call is ever in flight. */
enum { S_IDLE = 0,
       S_CLOSE_SET, S_CLOSE_CLOSED, S_CLOSE_LOOP,   /* §4.2's ReadableStreamClose */
       S_ERR_SET,   S_ERR_CLOSED,   S_ERR_LOOP,     /* §4.2's ReadableStreamError */
       S_REL_CLOSED, S_REL_LOOP };                  /* §4.3's release: the reader loses, the stream survives */
enum { P_IDLE = 0, P_TEST, P_CALL, P_RESOLVE, P_REJECT, P_THEN };

/* §4.3's `closed` promise, settled EXACTLY ONCE: it RESOLVES when the stream closes, REJECTS when it errors,
 * and REJECTS with a TypeError when the reader is released. Settling it is a call of the page's code like every
 * other settle, so it is a call request. `rd` may be NULL (a stream nobody is reading) and `value` is read only
 * on the first entry, which is the only entry that has one. */
static int reader_closed_run(JSContext *ctx, StreamWork *w, ReaderData *rd, int reject, JSValueConst value,
                             bool replace_if_settled, JSValue in, JSValue **out_cb, int *out_argc)
{
    JSValueConst arg;
    JSValue out;
    int r;

    if (w->phase == 0) {
        JSValue funcs[2];
        if (!rd) { JS_FreeValue(ctx, in); return 0; }
        if (rd->closed_settled) {
            JSValue p;
            if (!replace_if_settled) { JS_FreeValue(ctx, in); return 0; }
            /* §4.3's release, second arm: a reader whose stream had ALREADY finished gets a NEW `closed`
               promise, already rejected. The identity change is observable — `assert_not_equals` on the two
               promise objects is what the corpus checks — so it cannot be a no-op. */
            p = JS_NewPromiseCapability(ctx, funcs);
            if (JS_IsException(p)) { JS_FreeValue(ctx, in); return -1; }
            JS_FreeValue(ctx, rd->closed);
            rd->closed = p;
        } else {
            rd->closed_settled = 1;
            funcs[0] = rd->closed_funcs[0];               /* the pair is HANDED OVER and dropped together */
            funcs[1] = rd->closed_funcs[1];
            rd->closed_funcs[0] = rd->closed_funcs[1] = JS_UNDEFINED;
        }
        JS_FreeValue(ctx, w->func);
        w->func = funcs[reject];
        JS_FreeValue(ctx, funcs[reject ^ 1]);
        JS_FreeValue(ctx, w->value);
        w->value = JS_DupValue(ctx, value);
    }
    arg = w->value;
    r = step_call_run(ctx, &w->phase, w->cb, w->func, JS_UNDEFINED, 1, &arg, in, &out, out_cb, out_argc);
    if (r > 0) return r;
    if (JS_IsException(out)) return -1;
    JS_FreeValue(ctx, out);
    return 0;
}

/* §4.2's ReadableStreamClose and ReadableStreamError and §4.3's release, which are ONE sequence with three
 * entry points: move the stream's state (or, for a release, none), settle the reader's `closed` promise, then
 * answer EVERY parked read request. All three say "for each readRequest", and answering one is a CALL of the
 * page's code — so the tail is a LOOP OF CALL REQUESTS, one suspension per request. Answering only the first,
 * which is what this component did before the controller could pull, silently abandons the rest.
 * The caller sets `w->settle` (and `w->err` for the arms that carry a reason) and calls until it returns 0. */
static int stream_settle_run(JSContext *ctx, StreamWork *w, StreamData *d, JSValue in,
                             JSValue **out_cb, int *out_argc)
{
    ReaderData *rd = JS_IsUndefined(d->reader) ? NULL : reader_of(d->reader);
    int r;

    if (w->settle == S_CLOSE_SET) {
        if (d->state == RS_READABLE) d->state = RS_CLOSED;
        w->settle = S_CLOSE_CLOSED;
    } else if (w->settle == S_ERR_SET) {
        if (d->state == RS_READABLE) {
            d->state = RS_ERRORED;
            JS_FreeValue(ctx, d->stored_error);
            d->stored_error = w->err;      /* the reason is HANDED OVER: the stream owns it from here */
            w->err = JS_UNDEFINED;
            /* §4.5's ResetQueue: an errored stream has no chunks left to give */
            if (stream_queue_reset(ctx, d) < 0) { JS_FreeValue(ctx, in); return -1; }
        } else {
            JS_FreeValue(ctx, w->err);
            w->err = JS_UNDEFINED;
        }
        w->settle = S_ERR_CLOSED;
    }

    if (w->settle == S_CLOSE_CLOSED || w->settle == S_ERR_CLOSED || w->settle == S_REL_CLOSED) {
        int reject = w->settle != S_CLOSE_CLOSED;
        /* a CLOSE resolves with undefined; an ERROR rejects with what the stream stored; a RELEASE rejects
           with the TypeError the caller left in `err` */
        JSValueConst v = w->settle == S_ERR_CLOSED ? (JSValueConst)d->stored_error
                       : w->settle == S_REL_CLOSED ? (JSValueConst)w->err
                                                   : JS_UNDEFINED;
        r = reader_closed_run(ctx, w, rd, reject, v, w->settle == S_REL_CLOSED, in, out_cb, out_argc);
        if (r != 0) return r;
        in = JS_UNDEFINED;
        if (w->settle == S_REL_CLOSED) {
            /* §4.3's GenericRelease, in the one order that works: the `closed` promise is settled while the
               reader still holds the lock, and only then is the lock dropped. */
            DCHECK(rd != NULL, "a release sequence ran on a stream that has no reader");
            JS_FreeValue(ctx, d->reader);
            d->reader = JS_UNDEFINED;
            JS_FreeValue(ctx, rd->stream);
            rd->stream = JS_UNDEFINED;
            w->settle = S_REL_LOOP;
        } else {
            w->settle = w->settle == S_CLOSE_CLOSED ? S_CLOSE_LOOP : S_ERR_LOOP;
        }
    }

    DCHECK(w->settle == S_CLOSE_LOOP || w->settle == S_ERR_LOOP || w->settle == S_REL_LOOP,
           "the settle sequence resumed in a state it never parks in");

    for (;;) {
        JSValueConst arg;
        JSValue out;

        if (w->phase == 0) {
            JS_FreeValue(ctx, w->func);
            w->func = stream_take_read(ctx, d, w->settle != S_CLOSE_LOOP);
            if (JS_IsUndefined(w->func)) {
                JS_FreeValue(ctx, in);
                JS_FreeValue(ctx, w->err);
                w->err = JS_UNDEFINED;
                w->settle = S_IDLE;
                return 0;
            }
            JS_FreeValue(ctx, w->value);
            w->value = w->settle == S_CLOSE_LOOP ? read_result(ctx, JS_UNDEFINED, true)
                     : w->settle == S_ERR_LOOP   ? JS_DupValue(ctx, d->stored_error)
                                                 : JS_DupValue(ctx, w->err);
            if (JS_IsException(w->value)) { JS_FreeValue(ctx, in); return -1; }
        }
        arg = w->value;
        r = step_call_run(ctx, &w->phase, w->cb, w->func, JS_UNDEFINED, 1, &arg, in, &out, out_cb, out_argc);
        if (r > 0) return r;
        if (JS_IsException(out)) return -1;
        JS_FreeValue(ctx, out);
        in = JS_UNDEFINED;
    }
}

/* §4.5's reactions, by what they react to. Each is a step closure capturing the controller. */
enum { RXN_START_OK = 0, RXN_START_ERR, RXN_PULL_OK, RXN_PULL_ERR };

/* ATTACH A PAIR OF §4.5 REACTIONS. They are STEP CLOSURES because a reaction calls the page's `pull` again and
 * rejects parked read requests — work only a machine may do — and knows its controller only by capture.
 * PerformPromiseThen rather than a `.then` read, because that is what §4.5 performs: a page that replaces
 * Promise.prototype.then changes what its own `.then()` does and changes nothing about what the stream does. */
static int ctrl_react(JSContext *ctx, JSValueConst promise, JSValueConst ctrl, int ok, int err)
{
    JSValueConst data[1];
    data[0] = ctrl;
    return stream_react(ctx, promise, g_rxn_stepids[ok], g_rxn_stepids[err], data, 1);
}

/* ---- A REACTION THAT SETTLES A CAPABILITY THIS COMPONENT HOLDS ----------------------------------------------
 *
 * §4.2's cancel hands back a promise that settles when the SOURCE'S cancel promise does — step 7 is "the result
 * of reacting to sourceCancelPromise with a fulfilment step that returns undefined". So the reaction's whole job
 * is to call one of this component's own resolving functions, which it knows only by CAPTURE, and calling it is
 * the page's code. FWD_UNDEF discards the source's answer (which is what "returns undefined" states); FWD_ARG
 * passes its own value through, which is how a rejection keeps its reason. */
enum { FWD_UNDEF = 0, FWD_ARG };

typedef struct {
    JSStepHdr hdr;
    uint8_t   phase;
    JSValue   cb[3];
} JSFwdState;

static void js_fwd_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSFwdState *s = st;
    int k;
    for (k = 0; k < 3; k++) v->val(ctx, &s->cb[k]);
}

static JSValue js_fwd_fini(JSContext *ctx, void *st, bool take_result)
{
    JSFwdState *s = st;
    int k;
    (void)take_result;
    for (k = 0; k < 3; k++) { JS_FreeValue(ctx, s->cb[k]); s->cb[k] = JS_UNDEFINED; }
    return JS_UNDEFINED;
}

static int js_fwd_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    JSFwdState *s = st;
    JSValueConst arg = s->hdr.arg == FWD_ARG && s->hdr.argc > 0 ? s->hdr.argv[0] : JS_UNDEFINED;
    JSValue out;
    int r;

    r = step_call_run(ctx, &s->phase, s->cb, JS_StepClosureData(&s->hdr, 0), JS_UNDEFINED, 1, &arg,
                      cb_result, &out, out_cb, out_argc);
    if (r > 0) return r;
    if (JS_IsException(out)) return JS_STEP_ABRUPT;
    JS_FreeValue(ctx, out);
    return JS_STEP_DONE;
}

#define FWD_DEF(i) { sizeof(JSFwdState), js_fwd_step, js_fwd_fini, (i), \
                     .catches_abrupt = 1, .visit = js_fwd_visit }
static const JSTrampStepDef js_fwd_defs[2] = { FWD_DEF(FWD_UNDEF), FWD_DEF(FWD_ARG) };
#undef FWD_DEF
static int g_fwd_stepids[2] = { -1, -1 };

/* Settle `on_ok`/`on_err` when `promise` does. PerformPromiseThen rather than a `.then` read, for the reason
   ctrl_react gives. The two functions are BORROWED; each closure takes its own reference. */
static int ctrl_forward(JSContext *ctx, JSValueConst promise, JSValueConst on_ok, JSValueConst on_err)
{
    JSValueConst d0[1], d1[1];
    JSValue onf, onr, cap;

    DCHECK(g_fwd_stepids[0] >= 0 && g_fwd_stepids[1] >= 0,
           "a forwarding reaction was attached before its machines were registered");
    d0[0] = on_ok;
    d1[0] = on_err;
    onf = JS_NewStepClosure(ctx, g_fwd_stepids[FWD_UNDEF], 1, 1, d0);
    if (JS_IsException(onf)) return -1;
    onr = JS_NewStepClosure(ctx, g_fwd_stepids[FWD_ARG], 1, 1, d1);
    if (JS_IsException(onr)) { JS_FreeValue(ctx, onf); return -1; }
    cap = JS_PerformPromiseThen(ctx, promise, onf, onr);
    JS_FreeValue(ctx, onf);
    JS_FreeValue(ctx, onr);
    if (JS_IsException(cap)) return -1;
    JS_FreeValue(ctx, cap);
    return 0;
}

/* §4.5's ReadableStreamDefaultControllerCanCloseOrEnqueue. */
static bool ctrl_can_close_or_enqueue(ControllerData *c, StreamData *d)
{
    return !c->close_requested && d->state == RS_READABLE;
}

/* §4.5's ReadableStreamDefaultControllerShouldCallPull. The default strategy's high-water mark is 1, so the
   desired size is 1 minus what is queued — which is why a source with an empty queue is pulled once even
   though nobody is reading, and why a source with a chunk in hand is not. */
static bool ctrl_should_pull(JSContext *ctx, ControllerData *c, StreamData *d)
{
    if (!ctrl_can_close_or_enqueue(c, d)) return false;
    if (!c->started) return false;
    if (!JS_IsUndefined(d->reader) && stream_read_pending(ctx, d) > 0) return true;
    return c->hwm - d->queue_total > 0;
}

/* §4.5's ReadableStreamDefaultControllerCallPullIfNeeded. The caller sets `w->pull = P_TEST` and calls until it
   returns 0; every machine that can change what ShouldCallPull answers runs it. */
static int ctrl_pull_run(JSContext *ctx, StreamWork *w, JSValueConst ctrl_v, JSValue in,
                         JSValue **out_cb, int *out_argc)
{
    ControllerData *c = ctrl_of(ctrl_v);
    StreamData *d;
    int r;

    DCHECK(c != NULL, "the pull sequence ran on a value that is not a ReadableStreamDefaultController");
    d = stream_of(c->stream);
    DCHECK(d != NULL, "a controller's stream stopped being a ReadableStream");

    if (w->pull == P_TEST) {
        if (JS_IsUndefined(c->pull_fn) || !ctrl_should_pull(ctx, c, d)) {
            JS_FreeValue(ctx, in);
            w->pull = P_IDLE;
            return 0;
        }
        if (c->pulling) {
            /* §4.5 step 3: a pull asked for while one is in flight is REMEMBERED, and the fulfilment reaction
               runs it. Dropping it stalls a source that only enqueues one chunk per pull. */
            c->pull_again = 1;
            JS_FreeValue(ctx, in);
            w->pull = P_IDLE;
            return 0;
        }
        DCHECK(!c->pull_again, "a controller carried pullAgain while it was not pulling at all");
        c->pulling = 1;
        w->pull = P_CALL;
    }
    if (w->pull == P_CALL) {
        JSValueConst arg = ctrl_v;
        JSValue res;
        r = step_call_run(ctx, &w->phase, w->cb, c->pull_fn, c->source, 1, &arg, in, &res,
                          out_cb, out_argc);
        if (r > 0) return r;
        JS_FreeValue(ctx, w->value);
        /* §4.9.4 builds the pull algorithm with PromiseCall: a `pull` that THROWS yields a REJECTED PROMISE,
           and the controller is errored by the REACTION to it — not here, inline. The difference is one
           microtask and it is observable: the tee delivers the chunk it had already dequeued in between, so
           erroring inline lost that chunk. This is why these machines declare catches_abrupt. */
        w->pull = JS_IsException(res) ? P_REJECT : P_RESOLVE;
        w->value = JS_IsException(res) ? JS_GetException(ctx) : res;
        in = JS_UNDEFINED;
    }
    if (w->pull == P_RESOLVE || w->pull == P_REJECT) {
        r = stream_promise_of_run(ctx, w, w->pull == P_REJECT, in, out_cb, out_argc);
        if (r != 0) return r;
        w->pull = P_THEN;
    }
    DCHECK(w->pull == P_THEN, "the pull sequence resumed in a state it never parks in");
    r = ctrl_react(ctx, w->func, ctrl_v, RXN_PULL_OK, RXN_PULL_ERR);
    JS_FreeValue(ctx, w->func);
    w->func = JS_UNDEFINED;
    w->pull = P_IDLE;
    return r;
}

/* ---- §4.5's REACTIONS ---------------------------------------------------------------------------------------
 *
 * One machine, four definitions, because the definition is where the operation is declared — the same shape the
 * three controller members have. `arg` selects; the captured value is the controller. */
typedef struct {
    JSStepHdr hdr;
    StreamWork  w;
} JSRxnState;

static void js_rxn_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSRxnState *s = st;
    stream_work_visit(ctx, &s->w, v);
}

static JSValue js_rxn_fini(JSContext *ctx, void *st, bool take_result)
{
    JSRxnState *s = st;
    (void)take_result;
    stream_work_release(ctx, &s->w);
    return JS_UNDEFINED;
}

static int js_rxn_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    JSRxnState *s = st;
    JSValueConst ctrl = JS_StepClosureData(&s->hdr, 0);
    ControllerData *c = ctrl_of(ctrl);
    StreamData *d;
    int r;

    DCHECK(c != NULL, "a stream reaction captured something that is not a ReadableStreamDefaultController");
    d = stream_of(c->stream);
    DCHECK(d != NULL, "a controller's stream stopped being a ReadableStream");

    if (s->w.stage == 0) {
        int op = s->hdr.arg;
        stream_work_start(&s->w);
        JS_FreeValue(ctx, cb_result);
        cb_result = JS_UNDEFINED;
        s->w.stage = 1;
        if (op == RXN_START_OK) {
            /* §4.5's set-up: the controller is STARTED only when start's promise fulfils, so a source whose
               `pull` would have been called synchronously from the constructor is instead called from here —
               which is the ordering a page observes and pins. */
            c->started = 1;
            DCHECK(!c->pulling && !c->pull_again, "a controller pulled before its start promise fulfilled");
            s->w.pull = P_TEST;
        } else if (op == RXN_PULL_OK) {
            c->pulling = 0;
            if (!c->pull_again) return JS_STEP_DONE;
            c->pull_again = 0;
            s->w.pull = P_TEST;
        } else {
            DCHECK(op == RXN_START_ERR || op == RXN_PULL_ERR, "a stream reaction ran with no operation");
            if (op == RXN_PULL_ERR) c->pulling = 0;
            s->w.err = JS_DupValue(ctx, s->hdr.argc > 0 ? s->hdr.argv[0] : JS_UNDEFINED);
            s->w.settle = S_ERR_SET;
        }
    }
    r = s->w.pull != P_IDLE ? ctrl_pull_run(ctx, &s->w, ctrl, cb_result, out_cb, out_argc)
                            : stream_settle_run(ctx, &s->w, d, cb_result, out_cb, out_argc);
    if (r > 0) return r;
    if (r < 0) return JS_STEP_ABRUPT;
    return JS_STEP_DONE;
}

#define RXN_DEF(i) { sizeof(JSRxnState), js_rxn_step, js_rxn_fini, (i), \
                     .catches_abrupt = 1, .visit = js_rxn_visit }
static const JSTrampStepDef js_rxn_defs[4] = {
    RXN_DEF(RXN_START_OK), RXN_DEF(RXN_START_ERR), RXN_DEF(RXN_PULL_OK), RXN_DEF(RXN_PULL_ERR),
};
#undef RXN_DEF

/* ---- §4.3's reader ---------------------------------------------------------------------------------------- */

/* §4.3's `read()`. A MACHINE, because settling the promise it returns is the PAGE'S code: 27.2.1.3.2 step 8
 * reads `Get(resolution, "then")` off the result object, whose prototype the page owns. The same reason
 * body.c's readers are machines, and the same shape.
 *
 * It also performs §4.5's PullSteps, which is where a stream's `pull` is asked for on demand — and the spec's
 * ORDER is that the close-or-pull happens BEFORE the read request is answered, so those are stages ahead of the
 * settle rather than after it. */
enum { RD_START = 0, RD_CLOSE, RD_PULL, RD_SETTLE };

typedef struct {
    JSStepHdr hdr;
    StreamWork  w;
    JSValue   promise;   /* the capability handed back to the page */
    JSValue   settle;    /* its resolve or reject function, or JS_UNDEFINED when the request PARKED */
    JSValue   result;    /* what to settle it with */
    JSValue   stream;    /* the stream, so the close and pull stages can reach it */
} JSReadState;

static void js_read_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSReadState *s = st;
    stream_work_visit(ctx, &s->w, v);
    v->val(ctx, &s->promise);
    v->val(ctx, &s->settle);
    v->val(ctx, &s->result);
    v->val(ctx, &s->stream);
}

static JSValue js_read_fini(JSContext *ctx, void *st, bool take_result)
{
    JSReadState *s = st;
    JSValue r = take_result ? s->promise : JS_UNDEFINED;
    if (take_result) s->promise = JS_UNDEFINED;
    stream_work_release(ctx, &s->w);
    JS_FreeValue(ctx, s->promise);
    JS_FreeValue(ctx, s->settle);
    JS_FreeValue(ctx, s->result);
    JS_FreeValue(ctx, s->stream);
    s->promise = s->settle = s->result = s->stream = JS_UNDEFINED;
    return r;
}

static int js_read_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    JSReadState *s = st;
    JSValue out;
    JSValueConst arg;
    int r;

    if (s->w.stage == RD_START) {
        ReaderData *rd = reader_of(s->hdr.this_val);
        StreamData *d = NULL;
        JSValue funcs[2];
        int reject = 0;

        stream_work_start(&s->w);
        JS_FreeValue(ctx, cb_result);
        cb_result = JS_UNDEFINED;
        s->promise = s->settle = s->result = s->stream = JS_UNDEFINED;
        if (!rd) {
            JS_ThrowTypeError(ctx, "not a ReadableStreamDefaultReader");
            return JS_STEP_ABRUPT;
        }
        s->w.stage = RD_SETTLE;
        if (JS_IsUndefined(rd->stream)) {
            /* §4.3: a released reader's read() REJECTS rather than throwing — it is a promise-returning
               member, and a page's `.catch` is where it expects to see this. */
            JS_ThrowTypeError(ctx, "this reader has been released");
            s->result = JS_GetException(ctx);
            reject = 1;
        } else {
            d = stream_of(rd->stream);
            DCHECK(d != NULL, "a reader's stream stopped being a ReadableStream while it held the lock");
            s->stream = JS_DupValue(ctx, rd->stream);
            d->disturbed = 1;
            /* §4.2 asks the STATE first and the queue only after. A stream that has closed answers `done`
               whatever is still in its controller's queue — which is what `close()` inside `size()` produces:
               the state goes to closed while the chunk being weighed is still on its way in, and that chunk is
               then unreadable. Reading the queue first handed it back. */
            if (d->state == RS_ERRORED) {
                s->result = JS_DupValue(ctx, d->stored_error);
                reject = 1;
            } else if (d->state == RS_CLOSED) {
                s->result = read_result(ctx, JS_UNDEFINED, true);
            } else if (stream_queued(ctx, d) > 0) {
                ControllerData *c = ctrl_of(d->controller);
                JSValue chunk = stream_dequeue(ctx, d);
                s->result = read_result(ctx, chunk, false);
                /* §4.5's PullSteps: draining the LAST chunk of a stream whose close was requested is what
                   actually closes it — a page that calls close() with chunks still queued must see every one
                   of them before `done`. Otherwise the drain is what makes room, so the source is pulled. */
                DCHECK(c != NULL, "a ReadableStream reached a read with no controller");
                if (c->close_requested && stream_queued(ctx, d) == 0) {
                    s->w.stage = RD_CLOSE;
                    s->w.settle = S_CLOSE_SET;
                } else {
                    s->w.stage = RD_PULL;
                    s->w.pull = P_TEST;
                }
            } else {
                /* §4.2: a READABLE stream with an empty queue PARKS the read request. The promise is returned
                   unsettled and the controller answers it later — which is the whole reason a stream is not
                   just a queue. The source is then pulled, because a parked reader is what ShouldCallPull is
                   most interested in. */
                uint32_t at;
                s->promise = JS_NewPromiseCapability(ctx, funcs);
                if (JS_IsException(s->promise)) return JS_STEP_ABRUPT;
                at = stream_read_pending(ctx, d) + d->rhead;
                JS_SetPropertyUint32(ctx, d->read_resolve, at, funcs[0]);
                JS_SetPropertyUint32(ctx, d->read_reject, at, funcs[1]);
                s->w.stage = RD_PULL;
                s->w.pull = P_TEST;
                goto have_promise;
            }
            if (JS_IsException(s->result)) return JS_STEP_ABRUPT;
        }
        s->promise = JS_NewPromiseCapability(ctx, funcs);
        if (JS_IsException(s->promise)) return JS_STEP_ABRUPT;
        s->settle = funcs[reject];
        JS_FreeValue(ctx, funcs[reject ^ 1]);
    have_promise:;
    }

    if (s->w.stage == RD_CLOSE) {
        r = stream_settle_run(ctx, &s->w, stream_of(s->stream), cb_result, out_cb, out_argc);
        if (r > 0) return r;
        if (r < 0) return JS_STEP_ABRUPT;
        cb_result = JS_UNDEFINED;
        s->w.stage = RD_SETTLE;
    }
    if (s->w.stage == RD_PULL) {
        StreamData *d = stream_of(s->stream);
        DCHECK(d != NULL, "the read machine's stream stopped being a ReadableStream");
        r = ctrl_pull_run(ctx, &s->w, d->controller, cb_result, out_cb, out_argc);
        if (r > 0) return r;
        if (r < 0) return JS_STEP_ABRUPT;
        cb_result = JS_UNDEFINED;
        s->w.stage = RD_SETTLE;
    }

    DCHECK(s->w.stage == RD_SETTLE, "the read machine resumed in a stage it never parks in");
    if (JS_IsUndefined(s->settle)) {
        /* the request parked: nothing to settle yet, the controller owns the answer */
        JS_FreeValue(ctx, cb_result);
        return JS_STEP_DONE;
    }
    arg = s->result;
    r = step_call_run(ctx, &s->w.phase, s->w.cb, s->settle, JS_UNDEFINED, 1, &arg, cb_result, &out,
                      out_cb, out_argc);
    if (r > 0) return r;
    if (JS_IsException(out)) return JS_STEP_ABRUPT;
    JS_FreeValue(ctx, out);
    return JS_STEP_DONE;
}

static const JSTrampStepDef js_read_def = {
    sizeof(JSReadState), js_read_step, js_read_fini, 0, .catches_abrupt = 1, .visit = js_read_visit
};

/* §4.3's `releaseLock()`. A MACHINE, because releasing is not just dropping the lock: it REJECTS the reader's
 * `closed` promise with a TypeError and rejects every read request the reader had parked — both of which settle
 * promises, which is the page's code. A release that only dropped the lock left a page awaiting `reader.closed`
 * forever, and a `read()` issued before it never answered at all; that is what made the corpus's whole
 * default-reader file report NOTHING rather than a failure, because testharness cannot complete while a
 * promise_test is still unsettled. */
typedef struct {
    JSStepHdr hdr;
    StreamWork  w;
    /* THE STREAM, HELD BY THE MACHINE. The release clears the reader's own pointer to it partway through the
       sequence — that IS the release — so a resume that read it back off the reader would find nothing. */
    JSValue   stream;
} JSReleaseState;

static void js_release_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSReleaseState *s = st;
    stream_work_visit(ctx, &s->w, v);
    v->val(ctx, &s->stream);
}

static JSValue js_release_fini(JSContext *ctx, void *st, bool take_result)
{
    JSReleaseState *s = st;
    (void)take_result;
    stream_work_release(ctx, &s->w);
    JS_FreeValue(ctx, s->stream);
    s->stream = JS_UNDEFINED;
    return JS_UNDEFINED;
}

static int js_release_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    JSReleaseState *s = st;
    StreamData *d;
    int r;

    if (s->w.stage == 0) {
        ReaderData *rd = reader_of(s->hdr.this_val);
        stream_work_start(&s->w);
        JS_FreeValue(ctx, cb_result);
        cb_result = JS_UNDEFINED;
        s->stream = JS_UNDEFINED;
        s->w.stage = 1;
        if (!rd) {
            JS_ThrowTypeError(ctx, "not a ReadableStreamDefaultReader");
            return JS_STEP_ABRUPT;
        }
        if (JS_IsUndefined(rd->stream)) return JS_STEP_DONE;   /* §4.3: releasing twice is a no-op */
        s->stream = JS_DupValue(ctx, rd->stream);
        JS_ThrowTypeError(ctx, "this reader was released while it was still in use");
        s->w.err = JS_GetException(ctx);
        s->w.settle = S_REL_CLOSED;
    }
    if (s->w.settle == S_IDLE) { JS_FreeValue(ctx, cb_result); return JS_STEP_DONE; }
    d = stream_of(s->stream);
    DCHECK(d != NULL, "a reader held something that is not a ReadableStream");
    r = stream_settle_run(ctx, &s->w, d, cb_result, out_cb, out_argc);
    if (r > 0) return r;
    if (r < 0) return JS_STEP_ABRUPT;
    return JS_STEP_DONE;
}

static const JSTrampStepDef js_release_def = {
    sizeof(JSReleaseState), js_release_step, js_release_fini, 0, .catches_abrupt = 1, .visit = js_release_visit
};

static JSValue js_reader_closed(JSContext *ctx, JSValueConst this_val, int magic)
{
    ReaderData *r = reader_of(this_val);
    (void)magic;
    if (!r) return JS_ThrowTypeError(ctx, "not a ReadableStreamDefaultReader");
    return JS_DupValue(ctx, r->closed);
}

/* ---- §4.2's members --------------------------------------------------------------------------------------- */

static JSValue js_stream_locked(JSContext *ctx, JSValueConst this_val, int magic)
{
    StreamData *d = stream_of(this_val);
    (void)magic;
    if (!d) return JS_ThrowTypeError(ctx, "not a ReadableStream");
    return JS_NewBool(ctx, !JS_IsUndefined(d->reader));
}

/* §4.2's `dictionary ReadableStreamGetReaderOptions { ReadableStreamReaderMode mode; }`, whose one enumeration
   value is "byob". DECLARED rather than read from the body: `mode` is one accessor away from being the page's
   code and its value is one `toString` away, and reading it from C ran both — which is an abort at
   JS_ToPrimitiveFree naming exactly this, on the very first test of the corpus's default-reader file. */
/* §7's `dictionary QueuingStrategy { unrestricted double highWaterMark; QueuingStrategySize size; }`, whose
   members Web IDL reads lexicographically. */
static const IdlDictMember QUEUING_STRATEGY[] = {
    { "highWaterMark", IDL_UNRESTRICTED_DOUBLE },
    { "size",          IDL_CALLBACK },
};

static const char *const READER_MODES[] = { "byob", NULL };
static const IdlDictMember GET_READER_OPTIONS[] = {
    { "mode", IDL_ENUM, false, READER_MODES },
};

/* §4.2's `getReader(options)` and §4.3's constructor, which ARE one operation — SetUpReadableStreamDefaultReader
 * — reached by two names, so they are one machine reached with two magics.
 *
 * A MACHINE because §4.3's GenericInitialize settles `closed` IMMEDIATELY when the stream is already closed or
 * errored: a reader taken on a finished stream must find its `closed` promise already settled, and settling is
 * the page's code. `mode: "byob"` names the BYTE STREAM reader, which is a different interface over a different
 * controller — absent here, and a TypeError is what §4.2 says for a stream that is not one. */
enum { GR_SELF = 0, GR_CTOR };   /* the magic: which argument the stream arrives in */

typedef struct {
    StreamWork w;
    JSValue  reader;
} JSGetReaderState;

static void js_get_reader_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSGetReaderState *s = st;
    stream_work_visit(ctx, &s->w, v);
    v->val(ctx, &s->reader);
}

static void js_get_reader_release(JSContext *ctx, void *st)
{
    JSGetReaderState *s = st;
    stream_work_release(ctx, &s->w);
    JS_FreeValue(ctx, s->reader);
    s->reader = JS_UNDEFINED;
}

static int js_get_reader_step(JSContext *ctx, JSStepHdr *hdr, void *st, int argc, JSValueConst *argv,
                              JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    JSGetReaderState *s = st;
    int ctor = idl_step_magic(hdr) == GR_CTOR;
    JSValueConst stream_v = ctor ? (argc > 0 ? argv[0] : JS_UNDEFINED) : hdr->this_val;
    JSValueConst arg;
    JSValue out;
    int r;

    if (s->w.stage == 0) {
        StreamData *d = stream_of(stream_v);
        ReaderData *rd;
        JSValue obj;

        stream_work_start(&s->w);
        JS_FreeValue(ctx, cb_result);
        cb_result = JS_UNDEFINED;
        s->reader = JS_UNDEFINED;
        s->w.stage = 1;
        if (ctor && JS_IsUndefined(hdr->this_val)) {
            JS_ThrowTypeError(ctx, "constructor ReadableStreamDefaultReader requires 'new'");
            return -1;
        }
        if (!d) {
            JS_ThrowTypeError(ctx, "not a ReadableStream");
            return -1;
        }
        if (!JS_IsUndefined(d->reader)) {
            JS_ThrowTypeError(ctx, "this stream is already locked to a reader");
            return -1;
        }
        if (!ctor) {
            /* the declaration has already converted the options: an absent dictionary has every member
               absent, and the only value that survives the enumeration is "byob" */
            JSValue mode = argc > 0 ? idl_dict_get(ctx, argv[0], "mode") : JS_UNDEFINED;
            int byob = !JS_IsUndefined(mode);
            JS_FreeValue(ctx, mode);
            if (byob) {
                JS_ThrowTypeError(ctx, "this stream is not a byte stream, so it has no BYOB reader");
                return -1;
            }
        }
        obj = JS_NewObjectProtoClass(ctx, g_reader_proto, g_reader_class);
        if (JS_IsException(obj)) return -1;
        rd = js_mallocz(ctx, sizeof *rd);
        if (!rd) { JS_FreeValue(ctx, obj); return -1; }
        rd->stream = JS_DupValue(ctx, stream_v);
        rd->closed = JS_NewPromiseCapability(ctx, rd->closed_funcs);
        JS_SetOpaque(obj, rd);
        s->reader = obj;
        if (JS_IsException(rd->closed)) return -1;
        d->reader = JS_DupValue(ctx, obj);
        /* §4.3's GenericInitialize: a stream that is ALREADY finished settles `closed` here and now. */
        if (d->state == RS_READABLE) goto done;
        rd->closed_settled = 1;
        s->w.func = rd->closed_funcs[d->state == RS_ERRORED];
        JS_FreeValue(ctx, rd->closed_funcs[d->state == RS_ERRORED ? 0 : 1]);
        rd->closed_funcs[0] = rd->closed_funcs[1] = JS_UNDEFINED;
        s->w.value = d->state == RS_ERRORED ? JS_DupValue(ctx, d->stored_error) : JS_UNDEFINED;
        s->w.stage = 2;
    }

    if (s->w.stage == 2) {
        arg = s->w.value;
        r = step_call_run(ctx, &s->w.phase, s->w.cb, s->w.func, JS_UNDEFINED, 1, &arg, cb_result, &out,
                          out_cb, out_argc);
        if (r > 0) return r;
        JS_FreeValue(ctx, out);
        cb_result = JS_UNDEFINED;
    }
done:
    JS_FreeValue(ctx, cb_result);
    *presult = s->reader;
    s->reader = JS_UNDEFINED;
    return 0;
}

static const IdlStepDecl js_get_reader_decl = {
    js_get_reader_step, sizeof(JSGetReaderState), js_get_reader_visit, js_get_reader_release
};

/* §4.2's ReadableStreamCancel, reached from the stream's `cancel(reason)` and from the reader's — §4.3 says
 * a reader cancels the stream it holds, so the two ARE one operation and one machine.
 *
 * IT CALLS THE SOURCE'S OWN `cancel`, and the promise it hands back settles when the SOURCE'S does: §4.2 step 7
 * is "the result of reacting to sourceCancelPromise with a fulfilment step that returns undefined". That
 * reaction is a step closure over this component's own resolving function, the same shape §4.5's pull
 * reactions are. Before this, the page's `cancel` was never invoked at all — a stream whose source releases a
 * socket released nothing, and the returned promise settled ahead of the source rather than with it. */
enum { CN_START = 0, CN_CLOSE, CN_CALL, CN_RESOLVE, CN_THEN, CN_SETTLE };

typedef struct {
    JSStepHdr hdr;
    StreamWork  w;
    JSValue   promise;     /* the capability handed back to the page */
    JSValue   funcs[2];    /* its resolve and reject, which the source's own promise settles through */
    JSValue   settle;      /* the one of them a short-circuit path calls directly */
    JSValue   stream;
    uint8_t   reject_algorithm;   /* the source's `cancel` threw, so its promise is a rejected one */
} JSCancelState;

static void js_cancel_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSCancelState *s = st;
    stream_work_visit(ctx, &s->w, v);
    v->val(ctx, &s->promise);
    v->val(ctx, &s->funcs[0]);
    v->val(ctx, &s->funcs[1]);
    v->val(ctx, &s->settle);
    v->val(ctx, &s->stream);
}

static JSValue js_cancel_fini(JSContext *ctx, void *st, bool take_result)
{
    JSCancelState *s = st;
    JSValue r = take_result ? s->promise : JS_UNDEFINED;
    if (take_result) s->promise = JS_UNDEFINED;
    stream_work_release(ctx, &s->w);
    JS_FreeValue(ctx, s->promise);
    JS_FreeValue(ctx, s->funcs[0]);
    JS_FreeValue(ctx, s->funcs[1]);
    JS_FreeValue(ctx, s->settle);
    JS_FreeValue(ctx, s->stream);
    s->promise = s->funcs[0] = s->funcs[1] = s->settle = s->stream = JS_UNDEFINED;
    return r;
}

static int js_cancel_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    JSCancelState *s = st;
    StreamData *d;
    ControllerData *c;
    JSValueConst arg;
    JSValue out;
    int r;

    if (s->w.stage == CN_START) {
        /* The receiver is the STREAM for `stream.cancel()` and the READER for `reader.cancel()`. */
        ReaderData *rd;
        int reject = 0;

        stream_work_start(&s->w);
        JS_FreeValue(ctx, cb_result);
        cb_result = JS_UNDEFINED;
        s->promise = s->funcs[0] = s->funcs[1] = s->settle = s->stream = JS_UNDEFINED;
        d = stream_of(s->hdr.this_val);
        rd = d ? NULL : reader_of(s->hdr.this_val);
        if (!d && rd && !JS_IsUndefined(rd->stream)) d = stream_of(rd->stream);
        s->promise = JS_NewPromiseCapability(ctx, s->funcs);
        if (JS_IsException(s->promise)) return JS_STEP_ABRUPT;
        s->w.stage = CN_SETTLE;
        if (!d) {
            JS_ThrowTypeError(ctx, rd ? "this reader has been released"
                                      : "cancel was called on neither a ReadableStream nor an active reader");
            s->w.value = JS_GetException(ctx);
            reject = 1;
        } else if (!rd && !JS_IsUndefined(d->reader)) {
            /* §4.2: cancelling a LOCKED stream through the stream itself is a TypeError — the reader owns it. */
            JS_ThrowTypeError(ctx, "this stream is locked to a reader");
            s->w.value = JS_GetException(ctx);
            reject = 1;
        } else {
            d->disturbed = 1;
            s->stream = JS_DupValue(ctx, rd ? rd->stream : s->hdr.this_val);
            if (d->state == RS_ERRORED) {
                s->w.value = JS_DupValue(ctx, d->stored_error);
                reject = 1;
            } else if (d->state == RS_CLOSED) {
                s->w.value = JS_UNDEFINED;   /* §4.2 step 2: already closed, and the source is not asked */
            } else {
                s->w.stage = CN_CLOSE;
                s->w.settle = S_CLOSE_SET;
            }
        }
        if (s->w.stage == CN_SETTLE) {
            s->settle = s->funcs[reject];
            s->funcs[reject] = JS_UNDEFINED;
        }
    }

    if (s->w.stage == CN_CLOSE) {
        /* §4.2 step 4: the stream CLOSES before the source is asked to cancel, so a reader awaiting `closed`
           or parked on a `read()` is answered first. */
        r = stream_settle_run(ctx, &s->w, stream_of(s->stream), cb_result, out_cb, out_argc);
        if (r > 0) return r;
        if (r < 0) return JS_STEP_ABRUPT;
        cb_result = JS_UNDEFINED;
        s->w.stage = CN_CALL;
    }

    if (s->w.stage == CN_CALL) {
        d = stream_of(s->stream);
        DCHECK(d != NULL, "the cancel machine's stream stopped being a ReadableStream");
        c = JS_IsUndefined(d->controller) ? NULL : ctrl_of(d->controller);
        if (s->w.phase == 0) {
            /* §4.5's CancelSteps: the queue is dropped first, so a source's `cancel` sees a stream with
               nothing left to give. */
            if (stream_queue_reset(ctx, d) < 0) return JS_STEP_ABRUPT;
            JS_FreeValue(ctx, s->w.value);
            s->w.value = JS_UNDEFINED;
        }
        if (c && !JS_IsUndefined(c->cancel_fn)) {
            arg = s->hdr.argc > 0 ? s->hdr.argv[0] : JS_UNDEFINED;
            r = step_call_run(ctx, &s->w.phase, s->w.cb, c->cancel_fn, c->source, 1, &arg, cb_result, &out,
                              out_cb, out_argc);
            if (r > 0) return r;
            /* §4.9.4 builds the cancel algorithm with PromiseCall too: a throwing `cancel` becomes a REJECTED
               PROMISE, which this member's own promise then adopts — one path, not a special case. */
            JS_FreeValue(ctx, s->w.value);
            if (JS_IsException(out)) {
                s->w.value = JS_GetException(ctx);
                s->reject_algorithm = 1;
            } else {
                s->w.value = out;
            }
            cb_result = JS_UNDEFINED;
        }
        if (c) {
            /* §4.5's "clear algorithms": a cancelled controller pulls no more and cancels no more. */
            JS_FreeValue(ctx, c->pull_fn);
            JS_FreeValue(ctx, c->cancel_fn);
            c->pull_fn = c->cancel_fn = JS_UNDEFINED;
        }
        s->w.stage = CN_RESOLVE;
    }

    if (s->w.stage == CN_RESOLVE) {
        r = stream_promise_of_run(ctx, &s->w, s->reject_algorithm, cb_result, out_cb, out_argc);
        if (r > 0) return r;
        if (r < 0) return JS_STEP_ABRUPT;
        cb_result = JS_UNDEFINED;
        s->w.stage = CN_THEN;
    }

    if (s->w.stage == CN_THEN) {
        JS_FreeValue(ctx, cb_result);
        /* §4.2 step 7: this member's promise settles WITH the source's — fulfilling with undefined, because
           whatever the source's cancel resolved to is not this operation's answer, and rejecting with its
           reason. */
        r = ctrl_forward(ctx, s->w.func, s->funcs[0], s->funcs[1]);
        JS_FreeValue(ctx, s->w.func);
        s->w.func = JS_UNDEFINED;
        JS_FreeValue(ctx, s->funcs[0]);
        JS_FreeValue(ctx, s->funcs[1]);
        s->funcs[0] = s->funcs[1] = JS_UNDEFINED;
        return r < 0 ? JS_STEP_ABRUPT : JS_STEP_DONE;
    }

    DCHECK(s->w.stage == CN_SETTLE, "the cancel machine resumed in a stage it never parks in");
    arg = s->w.value;
    r = step_call_run(ctx, &s->w.phase, s->w.cb, s->settle, JS_UNDEFINED, 1, &arg, cb_result, &out,
                      out_cb, out_argc);
    if (r > 0) return r;
    if (JS_IsException(out)) return JS_STEP_ABRUPT;
    JS_FreeValue(ctx, out);
    return JS_STEP_DONE;
}

static const JSTrampStepDef js_cancel_def = {
    sizeof(JSCancelState), js_cancel_step, js_cancel_fini, 0, .catches_abrupt = 1, .visit = js_cancel_visit
};

/* ---- §4.5's DEFAULT CONTROLLER MEMBERS -----------------------------------------------------------------------
 *
 * `enqueue`, `close` and `error` are each MACHINES, because each may answer a PARKED READ — and answering one
 * settles a promise, which is the page's code. `enqueue` additionally ends in CallPullIfNeeded, which calls the
 * page's `pull`. That is the whole reason this component is machines rather than functions. */
enum { CTRL_ENQUEUE = 0, CTRL_CLOSE, CTRL_ERROR };
enum { CS_START = 0, CS_SIZE, CS_ENQUEUE, CS_SETTLE, CS_SETTLE_ALL, CS_PULL, CS_RETHROW };

typedef struct {
    JSStepHdr hdr;
    StreamWork  w;
    double    size;      /* what the strategy said this chunk weighs, held across its call */
    /* §4.5: a bad chunk size errors the stream AND "returns chunkSize" — the ORIGINAL abrupt, not the stream's
       stored error. A `size` that errors the controller itself and then throws must re-raise ITS throw while
       the stream keeps the reason it was errored with; re-raising the stored error conflates the two. */
    JSValue   rethrow;
} JSCtrlState;

static void js_ctrl_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSCtrlState *s = st;
    stream_work_visit(ctx, &s->w, v);
    v->val(ctx, &s->rethrow);
}

static JSValue js_ctrl_fini(JSContext *ctx, void *st, bool take_result)
{
    JSCtrlState *s = st;
    (void)take_result;
    stream_work_release(ctx, &s->w);
    JS_FreeValue(ctx, s->rethrow);
    s->rethrow = JS_UNDEFINED;
    return JS_UNDEFINED;
}

static int js_ctrl_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    JSCtrlState *s = st;
    ControllerData *c = ctrl_of(s->hdr.this_val);
    StreamData *d;
    JSValueConst arg;
    JSValue out;
    int r;

    if (!c) {
        JS_FreeValue(ctx, cb_result);
        JS_ThrowTypeError(ctx, "not a ReadableStreamDefaultController");
        return JS_STEP_ABRUPT;
    }
    d = stream_of(c->stream);
    DCHECK(d != NULL, "a controller's stream stopped being a ReadableStream");

    if (s->w.stage == CS_START) {
        int op = s->hdr.arg;
        JSValueConst a0 = s->hdr.argc > 0 ? s->hdr.argv[0] : JS_UNDEFINED;

        stream_work_start(&s->w);
        s->rethrow = JS_UNDEFINED;
        JS_FreeValue(ctx, cb_result);
        cb_result = JS_UNDEFINED;
        if (op == CTRL_ENQUEUE) {
            if (!ctrl_can_close_or_enqueue(c, d)) {
                JS_ThrowTypeError(ctx, "this stream can no longer be enqueued to");
                return JS_STEP_ABRUPT;
            }
            /* §4.5: a chunk goes to a WAITING READER if there is one, and to the queue otherwise. Queuing it
               when a read is parked would answer that read out of order, or never — and a chunk handed
               straight to a reader is never weighed, because it never enters the queue. */
            s->w.func = stream_take_read(ctx, d, 0);
            if (JS_IsUndefined(s->w.func)) {
                s->w.stage = JS_IsUndefined(c->size_fn) ? CS_ENQUEUE : CS_SIZE;
                s->size = 1;   /* the implicit strategy: one per chunk */
            } else {
                s->w.value = read_result(ctx, JS_DupValue(ctx, a0), false);
                if (JS_IsException(s->w.value)) return JS_STEP_ABRUPT;
                s->w.stage = CS_SETTLE;
            }
            s->w.pull = P_TEST;   /* §4.5 step 4: an enqueue always ends in CallPullIfNeeded */
        } else if (op == CTRL_CLOSE) {
            if (!ctrl_can_close_or_enqueue(c, d)) {
                JS_ThrowTypeError(ctx, "this stream can no longer be closed");
                return JS_STEP_ABRUPT;
            }
            c->close_requested = 1;
            /* §4.5: a close with chunks STILL QUEUED waits for them — the state moves when the queue drains,
               so a reader sees every chunk before `done`. */
            if (stream_queued(ctx, d) > 0) return JS_STEP_DONE;
            s->w.stage = CS_SETTLE_ALL;
            s->w.settle = S_CLOSE_SET;
        } else {
            DCHECK(op == CTRL_ERROR, "a controller member ran with an operation this component does not have");
            if (d->state != RS_READABLE) return JS_STEP_DONE;
            s->w.err = JS_DupValue(ctx, a0);
            s->w.stage = CS_SETTLE_ALL;
            s->w.settle = S_ERR_SET;
        }
    }

    if (s->w.stage == CS_SIZE) {
        JSValueConst chunk = s->hdr.argc > 0 ? s->hdr.argv[0] : JS_UNDEFINED;
        r = step_call_run(ctx, &s->w.phase, s->w.cb, c->size_fn, JS_UNDEFINED, 1, &chunk, cb_result, &out,
                          out_cb, out_argc);
        if (r > 0) return r;
        cb_result = JS_UNDEFINED;
        if (JS_IsException(out)) goto size_bad;
        s->size = 0;
        if (JS_ToFloat64(ctx, &s->size, out) < 0) { JS_FreeValue(ctx, out); goto size_bad; }
        JS_FreeValue(ctx, out);
        /* §4.5's EnqueueValueWithSize: a size that is not a FINITE NON-NEGATIVE number is a RangeError, and
           §4.5 answers both that and a throwing size the same way — error the stream, then re-raise, so the
           caller of enqueue() sees the failure AND every reader sees the stream die. */
        if (!isfinite(s->size) || s->size < 0) {
            JS_ThrowRangeError(ctx, "a queuing strategy's size must be a finite, non-negative number");
        size_bad:
            s->w.err = JS_GetException(ctx);
            s->rethrow = JS_DupValue(ctx, s->w.err);
            s->w.settle = S_ERR_SET;
            s->w.stage = CS_RETHROW;
            s->w.pull = P_IDLE;
        } else {
            s->w.stage = CS_ENQUEUE;
        }
    }
    if (s->w.stage == CS_ENQUEUE) {
        JSValueConst chunk = s->hdr.argc > 0 ? s->hdr.argv[0] : JS_UNDEFINED;
        JS_FreeValue(ctx, cb_result);
        cb_result = JS_UNDEFINED;
        stream_enqueue(ctx, d, chunk, s->size);
        s->w.stage = CS_PULL;
    }
    if (s->w.stage == CS_RETHROW) {
        /* the stream errors first — every parked read is rejected — and only then does enqueue() itself
           re-raise, which is the order §4.5 states and the order a page observes */
        r = stream_settle_run(ctx, &s->w, d, cb_result, out_cb, out_argc);
        if (r > 0) return r;
        JS_Throw(ctx, s->rethrow);
        s->rethrow = JS_UNDEFINED;
        return JS_STEP_ABRUPT;
    }

    if (s->w.stage == CS_SETTLE) {
        arg = s->w.value;
        r = step_call_run(ctx, &s->w.phase, s->w.cb, s->w.func, JS_UNDEFINED, 1, &arg, cb_result, &out,
                          out_cb, out_argc);
        if (r > 0) return r;
        if (JS_IsException(out)) return JS_STEP_ABRUPT;
        JS_FreeValue(ctx, out);
        cb_result = JS_UNDEFINED;
        s->w.stage = CS_PULL;
    }
    if (s->w.stage == CS_SETTLE_ALL) {
        r = stream_settle_run(ctx, &s->w, d, cb_result, out_cb, out_argc);
        if (r > 0) return r;
        if (r < 0) return JS_STEP_ABRUPT;
        return JS_STEP_DONE;
    }

    DCHECK(s->w.stage == CS_PULL, "a controller member resumed in a stage it never parks in");
    r = ctrl_pull_run(ctx, &s->w, s->hdr.this_val, cb_result, out_cb, out_argc);
    if (r > 0) return r;
    if (r < 0) return JS_STEP_ABRUPT;
    return JS_STEP_DONE;
}

#define CTRL_DEF(i) { sizeof(JSCtrlState), js_ctrl_step, js_ctrl_fini, (i), \
                      .catches_abrupt = 1, .visit = js_ctrl_visit }
static const JSTrampStepDef js_ctrl_defs[3] = {
    CTRL_DEF(CTRL_ENQUEUE), CTRL_DEF(CTRL_CLOSE), CTRL_DEF(CTRL_ERROR),
};
#undef CTRL_DEF

/* An interface with NO constructor still has an INTERFACE OBJECT — Web IDL puts one on the global for every
   [Exposed] interface, and it is where the prototype's `constructor` property comes from. A page reads that
   property list directly (`Object.getOwnPropertyNames(Object.getPrototypeOf(controller))`), so leaving the
   object out is not "one global missing", it is a prototype with the wrong shape. Calling it is a TypeError,
   which is what an interface object with no constructor operation does. */
static JSValue js_illegal_ctor(JSContext *ctx, JSValueConst nt, int argc, JSValueConst *argv)
{
    (void)nt; (void)argc; (void)argv;
    return JS_ThrowTypeError(ctx, "Illegal constructor");
}

/* §4.5's `desiredSize`. With the default strategy the high-water mark is 1, so it is 1 minus what is queued —
   a page uses it to decide whether to enqueue more, and answering a constant would make that decision wrong. */
static JSValue js_ctrl_desired(JSContext *ctx, JSValueConst this_val, int magic)
{
    ControllerData *c = ctrl_of(this_val);
    StreamData *d;
    (void)magic;
    if (!c) return JS_ThrowTypeError(ctx, "not a ReadableStreamDefaultController");
    d = stream_of(c->stream);
    DCHECK(d != NULL, "a controller's stream stopped being a ReadableStream");
    if (d->state == RS_ERRORED) return JS_NULL;
    if (d->state == RS_CLOSED) return JS_NewInt32(ctx, 0);
    return JS_NewFloat64(ctx, c->hwm - d->queue_total);
}

/* ---- §4.4's ASYNCHRONOUS ITERATION ---------------------------------------------------------------------------
 *
 * `for await (const chunk of stream)`. Two specs meet here: §4.4 states what one iteration DOES (read through
 * the reader; release it when the stream finishes; cancel it on an early return unless preventCancel), and Web
 * IDL §3.7.11 states the SHAPE around that — an `is finished` flag, and an ONGOING PROMISE that makes each
 * next() wait for the previous one. The chain is not decoration: `Promise.allSettled([it.next(), it.next(),
 * it.next()])` on a source that enqueues once and then errors must report fulfilled, rejected, fulfilled in
 * that order, which only holds if the second call waits for the first.
 *
 * IT REACHES THE OPERATIONS, NOT THE MEMBERS. §4.4 performs ReadableStreamDefaultReaderRead and
 * ...GenericCancel, which are abstract operations, and a page that replaces
 * ReadableStreamDefaultReader.prototype.read must not thereby change what `for await` does — the corpus asserts
 * exactly that. So this holds the FUNCTION OBJECTS this component installed and calls those: the same
 * algorithms, reached by a reference the page cannot rebind, with no property read in between.
 *
 * EVERY STEP IS A CALL OF THE PAGE'S CODE — settling the promise it hands back, reading through the reader —
 * so the whole of §4.4 is ONE machine with eight entry points, the way §4.5's controller is one machine with
 * three. Four are the members and the chained continuations; four are the reactions to what a read or a cancel
 * answered, each a step closure over what it needs. */
enum {
    AI_NEXT = 0,     /* the `next()` member */
    AI_RETURN,       /* the `return(value)` member */
    AI_RUN_NEXT,     /* the same steps, reached as a reaction to the ongoing promise */
    AI_RUN_RETURN,
    AI_READ_OK,      /* what a read answered */
    AI_READ_ERR,
    AI_CANCEL_OK,    /* what the source's cancel answered */
    AI_CANCEL_ERR,
    AI_N
};

typedef struct {
    JSValue stream;
    JSValue reader;        /* the default reader this iterator holds for its whole life */
    JSValue ongoing;       /* §3.7.11's ongoing promise, or JS_UNDEFINED before the first call */
    uint8_t prevent_cancel;
    uint8_t finished;      /* §3.7.11's `is finished` */
} AsyncIterData;

static JSClassID g_aiter_class;
static JSValue   g_aiter_proto = JS_UNDEFINED;
static int       g_aiter_stepids[AI_N];
/* THE ORIGINAL FUNCTION OBJECTS — see the note above. */
static JSValue   g_read_fn = JS_UNDEFINED, g_release_fn = JS_UNDEFINED, g_reader_cancel_fn = JS_UNDEFINED,
                 g_get_reader_fn = JS_UNDEFINED;

static void aiter_finalizer(JSRuntime *rt, JSValue val)
{
    AsyncIterData *a = JS_GetOpaque(val, g_aiter_class);
    if (!a) return;
    JS_FreeValueRT(rt, a->stream);
    JS_FreeValueRT(rt, a->reader);
    JS_FreeValueRT(rt, a->ongoing);
    js_free_rt(rt, a);
}

static void aiter_gc_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func)
{
    AsyncIterData *a = JS_GetOpaque(val, g_aiter_class);
    if (!a) return;
    JS_MarkValue(rt, a->stream, mark_func);
    JS_MarkValue(rt, a->reader, mark_func);
    JS_MarkValue(rt, a->ongoing, mark_func);
}

static AsyncIterData *aiter_of(JSValueConst v) { return JS_GetOpaque(v, g_aiter_class); }

enum { AIS_START = 0, AIS_STEPS, AIS_READ, AIS_CANCEL, AIS_RELEASE, AIS_SETTLE, AIS_DONE };

typedef struct {
    JSStepHdr hdr;
    uint8_t   stage;
    uint8_t   phase;
    uint8_t   reject;      /* which capability the settle stage calls */
    JSValue   iter;
    JSValue   promise;     /* what a member form hands back */
    JSValue   resolve, reject_fn;
    JSValue   value;       /* the member's argument, then the value to settle with */
    JSValue   cb[3];
} JSAiterState;

static void js_aiter_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSAiterState *s = st;
    int k;
    v->val(ctx, &s->iter);
    v->val(ctx, &s->promise);
    v->val(ctx, &s->resolve);
    v->val(ctx, &s->reject_fn);
    v->val(ctx, &s->value);
    for (k = 0; k < 3; k++) v->val(ctx, &s->cb[k]);
}

static JSValue js_aiter_fini(JSContext *ctx, void *st, bool take_result)
{
    JSAiterState *s = st;
    JSValue r = take_result ? s->promise : JS_UNDEFINED;
    int k;
    if (take_result) s->promise = JS_UNDEFINED;
    JS_FreeValue(ctx, s->iter);
    JS_FreeValue(ctx, s->promise);
    JS_FreeValue(ctx, s->resolve);
    JS_FreeValue(ctx, s->reject_fn);
    JS_FreeValue(ctx, s->value);
    s->iter = s->promise = s->resolve = s->reject_fn = s->value = JS_UNDEFINED;
    for (k = 0; k < 3; k++) { JS_FreeValue(ctx, s->cb[k]); s->cb[k] = JS_UNDEFINED; }
    return r;
}

/* §4.4's read request steps and its return steps both RELEASE the reader, and a release is a machine — so it
   is reached the way every other operation here is, through the function object this component installed. */
static int aiter_call_run(JSContext *ctx, JSAiterState *s, JSValueConst fn, JSValueConst this_val,
                          int argc, JSValueConst *argv, JSValue in, JSValue *pout,
                          JSValue **out_cb, int *out_argc)
{
    return step_call_run(ctx, &s->phase, s->cb, fn, this_val, argc, argv, in, pout, out_cb, out_argc);
}

static int js_aiter_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    JSAiterState *s = st;
    int op = s->hdr.arg;
    AsyncIterData *a;
    JSValueConst arg;
    JSValue out;
    int r;

    if (s->stage == AIS_START) {
        JSValue funcs[2];

        s->iter = s->promise = s->resolve = s->reject_fn = s->value = JS_UNDEFINED;
        { int k; for (k = 0; k < 3; k++) s->cb[k] = JS_UNDEFINED; }
        JS_FreeValue(ctx, cb_result);
        cb_result = JS_UNDEFINED;

        if (op == AI_NEXT || op == AI_RETURN) {
            /* A MEMBER: the receiver is the iterator and this call owns the promise it hands back. */
            s->iter = JS_DupValue(ctx, s->hdr.this_val);
            a = aiter_of(s->iter);
            if (!a) {
                JS_ThrowTypeError(ctx, "not a ReadableStream async iterator");
                return JS_STEP_ABRUPT;
            }
            s->value = JS_DupValue(ctx, s->hdr.argc > 0 ? s->hdr.argv[0] : JS_UNDEFINED);
            s->promise = JS_NewPromiseCapability(ctx, funcs);
            if (JS_IsException(s->promise)) return JS_STEP_ABRUPT;
            s->resolve = funcs[0];
            s->reject_fn = funcs[1];
            if (!JS_IsUndefined(a->ongoing)) {
                /* §3.7.11: this call's steps run only once the previous call has settled — either way, which
                   is why one closure is attached as BOTH handlers. */
                JSValueConst data[4];
                int id;
                data[0] = s->iter; data[1] = s->resolve; data[2] = s->reject_fn; data[3] = s->value;
                id = g_aiter_stepids[op == AI_NEXT ? AI_RUN_NEXT : AI_RUN_RETURN];
                r = stream_react(ctx, a->ongoing, id, id, data, 4);
                JS_FreeValue(ctx, a->ongoing);
                a->ongoing = JS_DupValue(ctx, s->promise);
                return r < 0 ? JS_STEP_ABRUPT : JS_STEP_DONE;
            }
            JS_FreeValue(ctx, a->ongoing);
            a->ongoing = JS_DupValue(ctx, s->promise);
        } else if (op == AI_RUN_NEXT || op == AI_RUN_RETURN) {
            s->iter      = JS_DupValue(ctx, JS_StepClosureData(&s->hdr, 0));
            s->resolve   = JS_DupValue(ctx, JS_StepClosureData(&s->hdr, 1));
            s->reject_fn = JS_DupValue(ctx, JS_StepClosureData(&s->hdr, 2));
            s->value     = JS_DupValue(ctx, JS_StepClosureData(&s->hdr, 3));
        } else if (op == AI_READ_OK || op == AI_READ_ERR) {
            s->iter      = JS_DupValue(ctx, JS_StepClosureData(&s->hdr, 0));
            s->resolve   = JS_DupValue(ctx, JS_StepClosureData(&s->hdr, 1));
            s->reject_fn = JS_DupValue(ctx, JS_StepClosureData(&s->hdr, 2));
            s->value     = JS_DupValue(ctx, s->hdr.argc > 0 ? s->hdr.argv[0] : JS_UNDEFINED);
        } else {
            DCHECK(op == AI_CANCEL_OK || op == AI_CANCEL_ERR,
                   "a §4.4 machine ran with an operation this component does not have");
            s->iter      = JS_DupValue(ctx, JS_StepClosureData(&s->hdr, 0));
            s->resolve   = JS_DupValue(ctx, JS_StepClosureData(&s->hdr, 1));
            s->reject_fn = JS_DupValue(ctx, JS_StepClosureData(&s->hdr, 2));
            s->value     = op == AI_CANCEL_ERR ? JS_DupValue(ctx, s->hdr.argc > 0 ? s->hdr.argv[0]
                                                                                  : JS_UNDEFINED)
                                               : JS_DupValue(ctx, JS_StepClosureData(&s->hdr, 3));
        }
        s->stage = AIS_STEPS;
    }

    a = aiter_of(s->iter);
    DCHECK(a != NULL, "a §4.4 machine captured something that is not a ReadableStream async iterator");

    if (s->stage == AIS_STEPS) {
        switch (op) {
        case AI_NEXT: case AI_RUN_NEXT:
            if (a->finished) {
                JS_FreeValue(ctx, s->value);
                s->value = read_result(ctx, JS_UNDEFINED, true);
                if (JS_IsException(s->value)) return JS_STEP_ABRUPT;
                s->stage = AIS_SETTLE;
            } else {
                s->stage = AIS_READ;
            }
            break;
        case AI_RETURN: case AI_RUN_RETURN:
            if (a->finished) {
                JSValue v = s->value;
                s->value = read_result(ctx, v, true);   /* §3.7.11 wraps the argument, whatever it was */
                if (JS_IsException(s->value)) return JS_STEP_ABRUPT;
                s->stage = AIS_SETTLE;
            } else {
                a->finished = 1;
                s->stage = a->prevent_cancel ? AIS_RELEASE : AIS_CANCEL;
            }
            break;
        case AI_READ_OK: {
            /* the result object is one this component built, so reading `done` off it runs nothing */
            JSValue done_v = JS_GetPropertyStr(ctx, s->value, "done");
            int done = JS_ToBool(ctx, done_v);
            JS_FreeValue(ctx, done_v);
            if (!done) { s->stage = AIS_SETTLE; break; }
            a->finished = 1;
            s->stage = AIS_RELEASE;   /* §4.4's close steps release the reader before answering */
            break;
        }
        default:
            DCHECK(op == AI_READ_ERR || op == AI_CANCEL_OK || op == AI_CANCEL_ERR,
                   "a §4.4 machine reached its steps with an operation that has none");
            if (op == AI_READ_ERR) {
                a->finished = 1;
                s->reject = 1;
                s->stage = AIS_RELEASE;   /* §4.4's error steps release the reader before rejecting */
            } else {
                JSValue v = s->value;
                s->reject = op == AI_CANCEL_ERR;
                s->value = s->reject ? v : read_result(ctx, v, true);
                if (JS_IsException(s->value)) return JS_STEP_ABRUPT;
                s->stage = AIS_SETTLE;
            }
            break;
        }
    }

    if (s->stage == AIS_READ) {
        JSValueConst data[3];
        r = aiter_call_run(ctx, s, g_read_fn, a->reader, 0, NULL, cb_result, &out, out_cb, out_argc);
        if (r > 0) return r;
        if (JS_IsException(out)) return JS_STEP_ABRUPT;
        r = stream_react(ctx, out, g_aiter_stepids[AI_READ_OK], g_aiter_stepids[AI_READ_ERR],
                         (data[0] = s->iter, data[1] = s->resolve, data[2] = s->reject_fn, data), 3);
        JS_FreeValue(ctx, out);
        return r < 0 ? JS_STEP_ABRUPT : JS_STEP_DONE;
    }

    if (s->stage == AIS_CANCEL) {
        /* §4.4's return steps: cancel FIRST, then release, and the promise this member handed back settles
           with the SOURCE's cancel promise rather than ahead of it. */
        JSValueConst data[4];
        arg = s->value;
        r = aiter_call_run(ctx, s, g_reader_cancel_fn, a->reader, 1, &arg, cb_result, &out, out_cb, out_argc);
        if (r > 0) return r;
        if (JS_IsException(out)) return JS_STEP_ABRUPT;
        r = stream_react(ctx, out, g_aiter_stepids[AI_CANCEL_OK], g_aiter_stepids[AI_CANCEL_ERR],
                         (data[0] = s->iter, data[1] = s->resolve, data[2] = s->reject_fn,
                          data[3] = s->value, data), 4);
        JS_FreeValue(ctx, out);
        if (r < 0) return JS_STEP_ABRUPT;
        /* the release still has to happen, and it happens now — the reactions above answer the page later */
        cb_result = JS_UNDEFINED;
        s->stage = AIS_RELEASE;
    }

    if (s->stage == AIS_RELEASE) {
        int settle_after = op != AI_RETURN && op != AI_RUN_RETURN;
        r = aiter_call_run(ctx, s, g_release_fn, a->reader, 0, NULL, cb_result, &out, out_cb, out_argc);
        if (r > 0) return r;
        if (JS_IsException(out)) return JS_STEP_ABRUPT;
        JS_FreeValue(ctx, out);
        cb_result = JS_UNDEFINED;
        if (!settle_after) {
            /* a cancelling return has already attached its reactions; a preventCancel return answers now */
            if (!a->prevent_cancel) return JS_STEP_DONE;
            {
                JSValue v = s->value;
                s->value = read_result(ctx, v, true);
                if (JS_IsException(s->value)) return JS_STEP_ABRUPT;
            }
        } else if (!s->reject) {
            /* AI_READ_OK on a done result: the result object is already { undefined, true } */
        }
        s->stage = AIS_SETTLE;
    }

    DCHECK(s->stage == AIS_SETTLE, "a §4.4 machine resumed in a stage it never parks in");
    arg = s->value;
    r = aiter_call_run(ctx, s, s->reject ? s->reject_fn : s->resolve, JS_UNDEFINED, 1, &arg,
                       cb_result, &out, out_cb, out_argc);
    if (r > 0) return r;
    if (JS_IsException(out)) return JS_STEP_ABRUPT;
    JS_FreeValue(ctx, out);
    return JS_STEP_DONE;
}

#define AITER_DEF(i) { sizeof(JSAiterState), js_aiter_step, js_aiter_fini, (i), \
                       .catches_abrupt = 1, .visit = js_aiter_visit }
static const JSTrampStepDef js_aiter_defs[AI_N] = {
    AITER_DEF(AI_NEXT), AITER_DEF(AI_RETURN), AITER_DEF(AI_RUN_NEXT), AITER_DEF(AI_RUN_RETURN),
    AITER_DEF(AI_READ_OK), AITER_DEF(AI_READ_ERR), AITER_DEF(AI_CANCEL_OK), AITER_DEF(AI_CANCEL_ERR),
};
#undef AITER_DEF

/* §4.4's `values(options)`, which is also `[Symbol.asyncIterator]`. A MACHINE because it ACQUIRES A READER, and
   §4.3's acquisition settles `closed` at once on a stream that has already finished. */
static const IdlDictMember ITERATOR_OPTIONS[] = {
    { "preventCancel", IDL_BOOLEAN },
};

typedef struct {
    uint8_t stage;
    uint8_t phase;
    uint8_t prevent_cancel;
    JSValue cb[3];
} JSValuesState;

static void js_values_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSValuesState *s = st;
    int k;
    for (k = 0; k < 3; k++) v->val(ctx, &s->cb[k]);
}

static void js_values_release(JSContext *ctx, void *st)
{
    JSValuesState *s = st;
    int k;
    for (k = 0; k < 3; k++) { JS_FreeValue(ctx, s->cb[k]); s->cb[k] = JS_UNDEFINED; }
}

static int js_values_step(JSContext *ctx, JSStepHdr *hdr, void *st, int argc, JSValueConst *argv,
                          JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    JSValuesState *s = st;
    AsyncIterData *a;
    JSValue reader, obj;
    int r;

    if (s->stage == 0) {
        int k;
        for (k = 0; k < 3; k++) s->cb[k] = JS_UNDEFINED;
        s->prevent_cancel = argc > 0 && idl_dict_bool(ctx, argv[0], "preventCancel");
        s->stage = 1;
    }
    r = step_call_run(ctx, &s->phase, s->cb, g_get_reader_fn, hdr->this_val, 0, NULL,
                      cb_result, &reader, out_cb, out_argc);
    if (r > 0) return r;
    if (JS_IsException(reader)) return -1;

    obj = JS_NewObjectProtoClass(ctx, g_aiter_proto, g_aiter_class);
    if (JS_IsException(obj)) { JS_FreeValue(ctx, reader); return -1; }
    a = js_mallocz(ctx, sizeof *a);
    if (!a) { JS_FreeValue(ctx, obj); JS_FreeValue(ctx, reader); return -1; }
    a->ongoing = JS_UNDEFINED;
    a->stream = JS_DupValue(ctx, hdr->this_val);
    a->reader = reader;
    a->prevent_cancel = s->prevent_cancel;
    JS_SetOpaque(obj, a);
    *presult = obj;
    return 0;
}

static const IdlStepDecl js_values_decl = {
    js_values_step, sizeof(JSValuesState), js_values_visit, js_values_release
};

/* ---- §4.2's TEE -----------------------------------------------------------------------------------------------
 *
 * ReadableStreamDefaultTee: one stream read once, its chunks handed to TWO. The two branches are ordinary
 * ReadableStreams whose pull and cancel algorithms are the HOST'S rather than a page's — which needs nothing new
 * from §4.5, because a controller calls whatever function it holds and a step closure is a function.
 *
 * THE SHARED STATE IS THE POINT. `reading`, `readAgain`, `canceled1/2` and one cancelPromise are read and
 * written by five algorithms that run at different times, so they live in one GC-traced record both branches
 * hold — which is also what makes the record a cycle (branch -> controller -> closure -> record -> branch) and
 * why it has a gc_mark like every other one here.
 *
 * IT USES NO GLOBAL. §4.2 builds the branches with CreateReadableStream, not with the `ReadableStream`
 * constructor, and the corpus asserts that a page which replaces the global sees no difference. Building them
 * here from the same two allocations the constructor uses is what makes that true rather than lucky. */
enum {
    TEE_PULL = 0,     /* both branches' pull algorithm */
    TEE_CANCEL1,      /* branch 1's cancel algorithm */
    TEE_CANCEL2,
    TEE_READ_OK,      /* what a read through the shared reader answered */
    TEE_READ_ERR,
    TEE_CLOSED_ERR,   /* the shared reader's `closed` promise rejecting */
    TEE_N
};

typedef struct {
    JSValue reader;
    JSValue branch[2];
    JSValue ctrl[2];
    JSValue cancel_promise;
    JSValue cancel_funcs[2];
    JSValue reason[2];
    uint8_t reading;
    uint8_t read_again;
    uint8_t canceled[2];
} TeeData;

static JSClassID g_tee_class;
static int       g_tee_stepids[TEE_N];
/* The controller members, as function objects — the same reason §4.4 holds the reader's. */
static JSValue   g_ctrl_fn[3] = { JS_UNDEFINED, JS_UNDEFINED, JS_UNDEFINED };

static void tee_finalizer(JSRuntime *rt, JSValue val)
{
    TeeData *t = JS_GetOpaque(val, g_tee_class);
    int i;
    if (!t) return;
    JS_FreeValueRT(rt, t->reader);
    JS_FreeValueRT(rt, t->cancel_promise);
    for (i = 0; i < 2; i++) {
        JS_FreeValueRT(rt, t->branch[i]);
        JS_FreeValueRT(rt, t->ctrl[i]);
        JS_FreeValueRT(rt, t->cancel_funcs[i]);
        JS_FreeValueRT(rt, t->reason[i]);
    }
    js_free_rt(rt, t);
}

static void tee_gc_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func)
{
    TeeData *t = JS_GetOpaque(val, g_tee_class);
    int i;
    if (!t) return;
    JS_MarkValue(rt, t->reader, mark_func);
    JS_MarkValue(rt, t->cancel_promise, mark_func);
    for (i = 0; i < 2; i++) {
        JS_MarkValue(rt, t->branch[i], mark_func);
        JS_MarkValue(rt, t->ctrl[i], mark_func);
        JS_MarkValue(rt, t->cancel_funcs[i], mark_func);
        JS_MarkValue(rt, t->reason[i], mark_func);
    }
}

static TeeData *tee_of(JSValueConst v) { return JS_GetOpaque(v, g_tee_class); }

enum { TS_START = 0, TS_READ, TS_B0, TS_B1, TS_RESOLVE_CANCEL, TS_CANCEL_SOURCE, TS_CANCEL_ADOPT };
enum { CF_ENQUEUE = 0, CF_CLOSE, CF_ERROR };

typedef struct {
    JSStepHdr hdr;
    uint8_t   stage;
    uint8_t   phase;
    uint8_t   done;      /* what the read answered */
    JSValue   tee;
    JSValue   value;     /* the chunk, or the reason a cancel carries */
    JSValue   result;    /* what this invocation answers */
    JSValue   cb[3];
} JSTeeState;

static void js_tee_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSTeeState *s = st;
    int k;
    v->val(ctx, &s->tee);
    v->val(ctx, &s->value);
    v->val(ctx, &s->result);
    for (k = 0; k < 3; k++) v->val(ctx, &s->cb[k]);
}

static JSValue js_tee_fini(JSContext *ctx, void *st, bool take_result)
{
    JSTeeState *s = st;
    JSValue r = take_result ? s->result : JS_UNDEFINED;
    int k;
    if (take_result) s->result = JS_UNDEFINED;
    JS_FreeValue(ctx, s->tee);
    JS_FreeValue(ctx, s->value);
    JS_FreeValue(ctx, s->result);
    s->tee = s->value = s->result = JS_UNDEFINED;
    for (k = 0; k < 3; k++) { JS_FreeValue(ctx, s->cb[k]); s->cb[k] = JS_UNDEFINED; }
    return r;
}

/* One branch's controller told to take, close or error. A CALL of the member's function object, so the
   controller's own machine runs — the enqueue weighs the chunk, answers a parked read and pulls again, none of
   which a direct write to the queue would do. */
static int tee_ctrl_run(JSContext *ctx, JSTeeState *s, TeeData *t, int i, int which, JSValueConst arg,
                        JSValue in, JSValue **out_cb, int *out_argc)
{
    JSValue out;
    int r, argc = which == CF_CLOSE ? 0 : 1;

    r = step_call_run(ctx, &s->phase, s->cb, g_ctrl_fn[which], t->ctrl[i], argc, &arg, in, &out,
                      out_cb, out_argc);
    if (r > 0) return r;
    if (JS_IsException(out)) {
        /* §4.2's tee ignores what a branch's own enqueue/close refuses: a branch that a page has already
           closed or errored is not this algorithm's problem, and the other branch must still be fed. */
        JS_FreeValue(ctx, JS_GetException(ctx));
        return 0;
    }
    JS_FreeValue(ctx, out);
    return 0;
}

static int js_tee_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    JSTeeState *s = st;
    int op = s->hdr.arg;
    TeeData *t;
    JSValue out;
    int r;

    if (s->stage == TS_START) {
        int k;
        s->tee = JS_DupValue(ctx, JS_StepClosureData(&s->hdr, 0));
        s->value = s->result = JS_UNDEFINED;
        for (k = 0; k < 3; k++) s->cb[k] = JS_UNDEFINED;
        JS_FreeValue(ctx, cb_result);
        cb_result = JS_UNDEFINED;
        t = tee_of(s->tee);
        DCHECK(t != NULL, "a tee machine captured something that is not a tee record");

        switch (op) {
        case TEE_PULL:
            if (t->reading) {
                /* §4.2: a pull asked for while a read is in flight is REMEMBERED, not dropped — the chunk
                   steps run it when they finish. */
                t->read_again = 1;
                return JS_STEP_DONE;
            }
            t->reading = 1;
            s->stage = TS_READ;
            break;
        case TEE_CANCEL1: case TEE_CANCEL2: {
            int i = op == TEE_CANCEL1 ? 0 : 1;
            t->canceled[i] = 1;
            JS_FreeValue(ctx, t->reason[i]);
            t->reason[i] = JS_DupValue(ctx, s->hdr.argc > 0 ? s->hdr.argv[0] : JS_UNDEFINED);
            s->result = JS_DupValue(ctx, t->cancel_promise);
            if (!t->canceled[i ^ 1]) return JS_STEP_DONE;   /* the other branch is still reading */
            /* BOTH branches have cancelled, so the SOURCE is cancelled — with both reasons, as an array, in
               branch order however the two calls were ordered in time. */
            s->value = JS_NewArray(ctx);
            if (JS_IsException(s->value)) return JS_STEP_ABRUPT;
            JS_SetPropertyUint32(ctx, s->value, 0, JS_DupValue(ctx, t->reason[0]));
            JS_SetPropertyUint32(ctx, s->value, 1, JS_DupValue(ctx, t->reason[1]));
            s->stage = TS_CANCEL_SOURCE;
            break;
        }
        case TEE_READ_OK: {
            JSValue done_v;
            s->value = JS_DupValue(ctx, s->hdr.argc > 0 ? s->hdr.argv[0] : JS_UNDEFINED);
            done_v = JS_GetPropertyStr(ctx, s->value, "done");
            s->done = (uint8_t)JS_ToBool(ctx, done_v);
            JS_FreeValue(ctx, done_v);
            if (!s->done) {
                JSValue chunk = JS_GetPropertyStr(ctx, s->value, "value");
                JS_FreeValue(ctx, s->value);
                s->value = chunk;
                t->read_again = 0;
            }
            s->stage = TS_B0;
            break;
        }
        default:
            DCHECK(op == TEE_READ_ERR || op == TEE_CLOSED_ERR,
                   "a tee machine ran with an operation this component does not have");
            if (op == TEE_READ_ERR) {
                /* §4.2's error steps do exactly one thing: the `closed` reaction below owns the erroring. */
                t->reading = 0;
                return JS_STEP_DONE;
            }
            s->value = JS_DupValue(ctx, s->hdr.argc > 0 ? s->hdr.argv[0] : JS_UNDEFINED);
            s->done = 2;   /* the closed-rejection arm: both branches are ERRORED with the reason */
            s->stage = TS_B0;
            break;
        }
    }

    t = tee_of(s->tee);
    DCHECK(t != NULL, "a tee machine's record stopped being one");

again:
    if (s->stage == TS_READ) {
        JSValueConst data[1];
        r = step_call_run(ctx, &s->phase, s->cb, g_read_fn, t->reader, 0, NULL, cb_result, &out,
                          out_cb, out_argc);
        if (r > 0) return r;
        if (JS_IsException(out)) return JS_STEP_ABRUPT;
        data[0] = s->tee;
        r = stream_react(ctx, out, g_tee_stepids[TEE_READ_OK], g_tee_stepids[TEE_READ_ERR], data, 1);
        JS_FreeValue(ctx, out);
        return r < 0 ? JS_STEP_ABRUPT : JS_STEP_DONE;
    }

    if (s->stage == TS_B0 || s->stage == TS_B1) {
        int i = s->stage == TS_B1;
        for (; i < 2; i++) {
            s->stage = i ? TS_B1 : TS_B0;
            /* §4.2: a branch the page has already cancelled is not fed, closed or errored. */
            if (s->done != 2 && t->canceled[i]) continue;
            r = tee_ctrl_run(ctx, s, t, i,
                             s->done == 2 ? CF_ERROR : s->done ? CF_CLOSE : CF_ENQUEUE,
                             s->value, cb_result, out_cb, out_argc);
            if (r > 0) return r;
            if (r < 0) return JS_STEP_ABRUPT;
            cb_result = JS_UNDEFINED;
        }
        if (s->done == 0) {
            t->reading = 0;
            if (t->read_again) {
                /* the pull that arrived while this read was in flight, run now — as a fresh READ rather than
                   as a recursive call, so the machine's own stages stay flat */
                t->read_again = 0;
                t->reading = 1;
                s->stage = TS_READ;
                cb_result = JS_UNDEFINED;
                goto again;   /* a LOOP, never a call: this machine may not recurse into itself */
            }
            return JS_STEP_DONE;
        }
        if (s->done == 1) t->reading = 0;
        /* CLOSE and ERROR both settle the tee's own cancel promise, so a branch cancelled afterwards is not
           left waiting on a source that is already finished. */
        s->stage = TS_RESOLVE_CANCEL;
    }

    if (s->stage == TS_RESOLVE_CANCEL) {
        JSValueConst undef = JS_UNDEFINED;
        if (t->canceled[0] && t->canceled[1]) { JS_FreeValue(ctx, cb_result); return JS_STEP_DONE; }
        r = step_call_run(ctx, &s->phase, s->cb, t->cancel_funcs[0], JS_UNDEFINED, 1, &undef, cb_result, &out,
                          out_cb, out_argc);
        if (r > 0) return r;
        if (JS_IsException(out)) return JS_STEP_ABRUPT;
        JS_FreeValue(ctx, out);
        return JS_STEP_DONE;
    }

    if (s->stage == TS_CANCEL_SOURCE) {
        JSValueConst arg = s->value;
        r = step_call_run(ctx, &s->phase, s->cb, g_reader_cancel_fn, t->reader, 1, &arg, cb_result, &out,
                          out_cb, out_argc);
        if (r > 0) return r;
        if (JS_IsException(out)) return JS_STEP_ABRUPT;
        JS_FreeValue(ctx, s->value);
        s->value = out;
        cb_result = JS_UNDEFINED;
        /* TWO CALLS, TWO STAGES. One `phase` byte serves one call at a time: writing the two in a row under a
           single byte made the resume re-enter the FIRST call with the SECOND's phase, which issued the second
           call again forever — a livelock the corpus found as a killed process, not as a failure. */
        s->stage = TS_CANCEL_ADOPT;
    }

    DCHECK(s->stage == TS_CANCEL_ADOPT, "a tee machine resumed in a stage it never parks in");
    {
        /* §4.2 resolves cancelPromise WITH the source's cancel result, so the branches' cancel promises adopt
           it rather than settling ahead of it. */
        JSValueConst arg = s->value;
        r = step_call_run(ctx, &s->phase, s->cb, t->cancel_funcs[0], JS_UNDEFINED, 1, &arg, cb_result, &out,
                          out_cb, out_argc);
        if (r > 0) return r;
        if (JS_IsException(out)) return JS_STEP_ABRUPT;
        JS_FreeValue(ctx, out);
        return JS_STEP_DONE;
    }
}

#define TEE_DEF(i) { sizeof(JSTeeState), js_tee_step, js_tee_fini, (i), \
                     .catches_abrupt = 1, .visit = js_tee_visit }
static const JSTrampStepDef js_tee_defs[TEE_N] = {
    TEE_DEF(TEE_PULL), TEE_DEF(TEE_CANCEL1), TEE_DEF(TEE_CANCEL2),
    TEE_DEF(TEE_READ_OK), TEE_DEF(TEE_READ_ERR), TEE_DEF(TEE_CLOSED_ERR),
};
#undef TEE_DEF

/* §4.2's `tee()`. A MACHINE because it acquires a reader, and §4.3's acquisition settles `closed` at once on a
   stream that has already finished. */
typedef struct {
    StreamWork w;
    JSValue  tee;
    JSValue  reader;
} JSTeeCallState;

static void js_tee_call_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSTeeCallState *s = st;
    stream_work_visit(ctx, &s->w, v);
    v->val(ctx, &s->tee);
    v->val(ctx, &s->reader);
}

static void js_tee_call_release(JSContext *ctx, void *st)
{
    JSTeeCallState *s = st;
    stream_work_release(ctx, &s->w);
    JS_FreeValue(ctx, s->tee);
    JS_FreeValue(ctx, s->reader);
    s->tee = s->reader = JS_UNDEFINED;
}

/* A BRANCH: a stream and a controller whose pull and cancel are the tee's, not a page's. */
static int tee_branch(JSContext *ctx, TeeData *t, JSValueConst tee_v, int i)
{
    JSValueConst data[1];
    ControllerData *c;
    StreamData *d;

    t->branch[i] = readable_stream_empty(ctx);
    if (JS_IsException(t->branch[i])) return -1;
    d = stream_of(t->branch[i]);
    t->ctrl[i] = JS_DupValue(ctx, d->controller);
    c = ctrl_of(t->ctrl[i]);
    data[0] = tee_v;
    c->pull_fn = JS_NewStepClosure(ctx, g_tee_stepids[TEE_PULL], 0, 1, data);
    if (JS_IsException(c->pull_fn)) { c->pull_fn = JS_UNDEFINED; return -1; }
    c->cancel_fn = JS_NewStepClosure(ctx, g_tee_stepids[i ? TEE_CANCEL2 : TEE_CANCEL1], 1, 1, data);
    if (JS_IsException(c->cancel_fn)) { c->cancel_fn = JS_UNDEFINED; return -1; }
    return 0;
}

static int js_tee_call_step(JSContext *ctx, JSStepHdr *hdr, void *st, int argc, JSValueConst *argv,
                            JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    JSTeeCallState *s = st;
    TeeData *t;
    JSValue obj, arr;
    int r, i;

    (void)argc; (void)argv;
    if (s->w.stage == 0) {
        stream_work_start(&s->w);
        s->tee = s->reader = JS_UNDEFINED;
        s->w.stage = 1;
        if (!stream_of(hdr->this_val)) {
            JS_FreeValue(ctx, cb_result);
            JS_ThrowTypeError(ctx, "not a ReadableStream");
            return -1;
        }
    }
    if (s->w.stage == 1) {
        r = step_call_run(ctx, &s->w.phase, s->w.cb, g_get_reader_fn, hdr->this_val, 0, NULL,
                          cb_result, &s->reader, out_cb, out_argc);
        if (r > 0) return r;
        if (JS_IsException(s->reader)) return -1;
        cb_result = JS_UNDEFINED;

        obj = JS_NewObjectClass(ctx, g_tee_class);
        if (JS_IsException(obj)) return -1;
        t = js_mallocz(ctx, sizeof *t);
        if (!t) { JS_FreeValue(ctx, obj); return -1; }
        t->reader = t->cancel_promise = JS_UNDEFINED;
        for (i = 0; i < 2; i++)
            t->branch[i] = t->ctrl[i] = t->cancel_funcs[i] = t->reason[i] = JS_UNDEFINED;
        JS_SetOpaque(obj, t);
        s->tee = obj;                      /* attached before anything that can fail, as everywhere here */
        t->reader = JS_DupValue(ctx, s->reader);
        t->cancel_promise = JS_NewPromiseCapability(ctx, t->cancel_funcs);
        if (JS_IsException(t->cancel_promise)) return -1;
        for (i = 0; i < 2; i++)
            if (tee_branch(ctx, t, s->tee, i) < 0) return -1;
        /* §4.2 step 18: the source's `closed` REJECTING is what errors both branches. */
        {
            JSValueConst data[1];
            ReaderData *rd = reader_of(s->reader);
            JSValue onr, cap;
            DCHECK(rd != NULL, "tee acquired something that is not a ReadableStreamDefaultReader");
            data[0] = s->tee;
            onr = JS_NewStepClosure(ctx, g_tee_stepids[TEE_CLOSED_ERR], 1, 1, data);
            if (JS_IsException(onr)) return -1;
            cap = JS_PerformPromiseThen(ctx, rd->closed, JS_UNDEFINED, onr);
            JS_FreeValue(ctx, onr);
            if (JS_IsException(cap)) return -1;
            JS_FreeValue(ctx, cap);
        }
        /* §4.5's set-up: neither branch is STARTED until a microtask has run, so a pull cannot happen inside
           tee() itself. One resolved promise serves both, which is the same tick each would have had. */
        s->w.value = JS_UNDEFINED;
        s->w.stage = 2;
    }
    if (s->w.stage == 2) {
        r = stream_promise_of_run(ctx, &s->w, 0, cb_result, out_cb, out_argc);
        if (r > 0) return r;
        if (r < 0) return -1;
        cb_result = JS_UNDEFINED;
        s->w.stage = 3;
    }
    JS_FreeValue(ctx, cb_result);
    t = tee_of(s->tee);
    DCHECK(t != NULL, "the tee call's record stopped being one");
    for (i = 0; i < 2; i++)
        if (ctrl_react(ctx, s->w.func, t->ctrl[i], RXN_START_OK, RXN_START_ERR) < 0) return -1;
    arr = JS_NewArray(ctx);
    if (JS_IsException(arr)) return -1;
    JS_SetPropertyUint32(ctx, arr, 0, JS_DupValue(ctx, t->branch[0]));
    JS_SetPropertyUint32(ctx, arr, 1, JS_DupValue(ctx, t->branch[1]));
    *presult = arr;
    return 0;
}

static const IdlStepDecl js_tee_call_decl = {
    js_tee_call_step, sizeof(JSTeeCallState), js_tee_call_visit, js_tee_call_release
};

/* ---- §4.2's `from` --------------------------------------------------------------------------------------------
 *
 * ReadableStreamFromIterable: a stream whose source is any ASYNC OR SYNC ITERABLE — an array, a Set, a
 * generator, another ReadableStream. Its pull algorithm is one `next()` and its cancel algorithm is one
 * `return()`, both of them the page's code, so both are step closures over a record holding the iterator.
 *
 * GetIterator(obj, ASYNC) IS PERFORMED, NOT APPROXIMATED. Its @@asyncIterator read, its fallback @@iterator
 * read and its method call are requests like any other; its one remaining step — CreateAsyncFromSyncIterator —
 * is an ECMAScript intrinsic whose `next` AWAITS the sync result's value, which is what makes
 * `ReadableStream.from([Promise.resolve(1)])` yield 1 rather than the promise. That one is the engine's, newly
 * exported, rather than written again here.
 *
 * THE HIGH-WATER MARK IS 0. §4.2 says so, and it is the difference between pulling one value ahead of the
 * reader and pulling only when asked — which `from(array)` with a push during reading observes directly. */
enum { FROM_PULL = 0, FROM_CANCEL, FROM_NEXT_OK, FROM_RET_OK, FROM_N };

typedef struct {
    JSValue iterator;
    JSValue next_fn;
    JSValue controller;
} FromData;

static JSClassID g_from_class;
static int       g_from_stepids[FROM_N];

static void from_finalizer(JSRuntime *rt, JSValue val)
{
    FromData *f = JS_GetOpaque(val, g_from_class);
    if (!f) return;
    JS_FreeValueRT(rt, f->iterator);
    JS_FreeValueRT(rt, f->next_fn);
    JS_FreeValueRT(rt, f->controller);
    js_free_rt(rt, f);
}

static void from_gc_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func)
{
    FromData *f = JS_GetOpaque(val, g_from_class);
    if (!f) return;
    JS_MarkValue(rt, f->iterator, mark_func);
    JS_MarkValue(rt, f->next_fn, mark_func);
    JS_MarkValue(rt, f->controller, mark_func);
}

enum { FS_START = 0, FS_CALL, FS_RESOLVE, FS_REJECT, FS_REACT, FS_READ_DONE, FS_READ_VALUE, FS_FEED };

typedef struct {
    JSStepHdr hdr;
    uint8_t   stage;
    uint8_t   phase;
    uint8_t   done;
    JSValue   from;
    JSValue   value;    /* the iteration result, then the value taken out of it */
    JSValue   result;   /* what this invocation answers */
    JSValue   chain;    /* a capability's resolving function, while §4.2's PromiseResolve is in flight */
    JSValue   cb[3];
} JSFromState;

static void js_from_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSFromState *s = st;
    int k;
    v->val(ctx, &s->from);
    v->val(ctx, &s->value);
    v->val(ctx, &s->result);
    v->val(ctx, &s->chain);
    for (k = 0; k < 3; k++) v->val(ctx, &s->cb[k]);
}

static JSValue js_from_fini(JSContext *ctx, void *st, bool take_result)
{
    JSFromState *s = st;
    JSValue r = take_result ? s->result : JS_UNDEFINED;
    int k;
    if (take_result) s->result = JS_UNDEFINED;
    JS_FreeValue(ctx, s->from);
    JS_FreeValue(ctx, s->value);
    JS_FreeValue(ctx, s->result);
    JS_FreeValue(ctx, s->chain);
    s->from = s->value = s->result = s->chain = JS_UNDEFINED;
    for (k = 0; k < 3; k++) { JS_FreeValue(ctx, s->cb[k]); s->cb[k] = JS_UNDEFINED; }
    return r;
}

static int js_from_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    JSFromState *s = st;
    int op = s->hdr.arg;
    FromData *f;
    JSValue out;
    JSAtom atom;
    int r;

    if (s->stage == FS_START) {
        int k;
        s->from = JS_DupValue(ctx, JS_StepClosureData(&s->hdr, 0));
        s->value = s->result = s->chain = JS_UNDEFINED;
        for (k = 0; k < 3; k++) s->cb[k] = JS_UNDEFINED;
        JS_FreeValue(ctx, cb_result);
        cb_result = JS_UNDEFINED;
        if (op == FROM_NEXT_OK || op == FROM_RET_OK) {
            /* §4.2: an iteration result that is not an OBJECT is a TypeError — for `next` it errors the
               stream through the pull rejection, for `return` it rejects the cancel promise. */
            s->value = JS_DupValue(ctx, s->hdr.argc > 0 ? s->hdr.argv[0] : JS_UNDEFINED);
            if (!JS_IsObject(s->value)) {
                JS_ThrowTypeError(ctx, "an iterator answered with something that is not an object");
                return JS_STEP_ABRUPT;
            }
            if (op == FROM_RET_OK) return JS_STEP_DONE;   /* the value is discarded: §4.2 answers undefined */
            s->stage = FS_READ_DONE;
        } else {
            s->value = JS_DupValue(ctx, s->hdr.argc > 0 ? s->hdr.argv[0] : JS_UNDEFINED);
            s->stage = FS_CALL;
        }
    }

    f = JS_GetOpaque(s->from, g_from_class);
    DCHECK(f != NULL, "a §4.2 `from` machine captured something that is not its record");

    if (s->stage == FS_CALL) {
        if (op == FROM_CANCEL) {
            /* §4.2's cancel algorithm: GetMethod(iterator, "return"), and an iterator that has none is simply
               finished — the promise resolves with undefined and nothing is called. */
            if (s->phase == 0) {
                JSValue m;
                atom = JS_NewAtom(ctx, "return");
                r = step_getprop_run(ctx, &s->hdr, f->iterator, atom, cb_result, &m, out_cb, out_argc);
                JS_FreeAtom(ctx, atom);
                if (r > 0) return r;
                if (r < 0) return JS_STEP_ABRUPT;
                cb_result = JS_UNDEFINED;
                if (JS_IsUndefined(m) || JS_IsNull(m)) { JS_FreeValue(ctx, m); return JS_STEP_DONE; }
                if (!JS_IsFunction(ctx, m)) {
                    /* 7.3.10 GetMethod throws for a non-callable, and §4.2 says an ABRUPT GetMethod here
                       "returns a promise rejected with" it — so the cancel promise rejects rather than the
                       member throwing at its caller. */
                    JS_FreeValue(ctx, m);
                    JS_ThrowTypeError(ctx, "an iterator's `return` is not callable");
                    JS_FreeValue(ctx, s->value);
                    s->value = JS_GetException(ctx);
                    s->stage = FS_REJECT;
                    goto settle_promise;
                }
                JS_FreeValue(ctx, s->result);
                s->result = m;   /* held here only until the call is issued */
            }
            {
                JSValueConst arg = s->value;
                r = step_call_run(ctx, &s->phase, s->cb, s->result, f->iterator, 1, &arg, cb_result, &out,
                                  out_cb, out_argc);
            }
        } else {
            DCHECK(op == FROM_PULL, "a §4.2 `from` machine ran with an operation it does not have");
            r = step_call_run(ctx, &s->phase, s->cb, f->next_fn, f->iterator, 0, NULL, cb_result, &out,
                              out_cb, out_argc);
        }
        if (r > 0) return r;
        if (JS_IsException(out)) {
            /* §4.2, in as many words: "If nextResult is an abrupt completion, return a promise REJECTED with
               nextResult.[[Value]]" — the algorithm answers a promise either way and never propagates, which
               is what makes a synchronously-throwing `next` error the stream through the same pull rejection
               a rejecting one takes. */
            JS_FreeValue(ctx, s->value);
            s->value = JS_GetException(ctx);
            s->stage = FS_REJECT;
        } else {
            JS_FreeValue(ctx, s->value);
            s->value = out;
            s->stage = FS_RESOLVE;
        }
        cb_result = JS_UNDEFINED;
    }

settle_promise:
    if (s->stage == FS_RESOLVE || s->stage == FS_REJECT) {
        /* §4.2 reacts to PromiseResolve(nextResult) — so what `next` answered goes through a capability
           FIRST, whatever it is. A sync iterator's plain `{value, done}` is not a promise, and reacting to it
           directly reads reaction lists off an object that has none. */
        JSValueConst arg = s->value;
        if (s->phase == 0) {
            JSValue funcs[2];
            JSValue p = JS_NewPromiseCapability(ctx, funcs);
            if (JS_IsException(p)) return JS_STEP_ABRUPT;
            JS_FreeValue(ctx, s->result);
            s->result = p;
            JS_FreeValue(ctx, s->chain);
            s->chain = funcs[s->stage == FS_REJECT];
            JS_FreeValue(ctx, funcs[s->stage == FS_REJECT ? 0 : 1]);
        }
        r = step_call_run(ctx, &s->phase, s->cb, s->chain, JS_UNDEFINED, 1, &arg, cb_result, &out,
                          out_cb, out_argc);
        if (r > 0) return r;
        if (JS_IsException(out)) return JS_STEP_ABRUPT;
        JS_FreeValue(ctx, out);
        JS_FreeValue(ctx, s->chain);
        s->chain = JS_UNDEFINED;
        cb_result = JS_UNDEFINED;
        s->stage = FS_REACT;
    }

    if (s->stage == FS_REACT) {
        /* The reaction's OWN capability is what the algorithm answers, so a later pull chains on THIS rather
           than on what `next` happened to return. A rejected one carries the rejection straight through: with
           no handler on that side it reaches §4.5's pull reaction, which errors the stream. */
        JSValueConst data[1];
        JSValue cap;
        data[0] = s->from;
        cap = stream_react_cap(ctx, s->result, g_from_stepids[op == FROM_PULL ? FROM_NEXT_OK : FROM_RET_OK],
                               -1, data, 1);
        if (JS_IsException(cap)) return JS_STEP_ABRUPT;
        JS_FreeValue(ctx, s->result);
        s->result = cap;
        return JS_STEP_DONE;
    }

    if (s->stage == FS_READ_DONE) {
        atom = JS_NewAtom(ctx, "done");
        r = step_getprop_run(ctx, &s->hdr, s->value, atom, cb_result, &out, out_cb, out_argc);
        JS_FreeAtom(ctx, atom);
        if (r > 0) return r;
        if (r < 0) return JS_STEP_ABRUPT;
        cb_result = JS_UNDEFINED;
        s->done = (uint8_t)JS_ToBool(ctx, out);
        JS_FreeValue(ctx, out);
        s->stage = s->done ? FS_FEED : FS_READ_VALUE;
    }
    if (s->stage == FS_READ_VALUE) {
        JSValue v;
        atom = JS_NewAtom(ctx, "value");
        r = step_getprop_run(ctx, &s->hdr, s->value, atom, cb_result, &v, out_cb, out_argc);
        JS_FreeAtom(ctx, atom);
        if (r > 0) return r;
        if (r < 0) return JS_STEP_ABRUPT;
        cb_result = JS_UNDEFINED;
        JS_FreeValue(ctx, s->value);
        s->value = v;
        s->stage = FS_FEED;
    }

    DCHECK(s->stage == FS_FEED, "a §4.2 `from` machine resumed in a stage it never parks in");
    {
        JSValueConst arg = s->value;
        r = step_call_run(ctx, &s->phase, s->cb, g_ctrl_fn[s->done ? CF_CLOSE : CF_ENQUEUE], f->controller,
                          s->done ? 0 : 1, &arg, cb_result, &out, out_cb, out_argc);
        if (r > 0) return r;
        if (JS_IsException(out)) return JS_STEP_ABRUPT;
        JS_FreeValue(ctx, out);
    }
    return JS_STEP_DONE;
}

#define FROM_DEF(i) { sizeof(JSFromState), js_from_step, js_from_fini, (i), \
                      .catches_abrupt = 1, .visit = js_from_visit }
static const JSTrampStepDef js_from_defs[FROM_N] = {
    FROM_DEF(FROM_PULL), FROM_DEF(FROM_CANCEL), FROM_DEF(FROM_NEXT_OK), FROM_DEF(FROM_RET_OK),
};
#undef FROM_DEF

/* `static ReadableStream from(any asyncIterable)`. A MACHINE for GetIterator(obj, ASYNC): three of its four
   steps are the page's code. */
enum { FC_START = 0, FC_ASYNC_CALL, FC_SYNC_GET, FC_SYNC_CALL, FC_NEXT, FC_BUILD, FC_STARTED };

typedef struct {
    StreamWork w;
    uint8_t  is_sync;
    JSValue  method;
    JSValue  iterator;
    JSValue  next_fn;
    JSValue  stream;
    JSValue  from;
} JSFromCallState;

static void js_from_call_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSFromCallState *s = st;
    stream_work_visit(ctx, &s->w, v);
    v->val(ctx, &s->method);
    v->val(ctx, &s->iterator);
    v->val(ctx, &s->next_fn);
    v->val(ctx, &s->stream);
    v->val(ctx, &s->from);
}

static void js_from_call_release(JSContext *ctx, void *st)
{
    JSFromCallState *s = st;
    stream_work_release(ctx, &s->w);
    JS_FreeValue(ctx, s->method);
    JS_FreeValue(ctx, s->iterator);
    JS_FreeValue(ctx, s->next_fn);
    JS_FreeValue(ctx, s->stream);
    JS_FreeValue(ctx, s->from);
    s->method = s->iterator = s->next_fn = s->stream = s->from = JS_UNDEFINED;
}

static int js_from_call_step(JSContext *ctx, JSStepHdr *hdr, void *st, int argc, JSValueConst *argv,
                             JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    JSFromCallState *s = st;
    JSValueConst src = argc > 0 ? argv[0] : JS_UNDEFINED;
    JSValue out;
    int r;

    if (s->w.stage == FC_START) {
        stream_work_start(&s->w);
        s->method = s->iterator = s->next_fn = s->stream = s->from = JS_UNDEFINED;
        /* AN OBJECT, not merely something iterable. A STRING has @@iterator and would give a stream of its
           characters, and the corpus lists it among the INVALID iterables beside null, a number and a symbol —
           so `from` requires an Object before GetIterator is reached at all. Nothing else in that list would
           get past GetIterator anyway; the string is the one case that says where the check is. */
        if (!JS_IsObject(src)) {
            JS_FreeValue(ctx, cb_result);
            JS_ThrowTypeError(ctx, "ReadableStream.from was given something that is not an object");
            return -1;
        }
        /* 7.4.2 GetIterator with the ASYNC hint: @@asyncIterator first, and only a NULLISH one falls back. */
        r = step_getprop_run(ctx, hdr, src, JS_WellKnownSymbolAtom(JS_WKS_ASYNC_ITERATOR), cb_result,
                             &s->method, out_cb, out_argc);
        if (r > 0) { s->w.stage = FC_START; return r; }
        if (r < 0) return -1;
        cb_result = JS_UNDEFINED;
        s->is_sync = JS_IsUndefined(s->method) || JS_IsNull(s->method);
        s->w.stage = s->is_sync ? FC_SYNC_GET : FC_ASYNC_CALL;
    }
    if (s->w.stage == FC_SYNC_GET) {
        JS_FreeValue(ctx, s->method);
        s->method = JS_UNDEFINED;
        r = step_getprop_run(ctx, hdr, src, JS_WellKnownSymbolAtom(JS_WKS_ITERATOR), cb_result,
                             &s->method, out_cb, out_argc);
        if (r > 0) return r;
        if (r < 0) return -1;
        cb_result = JS_UNDEFINED;
        if (JS_IsUndefined(s->method) || JS_IsNull(s->method)) {
            JS_ThrowTypeError(ctx, "ReadableStream.from was given something that is not iterable");
            return -1;
        }
        s->w.stage = FC_SYNC_CALL;
    }
    if (s->w.stage == FC_ASYNC_CALL || s->w.stage == FC_SYNC_CALL) {
        if (!JS_IsFunction(ctx, s->method)) {
            JS_FreeValue(ctx, cb_result);
            JS_ThrowTypeError(ctx, "an iterable's iterator method is not callable");
            return -1;
        }
        r = step_call_run(ctx, &s->w.phase, s->w.cb, s->method, src, 0, NULL, cb_result, &s->iterator,
                          out_cb, out_argc);
        if (r > 0) return r;
        if (JS_IsException(s->iterator)) return -1;
        cb_result = JS_UNDEFINED;
        if (!JS_IsObject(s->iterator)) {
            JS_ThrowTypeError(ctx, "an iterator method answered with something that is not an object");
            return -1;
        }
        s->w.stage = FC_NEXT;
    }
    if (s->w.stage == FC_NEXT) {
        JSAtom atom = JS_NewAtom(ctx, "next");
        r = step_getprop_run(ctx, hdr, s->iterator, atom, cb_result, &s->next_fn, out_cb, out_argc);
        JS_FreeAtom(ctx, atom);
        if (r > 0) return r;
        if (r < 0) return -1;
        cb_result = JS_UNDEFINED;
        if (s->is_sync) {
            /* 27.1.4.1: the sync record is WRAPPED, so `next` awaits each value — the engine's intrinsic,
               because writing that unwrap here would be a second copy of one. */
            JSValue nextw;
            JSValue wrapped = JS_NewAsyncFromSyncIterator(ctx, s->iterator, s->next_fn, &nextw);
            s->iterator = s->next_fn = JS_UNDEFINED;   /* both were consumed */
            if (JS_IsException(wrapped)) return -1;
            s->iterator = wrapped;
            s->next_fn = nextw;
        }
        s->w.stage = FC_BUILD;
    }
    if (s->w.stage == FC_BUILD) {
        JSValueConst data[1];
        ControllerData *c;
        StreamData *d;
        FromData *f;
        JSValue obj;

        JS_FreeValue(ctx, cb_result);
        cb_result = JS_UNDEFINED;
        obj = JS_NewObjectClass(ctx, g_from_class);
        if (JS_IsException(obj)) return -1;
        f = js_mallocz(ctx, sizeof *f);
        if (!f) { JS_FreeValue(ctx, obj); return -1; }
        f->iterator = f->next_fn = f->controller = JS_UNDEFINED;
        JS_SetOpaque(obj, f);
        s->from = obj;                     /* attached before anything that can fail */
        f->iterator = JS_DupValue(ctx, s->iterator);
        f->next_fn = JS_DupValue(ctx, s->next_fn);

        s->stream = readable_stream_empty(ctx);
        if (JS_IsException(s->stream)) return -1;
        d = stream_of(s->stream);
        c = ctrl_of(d->controller);
        f->controller = JS_DupValue(ctx, d->controller);
        c->hwm = 0;                        /* §4.2: `from` pulls only when asked */
        data[0] = s->from;
        c->pull_fn = JS_NewStepClosure(ctx, g_from_stepids[FROM_PULL], 0, 1, data);
        if (JS_IsException(c->pull_fn)) { c->pull_fn = JS_UNDEFINED; return -1; }
        c->cancel_fn = JS_NewStepClosure(ctx, g_from_stepids[FROM_CANCEL], 1, 1, data);
        if (JS_IsException(c->cancel_fn)) { c->cancel_fn = JS_UNDEFINED; return -1; }
        s->w.value = JS_UNDEFINED;
        s->w.stage = FC_STARTED;
    }
    if (s->w.stage == FC_STARTED) {
        r = stream_promise_of_run(ctx, &s->w, 0, cb_result, out_cb, out_argc);
        if (r > 0) return r;
        if (r < 0) return -1;
        cb_result = JS_UNDEFINED;
    }
    JS_FreeValue(ctx, cb_result);
    if (ctrl_react(ctx, s->w.func, stream_of(s->stream)->controller, RXN_START_OK, RXN_START_ERR) < 0)
        return -1;
    *presult = s->stream;
    s->stream = JS_UNDEFINED;
    return 0;
}

static const IdlStepDecl js_from_call_decl = {
    js_from_call_step, sizeof(JSFromCallState), js_from_call_visit, js_from_call_release
};

/* ---- FETCH §5.2's "FULLY READ" ---------------------------------------------------------------------------
 *
 * Draining a stream to a byte sequence. Every read answers a PROMISE, so the loop is a chain of reactions
 * rather than a loop — which is why this lives here, beside the machinery that already does that, rather than
 * in the byte reader that wants it. The caller acquires the reader and issues the first read (both are calls,
 * and a caller reaching this is already a machine); this owns the buffer and the reactions.
 *
 * WHAT `make` IS. The value the page finally gets — a string, a parsed JSON, an ArrayBuffer, a Blob — is the
 * CALLING spec's, not this one's, and it is one C function over the finished bytes. Carrying it here is what
 * lets a drain answer `response.json()`'s promise directly rather than answering bytes that something else
 * then has to react to a second time. */
enum { DRAIN_OK = 0, DRAIN_ERR, DRAIN_N };

typedef struct {
    JSValue reader;
    JSValue recv;          /* the object whose reader this is; `make` may need it (blob() reads its type) */
    JSValue funcs[2];      /* the capability this drain settles */
    uint8_t *buf;
    size_t   len, cap;
    JSValue (*make)(JSContext *ctx, JSValueConst recv, const char *bytes, size_t len);
} DrainData;

static JSClassID g_drain_class;
static int       g_drain_stepids[DRAIN_N];

static void drain_finalizer(JSRuntime *rt, JSValue val)
{
    DrainData *dr = JS_GetOpaque(val, g_drain_class);
    if (!dr) return;
    JS_FreeValueRT(rt, dr->reader);
    JS_FreeValueRT(rt, dr->recv);
    JS_FreeValueRT(rt, dr->funcs[0]);
    JS_FreeValueRT(rt, dr->funcs[1]);
    free(dr->buf);
    js_free_rt(rt, dr);
}

static void drain_gc_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func)
{
    DrainData *dr = JS_GetOpaque(val, g_drain_class);
    if (!dr) return;
    JS_MarkValue(rt, dr->reader, mark_func);
    JS_MarkValue(rt, dr->recv, mark_func);
    JS_MarkValue(rt, dr->funcs[0], mark_func);
    JS_MarkValue(rt, dr->funcs[1], mark_func);
}

/* Append a chunk's bytes. §5.2 says a chunk that is not a Uint8Array is a TypeError, and the union of things
   that ARE a byte view is what JS_GetArrayBufferView answers for. Returns -1 with a throw live. */
static int drain_append(JSContext *ctx, DrainData *dr, JSValueConst chunk)
{
    size_t off = 0, n = 0, whole = 0;
    JSValue buf;
    uint8_t *base;

    buf = JS_GetArrayBufferView(ctx, chunk, &off, &n);
    if (JS_IsException(buf)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        JS_ThrowTypeError(ctx, "a body stream answered with a chunk that is not a byte view");
        return -1;
    }
    base = JS_GetArrayBuffer(ctx, &whole, buf);
    if (!base) { JS_FreeValue(ctx, buf); return -1; }
    if (dr->len + n > dr->cap) {
        size_t want = dr->cap ? dr->cap * 2 : 256;
        uint8_t *grown;
        while (want < dr->len + n) want *= 2;
        grown = realloc(dr->buf, want);
        CHECK(grown != NULL, "fully read: OOM growing a body's byte sequence");
        dr->buf = grown;
        dr->cap = want;
    }
    memcpy(dr->buf + dr->len, base + off, n);
    dr->len += n;
    JS_FreeValue(ctx, buf);
    return 0;
}

enum { DS_START = 0, DS_READ, DS_RELEASE, DS_SETTLE };

typedef struct {
    JSStepHdr hdr;
    uint8_t   stage;
    uint8_t   phase;
    uint8_t   reject;
    JSValue   drain;
    JSValue   value;    /* the chunk, then the value the capability is settled with */
    JSValue   cb[3];
} JSDrainState;

static void js_drain_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSDrainState *s = st;
    int k;
    v->val(ctx, &s->drain);
    v->val(ctx, &s->value);
    for (k = 0; k < 3; k++) v->val(ctx, &s->cb[k]);
}

static JSValue js_drain_fini(JSContext *ctx, void *st, bool take_result)
{
    JSDrainState *s = st;
    int k;
    (void)take_result;
    JS_FreeValue(ctx, s->drain);
    JS_FreeValue(ctx, s->value);
    s->drain = s->value = JS_UNDEFINED;
    for (k = 0; k < 3; k++) { JS_FreeValue(ctx, s->cb[k]); s->cb[k] = JS_UNDEFINED; }
    return JS_UNDEFINED;
}

static int drain_react(JSContext *ctx, JSValueConst promise, JSValueConst drain)
{
    JSValueConst data[1];
    data[0] = drain;
    return stream_react(ctx, promise, g_drain_stepids[DRAIN_OK], g_drain_stepids[DRAIN_ERR], data, 1);
}

static int js_drain_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    JSDrainState *s = st;
    DrainData *dr;
    JSValue out;
    int r;

    if (s->stage == DS_START) {
        int k;
        s->drain = JS_DupValue(ctx, JS_StepClosureData(&s->hdr, 0));
        s->value = JS_UNDEFINED;
        for (k = 0; k < 3; k++) s->cb[k] = JS_UNDEFINED;
        JS_FreeValue(ctx, cb_result);
        cb_result = JS_UNDEFINED;
        dr = JS_GetOpaque(s->drain, g_drain_class);
        DCHECK(dr != NULL, "a drain reaction captured something that is not a drain record");
        if (s->hdr.arg == DRAIN_ERR) {
            /* The stream errored. §5.2 rejects the body's promise with the stream's reason, and the reader is
               released with it — there is nothing left to read. */
            s->reject = 1;
            s->value = JS_DupValue(ctx, s->hdr.argc > 0 ? s->hdr.argv[0] : JS_UNDEFINED);
            s->stage = DS_RELEASE;
        } else {
            JSValue done_v = JS_GetPropertyStr(ctx, s->hdr.argc > 0 ? s->hdr.argv[0] : JS_UNDEFINED, "done");
            int done = JS_ToBool(ctx, done_v);
            JS_FreeValue(ctx, done_v);
            if (done) {
                s->stage = DS_RELEASE;
            } else {
                JSValue chunk = JS_GetPropertyStr(ctx, s->hdr.argv[0], "value");
                int bad = drain_append(ctx, dr, chunk) < 0;
                JS_FreeValue(ctx, chunk);
                if (bad) {
                    s->reject = 1;
                    s->value = JS_GetException(ctx);
                    s->stage = DS_RELEASE;
                } else {
                    s->stage = DS_READ;
                }
            }
        }
    }

    dr = JS_GetOpaque(s->drain, g_drain_class);
    DCHECK(dr != NULL, "a drain machine's record stopped being one");

    if (s->stage == DS_READ) {
        r = step_call_run(ctx, &s->phase, s->cb, g_read_fn, dr->reader, 0, NULL, cb_result, &out,
                          out_cb, out_argc);
        if (r > 0) return r;
        if (JS_IsException(out)) return JS_STEP_ABRUPT;
        r = drain_react(ctx, out, s->drain);
        JS_FreeValue(ctx, out);
        return r < 0 ? JS_STEP_ABRUPT : JS_STEP_DONE;
    }

    if (s->stage == DS_RELEASE) {
        r = step_call_run(ctx, &s->phase, s->cb, g_release_fn, dr->reader, 0, NULL, cb_result, &out,
                          out_cb, out_argc);
        if (r > 0) return r;
        if (JS_IsException(out)) {
            /* a release cannot fail on a reader this component acquired and still holds */
            JS_FreeValue(ctx, JS_GetException(ctx));
        } else {
            JS_FreeValue(ctx, out);
        }
        cb_result = JS_UNDEFINED;
        if (!s->reject) {
            s->value = dr->make(ctx, dr->recv, dr->buf ? (const char *)dr->buf : "", dr->len);
            if (JS_IsException(s->value)) {
                /* §5.2: an abrupt completion INSIDE the read is what the promise rejects with — which is how
                   `json()`'s SyntaxError reaches the page's `.catch` rather than the call site. */
                s->value = JS_GetException(ctx);
                s->reject = 1;
            }
        }
        s->stage = DS_SETTLE;
    }

    DCHECK(s->stage == DS_SETTLE, "a drain machine resumed in a stage it never parks in");
    {
        JSValueConst arg = s->value;
        r = step_call_run(ctx, &s->phase, s->cb, dr->funcs[s->reject], JS_UNDEFINED, 1, &arg, cb_result, &out,
                          out_cb, out_argc);
        if (r > 0) return r;
        if (JS_IsException(out)) return JS_STEP_ABRUPT;
        JS_FreeValue(ctx, out);
    }
    return JS_STEP_DONE;
}

#define DRAIN_DEF(i) { sizeof(JSDrainState), js_drain_step, js_drain_fini, (i), \
                       .catches_abrupt = 1, .visit = js_drain_visit }
static const JSTrampStepDef js_drain_defs[DRAIN_N] = { DRAIN_DEF(DRAIN_OK), DRAIN_DEF(DRAIN_ERR) };
#undef DRAIN_DEF

JSValue readable_stream_drain(JSContext *ctx, JSValueConst reader, JSValueConst read_promise,
                              JSValueConst recv,
                              JSValue (*make)(JSContext *ctx, JSValueConst recv, const char *bytes,
                                              size_t len))
{
    DrainData *dr;
    JSValue obj, promise;

    DCHECK(make != NULL, "a drain was started with nothing to build from the bytes");
    obj = JS_NewObjectClass(ctx, g_drain_class);
    if (JS_IsException(obj)) return obj;
    dr = js_mallocz(ctx, sizeof *dr);
    if (!dr) { JS_FreeValue(ctx, obj); return JS_EXCEPTION; }
    dr->reader = dr->recv = dr->funcs[0] = dr->funcs[1] = JS_UNDEFINED;
    JS_SetOpaque(obj, dr);                  /* attached before anything that can fail */
    dr->reader = JS_DupValue(ctx, reader);
    dr->recv = JS_DupValue(ctx, recv);
    dr->make = make;
    promise = JS_NewPromiseCapability(ctx, dr->funcs);
    if (JS_IsException(promise)) { JS_FreeValue(ctx, obj); return promise; }
    if (drain_react(ctx, read_promise, obj) < 0) {
        JS_FreeValue(ctx, obj);
        JS_FreeValue(ctx, promise);
        return JS_EXCEPTION;
    }
    JS_FreeValue(ctx, obj);
    return promise;
}

JSValueConst readable_stream_op(ReadableStreamOp which)
{
    switch (which) {
    case RS_OP_GET_READER: return g_get_reader_fn;
    case RS_OP_READ:       return g_read_fn;
    default:
        DCHECK(which == RS_OP_RELEASE, "a stream operation was asked for by a name this component does not map");
        return g_release_fn;
    }
}

/* ---- the constructors -------------------------------------------------------------------------------------- */

/* §4.2's constructor, over §4.9.4's SetUpReadableStreamDefaultControllerFromUnderlyingSource. A MACHINE because
 * `start(controller)` is the page's code, reached as a call request — and so is every READ of the underlying
 * source, because each of its members is one accessor or Proxy trap away from being the page's code.
 *
 * THE READS ARE THE DICTIONARY CONVERSION, and Web IDL performs one by reading the members in LEXICOGRAPHIC
 * order. §4.2 does it IN THE BODY rather than at the IDL layer — step 2, after the strategy argument has
 * already crossed — which is a distinction a page pins directly (a throwing `get start` must be seen AFTER a
 * throwing `get size`), so it is a sequence of requests here rather than a declared IDL_DICT argument.
 *
 * THE ORDER IS THE SPEC'S. `start` runs synchronously; its result then goes through PromiseResolve, and only
 * when THAT fulfils is the controller [[started]] and the source pulled. So `pull` is never called from inside
 * the constructor even for a source whose `start` returns nothing — which is exactly what a page observes, and
 * what makes a thenable `start` no different from any other: it is the same PromiseResolve.
 *
 * A `start` that THROWS propagates out of the constructor, which is why this machine does not declare
 * catches_abrupt: §4.9.4 invokes it directly rather than through PromiseCall, unlike `pull`. */
enum { RSC_START = 0, RSC_PROTO, RSC_READ, RSC_TYPE, RSC_BUILD, RSC_CALL, RSC_RESOLVE, RSC_THEN };
/* UnderlyingSource's members, in the order Web IDL reads them. */
enum { SRC_CHUNKSIZE = 0, SRC_CANCEL, SRC_PULL, SRC_START, SRC_TYPE, SRC_N };

typedef struct {
    StreamWork w;
    uint8_t  member;         /* which UnderlyingSource member the read loop is on */
    JSValue  src[SRC_N];     /* what each read answered, before any of them is type-checked */
    JSValue  stream;
    JSValue  controller;
    JSValue  start_fn;
    JSValue  source;      /* the receiver `start` is invoked on */
    JSValue  proto;       /* new.target's `prototype`, so a SUBCLASS gets its own */
} JSRsCtorState;

static void js_rs_ctor_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSRsCtorState *s = st;
    int k;
    stream_work_visit(ctx, &s->w, v);
    for (k = 0; k < SRC_N; k++) v->val(ctx, &s->src[k]);
    v->val(ctx, &s->stream);
    v->val(ctx, &s->controller);
    v->val(ctx, &s->start_fn);
    v->val(ctx, &s->source);
    v->val(ctx, &s->proto);
}

static void js_rs_ctor_release(JSContext *ctx, void *st)
{
    JSRsCtorState *s = st;
    int k;
    stream_work_release(ctx, &s->w);
    for (k = 0; k < SRC_N; k++) { JS_FreeValue(ctx, s->src[k]); s->src[k] = JS_UNDEFINED; }
    JS_FreeValue(ctx, s->stream);
    JS_FreeValue(ctx, s->controller);
    JS_FreeValue(ctx, s->start_fn);
    JS_FreeValue(ctx, s->source);
    JS_FreeValue(ctx, s->proto);
    s->stream = s->controller = s->start_fn = s->source = s->proto = JS_UNDEFINED;
}

/* A `T? callback` member of UnderlyingSource: absent, or something the page can call. Anything else is the
   TypeError Web IDL's callback-function type states, and taking it would leave a stream that silently never
   starts. `*pv` is left owned by the state either way. */
static int ctor_callback_member(JSContext *ctx, JSValueConst v, const char *name)
{
    if (JS_IsUndefined(v) || JS_IsFunction(ctx, v)) return 0;
    JS_ThrowTypeError(ctx, "underlying source member `%s` is not callable", name);
    return -1;
}

static int js_rs_ctor_step(JSContext *ctx, JSStepHdr *hdr, void *st, int argc, JSValueConst *argv,
                           JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    static const char *const SRC_NAMES[SRC_N] = {
        "autoAllocateChunkSize", "cancel", "pull", "start", "type"
    };
    JSRsCtorState *s = st;
    JSValueConst source = argc > 0 ? argv[0] : JS_UNDEFINED;
    JSValueConst arg;
    int r;

    if (s->w.stage == RSC_START) {
        int k;
        stream_work_start(&s->w);
        JS_FreeValue(ctx, cb_result);
        cb_result = JS_UNDEFINED;
        if (JS_IsUndefined(hdr->this_val)) {
            JS_ThrowTypeError(ctx, "constructor ReadableStream requires 'new'");
            return -1;
        }
        /* §4.2 step 2 converts it to the UnderlyingSource DICTIONARY, and Web IDL's dictionary conversion
           refuses anything that is not undefined, null or an Object. `new ReadableStream(5)` is a TypeError. */
        if (!JS_IsUndefined(source) && !JS_IsObject(source)) {
            /* `optional object underlyingSource` — Web IDL's `object` type admits neither a primitive nor
               NULL, and only a MISSING argument is absent. `new ReadableStream(null)` is a TypeError. */
            JS_ThrowTypeError(ctx, "the underlying source must be an object");
            return -1;
        }
        for (k = 0; k < SRC_N; k++) s->src[k] = JS_UNDEFINED;
        s->stream = s->controller = s->start_fn = JS_UNDEFINED;
        s->source = JS_DupValue(ctx, source);
        s->proto = JS_UNDEFINED;
        s->member = 0;
        s->w.stage = RSC_PROTO;
    }

    if (s->w.stage == RSC_PROTO) {
        /* Web IDL §3.7.1: the object is created with `? Get(newTarget, "prototype")` when that is an Object,
           and with the interface prototype object otherwise. That read is what makes `class S extends
           ReadableStream {}` produce an S — and it is the page's code, because new.target can be any
           constructor a `Reflect.construct` names. */
        JSAtom a = JS_NewAtom(ctx, "prototype");
        r = step_getprop_run(ctx, hdr, hdr->this_val, a, cb_result, &s->proto, out_cb, out_argc);
        JS_FreeAtom(ctx, a);
        if (r > 0) return r;
        if (r < 0) return -1;
        cb_result = JS_UNDEFINED;
        s->w.stage = JS_IsObject(source) ? RSC_READ : RSC_BUILD;
    }

    while (s->w.stage == RSC_READ) {
        JSAtom a;
        if (s->member >= SRC_N) { s->w.stage = RSC_TYPE; break; }
        a = JS_NewAtom(ctx, SRC_NAMES[s->member]);
        r = step_getprop_run(ctx, hdr, source, a, cb_result, &s->src[s->member], out_cb, out_argc);
        JS_FreeAtom(ctx, a);
        if (r > 0) return r;
        if (r < 0) return -1;
        cb_result = JS_UNDEFINED;
        s->member++;
    }

    if (s->w.stage == RSC_TYPE) {
        /* The one member whose conversion is not a brand check: `ReadableStreamType type` is an enumeration,
           so it is ToString and then a comparison — and ToString is the page's code. */
        if (!JS_IsUndefined(s->src[SRC_TYPE])) {
            JSValue str;
            const char *cs;
            int bytes;
            r = step_tostring_run(ctx, hdr, s->src[SRC_TYPE], cb_result, &str, out_cb, out_argc);
            if (r > 0) return r;
            if (r < 0) return -1;
            cs = JS_ToCString(ctx, str);
            bytes = cs && !strcmp(cs, "bytes");
            if (cs) JS_FreeCString(ctx, cs);
            JS_FreeValue(ctx, str);
            if (!bytes) {
                JS_ThrowTypeError(ctx, "underlying source `type` must be \"bytes\"");
                return -1;
            }
            DFAIL("an underlying source declared `type: \"bytes\"` — build ReadableByteStreamController: a "
                  "BYOB reader, a byte queue of views, and §4.6's pull-into requests");
        }
        if (!JS_IsUndefined(s->src[SRC_CHUNKSIZE]))
            DFAIL("an underlying source declared `autoAllocateChunkSize`, which only the BYTE stream "
                  "controller has — build ReadableByteStreamController");
        if (ctor_callback_member(ctx, s->src[SRC_CANCEL], "cancel") < 0) return -1;
        if (ctor_callback_member(ctx, s->src[SRC_PULL], "pull") < 0) return -1;
        if (ctor_callback_member(ctx, s->src[SRC_START], "start") < 0) return -1;
        cb_result = JS_UNDEFINED;
        s->w.stage = RSC_BUILD;
    }

    if (s->w.stage == RSC_BUILD) {
        StreamData *d;
        ControllerData *c;
        JSValueConst strategy = argc > 1 ? argv[1] : JS_UNDEFINED;
        JSValue hv;
        int absent;
        double h = 1;

        JS_FreeValue(ctx, cb_result);
        cb_result = JS_UNDEFINED;
        /* §4.2 builds the stream and its controller before `start` runs, because `start` is handed that
           controller and may enqueue into it immediately. Both are COMPLETE — every owned slot placed, the
           record attached to its object, the two linked — before the first step that can FAIL, so a failure
           after this point is torn down by the two finalizers and nothing is orphaned. Filling the record
           first and attaching it last leaked the whole runtime graph on a bad highWaterMark: the orphaned
           record held the page's `size` function, and one page function roots everything it can see. */
        s->stream = readable_stream_empty(ctx);
        if (JS_IsException(s->stream)) return -1;
        if (JS_IsObject(s->proto) && JS_SetPrototype(ctx, s->stream, s->proto) < 0) return -1;
        d = stream_of(s->stream);
        s->controller = JS_DupValue(ctx, d->controller);
        c = ctrl_of(s->controller);

        c->pull_fn = s->src[SRC_PULL];
        s->src[SRC_PULL] = JS_UNDEFINED;
        c->cancel_fn = s->src[SRC_CANCEL];
        s->src[SRC_CANCEL] = JS_UNDEFINED;
        c->source = JS_DupValue(ctx, source);
        /* §4.2 steps 6 and 7: ExtractSizeAlgorithm, then ExtractHighWaterMark with a default of 1. Both read
           the strategy dictionary the IDL layer has ALREADY converted — which is why a throwing `get size` is
           seen before a throwing `get start`, the ordering §4.2 states and a page pins. */
        c->size_fn = idl_dict_get(ctx, strategy, "size");
        hv = idl_dict_get(ctx, strategy, "highWaterMark");
        absent = JS_IsUndefined(hv);
        if (!absent && JS_ToFloat64(ctx, &h, hv) < 0) { JS_FreeValue(ctx, hv); return -1; }
        JS_FreeValue(ctx, hv);
        if (!absent) {
            /* §4.2's own check, not the type's: `unrestricted double` accepts NaN, and the STREAM is what
               rejects it. */
            if (h != h || h < 0) {
                JS_ThrowRangeError(ctx, "a queuing strategy's highWaterMark must not be negative or NaN");
                return -1;
            }
            c->hwm = h;
        }
        s->start_fn = s->src[SRC_START];
        s->src[SRC_START] = JS_UNDEFINED;
        s->w.stage = JS_IsFunction(ctx, s->start_fn) ? RSC_CALL : RSC_RESOLVE;
    }

    if (s->w.stage == RSC_CALL) {
        JSValue res;
        arg = s->controller;
        r = step_call_run(ctx, &s->w.phase, s->w.cb, s->start_fn, s->source, 1, &arg,
                          cb_result, &res, out_cb, out_argc);
        if (r > 0) return r;
        JS_FreeValue(ctx, s->w.value);
        s->w.value = res;
        cb_result = JS_UNDEFINED;
        s->w.stage = RSC_RESOLVE;
    }
    if (s->w.stage == RSC_RESOLVE) {
        r = stream_promise_of_run(ctx, &s->w, 0, cb_result, out_cb, out_argc);
        if (r > 0) return r;
        if (r < 0) return -1;
        cb_result = JS_UNDEFINED;
        s->w.stage = RSC_THEN;
    }

    DCHECK(s->w.stage == RSC_THEN, "the ReadableStream constructor resumed in a stage it never parks in");
    JS_FreeValue(ctx, cb_result);
    if (ctrl_react(ctx, s->w.func, s->controller, RXN_START_OK, RXN_START_ERR) < 0) return -1;
    *presult = s->stream;
    s->stream = JS_UNDEFINED;
    return 0;
}

static const IdlStepDecl js_rs_ctor_decl = {
    js_rs_ctor_step, sizeof(JSRsCtorState), js_rs_ctor_visit, js_rs_ctor_release
};

/* ---- install ------------------------------------------------------------------------------------------------ */

void readable_stream_init(JSContext *ctx)
{
    JSClassDef sd = { "ReadableStream", .finalizer = stream_finalizer, .gc_mark = stream_gc_mark };
    JSClassDef rd = { "ReadableStreamDefaultReader", .finalizer = reader_finalizer, .gc_mark = reader_gc_mark };
    JSRuntime *rt = JS_GetRuntime(ctx);
    static const IdlArgType ONE_ANY[1] = { IDL_ANY };
    static const IdlArgType ONE_DICT[1] = { IDL_DICT };
    /* §4.2: `constructor(optional object underlyingSource, optional QueuingStrategy strategy = {})`. The
       SOURCE crosses as an object and is converted in the body (step 2); the STRATEGY is a declared dictionary,
       and that difference is the whole of what the corpus's first constructor test asserts. */
    static const IdlArgType SOURCE_AND_STRATEGY[2] = { IDL_ANY, IDL_DICT };
    int i;

    DCHECK(g_rs_rt == NULL || g_rs_rt == rt, "ReadableStream was installed into a second runtime");
    if (g_rs_rt == rt) return;
    g_rs_rt = rt;
    JS_NewClassID(rt, &g_stream_class);
    JS_NewClass(rt, g_stream_class, &sd);
    JS_NewClassID(rt, &g_reader_class);
    JS_NewClass(rt, g_reader_class, &rd);

    g_stream_proto = JS_NewObject(ctx);
    CHECK(!JS_IsException(g_stream_proto), "ReadableStream.prototype could not be allocated");
    idl_interface_tag(ctx, g_stream_proto, "ReadableStream");
    idl_install_accessor(ctx, g_stream_proto, "locked", js_stream_locked, 0, -1);
    idl_install_method(ctx, g_stream_proto, "getReader", 0,
                       idl_method_id_step(ctx, ONE_DICT, 1, GET_READER_OPTIONS,
                                          (int)(sizeof GET_READER_OPTIONS / sizeof *GET_READER_OPTIONS),
                                          &js_get_reader_decl, GR_SELF));
    idl_optional_from(0);   /* §4.2: `getReader(optional ReadableStreamGetReaderOptions options = {})` */
    g_cancel_stepid = JS_RegisterStepDef(rt, &js_cancel_def);
    CHECK(g_cancel_stepid >= 0, "streams: no step id for cancel");
    idl_install_step_method(ctx, g_stream_proto, "cancel", 0, g_cancel_stepid);

    {
        JSClassDef cd = { "ReadableStreamDefaultController", .finalizer = ctrl_finalizer,
                          .gc_mark = ctrl_gc_mark };
        static const char *const NAMES[3] = { "enqueue", "close", "error" };
        JS_NewClassID(rt, &g_ctrl_class);
        JS_NewClass(rt, g_ctrl_class, &cd);
        g_ctrl_proto = JS_NewObject(ctx);
        CHECK(!JS_IsException(g_ctrl_proto), "the controller prototype could not be allocated");
        idl_interface_tag(ctx, g_ctrl_proto, "ReadableStreamDefaultController");
        idl_install_accessor(ctx, g_ctrl_proto, "desiredSize", js_ctrl_desired, 0, -1);
        for (i = 0; i < 3; i++) {
            g_ctrl_stepids[i] = JS_RegisterStepDef(rt, &js_ctrl_defs[i]);
            CHECK(g_ctrl_stepids[i] >= 0, "streams: no step id for a controller member");
            JS_SetPropertyStr(ctx, g_ctrl_proto, NAMES[i],
                              JS_NewCFunction2(ctx, NULL, NAMES[i], 1, JS_CFUNC_step, g_ctrl_stepids[i]));
        }
    }
    for (i = 0; i < 4; i++) {
        g_rxn_stepids[i] = JS_RegisterStepDef(rt, &js_rxn_defs[i]);
        CHECK(g_rxn_stepids[i] >= 0, "streams: no step id for a §4.5 reaction");
    }
    for (i = 0; i < 2; i++) {
        g_fwd_stepids[i] = JS_RegisterStepDef(rt, &js_fwd_defs[i]);
        CHECK(g_fwd_stepids[i] >= 0, "streams: no step id for a forwarding reaction");
    }

    g_reader_proto = JS_NewObject(ctx);
    CHECK(!JS_IsException(g_reader_proto), "ReadableStreamDefaultReader.prototype could not be allocated");
    idl_interface_tag(ctx, g_reader_proto, "ReadableStreamDefaultReader");
    idl_install_accessor(ctx, g_reader_proto, "closed", js_reader_closed, 0, -1);
    g_release_stepid = JS_RegisterStepDef(rt, &js_release_def);
    CHECK(g_release_stepid >= 0, "streams: no step id for releaseLock");
    idl_install_step_method(ctx, g_reader_proto, "releaseLock", 0, g_release_stepid);
    g_read_stepid = JS_RegisterStepDef(rt, &js_read_def);
    CHECK(g_read_stepid >= 0, "streams: no step id for read");
    idl_install_step_method(ctx, g_reader_proto, "read", 0, g_read_stepid);
    idl_install_step_method(ctx, g_reader_proto, "cancel", 0, g_cancel_stepid);
    /* §4.4 performs the ABSTRACT OPERATIONS, so the iterator keeps the function objects INSTALLED HERE — a
       page that rebinds the members changes what its own calls do and changes nothing about `for await`. Read
       back off a prototype nothing of the page's has touched yet. */
    g_read_fn = JS_GetPropertyStr(ctx, g_reader_proto, "read");
    g_release_fn = JS_GetPropertyStr(ctx, g_reader_proto, "releaseLock");
    g_reader_cancel_fn = JS_GetPropertyStr(ctx, g_reader_proto, "cancel");
    g_get_reader_fn = JS_GetPropertyStr(ctx, g_stream_proto, "getReader");
    CHECK(JS_IsFunction(ctx, g_read_fn) && JS_IsFunction(ctx, g_release_fn) &&
          JS_IsFunction(ctx, g_reader_cancel_fn) && JS_IsFunction(ctx, g_get_reader_fn),
          "streams: an operation §4.4 performs was not installed before it was captured");

    /* §4.4's asynchronous iteration. §3.7.11 puts %AsyncIteratorPrototype% under the iterator prototype
       object, which is what gives it @@asyncIterator and the async-iterator helpers for nothing. */
    {
        JSClassDef ad = { "ReadableStream AsyncIterator", .finalizer = aiter_finalizer,
                          .gc_mark = aiter_gc_mark };
        JSValue aproto = JS_GetAsyncIteratorPrototype(ctx);
        JSValue values_fn;
        int vid;
        JS_NewClassID(rt, &g_aiter_class);
        JS_NewClass(rt, g_aiter_class, &ad);
        g_aiter_proto = JS_NewObjectProto(ctx, aproto);
        JS_FreeValue(ctx, aproto);
        CHECK(!JS_IsException(g_aiter_proto), "the async iterator prototype could not be allocated");
        for (i = 0; i < AI_N; i++) {
            g_aiter_stepids[i] = JS_RegisterStepDef(rt, &js_aiter_defs[i]);
            CHECK(g_aiter_stepids[i] >= 0, "streams: no step id for a §4.4 operation");
        }
        idl_install_step_method(ctx, g_aiter_proto, "next", 0, g_aiter_stepids[AI_NEXT]);
        idl_install_step_method(ctx, g_aiter_proto, "return", 1, g_aiter_stepids[AI_RETURN]);

        vid = idl_method_id_step(ctx, ONE_DICT, 1, ITERATOR_OPTIONS,
                                 (int)(sizeof ITERATOR_OPTIONS / sizeof *ITERATOR_OPTIONS),
                                 &js_values_decl, 0);
        idl_optional_from(0);   /* `async iterable<any>(optional ReadableStreamIteratorOptions options = {})` */
        idl_install_method(ctx, g_stream_proto, "values", 0, vid);
        /* §3.7.11: @@asyncIterator IS the same function object as `values`, writable and configurable but not
           enumerable — not a second function that forwards to it. */
        values_fn = JS_GetPropertyStr(ctx, g_stream_proto, "values");
        JS_DefinePropertyValue(ctx, g_stream_proto, JS_WellKnownSymbolAtom(JS_WKS_ASYNC_ITERATOR), values_fn,
                               JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
    }

    /* §4.2's tee. Its branches' pull and cancel are step closures, so they need the controller's members as
       function objects for the same reason §4.4 needs the reader's. */
    {
        JSClassDef td = { "ReadableStream tee", .finalizer = tee_finalizer, .gc_mark = tee_gc_mark };
        static const char *const CTRL_NAMES[3] = { "enqueue", "close", "error" };
        JS_NewClassID(rt, &g_tee_class);
        JS_NewClass(rt, g_tee_class, &td);
        for (i = 0; i < 3; i++) {
            g_ctrl_fn[i] = JS_GetPropertyStr(ctx, g_ctrl_proto, CTRL_NAMES[i]);
            CHECK(JS_IsFunction(ctx, g_ctrl_fn[i]),
                  "streams: a controller member the tee performs was not installed before it was captured");
        }
        for (i = 0; i < TEE_N; i++) {
            g_tee_stepids[i] = JS_RegisterStepDef(rt, &js_tee_defs[i]);
            CHECK(g_tee_stepids[i] >= 0, "streams: no step id for a tee operation");
        }
        idl_install_method(ctx, g_stream_proto, "tee", 0,
                           idl_method_id_step(ctx, NULL, 0, NULL, 0, &js_tee_call_decl, 0));
    }

    /* Fetch §5.2's "fully read", whose record is a class for the reason every other one here is: it is a
       cycle (record -> reader -> stream) and the collector has to see it. */
    {
        JSClassDef dd = { "ReadableStream drain", .finalizer = drain_finalizer, .gc_mark = drain_gc_mark };
        JS_NewClassID(rt, &g_drain_class);
        JS_NewClass(rt, g_drain_class, &dd);
        for (i = 0; i < DRAIN_N; i++) {
            g_drain_stepids[i] = JS_RegisterStepDef(rt, &js_drain_defs[i]);
            CHECK(g_drain_stepids[i] >= 0, "streams: no step id for a drain reaction");
        }
    }

    /* §4.2's `from`, whose source is any async or sync iterable. */
    {
        JSClassDef fd = { "ReadableStream from", .finalizer = from_finalizer, .gc_mark = from_gc_mark };
        static const IdlArgType ONE_ANY_ARG[1] = { IDL_ANY };
        JS_NewClassID(rt, &g_from_class);
        JS_NewClass(rt, g_from_class, &fd);
        for (i = 0; i < FROM_N; i++) {
            g_from_stepids[i] = JS_RegisterStepDef(rt, &js_from_defs[i]);
            CHECK(g_from_stepids[i] >= 0, "streams: no step id for a `from` operation");
        }
        g_from_ctor_stepid = idl_method_id_step(ctx, ONE_ANY_ARG, 1, NULL, 0, &js_from_call_decl, 0);
    }

    g_ctor_stepid = idl_method_id_step(ctx, SOURCE_AND_STRATEGY, 2, QUEUING_STRATEGY,
                                       (int)(sizeof QUEUING_STRATEGY / sizeof *QUEUING_STRATEGY),
                                       &js_rs_ctor_decl, 0);
    idl_optional_from(0);   /* §4.2: both constructor arguments are optional */
    /* §4.3's constructor IS getReader spelled the other way — SetUpReadableStreamDefaultReader reached with
       the stream in an argument instead of in the receiver, which is all the magic says. */
    g_reader_ctor_stepid = idl_method_id_step(ctx, ONE_ANY, 1, NULL, 0, &js_get_reader_decl, GR_CTOR);
}

void readable_stream_install(JSContext *ctx, JSValueConst global)
{
    JSValue ctor;
    DCHECK(g_ctor_stepid >= 0, "ReadableStream was installed before its constructor was declared");
    ctor = idl_step_constructor(ctx, "ReadableStream", 0, g_ctor_stepid);
    CHECK(!JS_IsException(ctor), "the ReadableStream interface object could not be allocated");
    JS_SetConstructor(ctx, ctor, g_stream_proto);
    /* §4.2's `from` is STATIC, so it lives on the interface object rather than the prototype. */
    idl_install_method(ctx, ctor, "from", 1, g_from_ctor_stepid);
    JS_SetPropertyStr(ctx, (JSValue)global, "ReadableStream", ctor);

    ctor = idl_step_constructor(ctx, "ReadableStreamDefaultReader", 1, g_reader_ctor_stepid);
    CHECK(!JS_IsException(ctor), "the reader interface object could not be allocated");
    JS_SetConstructor(ctx, ctor, g_reader_proto);
    JS_SetPropertyStr(ctx, (JSValue)global, "ReadableStreamDefaultReader", ctor);

    ctor = JS_NewCFunction2(ctx, js_illegal_ctor, "ReadableStreamDefaultController", 0,
                            JS_CFUNC_constructor, 0);
    CHECK(!JS_IsException(ctor), "the controller interface object could not be allocated");
    JS_SetConstructor(ctx, ctor, g_ctrl_proto);   /* .prototype and .constructor, both directions */
    JS_SetPropertyStr(ctx, (JSValue)global, "ReadableStreamDefaultController", ctor);
}

void readable_stream_free(JSContext *ctx)
{
    int i;
    if (!g_rs_rt) return;
    JS_FreeValue(ctx, g_stream_proto);
    JS_FreeValue(ctx, g_reader_proto);
    JS_FreeValue(ctx, g_ctrl_proto);
    JS_FreeValue(ctx, g_aiter_proto);
    g_stream_proto = g_reader_proto = g_ctrl_proto = g_aiter_proto = JS_UNDEFINED;
    JS_FreeValue(ctx, g_read_fn);
    JS_FreeValue(ctx, g_release_fn);
    JS_FreeValue(ctx, g_reader_cancel_fn);
    JS_FreeValue(ctx, g_get_reader_fn);
    g_read_fn = g_release_fn = g_reader_cancel_fn = g_get_reader_fn = JS_UNDEFINED;
    for (i = 0; i < AI_N; i++) g_aiter_stepids[i] = -1;
    for (i = 0; i < TEE_N; i++) g_tee_stepids[i] = -1;
    for (i = 0; i < FROM_N; i++) g_from_stepids[i] = -1;
    for (i = 0; i < DRAIN_N; i++) g_drain_stepids[i] = -1;
    g_from_ctor_stepid = -1;
    for (i = 0; i < 3; i++) { JS_FreeValue(ctx, g_ctrl_fn[i]); g_ctrl_fn[i] = JS_UNDEFINED; }
    g_rs_rt = NULL;
    g_ctor_stepid = g_reader_ctor_stepid = g_read_stepid = g_cancel_stepid = g_release_stepid = -1;
    for (i = 0; i < 3; i++) g_ctrl_stepids[i] = -1;
    for (i = 0; i < 4; i++) g_rxn_stepids[i] = -1;
    for (i = 0; i < 2; i++) g_fwd_stepids[i] = -1;
}
