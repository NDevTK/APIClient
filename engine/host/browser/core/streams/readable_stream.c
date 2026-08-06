/* ReadableStream — the Streams Standard §4.2 and §4.3.
 *
 * WHY IT IS BUILT NOW. It is the largest absent component by a wide margin: 74 corpus failures name it
 * directly, 19 more name TextDecoderStream, and every `response.body.getReader()` is another. It is also the
 * one place this engine's own architecture is most on display — a reader's `read()` settles a promise, and
 * settling is the page's code, so it is a machine like every other member that touches one.
 *
 * WHAT IS HERE AND WHAT IS NOT. The stream, the default reader, and a source that is the HOST'S BYTES. What is
 * absent is the page-supplied underlying source — `new ReadableStream({start, pull, cancel})` — and it is
 * absent for a reason that is the next piece of work rather than a judgement about it: §4.5's
 * ReadableStreamDefaultControllerCallPullIfNeeded reacts to the pull promise by calling the page's `pull` AGAIN
 * and, on rejection, by erroring the controller, which rejects every parked read. Attaching those reactions
 * without reading the page-visible `.then` needs PerformPromiseThen, which is internal to quickjs.c. The
 * constructor crashes naming that, which is the honest state — and the gate reports a file's subtests even when
 * it aborts, so nothing is hidden by it.
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
} StreamData;

typedef struct {
    JSValue stream;       /* the stream this reader locks, or JS_UNDEFINED once released */
    JSValue closed;       /* §4.3's `closed` promise */
    JSValue closed_funcs[2];
} ReaderData;

static JSClassID g_stream_class, g_reader_class;
static JSValue   g_stream_proto = JS_UNDEFINED, g_reader_proto = JS_UNDEFINED;
static JSRuntime *g_rs_rt;
static int       g_ctor_stepid = -1, g_reader_ctor_stepid = -1, g_read_stepid = -1, g_cancel_stepid = -1;

static void stream_finalizer(JSRuntime *rt, JSValue val)
{
    StreamData *d = JS_GetOpaque(val, g_stream_class);
    if (!d) return;
    JS_FreeValueRT(rt, d->stored_error);
    JS_FreeValueRT(rt, d->reader);
    JS_FreeValueRT(rt, d->queue);
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

static StreamData *stream_of(JSValueConst v) { return JS_GetOpaque(v, g_stream_class); }
static ReaderData *reader_of(JSValueConst v) { return JS_GetOpaque(v, g_reader_class); }

/* How many chunks are still unread. §4.2 has no length to expose; this is the queue's own bookkeeping. */
static uint32_t stream_queued(JSContext *ctx, StreamData *d)
{
    JSValue len_v = JS_GetPropertyStr(ctx, d->queue, "length");
    uint32_t n = 0;
    JS_ToUint32(ctx, &n, len_v);
    JS_FreeValue(ctx, len_v);
    return n - d->head;
}

JSValue readable_stream_from_bytes(JSContext *ctx, const char *bytes, size_t len)
{
    StreamData *d;
    JSValue obj, chunk;

    DCHECK(g_stream_class != 0, "a ReadableStream was built before the class existed");
    obj = JS_NewObjectProtoClass(ctx, g_stream_proto, g_stream_class);
    if (JS_IsException(obj)) return obj;
    d = js_mallocz(ctx, sizeof *d);
    if (!d) { JS_FreeValue(ctx, obj); return JS_EXCEPTION; }
    d->stored_error = JS_UNDEFINED;
    d->reader = JS_UNDEFINED;
    d->queue = JS_NewArray(ctx);
    JS_SetOpaque(obj, d);
    if (JS_IsException(d->queue)) { JS_FreeValue(ctx, obj); return JS_EXCEPTION; }
    /* ONE CHUNK, then closed. A byte sequence the host already holds has nothing left to arrive, and §4.2's
       "close" is exactly that statement — not an empty queue, which is a stream that may still be fed. */
    chunk = JS_NewUint8ArrayCopy(ctx, (const uint8_t *)(bytes ? bytes : ""), len);
    if (JS_IsException(chunk)) { JS_FreeValue(ctx, obj); return JS_EXCEPTION; }
    JS_SetPropertyUint32(ctx, d->queue, 0, chunk);
    d->state = RS_CLOSED;
    return obj;
}

/* ---- §4.3's reader ---------------------------------------------------------------------------------------- */

/* §4.3's `read()`. A MACHINE, because settling the promise it returns is the PAGE'S code: 27.2.1.3.2 step 8
 * reads `Get(resolution, "then")` off the result object, whose prototype the page owns. The same reason
 * body.c's readers are machines, and the same shape. */
typedef struct {
    JSStepHdr hdr;
    uint8_t   stage;
    uint8_t   cphase;
    uint8_t   reject;
    JSValue   promise;
    JSValue   func;
    JSValue   value;
    JSValue   cb[3];
} JSReadState;

static void js_read_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSReadState *s = st;
    int k;
    v->val(ctx, &s->promise);
    v->val(ctx, &s->func);
    v->val(ctx, &s->value);
    for (k = 0; k < 3; k++) v->val(ctx, &s->cb[k]);
}

