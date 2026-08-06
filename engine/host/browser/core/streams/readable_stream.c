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
#include "core/streams/readable_stream.h"

/* §4.2's states. A stream is readable until it is closed or errored, and those two are terminal. */
enum { RS_READABLE = 0, RS_CLOSED, RS_ERRORED };

typedef struct {
    uint8_t state;
    uint8_t disturbed;    /* §4.2: set by the first read, and what `bodyUsed` is defined over */
    JSValue stored_error;
    JSValue reader;       /* the ReadableStreamDefaultReader holding the lock, or JS_UNDEFINED */
    JSValue queue;        /* an Array of chunks not yet read */
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
    uint8_t started;
    uint8_t pulling;
    uint8_t pull_again;
    uint8_t close_requested;  /* §4.5's [[closeRequested]]: closed once the queue drains, not at once */
} ControllerData;

static JSClassID g_stream_class, g_reader_class, g_ctrl_class;
static JSValue   g_stream_proto = JS_UNDEFINED, g_reader_proto = JS_UNDEFINED, g_ctrl_proto = JS_UNDEFINED;
static JSRuntime *g_rs_rt;
static int       g_ctor_stepid = -1, g_reader_ctor_stepid = -1, g_read_stepid = -1, g_cancel_stepid = -1;
static int       g_release_stepid = -1;
static int       g_ctrl_stepids[3] = { -1, -1, -1 };
static int       g_rxn_stepids[4] = { -1, -1, -1, -1 };

static void stream_finalizer(JSRuntime *rt, JSValue val)
{
    StreamData *d = JS_GetOpaque(val, g_stream_class);
    if (!d) return;
    JS_FreeValueRT(rt, d->stored_error);
    JS_FreeValueRT(rt, d->reader);
    JS_FreeValueRT(rt, d->queue);
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
    js_free_rt(rt, c);
}

