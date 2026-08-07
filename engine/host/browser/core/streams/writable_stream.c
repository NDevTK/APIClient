/* WritableStream — the Streams Standard §5.
 *
 * THE MIRROR OF §4, AND NOT A COPY OF IT. Both are a queue, a mark and page-supplied algorithms, and both
 * settle promises at every edge — which is why the record a machine suspends in, the PromiseResolve every
 * "react to what the page returned" begins with, and the attach that reacts without a `.then` read all live in
 * stream_work.c and are used here unchanged. What is NOT shared is the state machine, because §5's is a
 * genuinely different one: a readable stream is readable until it is closed or errored, and a writable stream
 * has FOUR states with an ERRORING one in between, plus operations that can be IN FLIGHT while it moves.
 *
 * WHY ERRORING EXISTS. When a write fails the sink is still busy, and §5.2 cannot report the stream errored
 * until the in-flight operation has finished — the page would see `closed` reject while its own `write` promise
 * was still pending. So the stream moves to "erroring", the in-flight operation completes, and FinishErroring
 * then moves it to "errored" and settles everything that was waiting, in a stated order. Every failing path
 * goes through it, which is why there is one DealWithRejection rather than an error branch per operation.
 *
 * ONE MACHINE, MANY ENTRY POINTS. §5's members and the reactions to what the sink answered are not separate
 * algorithms: `writer.write()` can error the stream, and erroring is the same sequence the write reaction runs.
 * Written as separate machines each would need the whole tail, which is the seam this project has paid for
 * before. So the `arg` at each definition says which entry, and they converge on the stages below — the shape
 * §4.5's controller and §4.4's iterator already have, at a larger size because §5 is larger.
 *
 * EVERY SETTLE IS A CALL REQUEST, for the reason §4's are: 27.2.1.3.2 step 8 reads `then` off the resolution and
 * a page owns that prototype. Where §5 settles SEVERAL promises in a stated order, that is a chain of stages,
 * one settle each, not a loop of C calls. */
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "quickjs-step.h"
#include "core/idl_args.h"
#include "core/dom/abort.h"
#include "core/events/event_target.h"
#include "core/streams/stream_work.h"
#include "core/streams/writable_stream.h"

/* §5.2's four states are declared in the header: §4.2.4's pipeTo branches on them, and two spellings of one
   enum is one more than a component may have. */

typedef struct {
    uint8_t state;
    uint8_t backpressure;
    JSValue stored_error;
    JSValue writer;
    JSValue controller;
    /* §5.2's write requests: parallel Arrays of the capabilities' resolving functions, because a promise is
       settled by CALLING one of them and both must survive until it is. `whead` is how many have been taken. */
    JSValue write_resolve, write_reject;
    uint32_t whead;
    /* The one write and the one close that have been HANDED TO THE SINK. §5.2 keeps each out of the list while
       its answer is outstanding, which is the whole reason FinishInFlightWrite is a step of its own. */
    JSValue in_flight_write[2];
    JSValue in_flight_close[2];
    JSValue close_request[2];
    /* §5.2's pending abort request: its capability, its promise (handed to every later abort() call), and the
       reason. `was_erroring` is the spec's own flag — an abort that arrived while the stream was ALREADY
       failing does not get to name the reason, and its promise rejects with the stream's. */
    JSValue abort_funcs[2], abort_p, abort_reason;
    uint8_t abort_pending, abort_was_erroring;
} WsData;

typedef struct {
    JSValue stream;
    JSValue closed, closed_funcs[2];
    JSValue ready, ready_funcs[2];
    uint8_t closed_settled, ready_settled;
} WsWriterData;

typedef struct {
    JSValue stream;
    JSValue sink;                  /* the underlying sink, which is its methods' receiver */
    JSValue write_fn, close_fn, abort_fn, size_fn;
    JSValue signal;                /* §5.4's `signal`, the DOM's own AbortSignal */
    double  hwm;
    /* §5.4's queue holds the chunks not yet written and their sizes, with ONE CLOSE SENTINEL at the tail: the
       standard enqueues it so close takes its turn behind the writes rather than jumping them. */
    JSValue queue, queue_size;
    uint32_t qhead;
    double  queue_total;
    uint8_t started;
    uint8_t close_queued;
} WsCtrlData;

static JSClassID g_ws_class, g_wr_class, g_wc_class;
static JSValue   g_ws_proto = JS_UNDEFINED, g_wr_proto = JS_UNDEFINED, g_wc_proto = JS_UNDEFINED;
static JSRuntime *g_ws_rt;
static int g_ws_ctor_stepid = -1, g_wr_ctor_stepid = -1, g_getwriter_stepid = -1;

/* ---- the records ------------------------------------------------------------------------------------------ */

static void ws_finalizer(JSRuntime *rt, JSValue val)
{
    WsData *d = JS_GetOpaque(val, g_ws_class);
    int i;
    if (!d) return;
    JS_FreeValueRT(rt, d->stored_error);
    JS_FreeValueRT(rt, d->writer);
    JS_FreeValueRT(rt, d->controller);
    JS_FreeValueRT(rt, d->write_resolve);
    JS_FreeValueRT(rt, d->write_reject);
    JS_FreeValueRT(rt, d->abort_p);
    JS_FreeValueRT(rt, d->abort_reason);
    for (i = 0; i < 2; i++) {
        JS_FreeValueRT(rt, d->in_flight_write[i]);
        JS_FreeValueRT(rt, d->in_flight_close[i]);
        JS_FreeValueRT(rt, d->close_request[i]);
        JS_FreeValueRT(rt, d->abort_funcs[i]);
    }
    js_free_rt(rt, d);
}

static void ws_gc_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func)
{
    WsData *d = JS_GetOpaque(val, g_ws_class);
    int i;
    if (!d) return;
    JS_MarkValue(rt, d->stored_error, mark_func);
    JS_MarkValue(rt, d->writer, mark_func);
    JS_MarkValue(rt, d->controller, mark_func);
    JS_MarkValue(rt, d->write_resolve, mark_func);
    JS_MarkValue(rt, d->write_reject, mark_func);
    JS_MarkValue(rt, d->abort_p, mark_func);
    JS_MarkValue(rt, d->abort_reason, mark_func);
    for (i = 0; i < 2; i++) {
        JS_MarkValue(rt, d->in_flight_write[i], mark_func);
        JS_MarkValue(rt, d->in_flight_close[i], mark_func);
        JS_MarkValue(rt, d->close_request[i], mark_func);
        JS_MarkValue(rt, d->abort_funcs[i], mark_func);
    }
}

static void wr_finalizer(JSRuntime *rt, JSValue val)
{
    WsWriterData *w = JS_GetOpaque(val, g_wr_class);
    int i;
    if (!w) return;
    JS_FreeValueRT(rt, w->stream);
    JS_FreeValueRT(rt, w->closed);
    JS_FreeValueRT(rt, w->ready);
    for (i = 0; i < 2; i++) { JS_FreeValueRT(rt, w->closed_funcs[i]); JS_FreeValueRT(rt, w->ready_funcs[i]); }
    js_free_rt(rt, w);
}

static void wr_gc_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func)
{
    WsWriterData *w = JS_GetOpaque(val, g_wr_class);
    int i;
    if (!w) return;
    JS_MarkValue(rt, w->stream, mark_func);
    JS_MarkValue(rt, w->closed, mark_func);
    JS_MarkValue(rt, w->ready, mark_func);
    for (i = 0; i < 2; i++) {
        JS_MarkValue(rt, w->closed_funcs[i], mark_func);
        JS_MarkValue(rt, w->ready_funcs[i], mark_func);
    }
}

static void wc_finalizer(JSRuntime *rt, JSValue val)
{
    WsCtrlData *c = JS_GetOpaque(val, g_wc_class);
    if (!c) return;
    JS_FreeValueRT(rt, c->stream);
    JS_FreeValueRT(rt, c->sink);
    JS_FreeValueRT(rt, c->write_fn);
    JS_FreeValueRT(rt, c->close_fn);
    JS_FreeValueRT(rt, c->abort_fn);
    JS_FreeValueRT(rt, c->size_fn);
    JS_FreeValueRT(rt, c->signal);
    JS_FreeValueRT(rt, c->queue);
    JS_FreeValueRT(rt, c->queue_size);
    js_free_rt(rt, c);
}

static void wc_gc_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func)
{
    WsCtrlData *c = JS_GetOpaque(val, g_wc_class);
    if (!c) return;
    JS_MarkValue(rt, c->stream, mark_func);
    JS_MarkValue(rt, c->sink, mark_func);
    JS_MarkValue(rt, c->write_fn, mark_func);
    JS_MarkValue(rt, c->close_fn, mark_func);
    JS_MarkValue(rt, c->abort_fn, mark_func);
    JS_MarkValue(rt, c->size_fn, mark_func);
    JS_MarkValue(rt, c->signal, mark_func);
    JS_MarkValue(rt, c->queue, mark_func);
    JS_MarkValue(rt, c->queue_size, mark_func);
}

static WsData       *ws_of(JSValueConst v) { return JS_GetOpaque(v, g_ws_class); }
static WsWriterData *wr_of(JSValueConst v) { return JS_GetOpaque(v, g_wr_class); }
static WsCtrlData   *wc_of(JSValueConst v) { return JS_GetOpaque(v, g_wc_class); }

bool writable_stream_is(JSValueConst v)
{
    return g_ws_class != 0 && JS_GetOpaque(v, g_ws_class) != NULL;
}

/* ---- the queue, and the tests half a dozen steps ask ------------------------------------------------------- */

static uint32_t ws_array_len(JSContext *ctx, JSValueConst arr)
{
    JSValue len_v = JS_GetPropertyStr(ctx, arr, "length");
    uint32_t n = 0;
    JS_ToUint32(ctx, &n, len_v);
    JS_FreeValue(ctx, len_v);
    return n;
}

static uint32_t wc_queued(JSContext *ctx, WsCtrlData *c) { return ws_array_len(ctx, c->queue) - c->qhead; }

static void wc_enqueue(JSContext *ctx, WsCtrlData *c, JSValueConst chunk, double size)
{
    uint32_t n = ws_array_len(ctx, c->queue);
    JS_SetPropertyUint32(ctx, c->queue, n, JS_DupValue(ctx, chunk));
    JS_SetPropertyUint32(ctx, c->queue_size, n, JS_NewFloat64(ctx, size));
    c->queue_total += size;
}

static JSValue wc_peek(JSContext *ctx, WsCtrlData *c) { return JS_GetPropertyUint32(ctx, c->queue, c->qhead); }

static void wc_dequeue(JSContext *ctx, WsCtrlData *c)
{
    JSValue sz = JS_GetPropertyUint32(ctx, c->queue_size, c->qhead);
    double x = 0;
    JS_ToFloat64(ctx, &x, sz);
    JS_FreeValue(ctx, sz);
    JS_SetPropertyUint32(ctx, c->queue, c->qhead, JS_UNDEFINED);
    c->qhead++;
    c->queue_total -= x;
    if (c->queue_total < 0) c->queue_total = 0;
}