static JSValue js_read_fini(JSContext *ctx, void *st, bool take_result)
{
    JSReadState *s = st;
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

/* §4.3's read result: `{ value, done }`, a plain object this component builds — so nothing of the page's is on
   it until the settle hands it over. */
static JSValue read_result(JSContext *ctx, JSValue value, bool done)
{
    JSValue o = JS_NewObject(ctx);
    if (JS_IsException(o)) { JS_FreeValue(ctx, value); return o; }
    JS_SetPropertyStr(ctx, o, "value", value);
    JS_SetPropertyStr(ctx, o, "done", JS_NewBool(ctx, done));
    return o;
}

static int js_read_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    JSReadState *s = st;
    JSValue settled, funcs[2];
    int r;

    if (s->stage == 0) {
        ReaderData *rd = reader_of(s->hdr.this_val);
        StreamData *d;

        JS_FreeValue(ctx, cb_result);
        cb_result = JS_UNDEFINED;
        if (!rd) {
            JS_ThrowTypeError(ctx, "not a ReadableStreamDefaultReader");
            return JS_STEP_ABRUPT;
        }
        if (JS_IsUndefined(rd->stream)) {
            /* §4.3: a released reader's read() REJECTS rather than throwing — it is a promise-returning
               member, and a page's `.catch` is where it expects to see this. */
            JS_ThrowTypeError(ctx, "this reader has been released");
            s->value = JS_GetException(ctx);
            s->reject = 1;
        } else {
            d = stream_of(rd->stream);
            DCHECK(d != NULL, "a reader's stream stopped being a ReadableStream while it held the lock");
            d->disturbed = 1;
            if (d->state == RS_ERRORED) {
                s->value = JS_DupValue(ctx, d->stored_error);
                s->reject = 1;
            } else if (stream_queued(ctx, d) > 0) {
                JSValue chunk = JS_GetPropertyUint32(ctx, d->queue, d->head++);
                s->value = read_result(ctx, chunk, false);
            } else if (d->state == RS_CLOSED) {
                s->value = read_result(ctx, JS_UNDEFINED, true);
            } else {
                /* A READABLE STREAM WITH AN EMPTY QUEUE parks the read request until something is enqueued —
                   and nothing can be, because the only source this component builds is the host's bytes, which
                   are all present before the stream exists. A page-supplied source is what makes this
                   reachable, and it is the piece this file does not have yet. */
                DFAIL("a read found a readable stream with an empty queue — build the parked read-request "
                      "queue, which is what a page-supplied underlying source needs and what §4.5's controller "
                      "fills");
            }
            if (JS_IsException(s->value)) return JS_STEP_ABRUPT;
        }
        s->promise = JS_NewPromiseCapability(ctx, funcs);
        if (JS_IsException(s->promise)) return JS_STEP_ABRUPT;
        s->func = funcs[s->reject];
        JS_FreeValue(ctx, funcs[s->reject ^ 1]);
        s->stage = 1;
    }

    DCHECK(s->stage == 1, "the read machine was re-entered at a stage it never parks in");
    r = step_call_run(ctx, &s->cphase, s->cb, s->func, JS_UNDEFINED, 1, (JSValueConst *)&s->value,
                      cb_result, &settled, out_cb, out_argc);
    if (r > 0) return r;
    JS_FreeValue(ctx, settled);
    return JS_STEP_DONE;
}