static void ctrl_gc_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func)
{
    ControllerData *c = JS_GetOpaque(val, g_ctrl_class);
    if (!c) return;
    JS_MarkValue(rt, c->stream, mark_func);
    JS_MarkValue(rt, c->pull_fn, mark_func);
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
    d->read_resolve = JS_NewArray(ctx);
    d->read_reject = JS_NewArray(ctx);
    JS_SetOpaque(obj, d);
    if (JS_IsException(d->queue) || JS_IsException(d->read_resolve) || JS_IsException(d->read_reject)) {
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
    JS_SetPropertyUint32(ctx, d->queue, 0, chunk);
    d->state = RS_CLOSED;
    return obj;
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
enum { P_IDLE = 0, P_TEST, P_CALL, P_RESOLVE, P_THEN, P_ERROR };

typedef struct {
    uint8_t stage;   /* the owning MACHINE's own step */
    uint8_t phase;   /* step_call_run's, for whichever call is in flight */
    uint8_t settle;  /* ReadableStreamClose's / ReadableStreamError's */
    uint8_t pull;    /* CallPullIfNeeded's */
    JSValue func;    /* the function being called, or the promise a chain is being attached to */
    JSValue value;   /* its argument */
    JSValue chain;   /* a capability's resolve function, while PromiseResolve is in flight */
    JSValue err;     /* the reason a ControllerError sequence is carrying, until the stream adopts it */
    JSValue cb[3];   /* step_call_run's buffer: [this, func, arg] */
} CtrlWork;

static void ctrl_work_visit(JSContext *ctx, CtrlWork *w, JSStepVisit *v)
{
    int k;
    v->val(ctx, &w->func);
    v->val(ctx, &w->value);
    v->val(ctx, &w->chain);
    v->val(ctx, &w->err);
    for (k = 0; k < 3; k++) v->val(ctx, &w->cb[k]);
}

static void ctrl_work_release(JSContext *ctx, CtrlWork *w)
{
    int k;
    JS_FreeValue(ctx, w->func);
    JS_FreeValue(ctx, w->value);
    JS_FreeValue(ctx, w->chain);
    JS_FreeValue(ctx, w->err);
    w->func = w->value = w->chain = w->err = JS_UNDEFINED;
    for (k = 0; k < 3; k++) { JS_FreeValue(ctx, w->cb[k]); w->cb[k] = JS_UNDEFINED; }
}

/* §4.3's `closed` promise, settled EXACTLY ONCE: it RESOLVES when the stream closes, REJECTS when it errors,
 * and REJECTS with a TypeError when the reader is released. Settling it is a call of the page's code like every
 * other settle, so it is a call request. `rd` may be NULL (a stream nobody is reading) and `value` is read only
 * on the first entry, which is the only entry that has one. */
static int reader_closed_run(JSContext *ctx, CtrlWork *w, ReaderData *rd, int reject, JSValueConst value,
                             JSValue in, JSValue **out_cb, int *out_argc)
{
    JSValueConst arg;
    JSValue out;
    int r;

    if (w->phase == 0) {
        if (!rd || rd->closed_settled) { JS_FreeValue(ctx, in); return 0; }
        rd->closed_settled = 1;
        JS_FreeValue(ctx, w->func);
        w->func = rd->closed_funcs[reject];               /* the pair is HANDED OVER and dropped together */
        JS_FreeValue(ctx, rd->closed_funcs[reject ^ 1]);
        rd->closed_funcs[0] = rd->closed_funcs[1] = JS_UNDEFINED;
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
static int stream_settle_run(JSContext *ctx, CtrlWork *w, StreamData *d, JSValue in,
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
            JS_FreeValue(ctx, d->queue);
            d->queue = JS_NewArray(ctx);   /* §4.5's ResetQueue: an errored stream has no chunks left to give */
            d->head = 0;
            if (JS_IsException(d->queue)) { JS_FreeValue(ctx, in); return -1; }
        } else {
            JS_FreeValue(ctx, w->err);
            w->err = JS_UNDEFINED;
        }
        w->settle = S_ERR_CLOSED;
    }

    if (w->settle == S_CLOSE_CLOSED || w->settle == S_ERR_CLOSED || w->settle == S_REL_CLOSED) {
        int reject = w->settle != S_CLOSE_CLOSED;
        JSValueConst v = w->settle == S_ERR_CLOSED ? (JSValueConst)d->stored_error : (JSValueConst)w->err;
        r = reader_closed_run(ctx, w, rd, reject, v, in, out_cb, out_argc);
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

/* PromiseResolve(%Promise%, v) — 27.2.4.7 — as a sub-sequence. §4.5 reacts to what `start` and `pull` RETURNED,
 * which may be a plain value, a page THENABLE, or a promise; the one operation covering all three is a
 * capability whose RESOLVE function is called with it, and calling that function is exactly where 27.2.1.3.2
 * step 8 reads `then` off the page's object. So it is a call request like every other run of the page's code,
 * rather than a `JS_IsFunction(then)` test that would answer a patched thenable wrongly.
 * Takes `w->value`; leaves the capability's promise in `w->func`. */
static int step_promise_of_run(JSContext *ctx, CtrlWork *w, JSValue in, JSValue **out_cb, int *out_argc)
{
    JSValueConst arg;
    JSValue out;
    int r;

    if (w->phase == 0) {
        JSValue funcs[2];
        JSValue p = JS_NewPromiseCapability(ctx, funcs);
        if (JS_IsException(p)) { JS_FreeValue(ctx, in); return -1; }
        JS_FreeValue(ctx, w->func);
        w->func = p;
        JS_FreeValue(ctx, w->chain);
        w->chain = funcs[0];
        /* the reject half is dropped: 27.2.1.3.2 rejects through the resolve function's own steps when the
           `then` read throws, so nothing outside them ever needs it */
        JS_FreeValue(ctx, funcs[1]);
    }
    arg = w->value;
    r = step_call_run(ctx, &w->phase, w->cb, w->chain, JS_UNDEFINED, 1, &arg, in, &out, out_cb, out_argc);
    if (r > 0) return r;
    if (JS_IsException(out)) return -1;
    JS_FreeValue(ctx, out);
    JS_FreeValue(ctx, w->chain);
    w->chain = JS_UNDEFINED;
    return 0;
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
    JSValue onf, onr, cap;

    DCHECK(g_rxn_stepids[ok] >= 0 && g_rxn_stepids[err] >= 0,
           "a stream reaction was attached before its machines were registered");
    data[0] = ctrl;
    onf = JS_NewStepClosure(ctx, g_rxn_stepids[ok], 1, 1, data);
    if (JS_IsException(onf)) return -1;
    onr = JS_NewStepClosure(ctx, g_rxn_stepids[err], 1, 1, data);
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
    return stream_queued(ctx, d) < 1;
}

/* §4.5's ReadableStreamDefaultControllerCallPullIfNeeded. The caller sets `w->pull = P_TEST` and calls until it
   returns 0; every machine that can change what ShouldCallPull answers runs it. */
static int ctrl_pull_run(JSContext *ctx, CtrlWork *w, JSValueConst ctrl_v, JSValue in,
                         JSValue **out_cb, int *out_argc)
{
    ControllerData *c = ctrl_of(ctrl_v);
    StreamData *d;
    int r;

    DCHECK(c != NULL, "the pull sequence ran on a value that is not a ReadableStreamDefaultController");
    d = stream_of(c->stream);
    DCHECK(d != NULL, "a controller's stream stopped being a ReadableStream");

    if (w->pull == P_ERROR) {
        r = stream_settle_run(ctx, w, d, in, out_cb, out_argc);
        if (r != 0) return r;
        w->pull = P_IDLE;
        return 0;
    }
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
        r = step_call_run(ctx, &w->phase, w->cb, c->pull_fn, JS_UNDEFINED, 1, &arg, in, &res,
                          out_cb, out_argc);
        if (r > 0) return r;
        if (JS_IsException(res)) {
            /* §4.9.4 builds the pull algorithm with PromiseCall, so a `pull` that THROWS rejects its promise
               rather than propagating — and §4.5 answers that rejection by erroring the controller, which is
               why these machines declare catches_abrupt. */
            c->pulling = 0;
            w->err = JS_GetException(ctx);
            w->settle = S_ERR_SET;
            w->pull = P_ERROR;
            r = stream_settle_run(ctx, w, d, JS_UNDEFINED, out_cb, out_argc);
            if (r != 0) return r;
            w->pull = P_IDLE;
            return 0;
        }
        JS_FreeValue(ctx, w->value);
        w->value = res;
        w->pull = P_RESOLVE;
        in = JS_UNDEFINED;
    }
    if (w->pull == P_RESOLVE) {
        r = step_promise_of_run(ctx, w, in, out_cb, out_argc);
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
    CtrlWork  w;
} JSRxnState;

static void js_rxn_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSRxnState *s = st;
    ctrl_work_visit(ctx, &s->w, v);
}

static JSValue js_rxn_fini(JSContext *ctx, void *st, bool take_result)
{
    JSRxnState *s = st;
    (void)take_result;
    ctrl_work_release(ctx, &s->w);
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
    CtrlWork  w;
    JSValue   promise;   /* the capability handed back to the page */
    JSValue   settle;    /* its resolve or reject function, or JS_UNDEFINED when the request PARKED */
    JSValue   result;    /* what to settle it with */
    JSValue   stream;    /* the stream, so the close and pull stages can reach it */
} JSReadState;

static void js_read_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSReadState *s = st;
    ctrl_work_visit(ctx, &s->w, v);
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
    ctrl_work_release(ctx, &s->w);
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
            if (d->state == RS_ERRORED) {
                s->result = JS_DupValue(ctx, d->stored_error);
                reject = 1;
            } else if (stream_queued(ctx, d) > 0) {
                ControllerData *c = JS_IsUndefined(d->controller) ? NULL : ctrl_of(d->controller);
                JSValue chunk = JS_GetPropertyUint32(ctx, d->queue, d->head++);
                s->result = read_result(ctx, chunk, false);
                /* §4.5's PullSteps: draining the LAST chunk of a stream whose close was requested is what
                   actually closes it — a page that calls close() with chunks still queued must see every one
                   of them before `done`. Otherwise the drain is what makes room, so the source is pulled. */
                if (c && c->close_requested && stream_queued(ctx, d) == 0) {
                    s->w.stage = RD_CLOSE;
                    s->w.settle = S_CLOSE_SET;
                } else if (c) {
                    s->w.stage = RD_PULL;
                    s->w.pull = P_TEST;
                }
            } else if (d->state == RS_CLOSED) {
                s->result = read_result(ctx, JS_UNDEFINED, true);
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
                if (!JS_IsUndefined(d->controller)) {
                    s->w.stage = RD_PULL;
                    s->w.pull = P_TEST;
                }
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
    CtrlWork  w;
    /* THE STREAM, HELD BY THE MACHINE. The release clears the reader's own pointer to it partway through the
       sequence — that IS the release — so a resume that read it back off the reader would find nothing. */
    JSValue   stream;
} JSReleaseState;

static void js_release_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSReleaseState *s = st;
    ctrl_work_visit(ctx, &s->w, v);
    v->val(ctx, &s->stream);
}

static JSValue js_release_fini(JSContext *ctx, void *st, bool take_result)
{
    JSReleaseState *s = st;
    (void)take_result;
    ctrl_work_release(ctx, &s->w);
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
    CtrlWork w;
    JSValue  reader;
} JSGetReaderState;

static void js_get_reader_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSGetReaderState *s = st;
    ctrl_work_visit(ctx, &s->w, v);
    v->val(ctx, &s->reader);
}

static void js_get_reader_release(JSContext *ctx, void *st)
{
    JSGetReaderState *s = st;
    ctrl_work_release(ctx, &s->w);
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

/* §4.2's `cancel(reason)`. It empties the queue and closes the stream, and answers a promise. The settle is the
   page's code for the reason `read`'s is, so it is the same machine shape. */
typedef struct {
    JSStepHdr hdr;
    uint8_t   stage;
    uint8_t   cphase;
    JSValue   promise;
    JSValue   func;
    JSValue   value;
    JSValue   cb[3];
} JSCancelState;

static void js_cancel_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSCancelState *s = st;
    int k;
    v->val(ctx, &s->promise);
    v->val(ctx, &s->func);
    v->val(ctx, &s->value);
    for (k = 0; k < 3; k++) v->val(ctx, &s->cb[k]);
}

static JSValue js_cancel_fini(JSContext *ctx, void *st, bool take_result)
{
    JSCancelState *s = st;
    JSValue r = take_result ? s->promise : JS_UNDEFINED;
    int k;
    if (take_result) s->promise = JS_UNDEFINED;
    JS_FreeValue(ctx, s->promise);
    JS_FreeValue(ctx, s->func);
    JS_FreeValue(ctx, s->value);
    s->promise = s->func = s->value = JS_UNDEFINED;
    for (k = 0; k < 3; k++) { JS_FreeValue(ctx, s->cb[k]); s->cb[k] = JS_UNDEFINED; }
    return r;
}

static int js_cancel_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    JSCancelState *s = st;
    JSValue settled, funcs[2];
    int r, reject = 0;

    if (s->stage == 0) {
        /* The receiver is the STREAM for `stream.cancel()` and the READER for `reader.cancel()`; a reader
           cancels the stream it holds, which §4.3 says in as many words. */
        StreamData *d = stream_of(s->hdr.this_val);
        ReaderData *rd = d ? NULL : reader_of(s->hdr.this_val);

        JS_FreeValue(ctx, cb_result);
        cb_result = JS_UNDEFINED;
        if (!d && rd && !JS_IsUndefined(rd->stream)) d = stream_of(rd->stream);
        if (!d) {
            JS_ThrowTypeError(ctx, "cancel was called on neither a ReadableStream nor an active reader");
            s->value = JS_GetException(ctx);
            reject = 1;
        } else if (!rd && !JS_IsUndefined(d->reader)) {
            /* §4.2: cancelling a LOCKED stream through the stream itself is a TypeError — the reader owns it. */
            JS_ThrowTypeError(ctx, "this stream is locked to a reader");
            s->value = JS_GetException(ctx);
            reject = 1;
        } else {
            d->disturbed = 1;
            d->state = RS_CLOSED;
            JS_FreeValue(ctx, d->queue);
            d->queue = JS_NewArray(ctx);
            d->head = 0;
            if (JS_IsException(d->queue)) return JS_STEP_ABRUPT;
            s->value = JS_UNDEFINED;
        }
        s->promise = JS_NewPromiseCapability(ctx, funcs);
        if (JS_IsException(s->promise)) return JS_STEP_ABRUPT;
        s->func = funcs[reject];
        JS_FreeValue(ctx, funcs[reject ^ 1]);
        s->stage = 1;
    }

    DCHECK(s->stage == 1, "the cancel machine was re-entered at a stage it never parks in");
    r = step_call_run(ctx, &s->cphase, s->cb, s->func, JS_UNDEFINED, 1, (JSValueConst *)&s->value,
                      cb_result, &settled, out_cb, out_argc);
    if (r > 0) return r;
    JS_FreeValue(ctx, settled);
    return JS_STEP_DONE;
}