static int wc_reset_queue(JSContext *ctx, WsCtrlData *c)
{
    JS_FreeValue(ctx, c->queue);
    JS_FreeValue(ctx, c->queue_size);
    c->queue = JS_NewArray(ctx);
    c->queue_size = JS_NewArray(ctx);
    c->qhead = 0;
    c->queue_total = 0;
    return JS_IsException(c->queue) || JS_IsException(c->queue_size) ? -1 : 0;
}

static double wc_desired(WsCtrlData *c) { return c->hwm - c->queue_total; }
static bool   wc_backpressure(WsCtrlData *c) { return wc_desired(c) <= 0; }

static bool ws_close_queued_or_in_flight(WsData *d)
{
    return !JS_IsUndefined(d->close_request[0]) || !JS_IsUndefined(d->in_flight_close[0]);
}

static bool ws_in_flight(WsData *d)
{
    return !JS_IsUndefined(d->in_flight_write[0]) || !JS_IsUndefined(d->in_flight_close[0]);
}

/* THE ABSTRACT OPERATIONS, as the ORIGINAL function objects — captured at install, so a page that rebinds a
   prototype member changes what IT calls and nothing that the platform performs on its behalf. */
static JSValue g_op_fn[WS_OP_N];

JSValueConst writable_stream_op(WritableStreamOp which)
{
    DCHECK(which >= 0 && which < WS_OP_N, "a §5 operation was asked for by a name this component does not map");
    return g_op_fn[which];
}

bool writable_stream_query(JSValueConst v, WritableStreamState *pstate, bool *plocked, bool *pclose_queued)
{
    WsData *d = ws_of(v);

    if (!d) return false;
    if (pstate) *pstate = (WritableStreamState)d->state;
    if (plocked) *plocked = !JS_IsUndefined(d->writer);
    if (pclose_queued) *pclose_queued = ws_close_queued_or_in_flight(d);
    return true;
}

JSValue writable_stream_stored_error(JSContext *ctx, JSValueConst v)
{
    WsData *d = ws_of(v);
    return d ? JS_DupValue(ctx, d->stored_error) : JS_UNDEFINED;
}

JSValue writable_writer_ready(JSContext *ctx, JSValueConst writer)
{
    WsWriterData *w = wr_of(writer);
    DCHECK(w != NULL, "the ready promise of something that is not a writer was asked for");
    return JS_DupValue(ctx, w->ready);
}

JSValue writable_writer_closed(JSContext *ctx, JSValueConst writer)
{
    WsWriterData *w = wr_of(writer);
    DCHECK(w != NULL, "the closed promise of something that is not a writer was asked for");
    return JS_DupValue(ctx, w->closed);
}


static uint32_t ws_write_pending(JSContext *ctx, WsData *d)
{
    return ws_array_len(ctx, d->write_resolve) - d->whead;
}

static void ws_take_write(JSContext *ctx, WsData *d, JSValue out[2])
{
    if (ws_write_pending(ctx, d) == 0) { out[0] = out[1] = JS_UNDEFINED; return; }
    out[0] = JS_GetPropertyUint32(ctx, d->write_resolve, d->whead);
    out[1] = JS_GetPropertyUint32(ctx, d->write_reject, d->whead);
    JS_SetPropertyUint32(ctx, d->write_resolve, d->whead, JS_UNDEFINED);
    JS_SetPropertyUint32(ctx, d->write_reject, d->whead, JS_UNDEFINED);
    d->whead++;
}

static void ws_take_pair(JSValue slot[2], JSValue out[2])
{
    out[0] = slot[0];
    out[1] = slot[1];
    slot[0] = slot[1] = JS_UNDEFINED;
}

/* §5.2's AddWriteRequest: a capability on the list, and its promise handed back. */
static JSValue ws_add_write_request(JSContext *ctx, WsData *d)
{
    JSValue funcs[2];
    JSValue p = JS_NewPromiseCapability(ctx, funcs);
    uint32_t at;
    if (JS_IsException(p)) return p;
    at = ws_array_len(ctx, d->write_resolve);
    JS_SetPropertyUint32(ctx, d->write_resolve, at, funcs[0]);
    JS_SetPropertyUint32(ctx, d->write_reject, at, funcs[1]);
    return p;
}

/* ---- the settle sequences ---------------------------------------------------------------------------------- */

enum {
    W_IDLE = 0,
    W_REJECT_WRITES,               /* §5.2: EVERY queued write request, with w->err */
    W_CLOSED_REJ, W_CLOSED_RES, W_READY_REJ, W_READY_RES,
    W_ABORT_RES, W_ABORT_REJ,
    W_CLOSE_REQ_REJ,
    W_ONE                          /* the single capability the caller placed in w->func, called with w->value */
};

/* §5.3's `closed` and `ready` settle EXACTLY ONCE each, so a FLAG is the test rather than the slot: a zeroed
   JSValue is the integer 0, which this project has paid for five times.
   A REJECTION OF EITHER IS ALWAYS AN `Ensure…PromiseRejected`, AND THAT OPERATION REPLACES. Every §5 site that
   rejects one of these goes through it — StartErroring, RejectCloseAndClosedPromiseIfNeeded, and release alike
   — and its second step is "otherwise, set the promise to a promise rejected with error". Release was given a
   separate pair of settle states on the belief that only it replaced, which left the common case broken in the
   quiet direction: a stream with no backpressure has already FULFILLED `ready`, so a sink whose `close` threw
   rejected `close()` and left `ready` fulfilled forever. Two names for one operation is what made a difference
   that does not exist look deliberate, so there is one.
   The replacement is marked HANDLED because the spec's third step says so: the promise is the stream's
   bookkeeping and a writer that is never read again must not be reported as an unhandled rejection. */
static int wr_settle_run(JSContext *ctx, StreamWork *w, WsWriterData *wr, int which, int reject,
                         JSValueConst value, JSValue in, JSValue **out_cb, int *out_argc)
{
    JSValueConst arg;
    JSValue out;
    int r;

    if (w->phase == 0) {
        JSValue pair[2];
        JSValue *funcs;
        uint8_t *settled;
        if (!wr) { JS_FreeValue(ctx, in); return 0; }
        funcs = which ? wr->ready_funcs : wr->closed_funcs;
        settled = which ? &wr->ready_settled : &wr->closed_settled;
        if (*settled) {
            JSValue p;
            /* A promise that has already settled can only be REJECTED again, never resolved: the resolutions
               here are FinishInFlightClose's `closed` and UpdateBackpressure's `ready`, and both run on one
               that is still pending. So an already-settled slot arriving on the resolve side is the state
               machine having reached a place the spec says it cannot. */
            DCHECK(reject, "a §5.3 promise was resolved a second time");
            p = JS_NewPromiseCapability(ctx, pair);
            if (JS_IsException(p)) { JS_FreeValue(ctx, in); return -1; }
            JS_MarkPromiseHandled(ctx, p);
            JS_FreeValue(ctx, which ? wr->ready : wr->closed);
            if (which) wr->ready = p; else wr->closed = p;
        } else {
            *settled = 1;
            pair[0] = funcs[0];
            pair[1] = funcs[1];
            funcs[0] = funcs[1] = JS_UNDEFINED;
        }
        JS_FreeValue(ctx, w->func);
        w->func = pair[reject];
        JS_FreeValue(ctx, pair[reject ^ 1]);
        JS_FreeValue(ctx, w->value);
        w->value = JS_DupValue(ctx, value);
    }
    arg = w->value;
    r = step_call_run(ctx, &w->phase, STEP_CB(w->cb), w->func, JS_UNDEFINED, 1, &arg, in, &out, out_cb, out_argc);
    if (r > 0) return r;
    if (JS_IsException(out)) return -1;
    JS_FreeValue(ctx, out);
    return 0;
}

/* Run the sequence `w->settle` names to completion; 0 means finished (and it is cleared). */
static int ws_settle_run(JSContext *ctx, StreamWork *w, JSValueConst stream, JSValue in,
                         JSValue **out_cb, int *out_argc)
{
    WsData *d = ws_of(stream);
    WsWriterData *wr;
    JSValueConst arg;
    JSValue out;
    int r;

    DCHECK(d != NULL, "a §5 settle sequence ran on something that is not a WritableStream");
    wr = JS_IsUndefined(d->writer) ? NULL : wr_of(d->writer);

    if (w->settle == W_REJECT_WRITES) {
        /* §5.2 rejects EVERY queued write request. Answering only the first silently abandons the rest. */
        for (;;) {
            if (w->phase == 0) {
                JSValue pair[2];
                ws_take_write(ctx, d, pair);
                if (JS_IsUndefined(pair[1])) {
                    JS_FreeValue(ctx, pair[0]);
                    JS_FreeValue(ctx, in);
                    w->settle = W_IDLE;
                    return 0;
                }
                JS_FreeValue(ctx, w->func);
                w->func = pair[1];
                JS_FreeValue(ctx, pair[0]);
                JS_FreeValue(ctx, w->value);
                w->value = JS_DupValue(ctx, w->err);
            }
            arg = w->value;
            r = step_call_run(ctx, &w->phase, STEP_CB(w->cb), w->func, JS_UNDEFINED, 1, &arg, in, &out,
                              out_cb, out_argc);
            if (r > 0) return r;
            if (JS_IsException(out)) return -1;
            JS_FreeValue(ctx, out);
            in = JS_UNDEFINED;
        }
    }

    if (w->settle >= W_CLOSED_REJ && w->settle <= W_READY_RES) {
        int which  = (w->settle == W_READY_REJ || w->settle == W_READY_RES);
        int reject = (w->settle != W_CLOSED_RES && w->settle != W_READY_RES);
        r = wr_settle_run(ctx, w, wr, which, reject, reject ? (JSValueConst)w->err : JS_UNDEFINED,
                          in, out_cb, out_argc);
        if (r != 0) return r;
        w->settle = W_IDLE;
        return 0;
    }

    if (w->phase == 0) {
        JSValue pair[2];
        int reject = (w->settle == W_ABORT_REJ || w->settle == W_CLOSE_REQ_REJ);
        if (w->settle == W_ABORT_RES || w->settle == W_ABORT_REJ) {
            ws_take_pair(d->abort_funcs, pair);
            d->abort_pending = 0;
        } else if (w->settle == W_CLOSE_REQ_REJ) {
            ws_take_pair(d->close_request, pair);
        } else {
            DCHECK(w->settle == W_ONE, "a §5 settle sequence ran with a state this component does not have");
            pair[0] = w->func;
            pair[1] = JS_UNDEFINED;
            w->func = JS_UNDEFINED;
        }
        if (JS_IsUndefined(pair[reject])) {
            JS_FreeValue(ctx, pair[0]);
            JS_FreeValue(ctx, pair[1]);
            JS_FreeValue(ctx, in);
            w->settle = W_IDLE;
            return 0;
        }
        JS_FreeValue(ctx, w->func);
        w->func = pair[reject];
        JS_FreeValue(ctx, pair[reject ^ 1]);
        if (w->settle != W_ONE) {
            JS_FreeValue(ctx, w->value);
            w->value = reject ? JS_DupValue(ctx, w->err) : JS_UNDEFINED;
        }
    }
    arg = w->value;
    r = step_call_run(ctx, &w->phase, STEP_CB(w->cb), w->func, JS_UNDEFINED, 1, &arg, in, &out, out_cb, out_argc);
    if (r > 0) return r;
    if (JS_IsException(out)) return -1;
    JS_FreeValue(ctx, out);
    w->settle = W_IDLE;
    return 0;
}