static const JSTrampStepDef js_read_def = {
    sizeof(JSReadState), js_read_step, js_read_fini, 0, .visit = js_read_visit
};

/* §4.3's `releaseLock()` and §4.2's `locked`. Neither runs the page's code — the closed promise a released
   reader rejects with is settled by the same machine `read` uses, and until that exists the release simply
   drops the lock, which is the observable half a page uses to hand the stream to someone else. */
static JSValue js_reader_release(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    ReaderData *r = reader_of(this_val);
    StreamData *d;

    (void)argc; (void)argv; (void)magic;
    if (!r) return JS_ThrowTypeError(ctx, "not a ReadableStreamDefaultReader");
    if (JS_IsUndefined(r->stream)) return JS_UNDEFINED;   /* §4.3: releasing twice is a no-op */
    d = stream_of(r->stream);
    if (d) { JS_FreeValue(ctx, d->reader); d->reader = JS_UNDEFINED; }
    JS_FreeValue(ctx, r->stream);
    r->stream = JS_UNDEFINED;
    return JS_UNDEFINED;
}

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

/* §4.2's `getReader(options)`. `mode: "byob"` names the BYTE STREAM reader, which is a different interface over
   a different controller — absent here, and a TypeError is what §4.2 says for a stream that is not one. */
static JSValue js_stream_get_reader(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
                                    int magic)
{
    StreamData *d = stream_of(this_val);
    ReaderData *r;
    JSValue obj, mode;

    (void)magic;
    if (!d) return JS_ThrowTypeError(ctx, "not a ReadableStream");
    if (!JS_IsUndefined(d->reader))
        return JS_ThrowTypeError(ctx, "this stream is already locked to a reader");
    mode = argc > 0 ? idl_dict_get(ctx, argv[0], "mode") : JS_UNDEFINED;
    if (!JS_IsUndefined(mode)) {
        const char *m = JS_ToCString(ctx, mode);
        int byob = m && !strcmp(m, "byob");
        if (m) JS_FreeCString(ctx, m);
        JS_FreeValue(ctx, mode);
        if (byob)
            return JS_ThrowTypeError(ctx, "this stream is not a byte stream, so it has no BYOB reader");
    } else {
        JS_FreeValue(ctx, mode);
    }
    obj = JS_NewObjectProtoClass(ctx, g_reader_proto, g_reader_class);
    if (JS_IsException(obj)) return obj;
    r = js_mallocz(ctx, sizeof *r);
    if (!r) { JS_FreeValue(ctx, obj); return JS_EXCEPTION; }
    r->stream = JS_DupValue(ctx, this_val);
    r->closed = JS_NewPromiseCapability(ctx, r->closed_funcs);
    JS_SetOpaque(obj, r);
    if (JS_IsException(r->closed)) { JS_FreeValue(ctx, obj); return JS_EXCEPTION; }
    d->reader = JS_DupValue(ctx, obj);
    return obj;
}

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

/* ---- the constructors -------------------------------------------------------------------------------------- */

typedef struct { uint8_t unused; } JSRsCtorState;
static void js_rs_ctor_visit(JSContext *ctx, void *st, JSStepVisit *v) { (void)ctx; (void)st; (void)v; }
static void js_rs_ctor_release(JSContext *ctx, void *st) { (void)ctx; (void)st; }

static int js_rs_ctor_step(JSContext *ctx, JSStepHdr *hdr, void *st, int argc, JSValueConst *argv,
                           JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    (void)st; (void)argc; (void)argv; (void)out_cb; (void)out_argc; (void)presult;
    JS_FreeValue(ctx, cb_result);
    if (JS_IsUndefined(hdr->this_val)) {
        JS_ThrowTypeError(ctx, "constructor ReadableStream requires 'new'");
        return -1;
    }
    DFAIL("new ReadableStream(underlyingSource) — build §4.5's default controller and the pull chaining it "
          "needs: CallPullIfNeeded reacts to the pull promise by calling the page's `pull` again and, on "
          "rejection, by erroring the controller, so the reactions must be machines with the controller "
          "captured, attached with PerformPromiseThen rather than a page-visible `.then` read");
    return -1;
}