static const JSTrampStepDef js_cancel_def = {
    sizeof(JSCancelState), js_cancel_step, js_cancel_fini, 0, .visit = js_cancel_visit
};

/* ---- §4.5's DEFAULT CONTROLLER MEMBERS -----------------------------------------------------------------------
 *
 * `enqueue`, `close` and `error` are each MACHINES, because each may answer a PARKED READ — and answering one
 * settles a promise, which is the page's code. `enqueue` additionally ends in CallPullIfNeeded, which calls the
 * page's `pull`. That is the whole reason this component is machines rather than functions. */
enum { CTRL_ENQUEUE = 0, CTRL_CLOSE, CTRL_ERROR };
enum { CS_START = 0, CS_SETTLE, CS_SETTLE_ALL, CS_PULL };

typedef struct {
    JSStepHdr hdr;
    CtrlWork  w;
} JSCtrlState;

static void js_ctrl_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSCtrlState *s = st;
    ctrl_work_visit(ctx, &s->w, v);
}

static JSValue js_ctrl_fini(JSContext *ctx, void *st, bool take_result)
{
    JSCtrlState *s = st;
    (void)take_result;
    ctrl_work_release(ctx, &s->w);
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

        JS_FreeValue(ctx, cb_result);
        cb_result = JS_UNDEFINED;
        if (op == CTRL_ENQUEUE) {
            if (!ctrl_can_close_or_enqueue(c, d)) {
                JS_ThrowTypeError(ctx, "this stream can no longer be enqueued to");
                return JS_STEP_ABRUPT;
            }
            /* §4.5: a chunk goes to a WAITING READER if there is one, and to the queue otherwise. Queuing it
               when a read is parked would answer that read out of order, or never. */
            s->w.func = stream_take_read(ctx, d, 0);
            if (JS_IsUndefined(s->w.func)) {
                JSValue len_v = JS_GetPropertyStr(ctx, d->queue, "length");
                uint32_t n = 0;
                JS_ToUint32(ctx, &n, len_v);
                JS_FreeValue(ctx, len_v);
                JS_SetPropertyUint32(ctx, d->queue, n, JS_DupValue(ctx, a0));
                s->w.stage = CS_PULL;
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
    return JS_NewInt32(ctx, 1 - (int)stream_queued(ctx, d));
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
enum { RSC_START = 0, RSC_READ, RSC_TYPE, RSC_BUILD, RSC_CALL, RSC_RESOLVE, RSC_THEN };
/* UnderlyingSource's members, in the order Web IDL reads them. */
enum { SRC_CHUNKSIZE = 0, SRC_CANCEL, SRC_PULL, SRC_START, SRC_TYPE, SRC_N };

typedef struct {
    CtrlWork w;
    uint8_t  member;         /* which UnderlyingSource member the read loop is on */
    JSValue  src[SRC_N];     /* what each read answered, before any of them is type-checked */
    JSValue  stream;
    JSValue  controller;
    JSValue  start_fn;
} JSRsCtorState;

static void js_rs_ctor_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSRsCtorState *s = st;
    int k;
    ctrl_work_visit(ctx, &s->w, v);
    for (k = 0; k < SRC_N; k++) v->val(ctx, &s->src[k]);
    v->val(ctx, &s->stream);
    v->val(ctx, &s->controller);
    v->val(ctx, &s->start_fn);
}

static void js_rs_ctor_release(JSContext *ctx, void *st)
{
    JSRsCtorState *s = st;
    int k;
    ctrl_work_release(ctx, &s->w);
    for (k = 0; k < SRC_N; k++) { JS_FreeValue(ctx, s->src[k]); s->src[k] = JS_UNDEFINED; }
    JS_FreeValue(ctx, s->stream);
    JS_FreeValue(ctx, s->controller);
    JS_FreeValue(ctx, s->start_fn);
    s->stream = s->controller = s->start_fn = JS_UNDEFINED;
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
        JS_FreeValue(ctx, cb_result);
        cb_result = JS_UNDEFINED;
        if (JS_IsUndefined(hdr->this_val)) {
            JS_ThrowTypeError(ctx, "constructor ReadableStream requires 'new'");
            return -1;
        }
        for (k = 0; k < SRC_N; k++) s->src[k] = JS_UNDEFINED;
        s->stream = s->controller = s->start_fn = JS_UNDEFINED;
        s->w.value = JS_UNDEFINED;   /* the start result PromiseResolve sees when there is no `start` */
        s->member = 0;
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

        JS_FreeValue(ctx, cb_result);
        cb_result = JS_UNDEFINED;
        /* §4.2 builds the stream and its controller before `start` runs, because `start` is handed that
           controller and may enqueue into it immediately. */
        s->stream = readable_stream_empty(ctx);
        if (JS_IsException(s->stream)) return -1;
        d = stream_of(s->stream);
        s->controller = JS_NewObjectProtoClass(ctx, g_ctrl_proto, g_ctrl_class);
        if (JS_IsException(s->controller)) return -1;
        c = js_mallocz(ctx, sizeof *c);
        if (!c) return -1;
        c->stream = JS_DupValue(ctx, s->stream);
        c->pull_fn = s->src[SRC_PULL];
        s->src[SRC_PULL] = JS_UNDEFINED;
        JS_SetOpaque(s->controller, c);
        d->controller = JS_DupValue(ctx, s->controller);
        s->start_fn = s->src[SRC_START];
        s->src[SRC_START] = JS_UNDEFINED;
        s->w.stage = JS_IsFunction(ctx, s->start_fn) ? RSC_CALL : RSC_RESOLVE;
    }

    if (s->w.stage == RSC_CALL) {
        JSValue res;
        arg = s->controller;
        r = step_call_run(ctx, &s->w.phase, s->w.cb, s->start_fn, JS_UNDEFINED, 1, &arg,
                          cb_result, &res, out_cb, out_argc);
        if (r > 0) return r;
        JS_FreeValue(ctx, s->w.value);
        s->w.value = res;
        cb_result = JS_UNDEFINED;
        s->w.stage = RSC_RESOLVE;
    }
    if (s->w.stage == RSC_RESOLVE) {
        r = step_promise_of_run(ctx, &s->w, cb_result, out_cb, out_argc);
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
    static const IdlArgType TWO_ANY[2] = { IDL_ANY, IDL_ANY };
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

    g_ctor_stepid = idl_method_id_step(ctx, TWO_ANY, 2, NULL, 0, &js_rs_ctor_decl, 0);
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
    JS_SetPropertyStr(ctx, (JSValue)global, "ReadableStream", ctor);

    ctor = idl_step_constructor(ctx, "ReadableStreamDefaultReader", 1, g_reader_ctor_stepid);
    CHECK(!JS_IsException(ctor), "the reader interface object could not be allocated");
    JS_SetConstructor(ctx, ctor, g_reader_proto);
    JS_SetPropertyStr(ctx, (JSValue)global, "ReadableStreamDefaultReader", ctor);
}

void readable_stream_free(JSContext *ctx)
{
    int i;
    if (!g_rs_rt) return;
    JS_FreeValue(ctx, g_stream_proto);
    JS_FreeValue(ctx, g_reader_proto);
    JS_FreeValue(ctx, g_ctrl_proto);
    g_stream_proto = g_reader_proto = g_ctrl_proto = JS_UNDEFINED;
    g_rs_rt = NULL;
    g_ctor_stepid = g_reader_ctor_stepid = g_read_stepid = g_cancel_stepid = g_release_stepid = -1;
    for (i = 0; i < 3; i++) g_ctrl_stepids[i] = -1;
    for (i = 0; i < 4; i++) g_rxn_stepids[i] = -1;
}