/* ---- THE ONE MACHINE --------------------------------------------------------------------------------------- */

enum {
    OP_START_OK = 0, OP_START_ERR,     /* the start promise's reactions */
    OP_WRITE_OK,     OP_WRITE_ERR,     /* what the sink's `write` answered */
    OP_CLOSE_OK,     OP_CLOSE_ERR,     /* ... its `close` */
    OP_ABORT_OK,     OP_ABORT_ERR,     /* ... its `abort`, which FinishErroring waits for */
    OP_WS_ABORT, OP_WS_CLOSE,          /* §5.2's members */
    OP_WR_WRITE, OP_WR_RELEASE, OP_WR_CLOSE, OP_WR_ABORT,   /* §5.3's */
    OP_WC_ERROR,                       /* §5.4's */
    OP_N
};

enum {
    S_ENTRY = 0,
    S_SIGNAL,        /* §5.2 abort step 2's "signal abort", whose FIRE runs the page's listeners */
    S_ABORT_SETUP,
    S_SIZE,          /* §5.4's GetChunkSize, whose throw ERRORS the stream rather than propagating */
    S_WRITE_QUEUE,
    S_ERROR,         /* §5.2's DealWithRejection */
    S_START_ERR,     /* StartErroring's own settle: the writer's `ready` is rejected */
    S_FINISH_ERR,
    S_ERR_WRITES, S_ERR_ABORT, S_ERR_CLOSE, S_ERR_CLOSED,
    S_ADVANCE,       /* §5.4's AdvanceQueueIfNeeded */
    S_SINK_WRITE, S_SINK_CLOSE, S_SINK_REACT,
    S_WRITE_DONE,    /* ProcessWrite's fulfilment tail: dequeue, backpressure, advance */
    S_CLOSE_DONE, S_CLOSE_DONE2, S_CLOSE_ERR2,
    S_RELEASE, S_RELEASE2,   /* §5.3's release: reject `ready`, then `closed`, then detach */
    S_SETTLE,        /* run w.settle, then go to `next` */
    S_RESULT,        /* settle this member's OWN capability, for the short-circuit answers */
    S_DONE
};

typedef struct {
    JSStepHdr  hdr;
    StreamWork w;
    JSValue    ctrl;
    JSValue    stream;
    JSValue    chunk;
    JSValue    promise;    /* what a member hands back */
    JSValue    funcs[2];   /* its capability, when the member settles its own answer */
    AbortSignalWork sig;   /* §3.2 "signal abort"'s own record, while the controller's signal is aborting */
    uint8_t    next;       /* the stage S_SETTLE returns to */
    uint8_t    tail;       /* where the erroring chain ends */
    uint8_t    is_close;   /* the sink call in flight is the CLOSE, not a write */
    uint8_t    reject;
    double     size;
} JSWsState;

static int g_op_stepid[OP_N];

static void js_ws_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSWsState *s = st;
    stream_work_visit(ctx, &s->w, v);
    v->val(ctx, &s->ctrl);
    v->val(ctx, &s->stream);
    v->val(ctx, &s->chunk);
    v->val(ctx, &s->promise);
    v->val(ctx, &s->funcs[0]);
    v->val(ctx, &s->funcs[1]);
    abort_signal_work_visit(ctx, &s->sig, v);
}

static JSValue js_ws_fini(JSContext *ctx, void *st, bool take_result)
{
    JSWsState *s = st;
    JSValue r = take_result ? s->promise : JS_UNDEFINED;
    if (take_result) s->promise = JS_UNDEFINED;
    stream_work_release(ctx, &s->w);
    JS_FreeValue(ctx, s->ctrl);
    JS_FreeValue(ctx, s->stream);
    JS_FreeValue(ctx, s->chunk);
    JS_FreeValue(ctx, s->promise);
    JS_FreeValue(ctx, s->funcs[0]);
    JS_FreeValue(ctx, s->funcs[1]);
    abort_signal_work_release(ctx, &s->sig);
    s->ctrl = s->stream = s->chunk = s->promise = JS_UNDEFINED;
    s->funcs[0] = s->funcs[1] = JS_UNDEFINED;
    return r;
}

static int ws_react(JSContext *ctx, JSValueConst promise, JSValueConst ctrl, int ok, int err)
{
    JSValueConst data[1];
    data[0] = ctrl;
    return stream_react(ctx, promise, g_op_stepid[ok], g_op_stepid[err], data, 1);
}

/* §5.2's StartErroring, minus the settle its caller runs: the state moves, and §5.4's ErrorSteps drop the queue
   and tell the sink through its signal. The writer's `ready` rejection is a stage, because it is a call. */
static void ws_start_erroring(JSContext *ctx, WsData *d, JSValueConst reason)
{
    WsCtrlData *c = JS_IsUndefined(d->controller) ? NULL : wc_of(d->controller);
    d->state = WS_ERRORING;
    JS_FreeValue(ctx, d->stored_error);
    d->stored_error = JS_DupValue(ctx, reason);
    if (c) wc_reset_queue(ctx, c);
}

/* §5.2's UpdateBackpressure, minus its settle. Returns the sequence the change calls for, or W_IDLE. */
static int ws_update_backpressure(JSContext *ctx, WsData *d, WsCtrlData *c)
{
    bool bp = wc_backpressure(c);
    WsWriterData *wr = JS_IsUndefined(d->writer) ? NULL : wr_of(d->writer);

    if (d->state != WS_WRITABLE || ws_close_queued_or_in_flight(d)) return W_IDLE;
    if (bp == (d->backpressure != 0)) return W_IDLE;
    d->backpressure = (uint8_t)bp;
    if (!wr) return W_IDLE;
    if (!bp) return W_READY_RES;
    /* §5.3: backpressure returning gives the writer a NEW pending `ready`. */
    {
        JSValue funcs[2];
        JSValue p = JS_NewPromiseCapability(ctx, funcs);
        if (JS_IsException(p)) return W_IDLE;
        JS_FreeValue(ctx, wr->ready);
        JS_FreeValue(ctx, wr->ready_funcs[0]);
        JS_FreeValue(ctx, wr->ready_funcs[1]);
        wr->ready = p;
        wr->ready_funcs[0] = funcs[0];
        wr->ready_funcs[1] = funcs[1];
        wr->ready_settled = 0;
    }
    return W_IDLE;
}

/* A member's short-circuit answer: its own capability, settled at S_RESULT. */
static int ws_short(JSContext *ctx, JSWsState *s, int reject, JSValue value)
{
    s->promise = JS_NewPromiseCapability(ctx, s->funcs);
    if (JS_IsException(s->promise)) { JS_FreeValue(ctx, value); return -1; }
    s->reject = (uint8_t)reject;
    JS_FreeValue(ctx, s->w.value);
    s->w.value = value;
    s->w.stage = S_RESULT;
    return 0;
}