static const IdlStepDecl js_rs_ctor_decl = {
    js_rs_ctor_step, sizeof(JSRsCtorState), js_rs_ctor_visit, js_rs_ctor_release
};

static int js_reader_ctor_step(JSContext *ctx, JSStepHdr *hdr, void *st, int argc, JSValueConst *argv,
                               JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    (void)st; (void)out_cb; (void)out_argc;
    JS_FreeValue(ctx, cb_result);
    if (JS_IsUndefined(hdr->this_val)) {
        JS_ThrowTypeError(ctx, "constructor ReadableStreamDefaultReader requires 'new'");
        return -1;
    }
    /* §4.3's constructor is `getReader()` spelled the other way, so it IS that member — one implementation,
       reached from two names, which is what the spec's own "set up" step means. */
    *presult = js_stream_get_reader(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, 0, NULL, 0);
    return JS_IsException(*presult) ? -1 : 0;
}

static const IdlStepDecl js_reader_ctor_decl = {
    js_reader_ctor_step, sizeof(JSRsCtorState), js_rs_ctor_visit, js_rs_ctor_release
};

/* ---- install ------------------------------------------------------------------------------------------------ */

void readable_stream_init(JSContext *ctx)
{
    JSClassDef sd = { "ReadableStream", .finalizer = stream_finalizer, .gc_mark = stream_gc_mark };
    JSClassDef rd = { "ReadableStreamDefaultReader", .finalizer = reader_finalizer, .gc_mark = reader_gc_mark };
    JSRuntime *rt = JS_GetRuntime(ctx);
    static const IdlArgType ONE_ANY[1] = { IDL_ANY };
    static const IdlArgType TWO_ANY[2] = { IDL_ANY, IDL_ANY };

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
                       idl_method_id(ctx, ONE_ANY, 1, js_stream_get_reader, 0));
    idl_optional_from(0);   /* §4.2: `getReader(optional ReadableStreamGetReaderOptions options = {})` */
    g_cancel_stepid = JS_RegisterStepDef(rt, &js_cancel_def);
    CHECK(g_cancel_stepid >= 0, "streams: no step id for cancel");
    JS_SetPropertyStr(ctx, g_stream_proto, "cancel",
                      JS_NewCFunction2(ctx, NULL, "cancel", 0, JS_CFUNC_step, g_cancel_stepid));

    g_reader_proto = JS_NewObject(ctx);
    CHECK(!JS_IsException(g_reader_proto), "ReadableStreamDefaultReader.prototype could not be allocated");
    idl_interface_tag(ctx, g_reader_proto, "ReadableStreamDefaultReader");
    idl_install_accessor(ctx, g_reader_proto, "closed", js_reader_closed, 0, -1);
    idl_install_method(ctx, g_reader_proto, "releaseLock", 0,
                       idl_method_id(ctx, ONE_ANY, 0, js_reader_release, 0));
    g_read_stepid = JS_RegisterStepDef(rt, &js_read_def);
    CHECK(g_read_stepid >= 0, "streams: no step id for read");
    JS_SetPropertyStr(ctx, g_reader_proto, "read",
                      JS_NewCFunction2(ctx, NULL, "read", 0, JS_CFUNC_step, g_read_stepid));
    JS_SetPropertyStr(ctx, g_reader_proto, "cancel",
                      JS_NewCFunction2(ctx, NULL, "cancel", 0, JS_CFUNC_step, g_cancel_stepid));

    g_ctor_stepid = idl_method_id_step(ctx, TWO_ANY, 2, NULL, 0, &js_rs_ctor_decl, 0);
    idl_optional_from(0);   /* §4.2: both constructor arguments are optional */
    g_reader_ctor_stepid = idl_method_id_step(ctx, ONE_ANY, 1, NULL, 0, &js_reader_ctor_decl, 0);
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
    if (!g_rs_rt) return;
    JS_FreeValue(ctx, g_stream_proto);
    JS_FreeValue(ctx, g_reader_proto);
    g_stream_proto = g_reader_proto = JS_UNDEFINED;
    g_rs_rt = NULL;
    g_ctor_stepid = g_reader_ctor_stepid = g_read_stepid = g_cancel_stepid = -1;
}