static int js_ws_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    JSWsState *s = st;
    int op = s->hdr.arg;
    WsData *d;
    WsCtrlData *c;
    JSValue out;
    int r;

    if (s->w.stage == S_ENTRY) {
        stream_work_start(&s->w);
        s->ctrl = s->stream = s->chunk = s->promise = JS_UNDEFINED;
        abort_signal_work_start(&s->sig);
        s->funcs[0] = s->funcs[1] = JS_UNDEFINED;
        s->size = 1;
        s->tail = S_DONE;
        JS_FreeValue(ctx, cb_result);
        cb_result = JS_UNDEFINED;

        if (op <= OP_ABORT_ERR) {
            /* a REACTION: the controller is what it captured */
            s->ctrl = JS_DupValue(ctx, JS_StepClosureData(&s->hdr, 0));
            c = wc_of(s->ctrl);
            DCHECK(c != NULL, "a §5 reaction captured something that is not a controller");
            s->stream = JS_DupValue(ctx, c->stream);
        } else {
            /* a MEMBER: the receiver is the stream, the writer or the controller */
            JSValueConst recv = s->hdr.this_val;
            if (op == OP_WS_ABORT || op == OP_WS_CLOSE) {
                WsData *dd = ws_of(recv);
                if (!dd) {
                    JS_ThrowTypeError(ctx, "not a WritableStream");
                    return JS_STEP_ABRUPT;
                }
                s->stream = JS_DupValue(ctx, recv);
                /* §5.2's `abort()` and `close()` both begin with IsWritableStreamLocked, and both REJECT
                   rather than throw. A locked stream is the writer's to drive: letting the stream abort out
                   from under a writer would settle the writer's own close request behind its back. */
                if (!JS_IsUndefined(dd->writer)) {
                    JS_ThrowTypeError(ctx, "this stream is locked to a writer");
                    if (ws_short(ctx, s, 1, JS_GetException(ctx)) < 0) return JS_STEP_ABRUPT;
                    goto run;
                }
            } else if (op == OP_WC_ERROR) {
                WsCtrlData *cc = wc_of(recv);
                if (!cc) {
                    JS_ThrowTypeError(ctx, "not a WritableStreamDefaultController");
                    return JS_STEP_ABRUPT;
                }
                s->ctrl = JS_DupValue(ctx, recv);
                s->stream = JS_DupValue(ctx, cc->stream);
            } else {
                WsWriterData *wr = wr_of(recv);
                if (!wr) {
                    JS_ThrowTypeError(ctx, "not a WritableStreamDefaultWriter");
                    return JS_STEP_ABRUPT;
                }
                if (JS_IsUndefined(wr->stream)) {
                    /* §5.3: every member of a RELEASED writer answers a rejected promise, except release
                       itself, which is a no-op the second time. */
                    if (op == OP_WR_RELEASE) return JS_STEP_DONE;
                    JS_ThrowTypeError(ctx, "this writer has been released");
                    if (ws_short(ctx, s, 1, JS_GetException(ctx)) < 0) return JS_STEP_ABRUPT;
                    goto run;
                }
                s->stream = JS_DupValue(ctx, wr->stream);
            }
            if (JS_IsUndefined(s->ctrl)) {
                WsData *dd = ws_of(s->stream);
                s->ctrl = JS_DupValue(ctx, dd->controller);
            }
        }

        d = ws_of(s->stream);
        c = wc_of(s->ctrl);
        DCHECK(d != NULL && c != NULL, "a §5 machine reached its entry with no records");

        switch (op) {
        case OP_START_OK:
            c->started = 1;
            s->w.stage = S_ADVANCE;
            break;
        case OP_START_ERR:
            /* §5.4: a start that rejects errors the stream, even with nothing written. */
            c->started = 1;
            JS_FreeValue(ctx, s->w.err);
            s->w.err = JS_DupValue(ctx, step_arg(&s->hdr, 0));
            s->w.stage = S_ERROR;
            break;
        case OP_WRITE_OK: {
            JSValue pair[2];
            ws_take_pair(d->in_flight_write, pair);
            DCHECK(!JS_IsUndefined(pair[0]), "§5.2 finished a write that was never in flight");
            JS_FreeValue(ctx, s->w.func);
            s->w.func = pair[0];
            JS_FreeValue(ctx, pair[1]);
            JS_FreeValue(ctx, s->w.value);
            s->w.value = JS_UNDEFINED;
            s->w.settle = W_ONE;
            s->w.stage = S_SETTLE;
            s->next = S_WRITE_DONE;
            break;
        }
        case OP_WRITE_ERR: {
            JSValue pair[2];
            ws_take_pair(d->in_flight_write, pair);
            DCHECK(!JS_IsUndefined(pair[1]), "§5.2 finished a write that was never in flight");
            JS_FreeValue(ctx, s->w.err);
            s->w.err = JS_DupValue(ctx, step_arg(&s->hdr, 0));
            JS_FreeValue(ctx, s->w.func);
            s->w.func = pair[1];
            JS_FreeValue(ctx, pair[0]);
            JS_FreeValue(ctx, s->w.value);
            s->w.value = JS_DupValue(ctx, s->w.err);
            s->w.settle = W_ONE;
            s->w.stage = S_SETTLE;
            s->next = S_ERROR;
            break;
        }
        case OP_CLOSE_OK: {
            JSValue pair[2];
            ws_take_pair(d->in_flight_close, pair);
            DCHECK(!JS_IsUndefined(pair[0]), "§5.2 finished a close that was never in flight");
            JS_FreeValue(ctx, s->w.func);
            s->w.func = pair[0];
            JS_FreeValue(ctx, pair[1]);
            JS_FreeValue(ctx, s->w.value);
            s->w.value = JS_UNDEFINED;
            s->w.settle = W_ONE;
            s->w.stage = S_SETTLE;
            s->next = S_CLOSE_DONE;
            break;
        }
        case OP_CLOSE_ERR: {
            JSValue pair[2];
            ws_take_pair(d->in_flight_close, pair);
            DCHECK(!JS_IsUndefined(pair[1]), "§5.2 finished a close that was never in flight");
            JS_FreeValue(ctx, s->w.err);
            s->w.err = JS_DupValue(ctx, step_arg(&s->hdr, 0));
            JS_FreeValue(ctx, s->w.func);
            s->w.func = pair[1];
            JS_FreeValue(ctx, pair[0]);
            JS_FreeValue(ctx, s->w.value);
            s->w.value = JS_DupValue(ctx, s->w.err);
            s->w.settle = W_ONE;
            s->w.stage = S_SETTLE;
            s->next = S_CLOSE_ERR2;
            break;
        }
        case OP_ABORT_OK:
            s->w.settle = W_ABORT_RES;
            s->w.stage = S_SETTLE;
            s->next = S_ERR_CLOSE;
            break;
        case OP_ABORT_ERR:
            JS_FreeValue(ctx, s->w.err);
            s->w.err = JS_DupValue(ctx, step_arg(&s->hdr, 0));
            s->w.settle = W_ABORT_REJ;
            s->w.stage = S_SETTLE;
            s->next = S_ERR_CLOSE;
            break;

        case OP_WS_ABORT: case OP_WR_ABORT:
            /* §5.2's WritableStreamAbort. */
            if (d->state == WS_CLOSED || d->state == WS_ERRORED) {
                if (ws_short(ctx, s, 0, JS_UNDEFINED) < 0) return JS_STEP_ABRUPT;
                break;
            }
            JS_FreeValue(ctx, s->w.value);
            s->w.value = JS_DupValue(ctx, step_arg(&s->hdr, 0));
            s->w.stage = S_SIGNAL;
            break;

        case OP_WS_CLOSE: case OP_WR_CLOSE:
            /* §5.2's WritableStreamClose. */
            if (d->state == WS_CLOSED || d->state == WS_ERRORED || ws_close_queued_or_in_flight(d)) {
                JS_ThrowTypeError(ctx, "this stream cannot be closed");
                if (ws_short(ctx, s, 1, JS_GetException(ctx)) < 0) return JS_STEP_ABRUPT;
                break;
            }
            s->promise = JS_NewPromiseCapability(ctx, d->close_request);
            if (JS_IsException(s->promise)) return JS_STEP_ABRUPT;
            /* §5.2 step 7: a writer waiting on backpressure is released by the close — nothing more is coming,
               so `ready` resolves rather than staying pending forever. */
            s->w.stage = S_SETTLE;
            s->next = S_ADVANCE;
            s->w.settle = (!JS_IsUndefined(d->writer) && d->backpressure && d->state == WS_WRITABLE)
                        ? W_READY_RES : W_IDLE;
            /* §5.4's Close: the sentinel goes on the tail, so it is taken after every queued chunk. */
            wc_enqueue(ctx, c, JS_UNDEFINED, 0);
            c->close_queued = 1;
            if (s->w.settle == W_IDLE) s->w.stage = S_ADVANCE;
            break;

        case OP_WR_WRITE:
            JS_FreeValue(ctx, s->chunk);
            s->chunk = JS_DupValue(ctx, step_arg(&s->hdr, 0));
            s->w.stage = S_SIZE;
            break;

        case OP_WR_RELEASE:
            /* §5.3's release: `ready` and `closed` both reject with a TypeError, and a promise that had
               already settled is REPLACED so the identity change is visible. */
            JS_ThrowTypeError(ctx, "this writer was released while it was still in use");
            JS_FreeValue(ctx, s->w.err);
            s->w.err = JS_GetException(ctx);
            s->w.settle = W_READY_REJ;
            s->w.stage = S_SETTLE;
            s->next = S_RELEASE;
            break;

        default:
            DCHECK(op == OP_WC_ERROR, "a §5 machine ran with an operation this component does not have");
            if (d->state != WS_WRITABLE) return JS_STEP_DONE;
            JS_FreeValue(ctx, s->w.err);
            s->w.err = JS_DupValue(ctx, step_arg(&s->hdr, 0));
            s->w.stage = S_ERROR;
            break;
        }
    }

run:
    d = ws_of(s->stream);
    c = JS_IsUndefined(s->ctrl) ? NULL : wc_of(s->ctrl);

    for (;;) {
        if (s->w.stage == S_SETTLE) {
            if (s->w.settle == W_IDLE) { s->w.stage = s->next; continue; }
            r = ws_settle_run(ctx, &s->w, s->stream, cb_result, out_cb, out_argc);
            if (r > 0) return r;
            if (r < 0) return JS_STEP_ABRUPT;
            cb_result = JS_UNDEFINED;
            s->w.stage = s->next;
        }

        if (s->w.stage == S_SIGNAL) {
            /* §5.2 abort step 2: "signal abort on the controller's AbortController with reason". The WHOLE DOM
               operation, as a request — it runs the signal's abort algorithms and then fires `abort` at it, and
               both of those run code, which is why the state the stream is in is re-read in the next stage
               rather than remembered across this one. */
            if (!c || JS_IsUndefined(c->signal)) { s->w.stage = S_ABORT_SETUP; continue; }
            r = abort_signal_run(ctx, &s->sig, c->signal, s->w.value, cb_result, out_cb, out_argc);
            if (r > 0) return r;
            if (r < 0) return JS_STEP_ABRUPT;
            cb_result = JS_UNDEFINED;
            s->w.stage = S_ABORT_SETUP;
        }

        if (s->w.stage == S_ABORT_SETUP) {
            if (d->state == WS_CLOSED || d->state == WS_ERRORED) {
                if (ws_short(ctx, s, 0, JS_UNDEFINED) < 0) return JS_STEP_ABRUPT;
                continue;
            }
            if (d->abort_pending) {
                /* §5.2 step 5: a second abort() answers the FIRST one's promise. */
                JS_FreeValue(ctx, s->promise);
                s->promise = JS_DupValue(ctx, d->abort_p);
                s->w.stage = S_DONE;
                continue;
            }
            d->abort_was_erroring = (d->state == WS_ERRORING);
            JS_FreeValue(ctx, d->abort_reason);
            d->abort_reason = d->abort_was_erroring ? JS_UNDEFINED : JS_DupValue(ctx, s->w.value);
            JS_FreeValue(ctx, d->abort_p);
            d->abort_p = JS_NewPromiseCapability(ctx, d->abort_funcs);
            if (JS_IsException(d->abort_p)) return JS_STEP_ABRUPT;
            d->abort_pending = 1;
            s->promise = JS_DupValue(ctx, d->abort_p);
            if (d->abort_was_erroring) { s->w.stage = S_DONE; continue; }
            JS_FreeValue(ctx, s->w.err);
            s->w.err = JS_DupValue(ctx, s->w.value);
            s->w.stage = S_ERROR;
        }

        if (s->w.stage == S_SIZE) {
            /* §5.4's GetChunkSize. A `size` that THROWS errors the stream and the size is 1 — the throw does
               not propagate, and the state checks below are what then reject this write. */
            if (JS_IsUndefined(c->size_fn)) {
                s->size = 1;
                s->w.stage = S_WRITE_QUEUE;
            } else {
                JSValueConst arg = s->chunk;
                r = step_call_run(ctx, &s->w.phase, STEP_CB(s->w.cb), c->size_fn, JS_UNDEFINED, 1, &arg, cb_result,
                                  &out, out_cb, out_argc);
                if (r > 0) return r;
                cb_result = JS_UNDEFINED;
                s->size = 1;
                if (JS_IsException(out)) {
                    JS_FreeValue(ctx, s->w.err);
                    s->w.err = JS_GetException(ctx);
                    s->w.stage = d->state == WS_WRITABLE ? S_ERROR : S_WRITE_QUEUE;
                    s->tail = S_WRITE_QUEUE;
                    continue;
                }
                if (JS_ToFloat64(ctx, &s->size, out) < 0) JS_FreeValue(ctx, JS_GetException(ctx));
                JS_FreeValue(ctx, out);
                s->w.stage = S_WRITE_QUEUE;
            }
        }

        if (s->w.stage == S_WRITE_QUEUE) {
            WsWriterData *wr = wr_of(s->hdr.this_val);
            s->tail = S_DONE;
            if (!wr || JS_IsUndefined(wr->stream)) {
                JS_ThrowTypeError(ctx, "this writer has been released");
                if (ws_short(ctx, s, 1, JS_GetException(ctx)) < 0) return JS_STEP_ABRUPT;
                continue;
            }
            if (d->state == WS_ERRORED || d->state == WS_ERRORING) {
                if (ws_short(ctx, s, 1, JS_DupValue(ctx, d->stored_error)) < 0) return JS_STEP_ABRUPT;
                continue;
            }
            if (d->state == WS_CLOSED || ws_close_queued_or_in_flight(d)) {
                JS_ThrowTypeError(ctx, "this stream is closing or closed");
                if (ws_short(ctx, s, 1, JS_GetException(ctx)) < 0) return JS_STEP_ABRUPT;
                continue;
            }
            s->promise = ws_add_write_request(ctx, d);
            if (JS_IsException(s->promise)) return JS_STEP_ABRUPT;
            /* §5.4's EnqueueValueWithSize: a size that is not a finite non-negative number errors the stream
               instead of being queued, and the write request then rejects through the erroring chain. */
            if (!(s->size >= 0) || s->size != s->size || s->size > 1.7976931348623157e308) {
                JS_ThrowRangeError(ctx, "a queuing strategy's size must be a finite, non-negative number");
                JS_FreeValue(ctx, s->w.err);
                s->w.err = JS_GetException(ctx);
                s->w.stage = S_ERROR;
                continue;
            }
            wc_enqueue(ctx, c, s->chunk, s->size);
            s->w.settle = ws_update_backpressure(ctx, d, c);
            s->w.stage = S_SETTLE;
            s->next = S_ADVANCE;
            continue;
        }

        if (s->w.stage == S_ERROR) {
            /* §5.2's DealWithRejection: a stream still WRITABLE starts erroring; one already erroring only has
               to check whether it can finish. */
            if (d->state == WS_WRITABLE) {
                /* StartErroring does NOT touch the AbortSignal. Only §5.2's WritableStreamAbort signals it, and
                   the difference is the whole point of the signal: a sink watches it to learn that someone
                   ASKED to abort, not that a write of its own happened to fail. Signalling here told every sink
                   its own thrown error was an abort request. */
                ws_start_erroring(ctx, d, s->w.err);
                s->w.settle = W_READY_REJ;
                s->w.stage = S_SETTLE;
                s->next = S_FINISH_ERR;
                continue;
            }
            s->w.stage = S_FINISH_ERR;
        }

        if (s->w.stage == S_FINISH_ERR) {
            /* §5.2's FinishErroring runs only when NOTHING is in flight: the sink is still busy otherwise, and
               reporting the stream errored while the page's own write promise is pending is exactly the
               reordering the erroring state exists to prevent. */
            if (d->state != WS_ERRORING || ws_in_flight(d) || !c->started) { s->w.stage = s->tail; continue; }
            d->state = WS_ERRORED;
            wc_reset_queue(ctx, c);
            s->w.stage = S_ERR_WRITES;
        }

        if (s->w.stage == S_ERR_WRITES) {
            JS_FreeValue(ctx, s->w.err);
            s->w.err = JS_DupValue(ctx, d->stored_error);
            s->w.settle = W_REJECT_WRITES;
            s->w.stage = S_SETTLE;
            s->next = S_ERR_ABORT;
            continue;
        }

        if (s->w.stage == S_ERR_ABORT) {
            if (!d->abort_pending) { s->w.stage = S_ERR_CLOSE; continue; }
            if (d->abort_was_erroring) {
                /* §5.2 step 10: an abort that arrived while the stream was already failing rejects with the
                   stream's own reason rather than getting the sink asked a second time. */
                JS_FreeValue(ctx, s->w.err);
                s->w.err = JS_DupValue(ctx, d->stored_error);
                s->w.settle = W_ABORT_REJ;
                s->w.stage = S_SETTLE;
                s->next = S_ERR_CLOSE;
                continue;
            }
            if (JS_IsUndefined(c->abort_fn)) {
                s->w.settle = W_ABORT_RES;
                s->w.stage = S_SETTLE;
                s->next = S_ERR_CLOSE;
                continue;
            }
            {
                JSValueConst arg = d->abort_reason;
                r = step_call_run(ctx, &s->w.phase, STEP_CB(s->w.cb), c->abort_fn, c->sink, 1, &arg, cb_result, &out,
                                  out_cb, out_argc);
                if (r > 0) return r;
                cb_result = JS_UNDEFINED;
                /* §5.4 builds the abort algorithm with PromiseCall, so a throwing `abort` becomes a rejected
                   promise and takes the same reaction path a rejecting one does. */
                JS_FreeValue(ctx, s->w.value);
                s->is_close = 2;   /* the ABORT arm of the shared react stage */
                if (JS_IsException(out)) {
                    s->w.value = JS_GetException(ctx);
                    s->reject = 1;
                } else {
                    s->w.value = out;
                    s->reject = 0;
                }
                s->w.stage = S_SINK_REACT;
                continue;
            }
        }

        if (s->w.stage == S_ERR_CLOSE) {
            if (JS_IsUndefined(d->close_request[0])) { s->w.stage = S_ERR_CLOSED; continue; }
            JS_FreeValue(ctx, s->w.err);
            s->w.err = JS_DupValue(ctx, d->stored_error);
            s->w.settle = W_CLOSE_REQ_REJ;
            s->w.stage = S_SETTLE;
            s->next = S_ERR_CLOSED;
            continue;
        }

        if (s->w.stage == S_ERR_CLOSED) {
            JS_FreeValue(ctx, s->w.err);
            s->w.err = JS_DupValue(ctx, d->stored_error);
            s->w.settle = W_CLOSED_REJ;
            s->w.stage = S_SETTLE;
            s->next = s->tail;
            continue;
        }

        if (s->w.stage == S_ADVANCE) {
            /* §5.4's AdvanceQueueIfNeeded. */
            if (!c->started || ws_in_flight(d)) { s->w.stage = s->tail; continue; }
            if (d->state == WS_ERRORING) { s->w.stage = S_FINISH_ERR; continue; }
            if (wc_queued(ctx, c) == 0) { s->w.stage = s->tail; continue; }
            if (c->close_queued && wc_queued(ctx, c) == 1) {
                /* §5.4's ProcessClose: the sentinel is taken and dequeued at once. */
                JSValue pair[2];
                wc_dequeue(ctx, c);
                c->close_queued = 0;
                ws_take_pair(d->close_request, pair);
                d->in_flight_close[0] = pair[0];
                d->in_flight_close[1] = pair[1];
                s->w.stage = S_SINK_CLOSE;
            } else {
                /* §5.4's ProcessWrite PEEKS: the chunk stays on the queue while the write is in flight, so
                   `desiredSize` still counts it, and the fulfilment is what dequeues. */
                JSValue pair[2];
                JS_FreeValue(ctx, s->chunk);
                s->chunk = wc_peek(ctx, c);
                ws_take_write(ctx, d, pair);
                DCHECK(!JS_IsUndefined(pair[0]), "§5.4 advanced onto a chunk with no write request behind it");
                d->in_flight_write[0] = pair[0];
                d->in_flight_write[1] = pair[1];
                s->w.stage = S_SINK_WRITE;
            }
        }

        if (s->w.stage == S_SINK_WRITE || s->w.stage == S_SINK_CLOSE) {
            int is_close = (s->w.stage == S_SINK_CLOSE);
            JSValueConst args[2];
            JSValueConst fn = is_close ? c->close_fn : c->write_fn;
            /* §5.4 SetUpWritableStreamDefaultControllerFromUnderlyingSink gives each algorithm its OWN argument
               list, and they are not the same list: write is « chunk, controller », abort is « reason », and
               CLOSE IS EMPTY. The controller is what `start` was handed, so a close that also received it would
               be a second, undocumented way to reach it — and `close(){ arguments.length }` sees the difference. */
            args[0] = is_close ? JS_UNDEFINED : (JSValueConst)s->chunk;
            args[1] = s->ctrl;
            if (JS_IsUndefined(fn)) {
                /* §5.4's default algorithms accept everything and answer at once. */
                out = JS_UNDEFINED;
            } else {
                r = step_call_run(ctx, &s->w.phase, STEP_CB(s->w.cb), fn, c->sink, is_close ? 0 : 2, args,
                                  cb_result, &out, out_cb, out_argc);
                if (r > 0) return r;
            }
            cb_result = JS_UNDEFINED;
            s->is_close = (uint8_t)is_close;
            JS_FreeValue(ctx, s->w.value);
            if (JS_IsException(out)) {
                s->w.value = JS_GetException(ctx);
                s->reject = 1;
            } else {
                s->w.value = out;
                s->reject = 0;
            }
            s->w.stage = S_SINK_REACT;
        }

        if (s->w.stage == S_SINK_REACT) {
            static const int OKS[3]  = { OP_WRITE_OK,  OP_CLOSE_OK,  OP_ABORT_OK  };
            static const int ERRS[3] = { OP_WRITE_ERR, OP_CLOSE_ERR, OP_ABORT_ERR };
            int arm = s->is_close;
            r = stream_promise_of_run(ctx, &s->w, s->reject, cb_result, out_cb, out_argc);
            if (r > 0) return r;
            if (r < 0) return JS_STEP_ABRUPT;
            cb_result = JS_UNDEFINED;
            r = ws_react(ctx, s->w.func, s->ctrl, OKS[arm], ERRS[arm]);
            JS_FreeValue(ctx, s->w.func);
            s->w.func = JS_UNDEFINED;
            s->is_close = 0;
            if (r < 0) return JS_STEP_ABRUPT;
            s->w.stage = S_DONE;
            continue;
        }

        if (s->w.stage == S_WRITE_DONE) {
            /* §5.4's ProcessWrite fulfilment: the chunk leaves the queue now, not when the sink took it. */
            wc_dequeue(ctx, c);
            s->w.settle = (!ws_close_queued_or_in_flight(d) && d->state == WS_WRITABLE)
                        ? ws_update_backpressure(ctx, d, c) : W_IDLE;
            s->w.stage = S_SETTLE;
            s->next = S_ADVANCE;
            continue;
        }

        if (s->w.stage == S_CLOSE_DONE) {
            /* §5.2's FinishInFlightClose: a stream that was ERRORING when its close succeeded is CLOSED all
               the same — the abort it was carrying is answered and the error discarded. */
            if (d->state == WS_ERRORING) {
                JS_FreeValue(ctx, d->stored_error);
                d->stored_error = JS_UNDEFINED;
                if (d->abort_pending) {
                    s->w.settle = W_ABORT_RES;
                    s->w.stage = S_SETTLE;
                    s->next = S_CLOSE_DONE2;
                    continue;
                }
            }
            s->w.stage = S_CLOSE_DONE2;
        }

        if (s->w.stage == S_CLOSE_DONE2) {
            d->state = WS_CLOSED;
            s->w.settle = W_CLOSED_RES;
            s->w.stage = S_SETTLE;
            s->next = S_DONE;
            continue;
        }

        if (s->w.stage == S_CLOSE_ERR2) {
            /* FinishInFlightCloseWithError also rejects a pending abort with the same reason: the abort is
               what the close was serving. */
            if (d->abort_pending) {
                s->w.settle = W_ABORT_REJ;
                s->w.stage = S_SETTLE;
                s->next = S_ERROR;
                continue;
            }
            s->w.stage = S_ERROR;
            continue;
        }

        if (s->w.stage == S_RELEASE) {
            /* §5.3 release step 5, the SECOND of the two rejections. It is its own stage rather than a
               re-entry of this one testing `settle`, because S_SETTLE clears `settle` to W_IDLE when the
               sequence finishes — so the test could never be true and `closed` was never rejected at all. A
               stage that decides what to do next by reading a variable the step before it has just reset is a
               dead branch that reads like a live one. */
            s->w.settle = W_CLOSED_REJ;
            s->w.stage = S_SETTLE;
            s->next = S_RELEASE2;
            continue;
        }

        if (s->w.stage == S_RELEASE2) {
            WsWriterData *wr = wr_of(s->hdr.this_val);
            DCHECK(wr != NULL, "a §5.3 release lost its writer between two of its own stages");
            JS_FreeValue(ctx, d->writer);
            d->writer = JS_UNDEFINED;
            JS_FreeValue(ctx, wr->stream);
            wr->stream = JS_UNDEFINED;
            s->w.stage = S_DONE;
            continue;
        }

        if (s->w.stage == S_RESULT) {
            JSValueConst arg = s->w.value;
            r = step_call_run(ctx, &s->w.phase, STEP_CB(s->w.cb), s->funcs[s->reject], JS_UNDEFINED, 1, &arg,
                              cb_result, &out, out_cb, out_argc);
            if (r > 0) return r;
            if (JS_IsException(out)) return JS_STEP_ABRUPT;
            JS_FreeValue(ctx, out);
            return JS_STEP_DONE;
        }

        DCHECK(s->w.stage == S_DONE, "a §5 machine resumed in a stage it never parks in");
        JS_FreeValue(ctx, cb_result);
        return JS_STEP_DONE;
    }
}

#define WS_DEF(i) { sizeof(JSWsState), js_ws_step, js_ws_fini, (i), \
                    .catches_abrupt = 1, .visit = js_ws_visit }
static const JSTrampStepDef js_ws_defs[OP_N] = {
    WS_DEF(0),  WS_DEF(1),  WS_DEF(2),  WS_DEF(3),  WS_DEF(4),  WS_DEF(5),  WS_DEF(6),  WS_DEF(7),
    WS_DEF(8),  WS_DEF(9),  WS_DEF(10), WS_DEF(11), WS_DEF(12), WS_DEF(13), WS_DEF(14),
};
#undef WS_DEF

/* ---- the plain accessors ------------------------------------------------------------------------------------ */

static JSValue js_ws_locked(JSContext *ctx, JSValueConst this_val, int magic)
{
    WsData *d = ws_of(this_val);
    (void)magic;
    if (!d) return JS_ThrowTypeError(ctx, "not a WritableStream");
    return JS_NewBool(ctx, !JS_IsUndefined(d->writer));
}

enum { WR_CLOSED = 0, WR_READY, WR_DESIRED };

static JSValue js_wr_get(JSContext *ctx, JSValueConst this_val, int magic)
{
    WsWriterData *w = wr_of(this_val);
    if (!w) return JS_ThrowTypeError(ctx, "not a WritableStreamDefaultWriter");
    if (magic == WR_CLOSED) return JS_DupValue(ctx, w->closed);
    if (magic == WR_READY)  return JS_DupValue(ctx, w->ready);
    DCHECK(magic == WR_DESIRED, "a §5.3 attribute was declared with a magic this component does not answer");
    /* §5.3: a RELEASED writer's desiredSize is a TypeError, not null — null is what a stream that has failed
       answers, and a page tells the two apart. */
    if (JS_IsUndefined(w->stream)) return JS_ThrowTypeError(ctx, "this writer has been released");
    {
        WsData *d = ws_of(w->stream);
        DCHECK(d != NULL, "a writer held something that is not a WritableStream");
        if (d->state == WS_ERRORED || d->state == WS_ERRORING) return JS_NULL;
        if (d->state == WS_CLOSED) return JS_NewInt32(ctx, 0);
        return JS_NewFloat64(ctx, wc_desired(wc_of(d->controller)));
    }
}

static JSValue js_wc_signal(JSContext *ctx, JSValueConst this_val, int magic)
{
    WsCtrlData *c = wc_of(this_val);
    (void)magic;
    if (!c) return JS_ThrowTypeError(ctx, "not a WritableStreamDefaultController");
    return JS_DupValue(ctx, c->signal);
}

/* An interface with no constructor still has an INTERFACE OBJECT, and it is where the prototype's `constructor`
   comes from — a page reads that property list directly. */
static JSValue js_illegal_ctor(JSContext *ctx, JSValueConst nt, int argc, JSValueConst *argv)
{
    (void)nt; (void)argc; (void)argv;
    return JS_ThrowTypeError(ctx, "Illegal constructor");
}

/* ---- §5.3's set-up, which is `getWriter()` and the writer's constructor both ------------------------------- */

enum { GW_SELF = 0, GW_CTOR };
enum { GWS_START = 0, GWS_A, GWS_B, GWS_DONE };

typedef struct {
    StreamWork w;
    JSValue    obj;
    JSValue    stream;
    uint8_t    ready_kind, closed_kind;   /* W_* for each, or W_IDLE when it starts pending */
} JSGetWriterState;

static void js_gw_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSGetWriterState *s = st;
    stream_work_visit(ctx, &s->w, v);
    v->val(ctx, &s->obj);
    v->val(ctx, &s->stream);
}

static void js_gw_release(JSContext *ctx, void *st)
{
    JSGetWriterState *s = st;
    stream_work_release(ctx, &s->w);
    JS_FreeValue(ctx, s->obj);
    JS_FreeValue(ctx, s->stream);
    s->obj = s->stream = JS_UNDEFINED;
}

static int js_gw_step(JSContext *ctx, JSStepHdr *hdr, void *st, int argc, JSValueConst *argv,
                      JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    JSGetWriterState *s = st;
    int ctor = idl_step_magic(hdr) == GW_CTOR;
    JSValueConst stream_v = ctor ? (argc > 0 ? argv[0] : JS_UNDEFINED) : hdr->this_val;
    WsData *d;
    int r;

    if (s->w.stage == GWS_START) {
        WsWriterData *wr;
        JSValue obj;

        stream_work_start(&s->w);
        s->obj = s->stream = JS_UNDEFINED;
        s->w.stage = GWS_A;
        JS_FreeValue(ctx, cb_result);
        cb_result = JS_UNDEFINED;
        if (ctor && JS_IsUndefined(hdr->this_val)) {
            JS_ThrowTypeError(ctx, "constructor WritableStreamDefaultWriter requires 'new'");
            return -1;
        }
        d = ws_of(stream_v);
        if (!d) { JS_ThrowTypeError(ctx, "not a WritableStream"); return -1; }
        if (!JS_IsUndefined(d->writer)) {
            JS_ThrowTypeError(ctx, "this stream is already locked to a writer");
            return -1;
        }
        obj = JS_NewObjectProtoClass(ctx, g_wr_proto, g_wr_class);
        if (JS_IsException(obj)) return -1;
        wr = js_mallocz(ctx, sizeof *wr);
        if (!wr) { JS_FreeValue(ctx, obj); return -1; }
        wr->stream = wr->closed = wr->ready = JS_UNDEFINED;
        wr->closed_funcs[0] = wr->closed_funcs[1] = JS_UNDEFINED;
        wr->ready_funcs[0] = wr->ready_funcs[1] = JS_UNDEFINED;
        JS_SetOpaque(obj, wr);          /* attached before anything that can fail */
        s->obj = obj;
        s->stream = JS_DupValue(ctx, stream_v);
        wr->stream = JS_DupValue(ctx, stream_v);
        d->writer = JS_DupValue(ctx, obj);
        wr->closed = JS_NewPromiseCapability(ctx, wr->closed_funcs);
        if (JS_IsException(wr->closed)) return -1;
        wr->ready = JS_NewPromiseCapability(ctx, wr->ready_funcs);
        if (JS_IsException(wr->ready)) return -1;

        /* §5.3's set-up: WHICH of the two promises starts already settled depends on the state the writer
           arrived at. A stream that has already failed hands over a writer whose `ready` is rejected. */
        s->ready_kind = s->closed_kind = W_IDLE;
        switch (d->state) {
        case WS_WRITABLE:
            if (!ws_close_queued_or_in_flight(d) && d->backpressure) break;   /* both stay pending */
            s->ready_kind = W_READY_RES;
            break;
        case WS_ERRORING:
            s->ready_kind = W_READY_REJ;
            break;
        case WS_CLOSED:
            s->ready_kind = W_READY_RES;
            s->closed_kind = W_CLOSED_RES;
            break;
        default:
            DCHECK(d->state == WS_ERRORED, "a §5.3 writer was set up on a stream in no state");
            s->ready_kind = W_READY_REJ;
            s->closed_kind = W_CLOSED_REJ;
            break;
        }
        JS_FreeValue(ctx, s->w.err);
        s->w.err = JS_DupValue(ctx, d->stored_error);
    }

    if (s->w.stage == GWS_A) {
        if (s->ready_kind != W_IDLE) {
            s->w.settle = s->ready_kind;
            r = ws_settle_run(ctx, &s->w, s->stream, cb_result, out_cb, out_argc);
            if (r > 0) return r;
            if (r < 0) return -1;
            cb_result = JS_UNDEFINED;
        }
        s->w.stage = GWS_B;
    }
    if (s->w.stage == GWS_B) {
        if (s->closed_kind != W_IDLE) {
            s->w.settle = s->closed_kind;
            r = ws_settle_run(ctx, &s->w, s->stream, cb_result, out_cb, out_argc);
            if (r > 0) return r;
            if (r < 0) return -1;
            cb_result = JS_UNDEFINED;
        }
        s->w.stage = GWS_DONE;
    }
    JS_FreeValue(ctx, cb_result);
    *presult = s->obj;
    s->obj = JS_UNDEFINED;
    return 0;
}

static const IdlStepDecl js_gw_decl = { js_gw_step, sizeof(JSGetWriterState), js_gw_visit, js_gw_release };

/* ---- §5.2's constructor ------------------------------------------------------------------------------------- */

/* §7's QueuingStrategy, read at the IDL layer so a throwing `get size` is seen before a throwing `get start`. */
static const IdlDictMember QUEUING_STRATEGY[] = {
    { "highWaterMark", IDL_UNRESTRICTED_DOUBLE },
    { "size",          IDL_CALLBACK },
};

/* UnderlyingSink's members, in the order Web IDL reads them. */
enum { SNK_ABORT = 0, SNK_CLOSE, SNK_START, SNK_TYPE, SNK_WRITE, SNK_N };

enum { WSC_START = 0, WSC_PROTO, WSC_READ, WSC_BUILD, WSC_CALL, WSC_RESOLVE, WSC_THEN };

typedef struct {
    StreamWork w;
    uint8_t    member;
    JSValue    snk[SNK_N];
    JSValue    stream;
    JSValue    ctrl;
    JSValue    start_fn;
    JSValue    sink;
    JSValue    proto;
} JSWsCtorState;

static void js_ws_ctor_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSWsCtorState *s = st;
    int k;
    stream_work_visit(ctx, &s->w, v);
    for (k = 0; k < SNK_N; k++) v->val(ctx, &s->snk[k]);
    v->val(ctx, &s->stream);
    v->val(ctx, &s->ctrl);
    v->val(ctx, &s->start_fn);
    v->val(ctx, &s->sink);
    v->val(ctx, &s->proto);
}

static void js_ws_ctor_release(JSContext *ctx, void *st)
{
    JSWsCtorState *s = st;
    int k;
    stream_work_release(ctx, &s->w);
    for (k = 0; k < SNK_N; k++) { JS_FreeValue(ctx, s->snk[k]); s->snk[k] = JS_UNDEFINED; }
    JS_FreeValue(ctx, s->stream);
    JS_FreeValue(ctx, s->ctrl);
    JS_FreeValue(ctx, s->start_fn);
    JS_FreeValue(ctx, s->sink);
    JS_FreeValue(ctx, s->proto);
    s->stream = s->ctrl = s->start_fn = s->sink = s->proto = JS_UNDEFINED;
}

static int ctor_callback_member(JSContext *ctx, JSValueConst v, const char *name)
{
    if (JS_IsUndefined(v) || JS_IsFunction(ctx, v)) return 0;
    JS_ThrowTypeError(ctx, "underlying sink member `%s` is not callable", name);
    return -1;
}

static int js_ws_ctor_step(JSContext *ctx, JSStepHdr *hdr, void *st, int argc, JSValueConst *argv,
                           JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    static const char *const SNK_NAMES[SNK_N] = { "abort", "close", "start", "type", "write" };
    JSWsCtorState *s = st;
    JSValueConst sink = argc > 0 ? argv[0] : JS_UNDEFINED;
    int r;

    if (s->w.stage == WSC_START) {
        int k;
        stream_work_start(&s->w);
        for (k = 0; k < SNK_N; k++) s->snk[k] = JS_UNDEFINED;
        s->stream = s->ctrl = s->start_fn = s->proto = JS_UNDEFINED;
        s->sink = JS_DupValue(ctx, sink);
        s->member = 0;
        JS_FreeValue(ctx, cb_result);
        cb_result = JS_UNDEFINED;
        if (JS_IsUndefined(hdr->this_val)) {
            JS_ThrowTypeError(ctx, "constructor WritableStream requires 'new'");
            return -1;
        }
        if (!JS_IsUndefined(sink) && !JS_IsObject(sink)) {
            JS_ThrowTypeError(ctx, "the underlying sink must be an object");
            return -1;
        }
        s->w.stage = WSC_PROTO;
    }

    if (s->w.stage == WSC_PROTO) {
        /* Web IDL §3.7.1: the object is created from `? Get(newTarget, "prototype")` when that is an Object,
           and from the interface prototype object otherwise — which is the whole of what makes
           `class S extends WritableStream {}` produce an S. It is a REQUEST because new.target can be any
           constructor a `Reflect.construct` names, so the read is the page's own accessor. */
        JSAtom a = JS_NewAtom(ctx, "prototype");
        r = step_getprop_run(ctx, hdr, hdr->this_val, a, cb_result, &s->proto, out_cb, out_argc);
        JS_FreeAtom(ctx, a);
        if (r > 0) return r;
        if (r < 0) return -1;
        cb_result = JS_UNDEFINED;
        s->w.stage = JS_IsObject(sink) ? WSC_READ : WSC_BUILD;
    }

    while (s->w.stage == WSC_READ) {
        JSAtom a;
        if (s->member >= SNK_N) {
            /* §5.2 step 3: `type` is reserved, and any value for it is a RangeError rather than a silent
               fall-through to the default controller. */
            if (!JS_IsUndefined(s->snk[SNK_TYPE])) {
                JS_ThrowRangeError(ctx, "an underlying sink may not declare a `type`");
                return -1;
            }
            if (ctor_callback_member(ctx, s->snk[SNK_ABORT], "abort") < 0 ||
                ctor_callback_member(ctx, s->snk[SNK_CLOSE], "close") < 0 ||
                ctor_callback_member(ctx, s->snk[SNK_START], "start") < 0 ||
                ctor_callback_member(ctx, s->snk[SNK_WRITE], "write") < 0)
                return -1;
            s->w.stage = WSC_BUILD;
            break;
        }
        a = JS_NewAtom(ctx, SNK_NAMES[s->member]);
        r = step_getprop_run(ctx, hdr, sink, a, cb_result, &s->snk[s->member], out_cb, out_argc);
        JS_FreeAtom(ctx, a);
        if (r > 0) return r;
        if (r < 0) return -1;
        cb_result = JS_UNDEFINED;
        s->member++;
    }

    if (s->w.stage == WSC_BUILD) {
        JSValueConst strategy = argc > 1 ? argv[1] : JS_UNDEFINED;
        WsCtrlData *c;
        JSValue hv;
        double h = 1;
        int absent;

        JS_FreeValue(ctx, cb_result);
        cb_result = JS_UNDEFINED;
        /* §5.2 steps 5 and 6: ExtractSizeAlgorithm, then ExtractHighWaterMark with a default of 1. Both read
           the strategy dictionary the IDL layer has ALREADY converted, which is why a throwing `get size` is
           seen before a throwing `get start` — the ordering §5.2 states and a page pins. */
        hv = idl_dict_get(ctx, strategy, "highWaterMark");
        absent = JS_IsUndefined(hv);
        if (!absent && JS_ToFloat64(ctx, &h, hv) < 0) { JS_FreeValue(ctx, hv); return -1; }
        JS_FreeValue(ctx, hv);
        if (!absent && (h != h || h < 0)) {
            /* §5.2's own check, not the type's: `unrestricted double` accepts NaN and the STREAM rejects it. */
            JS_ThrowRangeError(ctx, "a queuing strategy's highWaterMark must not be negative or NaN");
            return -1;
        }
        {
            JSValue size_fn = idl_dict_get(ctx, strategy, "size");
            s->stream = writable_stream_create(ctx, s->snk[SNK_WRITE], s->snk[SNK_CLOSE], s->snk[SNK_ABORT],
                                               absent ? 1 : h, size_fn);
            JS_FreeValue(ctx, size_fn);
        }
        if (JS_IsException(s->stream)) return -1;
        /* §3.7.1's prototype, applied to the object CreateWritableStream just built: the operation makes a
           WritableStream and the CONSTRUCTOR decides which one a subclass gets. */
        if (JS_IsObject(s->proto) && JS_SetPrototype(ctx, s->stream, s->proto) < 0) return -1;
        s->ctrl = JS_DupValue(ctx, ws_of(s->stream)->controller);
        c = wc_of(s->ctrl);
        DCHECK(c != NULL, "CreateWritableStream answered a stream with no controller");
        /* The SINK is the receiver §5.4 invokes the page's methods on — CreateWritableStream has no sink at
           all, so it is set here rather than inside the operation. */
        JS_FreeValue(ctx, c->sink);
        c->sink = JS_DupValue(ctx, sink);
        s->start_fn = s->snk[SNK_START];  s->snk[SNK_START] = JS_UNDEFINED;
        s->w.value = JS_UNDEFINED;
        s->w.stage = JS_IsFunction(ctx, s->start_fn) ? WSC_CALL : WSC_RESOLVE;
    }

    if (s->w.stage == WSC_CALL) {
        JSValue res;
        JSValueConst arg = s->ctrl;
        r = step_call_run(ctx, &s->w.phase, STEP_CB(s->w.cb), s->start_fn, s->sink, 1, &arg, cb_result, &res,
                          out_cb, out_argc);
        if (r > 0) return r;
        /* §5.4 invokes `start` directly rather than through PromiseCall, so a throw propagates out of the
           constructor — which is why this machine does not declare catches_abrupt. */
        JS_FreeValue(ctx, s->w.value);
        s->w.value = res;
        cb_result = JS_UNDEFINED;
        s->w.stage = WSC_RESOLVE;
    }
    if (s->w.stage == WSC_RESOLVE) {
        r = stream_promise_of_run(ctx, &s->w, 0, cb_result, out_cb, out_argc);
        if (r > 0) return r;
        if (r < 0) return -1;
        cb_result = JS_UNDEFINED;
        s->w.stage = WSC_THEN;
    }

    DCHECK(s->w.stage == WSC_THEN, "the WritableStream constructor resumed in a stage it never parks in");
    JS_FreeValue(ctx, cb_result);
    if (writable_stream_start(ctx, s->stream, s->w.func) < 0) return -1;
    *presult = s->stream;
    s->stream = JS_UNDEFINED;
    return 0;
}

static const IdlStepDecl js_ws_ctor_decl = {
    js_ws_ctor_step, sizeof(JSWsCtorState), js_ws_ctor_visit, js_ws_ctor_release
};

/* §5.4's CreateWritableStream — a stream whose ALGORITHMS are the caller's function objects rather than a
   page's underlying sink. It is the operation §6's TransformStream is built out of: a transform stream's
   writable half has no sink object at all, only closures over the transform stream itself.
   The three algorithms are called with the arguments the matching underlying-sink member takes — `write(chunk)`,
   `close()`, `abort(reason)` — and with `this` = the sink, which for this operation is undefined. `size_fn` may
   be undefined for the implicit one-per-chunk strategy. All four are BORROWED.
   THE START ALGORITHM IS NOT HERE. Starting is a separate operation because the caller decides what the start
   promise is and when it settles; see writable_stream_start. Until it runs the controller is not started, which
   is exactly the state §5.4 wants a stream to be in while its start algorithm is outstanding. */
JSValue writable_stream_create(JSContext *ctx, JSValueConst write_fn, JSValueConst close_fn,
                               JSValueConst abort_fn, double hwm, JSValueConst size_fn)
{
    JSValue obj, ctrl;
    WsData *d;
    WsCtrlData *c;

    DCHECK(g_ws_rt != NULL, "a WritableStream was created before writable_stream_init ran");
    /* The stream and its controller are COMPLETE — every owned slot placed, each attached to its object, the
       two linked — before the first step that can fail, so a failure after this is torn down by the two
       finalizers and nothing is orphaned. */
    obj = JS_NewObjectProtoClass(ctx, g_ws_proto, g_ws_class);
    if (JS_IsException(obj)) return obj;
    d = js_mallocz(ctx, sizeof *d);
    if (!d) { JS_FreeValue(ctx, obj); return JS_EXCEPTION; }
    d->stored_error = d->writer = d->controller = JS_UNDEFINED;
    d->abort_p = d->abort_reason = JS_UNDEFINED;
    d->in_flight_write[0] = d->in_flight_write[1] = JS_UNDEFINED;
    d->in_flight_close[0] = d->in_flight_close[1] = JS_UNDEFINED;
    d->close_request[0] = d->close_request[1] = JS_UNDEFINED;
    d->abort_funcs[0] = d->abort_funcs[1] = JS_UNDEFINED;
    d->write_resolve = JS_NewArray(ctx);
    d->write_reject = JS_NewArray(ctx);
    JS_SetOpaque(obj, d);
    if (JS_IsException(d->write_resolve) || JS_IsException(d->write_reject)) goto fail;

    ctrl = JS_NewObjectProtoClass(ctx, g_wc_proto, g_wc_class);
    if (JS_IsException(ctrl)) goto fail;
    c = js_mallocz(ctx, sizeof *c);
    if (!c) { JS_FreeValue(ctx, ctrl); goto fail; }
    c->stream = c->sink = c->write_fn = c->close_fn = c->abort_fn = c->size_fn = JS_UNDEFINED;
    c->signal = JS_UNDEFINED;
    c->hwm = 1;
    c->queue = JS_NewArray(ctx);
    c->queue_size = JS_NewArray(ctx);
    JS_SetOpaque(ctrl, c);
    d->controller = ctrl;                       /* the stream owns it from here */
    if (JS_IsException(c->queue) || JS_IsException(c->queue_size)) goto fail;
    c->stream = JS_DupValue(ctx, obj);
    c->signal = abort_signal_new(ctx);
    if (JS_IsException(c->signal)) { c->signal = JS_UNDEFINED; goto fail; }

    c->write_fn = JS_DupValue(ctx, write_fn);
    c->close_fn = JS_DupValue(ctx, close_fn);
    c->abort_fn = JS_DupValue(ctx, abort_fn);
    c->size_fn = JS_DupValue(ctx, size_fn);
    c->hwm = hwm;
    /* §5.2 step 6: the stream starts with backpressure exactly when its mark is already met. */
    d->backpressure = (uint8_t)wc_backpressure(c);
    return obj;

fail:
    JS_FreeValue(ctx, obj);
    return JS_EXCEPTION;
}

int writable_stream_start(JSContext *ctx, JSValueConst stream, JSValueConst start_promise)
{
    WsData *d = ws_of(stream);
    DCHECK(d != NULL, "a §5 start was reported for something that is not a WritableStream");
    return ws_react(ctx, start_promise, d->controller, OP_START_OK, OP_START_ERR);
}

/* ---- install ------------------------------------------------------------------------------------------------ */

void writable_stream_init(JSContext *ctx)
{
    JSClassDef sd = { "WritableStream", .finalizer = ws_finalizer, .gc_mark = ws_gc_mark };
    JSClassDef rd = { "WritableStreamDefaultWriter", .finalizer = wr_finalizer, .gc_mark = wr_gc_mark };
    JSClassDef cd = { "WritableStreamDefaultController", .finalizer = wc_finalizer, .gc_mark = wc_gc_mark };
    JSRuntime *rt = JS_GetRuntime(ctx);
    static const IdlArgType ONE_ANY[1] = { IDL_ANY };
    static const IdlArgType SINK_AND_STRATEGY[2] = { IDL_ANY, IDL_DICT };
    int i;

    DCHECK(g_ws_rt == NULL || g_ws_rt == rt, "WritableStream was installed into a second runtime");
    if (g_ws_rt == rt) return;
    g_ws_rt = rt;
    JS_NewClassID(rt, &g_ws_class);  JS_NewClass(rt, g_ws_class, &sd);
    JS_NewClassID(rt, &g_wr_class);  JS_NewClass(rt, g_wr_class, &rd);
    JS_NewClassID(rt, &g_wc_class);  JS_NewClass(rt, g_wc_class, &cd);

    for (i = 0; i < OP_N; i++) {
        g_op_stepid[i] = JS_RegisterStepDef(rt, &js_ws_defs[i]);
        CHECK(g_op_stepid[i] >= 0, "streams: no step id for a §5 operation");
    }

    g_ws_proto = JS_NewObject(ctx);
    CHECK(!JS_IsException(g_ws_proto), "WritableStream.prototype could not be allocated");
    idl_interface_tag(ctx, g_ws_proto, "WritableStream");
    idl_install_accessor(ctx, g_ws_proto, "locked", js_ws_locked, 0, -1);
    idl_install_step_method(ctx, g_ws_proto, "abort", 0, g_op_stepid[OP_WS_ABORT]);
    idl_install_step_method(ctx, g_ws_proto, "close", 0, g_op_stepid[OP_WS_CLOSE]);
    g_getwriter_stepid = idl_method_id_step(ctx, NULL, 0, NULL, 0, &js_gw_decl, GW_SELF);
    idl_install_method(ctx, g_ws_proto, "getWriter", 0, g_getwriter_stepid);

    g_wr_proto = JS_NewObject(ctx);
    CHECK(!JS_IsException(g_wr_proto), "WritableStreamDefaultWriter.prototype could not be allocated");
    idl_interface_tag(ctx, g_wr_proto, "WritableStreamDefaultWriter");
    idl_install_accessor(ctx, g_wr_proto, "closed", js_wr_get, WR_CLOSED, -1);
    idl_install_accessor(ctx, g_wr_proto, "ready", js_wr_get, WR_READY, -1);
    idl_install_accessor(ctx, g_wr_proto, "desiredSize", js_wr_get, WR_DESIRED, -1);
    idl_install_step_method(ctx, g_wr_proto, "abort", 0, g_op_stepid[OP_WR_ABORT]);
    idl_install_step_method(ctx, g_wr_proto, "close", 0, g_op_stepid[OP_WR_CLOSE]);
    idl_install_step_method(ctx, g_wr_proto, "write", 0, g_op_stepid[OP_WR_WRITE]);
    idl_install_step_method(ctx, g_wr_proto, "releaseLock", 0, g_op_stepid[OP_WR_RELEASE]);

    g_wc_proto = JS_NewObject(ctx);
    CHECK(!JS_IsException(g_wc_proto), "the §5.4 controller prototype could not be allocated");
    idl_interface_tag(ctx, g_wc_proto, "WritableStreamDefaultController");
    idl_install_accessor(ctx, g_wc_proto, "signal", js_wc_signal, 0, -1);
    idl_install_step_method(ctx, g_wc_proto, "error", 0, g_op_stepid[OP_WC_ERROR]);

    /* THE ABSTRACT OPERATIONS, read off the prototypes ONCE, while they are still the ones just installed. */
    g_op_fn[WS_OP_GET_WRITER]   = JS_GetPropertyStr(ctx, g_ws_proto, "getWriter");
    g_op_fn[WS_OP_STREAM_ABORT] = JS_GetPropertyStr(ctx, g_ws_proto, "abort");
    g_op_fn[WS_OP_WRITE]        = JS_GetPropertyStr(ctx, g_wr_proto, "write");
    g_op_fn[WS_OP_CLOSE]        = JS_GetPropertyStr(ctx, g_wr_proto, "close");
    g_op_fn[WS_OP_ABORT]        = JS_GetPropertyStr(ctx, g_wr_proto, "abort");
    g_op_fn[WS_OP_RELEASE]      = JS_GetPropertyStr(ctx, g_wr_proto, "releaseLock");
    for (i = 0; i < WS_OP_N; i++)
        CHECK(JS_IsFunction(ctx, g_op_fn[i]), "a §5 abstract operation was not installed before it was captured");

    g_ws_ctor_stepid = idl_method_id_step(ctx, SINK_AND_STRATEGY, 2, QUEUING_STRATEGY,
                                          (int)(sizeof QUEUING_STRATEGY / sizeof QUEUING_STRATEGY[0]),
                                          &js_ws_ctor_decl, 0);
    idl_optional_from(0);   /* §5.2: both constructor arguments are optional */
    g_wr_ctor_stepid = idl_method_id_step(ctx, ONE_ANY, 1, NULL, 0, &js_gw_decl, GW_CTOR);
}

void writable_stream_install(JSContext *ctx, JSValueConst global)
{
    JSValue ctor;
    DCHECK(g_ws_ctor_stepid >= 0, "WritableStream was installed before its constructor was declared");
    ctor = idl_step_constructor(ctx, "WritableStream", 0, g_ws_ctor_stepid);
    CHECK(!JS_IsException(ctor), "the WritableStream interface object could not be allocated");
    JS_SetConstructor(ctx, ctor, g_ws_proto);
    JS_SetPropertyStr(ctx, (JSValue)global, "WritableStream", ctor);

    ctor = idl_step_constructor(ctx, "WritableStreamDefaultWriter", 1, g_wr_ctor_stepid);
    CHECK(!JS_IsException(ctor), "the writer interface object could not be allocated");
    JS_SetConstructor(ctx, ctor, g_wr_proto);
    JS_SetPropertyStr(ctx, (JSValue)global, "WritableStreamDefaultWriter", ctor);

    ctor = JS_NewCFunction2(ctx, js_illegal_ctor, "WritableStreamDefaultController", 0,
                            JS_CFUNC_constructor, 0);
    CHECK(!JS_IsException(ctor), "the §5.4 controller interface object could not be allocated");
    JS_SetConstructor(ctx, ctor, g_wc_proto);
    JS_SetPropertyStr(ctx, (JSValue)global, "WritableStreamDefaultController", ctor);
}

void writable_stream_free(JSContext *ctx)
{
    int i;
    if (!g_ws_rt) return;
    JS_FreeValue(ctx, g_ws_proto);
    JS_FreeValue(ctx, g_wr_proto);
    JS_FreeValue(ctx, g_wc_proto);
    g_ws_proto = g_wr_proto = g_wc_proto = JS_UNDEFINED;
    for (i = 0; i < WS_OP_N; i++) { JS_FreeValue(ctx, g_op_fn[i]); g_op_fn[i] = JS_UNDEFINED; }
    g_ws_rt = NULL;
    g_ws_ctor_stepid = g_wr_ctor_stepid = g_getwriter_stepid = -1;
    for (i = 0; i < OP_N; i++) g_op_stepid[i] = -1;
}
